/*
 * test_interrupts.c - interrupt sampling, latency, priorities and the
 * blocking rules of the 8051 interrupt system.
 *
 * Main programs run from 0030h; every vector holds a short service routine.
 * Requests are raised either by a timer overflow or by setting the flag in
 * TCON/SCON directly between two steps (a software interrupt).
 */
#include <string.h>

#include "testkit.h"

#define SFR(c, a) MCS51_SFR(c, a)

/* NOPs everywhere; RETI at the vectors unless a test installs more code. */
static mcs51 *irq_cpu(void)
{
    mcs51 *cpu = tk_cpu(NULL, 0);
    memset(cpu->code, 0x00, sizeof cpu->code);
    for (unsigned v = 0; v < MCS51_IRQ_COUNT; v++)
        cpu->code[8 * v + 3] = 0x32;
    cpu->pc = 0x0030;
    return cpu;
}

static uint16_t stacked_pc(const mcs51 *cpu)
{
    uint8_t sp = SFR(cpu, MCS51_SP);
    return (uint16_t)(cpu->iram[sp] << 8 | cpu->iram[(uint8_t)(sp - 1)]);
}

/* Overflow in cycle 1; polled in cycle 2; LCALL in cycles 3-4: the first
 * instruction of the service routine starts after 3 complete cycles. */
static void test_minimum_latency(void)
{
    mcs51 *cpu = irq_cpu();
    SFR(cpu, MCS51_TMOD) = 0x01;
    SFR(cpu, MCS51_TH0) = 0xFF;
    SFR(cpu, MCS51_TL0) = 0xFF;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR0;
    SFR(cpu, MCS51_IE) = MCS51_IE_EA | MCS51_IE_ET0;

    mcs51_step(cpu);
    CHECK(SFR(cpu, MCS51_TCON) & MCS51_TCON_TF0);
    CHECK_EQ(cpu->irq.pending, -1);
    mcs51_step(cpu);
    CHECK_EQ(cpu->irq.pending, MCS51_IRQ_T0);
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x000B);
    CHECK_EQ(cpu->cycles, 4);
    CHECK_EQ(stacked_pc(cpu), 0x0032);
    CHECK_EQ(SFR(cpu, MCS51_TCON) & MCS51_TCON_TF0, 0); /* cleared by the vector */
    CHECK_EQ(cpu->irq.active, 1);
}

/* A request seen by the poll in the last cycle of a long instruction is
 * served right after it; a request that appears in that last cycle is
 * only seen one instruction later. */
static void test_request_during_multicycle_instruction(void)
{
    static const uint8_t mul[] = {0xA4};
    for (int late = 0; late < 2; late++) {
        mcs51 *cpu = irq_cpu();
        mcs51_load_code(cpu, 0x0030, mul, 1);
        SFR(cpu, MCS51_TMOD) = 0x01;
        SFR(cpu, MCS51_TH0) = 0xFF;
        SFR(cpu, MCS51_TL0) = late ? 0xFC : 0xFD; /* overflow in cycle 4 or 3 */
        SFR(cpu, MCS51_TCON) = MCS51_TCON_TR0;
        SFR(cpu, MCS51_IE) = MCS51_IE_EA | MCS51_IE_ET0;
        mcs51_step(cpu); /* MUL AB, cycles 1-4 */
        CHECK_EQ(cpu->cycles, 4);
        CHECK_EQ(cpu->irq.pending, late ? -1 : MCS51_IRQ_T0);
        if (late) {
            mcs51_step(cpu); /* NOP at 0031h polls the sample of cycle 4 */
            CHECK_EQ(cpu->irq.pending, MCS51_IRQ_T0);
        }
        mcs51_step(cpu);
        CHECK_EQ(cpu->pc, 0x000B);
        CHECK_EQ(stacked_pc(cpu), late ? 0x0032 : 0x0031);
    }
}

static void set_request(mcs51 *cpu, uint8_t tcon_bits)
{
    SFR(cpu, MCS51_TCON) |= tcon_bits;
}

/* Low priority T0 is interrupted by high priority EX0; low priority T1 must
 * wait until T0's RETI, and then one main-program instruction executes
 * before T1 is vectored. */
