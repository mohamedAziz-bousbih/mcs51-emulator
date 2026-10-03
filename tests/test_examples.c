/*
 * test_examples.c - end-to-end tests: every program in examples/ is loaded
 * from its committed Intel HEX file and run on the emulated lab board.
 *
 * The expected cycle counts are derived from the programs themselves with
 * the cycle column of the Intel instruction set table (the derivations are
 * repeated next to each check), so a wrong cycle count in the core or a
 * timing change in a peripheral shows up here as well as in the unit tests.
 */
#include <string.h>

#include "testkit.h"

#define SFR(c, a) MCS51_SFR(c, a)

/* Every write to a port latch: port, value and the machine cycles completed
 * before the writing instruction started. */
typedef struct port_log {
    const mcs51 *cpu;
    unsigned count;
    struct { uint8_t port, value; uint64_t when; } w[256];
} port_log;

static void log_port(void *ctx, unsigned port, uint8_t latch)
{
    port_log *log = ctx;
    if (log->count < sizeof log->w / sizeof log->w[0]) {
        log->w[log->count].port = (uint8_t)port;
        log->w[log->count].value = latch;
        log->w[log->count].when = log->cpu->cycles;
    }
    log->count++;
}

static void attach_port_log(mcs51 *cpu, port_log *log)
{
    memset(log, 0, sizeof *log);
    log->cpu = cpu;
    cpu->io.port_out = log_port;
    cpu->io.port_ctx = log;
}

/* Bytes sent by the UART with the cycle count at which TI was set. */
typedef struct uart_log {
    const mcs51 *cpu;
    unsigned count;
    char text[64];
    uint64_t when[64];
} uart_log;

static void log_uart(void *ctx, uint16_t data)
{
    uart_log *log = ctx;
    if (log->count + 1 < sizeof log->text) {
        log->text[log->count] = (char)data;
        log->when[log->count] = log->cpu->cycles;
        log->count++;
    }
}

static void attach_uart_log(mcs51 *cpu, uart_log *log)
{
    memset(log, 0, sizeof *log);
    log->cpu = cpu;
    cpu->io.serial_tx = log_uart;
    cpu->io.serial_ctx = log;
}

/* delay.a51: P1.0 toggles every 1000 cycles, ten times; 10003 cycles. */
static void test_delay_loop(void)
{
    static port_log log;
    mcs51_board *board = tk_board("examples/delay.hex");
    if (!board)
        return;
    mcs51 *cpu = &board->cpu;
    attach_port_log(cpu, &log);
    CHECK_EQ(mcs51_run(cpu, 100000), MCS51_HALT);
    CHECK_EQ(cpu->cycles, 10003);
    CHECK_EQ(log.count, 10);
    for (unsigned i = 0; i < log.count && i < 10; i++) {
        CHECK_EQ(log.w[i].port, 1);
        CHECK_EQ(log.w[i].value, (i % 2) ? 0xFF : 0xFE);
        /* MOV R5,#10 takes cycle 1, so CPL P1.0 starts after 1 + 1000 i */
        CHECK_EQ(log.w[i].when, 1 + 1000 * i);
    }
    CHECK_EQ(SFR(cpu, MCS51_P1), 0xFF);
}

/* subroutine.a51: recursive n! for n = 0..8, stack depth and cycle count. */
static void test_subroutine_factorials(void)
{
    static const unsigned fact[9] = {1, 1, 2, 6, 24, 120, 720, 5040, 40320};
    mcs51_board *board = tk_board("examples/subroutine.hex");
    if (!board)
        return;
    mcs51 *cpu = &board->cpu;
    uint8_t max_sp = 0;
    mcs51_status status;
    while ((status = mcs51_step(cpu)) == MCS51_OK && cpu->cycles < 100000) {
        if (SFR(cpu, MCS51_SP) > max_sp)
            max_sp = SFR(cpu, MCS51_SP);
    }
    CHECK_EQ(status, MCS51_HALT);
    for (unsigned n = 0; n < 9; n++) {
        unsigned value = (unsigned)(cpu->iram[0x30 + 2 * n] << 8 | cpu->iram[0x31 + 2 * n]);
        CHECK_EQ(value, fact[n]);
    }
    /* outer return address (2 bytes) + 8 levels of (saved n + return address) */
    CHECK_EQ(max_sp, 0x5F + 2 + 8 * 3);
    CHECK_EQ(SFR(cpu, MCS51_SP), 0x5F);
    /* setup 4, per argument 12 + FACT(n) = 18 + 28 n, final SJMP 2 */
    uint64_t expected = 4 + 2;
    for (unsigned n = 0; n < 9; n++)
        expected += 18 + 28 * n;
    CHECK_EQ(expected, 1176);
    CHECK_EQ(cpu->cycles, expected);
}

