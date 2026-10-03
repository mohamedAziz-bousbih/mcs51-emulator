/*
 * test_serial.c - serial port timing and framing in modes 0-3.
 *
 * Baud rate setup used for modes 1 and 3: timer 1 in mode 2 with
 * TH1 = FDh overflows every 3 machine cycles; with SMOD = 0 the 16x baud
 * clock ticks every 6 cycles and one bit lasts 96 machine cycles
 * (9600 baud at 11.0592 MHz). After reset the free-running divide-by-16
 * first rolls over at cycle 96.
 */
#include <string.h>

#include "testkit.h"

#define SFR(c, a) MCS51_SFR(c, a)

typedef struct tx_log {
    const mcs51 *cpu;
    unsigned count;
    uint16_t data[32];
    uint64_t when[32];
} tx_log;

static void log_tx(void *ctx, uint16_t data)
{
    tx_log *log = ctx;
    if (log->count < 32) {
        log->data[log->count] = data;
        log->when[log->count] = log->cpu->cycles;
    }
    log->count++;
}

static mcs51 *uart_cpu(uint8_t scon, bool smod, tx_log *log)
{
    mcs51 *cpu = tk_cpu(NULL, 0);
    memset(cpu->code, 0x00, sizeof cpu->code);
    SFR(cpu, MCS51_TMOD) = 0x20;
    SFR(cpu, MCS51_TH1) = 0xFD;
    SFR(cpu, MCS51_TL1) = 0xFD;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR1;
    SFR(cpu, MCS51_SCON) = scon;
    SFR(cpu, MCS51_PCON) = smod ? MCS51_PCON_SMOD : 0;
    memset(log, 0, sizeof *log);
    log->cpu = cpu;
    cpu->io.serial_tx = log_tx;
    cpu->io.serial_ctx = log;
    return cpu;
}

/* Step until flag is set in SCON; returns the cycle count at that point. */
static uint64_t until_flag(mcs51 *cpu, uint8_t flag, unsigned max_cycles)
{
    while (!(SFR(cpu, MCS51_SCON) & flag) && cpu->cycles < max_cycles)
        mcs51_step(cpu);
    return cpu->cycles;
}

/* Mode 1: the frame starts at the next divide-by-16 rollover (cycle 96) and
 * TI rises at the start of the stop bit, 9 bit times later. Sending the next
 * byte as soon as TI is seen gives one frame (10 bits) every 960 cycles. */
static void test_mode1_transmit_timing(void)
{
    static tx_log log;
    mcs51 *cpu = uart_cpu(0x40, false, &log);
    static const char text[] = "8051";
    mcs51_write_direct(cpu, MCS51_SBUF, (uint8_t)text[0]);
    for (unsigned i = 1; i <= 4; i++) {
        until_flag(cpu, MCS51_SCON_TI, 20000);
        SFR(cpu, MCS51_SCON) &= (uint8_t)~MCS51_SCON_TI;
        if (i < 4)
            mcs51_write_direct(cpu, MCS51_SBUF, (uint8_t)text[i]);
    }
    CHECK_EQ(log.count, 4);
    CHECK_EQ(log.when[0], 96 + 9 * 96);
    for (unsigned i = 0; i < 4; i++)
        CHECK_EQ(log.data[i], (uint8_t)text[i]);
    for (unsigned i = 1; i < 4; i++)
        CHECK_EQ(log.when[i] - log.when[i - 1], 960);
}

/* SMOD = 1 removes the divide-by-2: twice the baud rate. */
static void test_mode1_smod_doubles_baud_rate(void)
{
    static tx_log log;
    mcs51 *cpu = uart_cpu(0x40, true, &log);
    mcs51_write_direct(cpu, MCS51_SBUF, 'U');
    CHECK_EQ(until_flag(cpu, MCS51_SCON_TI, 5000), 48 + 9 * 48);
}

/* Mode 1 reception: the start bit begins at the first 16x tick (cycle 6);
 * the byte is taken at the middle of the stop bit, 16 * 9 + 9 ticks later. */