static void test_priorities_and_nesting(void)
{
    static const uint8_t t0_isr[] = {0x00, 0x00, 0x00, 0x00, 0x32};
    static const uint8_t ex0_isr[] = {0x00, 0x32};
    mcs51 *cpu = irq_cpu();
    mcs51_load_code(cpu, 0x000B, t0_isr, sizeof t0_isr);
    mcs51_load_code(cpu, 0x0003, ex0_isr, sizeof ex0_isr);
    SFR(cpu, MCS51_IE) = MCS51_IE_EA | MCS51_IE_EX0 | MCS51_IE_ET0 | MCS51_IE_ET1;
    SFR(cpu, MCS51_IP) = MCS51_IE_EX0; /* PX0: external 0 high priority */
    SFR(cpu, MCS51_TCON) = MCS51_TCON_IT0;

    set_request(cpu, MCS51_TCON_TF0);
    mcs51_step(cpu);
    mcs51_step(cpu);
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x000B);
    CHECK_EQ(cpu->irq.active, 1);

    set_request(cpu, MCS51_TCON_TF1); /* same level: must wait */
    mcs51_step(cpu);
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x000D);
    CHECK_EQ(cpu->irq.pending, -1);

    set_request(cpu, MCS51_TCON_IE0); /* higher level: nests */
    mcs51_step(cpu);
    mcs51_step(cpu);
    CHECK_EQ(cpu->irq.pending, MCS51_IRQ_EX0);
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x0003);
    CHECK_EQ(cpu->irq.active, 3);
    CHECK_EQ(stacked_pc(cpu), 0x000F);

    mcs51_step(cpu);                 /* NOP */
    mcs51_step(cpu);                 /* RETI: back into the T0 routine */
    CHECK_EQ(cpu->pc, 0x000F);
    CHECK_EQ(cpu->irq.active, 1);
    CHECK_EQ(cpu->irq.pending, -1);  /* TF1 still blocked by the low level */
    mcs51_step(cpu);                 /* RETI of T0 */
    CHECK_EQ(cpu->pc, 0x0032);
    CHECK_EQ(cpu->irq.active, 0);
    CHECK_EQ(cpu->irq.pending, -1);  /* RETI blocks the poll of its own cycle */
    mcs51_step(cpu);                 /* one main instruction */
    CHECK_EQ(cpu->pc, 0x0033);
    CHECK_EQ(cpu->irq.pending, MCS51_IRQ_T1);
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x001B);
}

/* Records the vector address of every interrupt entry. */
static unsigned vectors_taken(mcs51 *cpu, unsigned steps, uint16_t *out, unsigned max)
{
    unsigned n = 0;
    for (unsigned i = 0; i < steps; i++) {
        bool entering = cpu->irq.pending >= 0;
        mcs51_step(cpu);
        if (entering && n < max)
            out[n++] = cpu->pc;
    }
    return n;
}

/* Simultaneous requests of one level are served in the fixed polling order
 * IE0, TF0, IE1, TF1, serial; IP lifts a source above all low ones. */
static void test_polling_order(void)
{
    static const uint8_t serial_isr[] = {0xC2, 0x98, 0x32}; /* CLR RI ; RETI */
    uint16_t seen[8];
    mcs51 *cpu = irq_cpu();
    mcs51_load_code(cpu, 0x0023, serial_isr, sizeof serial_isr);
    SFR(cpu, MCS51_IE) = 0x9F;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_IT0 | MCS51_TCON_IT1;
    set_request(cpu, MCS51_TCON_TF1 | MCS51_TCON_IE1 | MCS51_TCON_TF0 | MCS51_TCON_IE0);
    SFR(cpu, MCS51_SCON) = MCS51_SCON_RI;
    unsigned n = vectors_taken(cpu, 40, seen, 8);
    CHECK_EQ(n, 5);
    CHECK_EQ(seen[0], 0x0003);
    CHECK_EQ(seen[1], 0x000B);
    CHECK_EQ(seen[2], 0x0013);
    CHECK_EQ(seen[3], 0x001B);
    CHECK_EQ(seen[4], 0x0023);

    cpu = irq_cpu();
    SFR(cpu, MCS51_IE) = MCS51_IE_EA | MCS51_IE_EX0 | MCS51_IE_ET1;
    SFR(cpu, MCS51_IP) = MCS51_IE_ET1; /* PT1 */
    SFR(cpu, MCS51_TCON) = MCS51_TCON_IT0;
    set_request(cpu, MCS51_TCON_TF1 | MCS51_TCON_IE0);
    n = vectors_taken(cpu, 20, seen, 8);
    CHECK_EQ(n, 2);
    CHECK_EQ(seen[0], 0x001B);
    CHECK_EQ(seen[1], 0x0003);
}

/* Without EA, or with the source disabled, nothing is vectored. */
static void test_enable_bits(void)
{
    uint16_t seen[2];
    mcs51 *cpu = irq_cpu();
    SFR(cpu, MCS51_IE) = MCS51_IE_ET0; /* EA = 0 */
    set_request(cpu, MCS51_TCON_TF0);
    CHECK_EQ(vectors_taken(cpu, 10, seen, 2), 0);
    SFR(cpu, MCS51_IE) = MCS51_IE_EA | MCS51_IE_ET1; /* ET0 = 0 */
    CHECK_EQ(vectors_taken(cpu, 10, seen, 2), 0);
    CHECK(SFR(cpu, MCS51_TCON) & MCS51_TCON_TF0);
}