/* bcd_counter.a51: P1 counts 00..99 in packed BCD, P2 shows the units digit
 * as a 7-segment pattern, then 99 + 1 wraps to 00 with a decimal carry. */
static void test_bcd_counter(void)
{
    static const uint8_t segments[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    static port_log log;
    mcs51_board *board = tk_board("examples/bcd_counter.hex");
    if (!board)
        return;
    mcs51 *cpu = &board->cpu;
    attach_port_log(cpu, &log);
    CHECK_EQ(mcs51_run(cpu, 100000), MCS51_HALT);
    CHECK_EQ(log.count, 201); /* 100 x (P1, P2) + the final P1 = 00 */
    for (unsigned n = 0; n < 100 && log.count == 201; n++) {
        CHECK_CASE(log.w[2 * n].port == 1 && log.w[2 * n].value == ((n / 10) << 4 | n % 10),
                   "count %u: P%u = %02X", n, log.w[2 * n].port, log.w[2 * n].value);
        CHECK_CASE(log.w[2 * n + 1].port == 2 && log.w[2 * n + 1].value == segments[n % 10],
                   "count %u: P%u = %02X", n, log.w[2 * n + 1].port, log.w[2 * n + 1].value);
    }
    CHECK_EQ(SFR(cpu, MCS51_P1), 0x00);
    CHECK_EQ(SFR(cpu, MCS51_PSW) & MCS51_PSW_CY, MCS51_PSW_CY);
    /* setup 3, 100 loop iterations of 13 cycles, MOV P1,R2 2, SJMP $ 2 */
    CHECK_EQ(cpu->cycles, 3 + 100 * 13 + 2 + 2);
}

/* sort.a51: the array is sorted, and the cycle count equals a model of the
 * bubble sort with early exit using the per-step costs of the listing. */
static void test_sort(void)
{
    mcs51_board *board = tk_board("examples/sort.hex");
    if (!board)
        return;
    mcs51 *cpu = &board->cpu;
    /* the input table follows the code: find it via the load loop's DPTR */
    CHECK_EQ(mcs51_run(cpu, 100000), MCS51_HALT);
    const uint8_t *input = &cpu->code[(uint16_t)(cpu->code[1] << 8 | cpu->code[2])];

    uint8_t model[16];
    memcpy(model, input, sizeof model);
    uint64_t cycles = 4 + 16 * 9 + 1;
    for (unsigned r6 = 15; r6 > 0; r6--) {
        bool swapped = false;
        cycles += 7;
        for (unsigned i = 0; i < r6; i++) {
            if (model[i] > model[i + 1]) {
                uint8_t t = model[i];
                model[i] = model[i + 1];
                model[i + 1] = t;
                swapped = true;
                cycles += 14;
            } else {
                cycles += 11;
            }
        }
        if (!swapped)
            break;
        cycles += 2; /* DJNZ R6,PASS */
    }
    cycles += 2; /* SJMP $ */

    for (unsigned i = 0; i < 16; i++)
        CHECK_EQ(cpu->iram[0x40 + i], model[i]);
    for (unsigned i = 1; i < 16; i++)
        CHECK(cpu->iram[0x40 + i - 1] <= cpu->iram[0x40 + i]);
    CHECK_EQ(cpu->iram[0x40], 0x00);
    CHECK_EQ(cpu->iram[0x4F], 0xFF);
    CHECK_EQ(cpu->cycles, cycles);
}

/* port_counter.a51 with a simulated push button on P3.2. The port
 * callbacks share one context, so the button also carries the write log. */
typedef struct button {
    port_log log;
    const uint32_t (*low)[2]; /* [from, to) intervals in which the pin is low */
    unsigned count;
} button;

static uint8_t button_pins(void *ctx, unsigned port)
{
    const button *b = ctx;
    if (port != 3)
        return 0xFF;
    for (unsigned i = 0; i < b->count; i++) {
        if (b->log.cpu->cycles >= b->low[i][0] && b->log.cpu->cycles < b->low[i][1])
            return (uint8_t)~MCS51_P3_INT0; /* P3.2 */
    }
    return 0xFF;
}

static void button_leds(void *ctx, unsigned port, uint8_t latch)
{
    log_port(&((button *)ctx)->log, port, latch);
}

static void test_port_counter_debounce(void)
{
    /* press 1 bounces for 600 cycles and its release for 250 cycles; a
     * 1000-cycle glitch is shorter than the 4995-cycle debounce time and must
     * not count; presses 2 and 3 are clean */
    static const uint32_t low[][2] = {
        {10000, 10150}, {10300, 10500}, {10600, 30000}, {30100, 30250},
        {40000, 41000},
        {50000, 70000},
        {80000, 100000},
    };
    static const uint32_t press_start[3] = {10000, 50000, 80000};
    static button b;
    mcs51_board *board = tk_board("examples/port_counter.hex");
    if (!board)
        return;
    mcs51 *cpu = &board->cpu;
    memset(&b, 0, sizeof b);
    b.log.cpu = cpu;
    b.low = low;
    b.count = sizeof low / sizeof low[0];
    cpu->io.port_in = button_pins;
    cpu->io.port_out = button_leds;
    cpu->io.port_ctx = &b;

    CHECK_EQ(mcs51_run(cpu, 120000), MCS51_CYCLE_LIMIT);
    CHECK_EQ(mcs51_reg(cpu, 5), 3);
    CHECK_EQ(b.log.count, 4);
    CHECK_EQ(b.log.w[0].value, 0xFF); /* LEDs off */
    for (unsigned i = 1; i < b.log.count && i < 4; i++) {
        CHECK_EQ(b.log.w[i].port, 1);
        CHECK_EQ(b.log.w[i].value, (uint8_t)~i);
        /* The JB that first sees the low level (2 cycles; it starts 0 or 1
         * cycle after the edge because the wait loop polls every 2 cycles),
         * the debounce call (4995), the second JB (2), INC/MOV/CPL (3), then
         * MOV P1,A writes the LEDs. */
        uint64_t earliest = press_start[i - 1] + 2 + 4995 + 2 + 3;
        CHECK(b.log.w[i].when >= earliest);
        CHECK(b.log.w[i].when <= earliest + 1);
    }
}

/* timer0_blink.a51: the ISR toggles P1.0 every 50000 cycles exactly. */
static void test_timer0_blink(void)
{
    static port_log log;
    mcs51_board *board = tk_board("examples/timer0_blink.hex");
    if (!board)
        return;
    mcs51 *cpu = &board->cpu;
    attach_port_log(cpu, &log);
    CHECK_EQ(mcs51_run(cpu, 1000100), MCS51_CYCLE_LIMIT);
    CHECK_EQ(log.count, 20);
    /* SETB TR0 starts the timer in cycle 13; 50000 counts later TF0 is set
     * (cycle 50012), polled in 50013, LCALL 50014-15, then LJMP, two PUSHes
     * and the 8 one-cycle instructions of the reload: CPL P1.0 starts after
     * 50029 cycles. */
    for (unsigned i = 0; i < log.count && i < 20; i++) {
        CHECK_EQ(log.w[i].port, 1);
        CHECK_EQ(log.w[i].value, (i % 2) ? 0xFF : 0xFE);
        CHECK_EQ(log.w[i].when, 50029 + 50000 * (uint64_t)i);
    }
    CHECK_EQ(SFR(cpu, MCS51_SP), 0x2F); /* every ISR returned */
}

/* uart_hello.a51: the greeting at 9600 baud, frames back to back. */
static void test_uart_hello(void)
{
    static uart_log log;
    mcs51_board *board = tk_board("examples/uart_hello.hex");
    if (!board)
        return;
    mcs51 *cpu = &board->cpu;
    attach_uart_log(cpu, &log);
    CHECK_EQ(mcs51_run(cpu, 100000), MCS51_HALT);
    CHECK(strcmp(log.text, "Hello, 8051!\r\n") == 0);
    /* SETB TR1 executes in cycle 9; TL1 = FDh overflows in cycles 11, 14, ...
     * and every second overflow (SMOD = 0) is a 16x baud tick: 14, 20, ...
     * The 16th tick (cycle 104) is the first divide-by-16 rollover, where
     * the first frame starts; TI rises 9 bit times (9 * 96) later. */
    for (unsigned i = 0; i < log.count; i++)
        CHECK_EQ(log.when[i], 968 + 960 * (uint64_t)i);
    /* the last stop bit ends one bit time after the last TI; the SJMP $
     * loop halts at the first instruction boundary after that */
    CHECK_EQ(cpu->cycles, 968 + 960 * 13 + 96);
}

/* uart_echo.a51: received text is echoed in upper case; CR becomes CR LF. */
static void test_uart_echo(void)
{
    static uart_log log;
    static const char input[] = "hello, World 8051\r";
    mcs51_board *board = tk_board("examples/uart_echo.hex");
    if (!board)
        return;
    mcs51 *cpu = &board->cpu;
    attach_uart_log(cpu, &log);
    for (const char *p = input; *p; p++)
        CHECK(mcs51_serial_inject(cpu, (uint8_t)*p));
    CHECK_EQ(mcs51_run(cpu, 200000), MCS51_HALT);
    CHECK(strcmp(log.text, "HELLO, WORLD 8051\r\n") == 0);
    CHECK_EQ(cpu->serial.overruns, 0);
    CHECK_EQ(cpu->serial.q_count, 0);
    /* an echo cannot leave before its character has been received: 19 frames
     * of 960 cycles at least */
    CHECK(cpu->cycles > 19 * 960);
}

/* xmem_copy.a51: block copies in the SRAM, paged MOVX @R0, and the results
 * reported through the 8255 (reached via a mirror address) and the latch. */
typedef struct bus_log {
    unsigned writes_ppi_mirror;
} bus_log;

static void watch_bus(void *ctx, uint16_t addr, uint8_t value, bool write, const mcs51_xbus_device *dev)
{
    bus_log *log = ctx;
    (void)value;
    if (write && addr == 0xC006 && dev && !strcmp(dev->name, "PPI 8255"))
        log->writes_ppi_mirror++;
}

static void test_xmem_copy(void)
{
    static bus_log log;
    mcs51_board *board = tk_board("examples/xmem_copy.hex");
    if (!board)
        return;
    mcs51 *cpu = &board->cpu;
    memset(&log, 0, sizeof log);
    board->bus.monitor = watch_bus;
    board->bus.monitor_ctx = &log;
    CHECK_EQ(mcs51_run(cpu, 100000), MCS51_HALT);

    const uint8_t *block = &cpu->code[0x0100];
    unsigned sum = 0;
    for (unsigned i = 0; i < 64; i++) {
        CHECK_CASE(board->xram[i] == block[i] && board->xram[0x4000 + i] == block[i],
                   "byte %u: %02X / %02X, expected %02X", i, board->xram[i], board->xram[0x4000 + i],
                   block[i]);
        sum += block[i];
    }
    CHECK_EQ(board->xram[0x0040], 0x00); /* nothing beyond the 64 bytes */
    CHECK_EQ(board->xram[0x4040], 0x00);
    for (unsigned i = 0; i < 256; i++)
        CHECK_CASE(board->xram[0x7F00 + i] == (uint8_t)~i, "7F%02Xh = %02X", i, board->xram[0x7F00 + i]);

    CHECK_EQ(board->ppi.control, 0x80);
    CHECK_EQ(mcs51_ppi_port(&board->ppi, MCS51_PPI_A), sum >> 8);
    CHECK_EQ(mcs51_ppi_port(&board->ppi, MCS51_PPI_B), sum & 0xFF);
    CHECK_EQ(mcs51_ppi_port(&board->ppi, MCS51_PPI_C), 0); /* no mismatches */
    CHECK_EQ(log.writes_ppi_mirror, 1);
    CHECK_EQ(board->latch, 0xA5);
    CHECK_EQ(board->latch_writes, 1);
    CHECK_EQ(SFR(cpu, MCS51_P0), 0xFF); /* address/data bus */
    CHECK_EQ(SFR(cpu, MCS51_P2), 0x7F);

    /* 1. MOV R6 1 + 64 x 14;  2. setup 3 + 64 x 12;  3. setup 4 + 64 x 24;
     * 4. setup 3 + 256 x 7;  5. reporting 31 (including SJMP $) */
    CHECK_EQ(cpu->cycles, (1 + 64 * 14) + (3 + 64 * 12) + (4 + 64 * 24) + (3 + 256 * 7) + 31);
}

/* The committed HEX file of every example loads into the board. */
static void test_all_examples_load(void)
{
    static const char *const files[] = {
        "examples/delay.hex", "examples/subroutine.hex", "examples/port_counter.hex",
        "examples/bcd_counter.hex", "examples/timer0_blink.hex", "examples/uart_hello.hex",
        "examples/uart_echo.hex", "examples/xmem_copy.hex", "examples/sort.hex",
    };
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
        mcs51_board *board = tk_board(files[i]);
        CHECK(board != NULL);
        if (board)
            CHECK(board->cpu.code[0] != 0xFF);
    }
}

int main(void)
{
    TK_RUN(test_delay_loop);
    TK_RUN(test_subroutine_factorials);
    TK_RUN(test_bcd_counter);
    TK_RUN(test_sort);
    TK_RUN(test_port_counter_debounce);
    TK_RUN(test_timer0_blink);
    TK_RUN(test_uart_hello);
    TK_RUN(test_uart_echo);
    TK_RUN(test_xmem_copy);
    TK_RUN(test_all_examples_load);
    return tk_summary();
}