static void test_mode1_receive(void)
{
    static tx_log log;
    mcs51 *cpu = uart_cpu(0x50, false, &log);
    CHECK(mcs51_serial_inject(cpu, 'A'));
    CHECK_EQ(until_flag(cpu, MCS51_SCON_RI, 5000), 6 + (16 * 9 + 9) * 6);
    CHECK_EQ(mcs51_read_direct(cpu, MCS51_SBUF), 'A');
    CHECK(SFR(cpu, MCS51_SCON) & MCS51_SCON_RB8); /* stop bit */
}

/* REN = 0 keeps frames waiting on the line. */
static void test_receiver_disabled(void)
{
    static tx_log log;
    mcs51 *cpu = uart_cpu(0x40, false, &log);
    CHECK(mcs51_serial_inject(cpu, 'B'));
    CHECK_EQ(until_flag(cpu, MCS51_SCON_RI, 3000), 3000);
    CHECK_EQ(cpu->serial.q_count, 1);
    mcs51_write_direct(cpu, MCS51_SCON, 0x50);
    until_flag(cpu, MCS51_SCON_RI, 6000);
    CHECK_EQ(mcs51_read_direct(cpu, MCS51_SBUF), 'B');
}

/* A frame that completes while RI is still set is lost (overrun). */
static void test_receive_overrun(void)
{
    static tx_log log;
    mcs51 *cpu = uart_cpu(0x50, false, &log);
    mcs51_serial_inject(cpu, 0x11);
    mcs51_serial_inject(cpu, 0x22);
    until_flag(cpu, MCS51_SCON_RI, 5000);
    for (unsigned i = 0; i < 1200; i++)
        mcs51_step(cpu);
    CHECK_EQ(cpu->serial.overruns, 1);
    CHECK_EQ(mcs51_read_direct(cpu, MCS51_SBUF), 0x11);
}

/* Mode 3 multiprocessor reception: with SM2 = 1 only frames whose ninth bit
 * is 1 (address frames) set RI. */
static void test_mode3_sm2_filter(void)
{
    static tx_log log;
    mcs51 *cpu = uart_cpu(0xF0, false, &log); /* mode 3, SM2, REN */
    mcs51_serial_inject(cpu, 0x055);           /* data frame: ignored */
    mcs51_serial_inject(cpu, 0x1AA);           /* address frame */
    uint64_t t = until_flag(cpu, MCS51_SCON_RI, 10000);
    CHECK_EQ(mcs51_read_direct(cpu, MCS51_SBUF), 0xAA);
    CHECK(SFR(cpu, MCS51_SCON) & MCS51_SCON_RB8);
    CHECK(t > 11 * 96); /* the first frame passed without setting RI */
    CHECK_EQ(cpu->serial.overruns, 0);
}

/* Mode 3 transmission: 11-bit frame, TB8 travels as the ninth bit; TI at
 * the start of the stop bit, 10 bit times after the start bit. */
static void test_mode3_transmit_tb8(void)
{
    static tx_log log;
    mcs51 *cpu = uart_cpu(0xC8, false, &log); /* mode 3, TB8 = 1 */
    mcs51_write_direct(cpu, MCS51_SBUF, 0x42);
    CHECK_EQ(until_flag(cpu, MCS51_SCON_TI, 5000), 96 + 10 * 96);
    CHECK_EQ(log.count, 1);
    CHECK_EQ(log.data[0], 0x142);
}

/* Mode 2: fixed baud rate fosc/64, i.e. 16 baud ticks per 16/3 machine
 * cycles; three back-to-back 11-bit frames take 3 * 11 * 16 / 3 = 176. */