/* An instruction that writes IE (or IP) blocks the poll in its last cycle,
 * so one more instruction completes before the vector. */
static void test_ie_write_blocks_one_instruction(void)
{
    static const uint8_t code[] = {0x75, 0xA8, 0x82, 0x00, 0x00}; /* MOV IE,#82h ; NOP ; NOP */
    mcs51 *cpu = irq_cpu();
    mcs51_load_code(cpu, 0x0030, code, sizeof code);
    set_request(cpu, MCS51_TCON_TF0);
    mcs51_step(cpu);
    CHECK_EQ(cpu->irq.pending, -1);
    mcs51_step(cpu);
    CHECK_EQ(cpu->irq.pending, MCS51_IRQ_T0);
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x000B);
    CHECK_EQ(stacked_pc(cpu), 0x0034);
}

/* Ending a service routine with RET leaves its priority level marked as in
 * progress: later requests of that level are never served. */
static void test_ret_instead_of_reti(void)
{
    uint16_t seen[4];
    mcs51 *cpu = irq_cpu();
    cpu->code[0x000B] = 0x22; /* RET */
    SFR(cpu, MCS51_IE) = MCS51_IE_EA | MCS51_IE_ET0 | MCS51_IE_EX0;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_IT0;
    set_request(cpu, MCS51_TCON_TF0);
    CHECK_EQ(vectors_taken(cpu, 10, seen, 4), 1);
    CHECK_EQ(cpu->irq.active, 1);
    set_request(cpu, MCS51_TCON_TF0 | MCS51_TCON_IE0);
    CHECK_EQ(vectors_taken(cpu, 20, seen, 4), 0);

    SFR(cpu, MCS51_IP) = MCS51_IE_EX0; /* a high priority source still gets through */
    CHECK_EQ(vectors_taken(cpu, 10, seen, 4), 1);
    CHECK_EQ(seen[0], 0x0003);
}

/* Edge-triggered INT0 sets IE0 once per falling edge and the vector clears
 * it; level-triggered IE0 follows the pin and the routine is re-entered as
 * long as the pin stays low. */
static void test_external_edge_and_level(void)
{
    uint16_t seen[16];
    mcs51 *cpu = irq_cpu();
    SFR(cpu, MCS51_IE) = MCS51_IE_EA | MCS51_IE_EX0;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_IT0;
    cpu->pins_in[3] = (uint8_t)~MCS51_P3_INT0;
    mcs51_step(cpu);
    CHECK(SFR(cpu, MCS51_TCON) & MCS51_TCON_IE0);
    CHECK_EQ(vectors_taken(cpu, 30, seen, 16), 1);
    CHECK_EQ(SFR(cpu, MCS51_TCON) & MCS51_TCON_IE0, 0);

    cpu = irq_cpu();
    SFR(cpu, MCS51_IE) = MCS51_IE_EA | MCS51_IE_EX0;
    cpu->pins_in[3] = (uint8_t)~MCS51_P3_INT0; /* IT0 = 0: level */
    /* NOP, NOP (poll), then entry, RETI, one NOP (poll) per round: the
     * flag is still set after RETI, so there is an entry every 3 steps */
    unsigned n = vectors_taken(cpu, 30, seen, 16);
    CHECK_EQ(n, 10);
    CHECK(SFR(cpu, MCS51_TCON) & MCS51_TCON_IE0);
    cpu->pins_in[3] = 0xFF;
    mcs51_step(cpu);
    mcs51_step(cpu);
    mcs51_step(cpu);
    CHECK_EQ(SFR(cpu, MCS51_TCON) & MCS51_TCON_IE0, 0);
    CHECK_EQ(vectors_taken(cpu, 20, seen, 16), 0);

    /* Writing 0 to the P3.2 latch also pulls INT0 low. */
    static const uint8_t clr_int0[] = {0xC2, 0xB2}; /* CLR P3.2 */
    cpu = irq_cpu();
    mcs51_load_code(cpu, 0x0030, clr_int0, sizeof clr_int0);
    SFR(cpu, MCS51_IE) = MCS51_IE_EA | MCS51_IE_EX0;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_IT0;
    CHECK_EQ(vectors_taken(cpu, 10, seen, 16), 1);
}

