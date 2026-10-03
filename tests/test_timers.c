/*
 * test_timers.c - Timer/Counter 0 and 1 in modes 0-3, gate and counter input.
 *
 * The CPU executes NOPs (one machine cycle each), so after n steps exactly n
 * timer clocks have happened.
 */
#include <string.h>

#include "testkit.h"

#define SFR(c, a) MCS51_SFR(c, a)

static mcs51 *nop_cpu(void)
{
    mcs51 *cpu = tk_cpu(NULL, 0);
    memset(cpu->code, 0x00, sizeof cpu->code);
    return cpu;
}

static void cycles(mcs51 *cpu, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        mcs51_step(cpu);
}

static bool tf0(const mcs51 *cpu)
{
    return (SFR(cpu, MCS51_TCON) & MCS51_TCON_TF0) != 0;
}

static bool tf1(const mcs51 *cpu)
{
    return (SFR(cpu, MCS51_TCON) & MCS51_TCON_TF1) != 0;
}

static void test_mode1_overflow(void)
{
    mcs51 *cpu = nop_cpu();
    SFR(cpu, MCS51_TMOD) = 0x01;
    SFR(cpu, MCS51_TH0) = 0xFF;
    SFR(cpu, MCS51_TL0) = 0xFD;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR0;
    cycles(cpu, 1);
    CHECK_EQ(SFR(cpu, MCS51_TL0), 0xFE);
    cycles(cpu, 1);
    CHECK_EQ(SFR(cpu, MCS51_TL0), 0xFF);
    CHECK(!tf0(cpu));
    cycles(cpu, 1);
    CHECK_EQ(SFR(cpu, MCS51_TL0), 0x00);
    CHECK_EQ(SFR(cpu, MCS51_TH0), 0x00);
    CHECK(tf0(cpu));
}

/* From 0000h, mode 1 overflows after exactly 65536 machine cycles. */
static void test_mode1_full_period(void)
{
    mcs51 *cpu = nop_cpu();
    SFR(cpu, MCS51_TMOD) = 0x01;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR0;
    unsigned first = 0;
    for (unsigned i = 1; i <= 70000 && !first; i++) {
        mcs51_step(cpu);
        if (tf0(cpu))
            first = i;
    }
    CHECK_EQ(first, 65536);
}

/* Mode 0: 13-bit counter TH0 : TL0[4:0]; TL0[7:5] are left alone. */
static void test_mode0_13bit(void)
{
    mcs51 *cpu = nop_cpu();
    SFR(cpu, MCS51_TMOD) = 0x00;
    SFR(cpu, MCS51_TH0) = 0xFF;
    SFR(cpu, MCS51_TL0) = 0xFE; /* low five bits 1Eh, upper bits 111 */
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR0;
    cycles(cpu, 1);
    CHECK_EQ(SFR(cpu, MCS51_TL0), 0xFF);
    CHECK(!tf0(cpu));
    cycles(cpu, 1);
    CHECK_EQ(SFR(cpu, MCS51_TL0), 0xE0);
    CHECK_EQ(SFR(cpu, MCS51_TH0), 0x00);
    CHECK(tf0(cpu));

    SFR(cpu, MCS51_TL0) = 0;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR0;
    cycles(cpu, 8191);
    CHECK(!tf0(cpu));
    cycles(cpu, 1);
    CHECK(tf0(cpu));
}

/* Mode 2: TL0 reloads from TH0 on every overflow. */
static void test_mode2_auto_reload(void)
{
    mcs51 *cpu = nop_cpu();
    SFR(cpu, MCS51_TMOD) = 0x02;
    SFR(cpu, MCS51_TH0) = 0xF0;
    SFR(cpu, MCS51_TL0) = 0xFE;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR0;
    cycles(cpu, 2);
    CHECK(tf0(cpu));
    CHECK_EQ(SFR(cpu, MCS51_TL0), 0xF0);
    SFR(cpu, MCS51_TCON) &= (uint8_t)~MCS51_TCON_TF0;
    cycles(cpu, 15);
    CHECK(!tf0(cpu));
    CHECK_EQ(SFR(cpu, MCS51_TL0), 0xFF);
    cycles(cpu, 1);
    CHECK(tf0(cpu));
    CHECK_EQ(SFR(cpu, MCS51_TL0), 0xF0);
    CHECK_EQ(SFR(cpu, MCS51_TH0), 0xF0);
}

/* Mode 3: TL0 is an 8-bit timer under TR0/TF0, TH0 an 8-bit timer that uses
 * TR1/TF1. Timer 1 in mode 3 holds its count. */
static void test_mode3(void)
{
    mcs51 *cpu = nop_cpu();
    SFR(cpu, MCS51_TMOD) = 0x33; /* timer 1 mode 3 (stopped), timer 0 mode 3 */
    SFR(cpu, MCS51_TL0) = 0xFF;
    SFR(cpu, MCS51_TH0) = 0xFE;
    SFR(cpu, MCS51_TL1) = 0x12;
    SFR(cpu, MCS51_TH1) = 0x34;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR0;
    cycles(cpu, 1);
    CHECK(tf0(cpu));
    CHECK_EQ(SFR(cpu, MCS51_TL0), 0x00);
    CHECK_EQ(SFR(cpu, MCS51_TH0), 0xFE); /* TR1 = 0: TH0 stopped */

    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR1; /* TH0 runs, TL0 stops */
    cycles(cpu, 1);
    CHECK_EQ(SFR(cpu, MCS51_TH0), 0xFF);
    CHECK(!tf1(cpu));
    cycles(cpu, 1);
    CHECK_EQ(SFR(cpu, MCS51_TH0), 0x00);
    CHECK(tf1(cpu));
    CHECK_EQ(SFR(cpu, MCS51_TL0), 0x00);
    CHECK_EQ(SFR(cpu, MCS51_TL1), 0x12); /* timer 1 in mode 3 holds */
    CHECK_EQ(SFR(cpu, MCS51_TH1), 0x34);
}