static void test_mode2_fixed_baud_rate(void)
{
    static tx_log log;
    mcs51 *cpu = uart_cpu(0x80, false, &log);
    SFR(cpu, MCS51_TCON) = 0; /* timer 1 is not involved */
    mcs51_write_direct(cpu, MCS51_SBUF, 1);
    for (unsigned i = 2; i <= 5; i++) {
        until_flag(cpu, MCS51_SCON_TI, 2000);
        SFR(cpu, MCS51_SCON) &= (uint8_t)~MCS51_SCON_TI;
        if (i <= 4)
            mcs51_write_direct(cpu, MCS51_SBUF, (uint8_t)i);
    }
    CHECK_EQ(log.count, 4);
    CHECK_EQ(log.when[3] - log.when[0], 176);

    /* SMOD = 1: fosc/32, three frames in 88 cycles */
    cpu = uart_cpu(0x80, true, &log);
    mcs51_write_direct(cpu, MCS51_SBUF, 1);
    for (unsigned i = 2; i <= 5; i++) {
        until_flag(cpu, MCS51_SCON_TI, 2000);
        SFR(cpu, MCS51_SCON) &= (uint8_t)~MCS51_SCON_TI;
        if (i <= 4)
            mcs51_write_direct(cpu, MCS51_SBUF, (uint8_t)i);
    }
    CHECK_EQ(log.when[3] - log.when[0], 88);
}

/* Mode 0: shift register at one bit per machine cycle. */
static void test_mode0_shift_register(void)
{
    static tx_log log;
    mcs51 *cpu = uart_cpu(0x00, false, &log);
    mcs51_write_direct(cpu, MCS51_SBUF, 0x3C);
    CHECK_EQ(until_flag(cpu, MCS51_SCON_TI, 100), 9); /* start in cycle 1, 8 bits */
    CHECK_EQ(log.data[0], 0x3C);

    cpu = uart_cpu(0x10, false, &log); /* REN = 1, RI = 0 starts a reception */
    mcs51_serial_inject(cpu, 0xC3);
    CHECK_EQ(until_flag(cpu, MCS51_SCON_RI, 100), 9);
    CHECK_EQ(mcs51_read_direct(cpu, MCS51_SBUF), 0xC3);
}

/* Timer 1 keeps clocking the serial port while timer 0 is in mode 3 (TF1
 * then belongs to TH0, see test_timers). */
static void test_baud_rate_with_timer0_mode3(void)
{
    static tx_log log;
    mcs51 *cpu = uart_cpu(0x40, false, &log);
    SFR(cpu, MCS51_TMOD) = 0x23;
    mcs51_write_direct(cpu, MCS51_SBUF, 'x');
    CHECK_EQ(until_flag(cpu, MCS51_SCON_TI, 5000), 960);
}

/* A program-driven transmission: SBUF written by MOV, TI polled by JNB. */
static void test_program_polls_ti(void)
{
    static const uint8_t code[] = {
        0x75, 0x99, 0x48, /* MOV SBUF,#'H' */
        0x30, 0x99, 0xFD, /* JNB TI,$ */
        0xC2, 0x99,       /* CLR TI */
        0x80, 0xFE,       /* SJMP $ */
    };
    static tx_log log;
    mcs51 *cpu = uart_cpu(0x40, false, &log);
    mcs51_load_code(cpu, 0, code, sizeof code);
    CHECK_EQ(mcs51_run(cpu, 5000), MCS51_HALT);
    CHECK_EQ(log.count, 1);
    CHECK_EQ(log.data[0], 'H');
    /* HALT waits for the stop bit to finish: it ends 10 bit times after the
     * start at cycle 96 */
    CHECK(cpu->cycles >= 96 + 10 * 96);
    CHECK(cpu->cycles < 96 + 10 * 96 + 4);
}

int main(void)
{
    TK_RUN(test_mode1_transmit_timing);
    TK_RUN(test_mode1_smod_doubles_baud_rate);
    TK_RUN(test_mode1_receive);
    TK_RUN(test_receiver_disabled);
    TK_RUN(test_receive_overrun);
    TK_RUN(test_mode3_sm2_filter);
    TK_RUN(test_mode3_transmit_tb8);
    TK_RUN(test_mode2_fixed_baud_rate);
    TK_RUN(test_mode0_shift_register);
    TK_RUN(test_baud_rate_with_timer0_mode3);
    TK_RUN(test_program_polls_ti);
    return tk_summary();
}