/* The serial interrupt is requested by RI or TI and does not clear them. */
static void test_serial_flags_not_cleared(void)
{
    uint16_t seen[2];
    mcs51 *cpu = irq_cpu();
    cpu->code[0x0023] = 0x00; /* NOP ; RETI */
    cpu->code[0x0024] = 0x32;
    SFR(cpu, MCS51_IE) = MCS51_IE_EA | MCS51_IE_ES;
    SFR(cpu, MCS51_SCON) = MCS51_SCON_TI;
    CHECK_EQ(vectors_taken(cpu, 4, seen, 2), 1);
    CHECK_EQ(seen[0], 0x0023);
    CHECK(SFR(cpu, MCS51_SCON) & MCS51_SCON_TI);
}

/* Idle mode: the CPU stops fetching, the timer keeps running and its
 * interrupt ends idle; RETI returns behind the instruction that set IDL. */
static void test_idle_wakeup(void)
{
    static const uint8_t code[] = {0x43, 0x87, 0x01, 0x00}; /* ORL PCON,#1 ; NOP */
    mcs51 *cpu = irq_cpu();
    mcs51_load_code(cpu, 0x0030, code, sizeof code);
    SFR(cpu, MCS51_TMOD) = 0x01;
    SFR(cpu, MCS51_TH0) = 0xFF;
    SFR(cpu, MCS51_TL0) = 0xF0;
    SFR(cpu, MCS51_TCON) = MCS51_TCON_TR0;
    SFR(cpu, MCS51_IE) = MCS51_IE_EA | MCS51_IE_ET0;
    CHECK_EQ(mcs51_step(cpu), MCS51_OK);
    CHECK(cpu->idle);
    CHECK_EQ(cpu->pc, 0x0033);
    unsigned idle_steps = 0;
    while (cpu->idle && idle_steps < 100) {
        CHECK_EQ(tk_step_cycles(cpu), 1);
        idle_steps++;
    }
    CHECK_EQ(cpu->pc, 0x0033);
    CHECK_EQ(SFR(cpu, MCS51_PCON) & MCS51_PCON_IDL, 0);
    CHECK_EQ(cpu->irq.pending, MCS51_IRQ_T0);
    /* overflow after 16 timer counts (cycles 1-16), polled in cycle 17 */
    CHECK_EQ(cpu->cycles, 17);
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x000B);
    mcs51_step(cpu); /* RETI */
    CHECK_EQ(cpu->pc, 0x0033);
    CHECK(!cpu->idle);
}

/* Nothing interrupts a high-priority service routine: neither a second
 * high-priority request nor a low-priority one is vectored before its RETI. */
static void test_high_priority_not_interrupted(void)
{
    static const uint8_t ex0_isr[] = {0x00, 0x00, 0x00, 0x32}; /* NOP x3 ; RETI */
    uint16_t seen[4];
    mcs51 *cpu = irq_cpu();
    mcs51_load_code(cpu, 0x0003, ex0_isr, sizeof ex0_isr);
    SFR(cpu, MCS51_IE) = MCS51_IE_EA | MCS51_IE_EX0 | MCS51_IE_ET0 | MCS51_IE_ET1;
    SFR(cpu, MCS51_IP) = MCS51_IE_EX0 | MCS51_IE_ET0; /* PX0, PT0 high; T1 low */
    SFR(cpu, MCS51_TCON) = MCS51_TCON_IT0;

    set_request(cpu, MCS51_TCON_IE0);
    CHECK_EQ(vectors_taken(cpu, 3, seen, 4), 1);
    CHECK_EQ(cpu->pc, 0x0003);
    CHECK_EQ(cpu->irq.active, 2);

    set_request(cpu, MCS51_TCON_TF0 | MCS51_TCON_TF1);
    mcs51_step(cpu);
    mcs51_step(cpu);
    CHECK_EQ(cpu->irq.pending, -1);
    CHECK_EQ(cpu->pc, 0x0005);
    mcs51_step(cpu); /* NOP */
    mcs51_step(cpu); /* RETI */
    CHECK_EQ(cpu->irq.active, 0);
    CHECK_EQ(cpu->irq.pending, -1); /* RETI blocks its own poll */
    /* after one main instruction the high-priority T0 goes first, then T1 */
    CHECK_EQ(vectors_taken(cpu, 12, seen, 4), 2);
    CHECK_EQ(seen[0], 0x000B);
    CHECK_EQ(seen[1], 0x001B);
}

int main(void)
{
    TK_RUN(test_minimum_latency);
    TK_RUN(test_request_during_multicycle_instruction);
    TK_RUN(test_priorities_and_nesting);
    TK_RUN(test_high_priority_not_interrupted);
    TK_RUN(test_polling_order);
    TK_RUN(test_enable_bits);
    TK_RUN(test_ie_write_blocks_one_instruction);
    TK_RUN(test_ret_instead_of_reti);
    TK_RUN(test_external_edge_and_level);
    TK_RUN(test_serial_flags_not_cleared);
    TK_RUN(test_idle_wakeup);
    return tk_summary();
}