/* With timer 0 in mode 3, timer 1 still counts (e.g. as baud rate
 * generator) but its overflow no longer sets TF1. */
static void test_timer1_while_timer0_in_mode3(void)
{
    mcs51 *cpu = nop_cpu();
    SFR(cpu, MCS51_TMOD) = 0x23; /* timer 1 mode 2, timer 0 mode 3 */
    SFR(cpu, MCS51_TH1) = 0xFF;
    SFR(cpu, MCS51_TL1) = 0xFF;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR1;
    cycles(cpu, 10);
    CHECK_EQ(SFR(cpu, MCS51_TL1), 0xFF);   /* overflowed and reloaded each cycle */
    CHECK_EQ(SFR(cpu, MCS51_TH0), 0x0A);   /* TH0 counted under TR1 */
    CHECK(!tf1(cpu));
}

/* GATE = 1: the timer counts only while INT0 is high. The pin is sampled at
 * the end of each cycle and the sample controls the next cycle. */
static void test_gate(void)
{
    mcs51 *cpu = nop_cpu();
    SFR(cpu, MCS51_TMOD) = 0x09; /* gate, mode 1 */
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR0;
    cpu->pins_in[3] = (uint8_t)~MCS51_P3_INT0;
    cycles(cpu, 5);
    CHECK_EQ(SFR(cpu, MCS51_TL0), 1); /* only the cycle that used the reset sample */
    cpu->pins_in[3] = 0xFF;
    cycles(cpu, 5);
    CHECK_EQ(SFR(cpu, MCS51_TL0), 5);
}

/* T0 input: low while bit `shift` of the cycle counter is set. */
typedef struct wave {
    const mcs51 *cpu;
    unsigned shift;
} wave;

static uint8_t square_wave_t0(void *ctx, unsigned port)
{
    const wave *w = ctx;
    if (port != 3)
        return 0xFF;
    return ((w->cpu->cycles >> w->shift) & 1u) ? (uint8_t)~MCS51_P3_T0 : 0xFF;
}

/* Counter mode: falling edges on T0 between two samples are counted one
 * cycle later. */
static void test_counter_mode(void)
{
    static wave w;
    mcs51 *cpu = nop_cpu();
    w.cpu = cpu;
    w.shift = 1; /* period of 4 machine cycles */
    cpu->io.port_in = square_wave_t0;
    cpu->io.port_ctx = &w;
    SFR(cpu, MCS51_TMOD) = 0x05; /* counter, mode 1 */
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR0;
    cycles(cpu, 2);
    CHECK_EQ(SFR(cpu, MCS51_TL0), 0); /* edge sampled at the end of cycle 2 ... */
    cycles(cpu, 1);
    CHECK_EQ(SFR(cpu, MCS51_TL0), 1); /* ... counted in cycle 3 */
    cycles(cpu, 397);
    CHECK_EQ(SFR(cpu, MCS51_TL0), 100);

    /* Fastest input: a level change every cycle gives one count per two
     * machine cycles (fosc / 24). The pin falls in cycles 1, 3, ..., 99. */
    cpu = nop_cpu();
    w.shift = 0;
    cpu->io.port_in = square_wave_t0;
    cpu->io.port_ctx = &w;
    SFR(cpu, MCS51_TMOD) = 0x05;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR0;
    cycles(cpu, 100);
    CHECK_EQ(SFR(cpu, MCS51_TL0), 50);
}

static void test_timer1_mode1(void)
{
    mcs51 *cpu = nop_cpu();
    SFR(cpu, MCS51_TMOD) = 0x10;
    SFR(cpu, MCS51_TH1) = 0xFF;
    SFR(cpu, MCS51_TL1) = 0xF0;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR1;
    cycles(cpu, 15);
    CHECK(!tf1(cpu));
    cycles(cpu, 1);
    CHECK(tf1(cpu));
    CHECK(!tf0(cpu));
}

/* A program reads the running count with MOV A,TL0. */
static void test_read_running_timer(void)
{
    static const uint8_t code[] = {0x00, 0x00, 0x00, 0xE5, 0x8A}; /* NOP x3 ; MOV A,TL0 */
    mcs51 *cpu = tk_cpu(code, sizeof code);
    SFR(cpu, MCS51_TMOD) = 0x01;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR0;
    cycles(cpu, 4);
    CHECK_EQ(SFR(cpu, MCS51_ACC), 3);
}

int main(void)
{
    TK_RUN(test_mode1_overflow);
    TK_RUN(test_mode1_full_period);
    TK_RUN(test_mode0_13bit);
    TK_RUN(test_mode2_auto_reload);
    TK_RUN(test_mode3);
    TK_RUN(test_timer1_while_timer0_in_mode3);
    TK_RUN(test_gate);
    TK_RUN(test_counter_mode);
    TK_RUN(test_timer1_mode1);
    TK_RUN(test_read_running_timer);
    return tk_summary();
}
