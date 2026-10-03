/*
 * test_instructions.c - semantics of the data transfer, logic, bit and
 * branch instructions and of the addressing modes.
 */
#include <string.h>

#include "testkit.h"

#define SFR(c, a) MCS51_SFR(c, a)
#define ACC(c) MCS51_SFR(c, MCS51_ACC)
#define PSW(c) MCS51_SFR(c, MCS51_PSW)

static void run(mcs51 *cpu, unsigned instructions)
{
    for (unsigned i = 0; i < instructions; i++)
        mcs51_step(cpu);
}

static void test_mov_addressing_modes(void)
{
    static const uint8_t code[] = {
        0x74, 0x5A,       /* MOV A,#5Ah        immediate */
        0xF5, 0x30,       /* MOV 30h,A         direct */
        0x78, 0x31,       /* MOV R0,#31h       register */
        0xF6,             /* MOV @R0,A         register indirect */
        0x85, 0x30, 0x32, /* MOV 32h,30h       direct to direct: source first */
        0x79, 0x90,       /* MOV R1,#90h */
        0xA7, 0x32,       /* MOV @R1,32h       upper RAM through @R1 */
        0xE5, 0x32,       /* MOV A,32h */
        0x75, 0x33, 0xA5, /* MOV 33h,#0A5h */
        0xAA, 0x33,       /* MOV R2,33h */
        0x8A, 0x34,       /* MOV 34h,R2 */
        0x86, 0x35,       /* MOV 35h,@R0 */
        0x77, 0x66,       /* MOV @R1,#66h */
        0xE7,             /* MOV A,@R1 */
    };
    mcs51 *cpu = tk_cpu(code, sizeof code);
    run(cpu, 14);
    CHECK_EQ(cpu->iram[0x30], 0x5A);
    CHECK_EQ(cpu->iram[0x31], 0x5A);
    CHECK_EQ(cpu->iram[0x32], 0x5A);
    CHECK_EQ(cpu->iram[0x90], 0x66);        /* indirect 90h is RAM ... */
    CHECK_EQ(SFR(cpu, MCS51_P1), 0xFF);     /* ... not the P1 SFR */
    CHECK_EQ(cpu->iram[0x33], 0xA5);
    CHECK_EQ(cpu->iram[0x34], 0xA5);
    CHECK_EQ(cpu->iram[0x35], 0x5A);
    CHECK_EQ(ACC(cpu), 0x66);
    CHECK_EQ(cpu->pc, sizeof code);
    CHECK_EQ(cpu->cycles, 1 + 1 + 1 + 1 + 2 + 1 + 2 + 1 + 2 + 2 + 2 + 2 + 1 + 1);
}

static void test_register_banks(void)
{
    static const uint8_t code[] = {
        0x7B, 0x11,       /* MOV R3,#11h   bank 0 -> 03h */
        0x75, 0xD0, 0x10, /* MOV PSW,#10h  bank 2 */
        0x7B, 0x22,       /* MOV R3,#22h   -> 13h */
        0xD2, 0xD3,       /* SETB RS0      bank 3 */
        0x7B, 0x33,       /* MOV R3,#33h   -> 1Bh */
        0x78, 0x1B,       /* MOV R0,#1Bh   -> 18h */
        0xE6,             /* MOV A,@R0     reads 1Bh */
    };
    mcs51 *cpu = tk_cpu(code, sizeof code);
    run(cpu, 7);
    CHECK_EQ(cpu->iram[0x03], 0x11);
    CHECK_EQ(cpu->iram[0x13], 0x22);
    CHECK_EQ(cpu->iram[0x1B], 0x33);
    CHECK_EQ(cpu->iram[0x18], 0x1B);
    CHECK_EQ(mcs51_reg(cpu, 3), 0x33);
    CHECK_EQ(ACC(cpu), 0x33);
}

static void test_bit_addressing(void)
{
    static const uint8_t code[] = {
        0xD2, 0x00,       /* SETB 20h.0 */
        0xD2, 0x7F,       /* SETB 2Fh.7 */
        0xD2, 0xE3,       /* SETB ACC.3 */
        0xB2, 0x01,       /* CPL 20h.1 */
        0xC2, 0x00,       /* CLR 20h.0 */
        0xD3,             /* SETB C */
        0x92, 0x0A,       /* MOV 21h.2,C */
        0xC3,             /* CLR C */
        0xA2, 0x7F,       /* MOV C,2Fh.7 */
        0x82, 0x0A,       /* ANL C,21h.2      1 AND 1 = 1 */
        0xB0, 0x0A,       /* ANL C,/21h.2     1 AND 0 = 0 */
        0x72, 0x01,       /* ORL C,20h.1      0 OR 1 = 1 */
        0xB3,             /* CPL C            0 */
        0xA0, 0x00,       /* ORL C,/20h.0     0 OR NOT 0 = 1 */
    };
    mcs51 *cpu = tk_cpu(code, sizeof code);
    ACC(cpu) = 0;
    run(cpu, 7);
    CHECK_EQ(cpu->iram[0x20], 0x02);
    CHECK_EQ(cpu->iram[0x2F], 0x80);
    CHECK_EQ(cpu->iram[0x21], 0x04);
    CHECK_EQ(ACC(cpu), 0x08);
    mcs51_step(cpu); /* CLR C */
    mcs51_step(cpu); /* MOV C,2Fh.7 */
    CHECK_EQ(PSW(cpu) & MCS51_PSW_CY, MCS51_PSW_CY);
    mcs51_step(cpu); /* ANL C,21h.2 */
    CHECK_EQ(PSW(cpu) & MCS51_PSW_CY, MCS51_PSW_CY);
    mcs51_step(cpu); /* ANL C,/21h.2 */
    CHECK_EQ(PSW(cpu) & MCS51_PSW_CY, 0);
    mcs51_step(cpu); /* ORL C,20h.1 */
    CHECK_EQ(PSW(cpu) & MCS51_PSW_CY, MCS51_PSW_CY);
    mcs51_step(cpu); /* CPL C */
    CHECK_EQ(PSW(cpu) & MCS51_PSW_CY, 0);
    mcs51_step(cpu); /* ORL C,/20h.0 */
    CHECK_EQ(PSW(cpu) & MCS51_PSW_CY, MCS51_PSW_CY);
}

static void test_logic_on_direct(void)
{
    static const uint8_t code[] = {
        0x74, 0x0F,       /* MOV A,#0Fh */
        0x75, 0x40, 0x3C, /* MOV 40h,#3Ch */
        0x42, 0x40,       /* ORL 40h,A    3F */
        0x53, 0x40, 0xF5, /* ANL 40h,#0F5h  35 */
        0x62, 0x40,       /* XRL 40h,A    3A */
        0x63, 0x40, 0xFF, /* XRL 40h,#0FFh  C5 */
        0x43, 0x40, 0x02, /* ORL 40h,#02h   C7 */
        0x52, 0x40,       /* ANL 40h,A    07 */
        0xC5, 0x40,       /* XCH A,40h    A = 07, 40h = 0F */
        0xE4,             /* CLR A */
        0xF4,             /* CPL A */
    };
    mcs51 *cpu = tk_cpu(code, sizeof code);
    run(cpu, 3);
    CHECK_EQ(cpu->iram[0x40], 0x3F);
    run(cpu, 1);
    CHECK_EQ(cpu->iram[0x40], 0x35);
    run(cpu, 1);
    CHECK_EQ(cpu->iram[0x40], 0x3A);
    run(cpu, 1);
    CHECK_EQ(cpu->iram[0x40], 0xC5);
    run(cpu, 1);
    CHECK_EQ(cpu->iram[0x40], 0xC7);
    run(cpu, 1);
    CHECK_EQ(cpu->iram[0x40], 0x07);
    run(cpu, 1);
    CHECK_EQ(ACC(cpu), 0x07);
    CHECK_EQ(cpu->iram[0x40], 0x0F);
    run(cpu, 2);
    CHECK_EQ(ACC(cpu), 0xFF);
}

static void test_exchange_instructions(void)
{
    static const uint8_t code[] = {
        0x78, 0x50,       /* MOV R0,#50h */
        0x74, 0x12,       /* MOV A,#12h */
        0xC6,             /* XCH A,@R0   A = AB, 50h = 12 */
        0xD6,             /* XCHD A,@R0  A = A2, 50h = 1B */
        0xCD,             /* XCH A,R5    A = 77, R5 = A2 */
    };
    mcs51 *cpu = tk_cpu(code, sizeof code);
    cpu->iram[0x50] = 0xAB;
    cpu->iram[0x05] = 0x77;
    run(cpu, 3);
    CHECK_EQ(ACC(cpu), 0xAB);
    CHECK_EQ(cpu->iram[0x50], 0x12);
    run(cpu, 1);
    CHECK_EQ(ACC(cpu), 0xA2);
    CHECK_EQ(cpu->iram[0x50], 0x1B);
    run(cpu, 1);
    CHECK_EQ(ACC(cpu), 0x77);
    CHECK_EQ(cpu->iram[0x05], 0xA2);
}

static void test_inc_dec_variants(void)
{
    static const uint8_t code[] = {
        0x05, 0x40,       /* INC 40h       FF -> 00 */
        0x15, 0x41,       /* DEC 41h       00 -> FF */
        0x06,             /* INC @R0       (R0 = 42h) */
        0x17,             /* DEC @R1       (R1 = 43h) */
        0x0C,             /* INC R4 */
        0x1D,             /* DEC R5 */
        0x90, 0xFF, 0xFF, /* MOV DPTR,#0FFFFh */
        0xA3,             /* INC DPTR      -> 0000 */
    };
    mcs51 *cpu = tk_cpu(code, sizeof code);
    cpu->iram[0x40] = 0xFF;
    cpu->iram[0x41] = 0x00;
    cpu->iram[0] = 0x42;
    cpu->iram[1] = 0x43;
    cpu->iram[0x42] = 0x10;
    cpu->iram[0x43] = 0x10;
    cpu->iram[4] = 0x7F;
    cpu->iram[5] = 0x80;
    PSW(cpu) = MCS51_PSW_CY;
    run(cpu, 6);
    CHECK_EQ(cpu->iram[0x40], 0x00);
    CHECK_EQ(cpu->iram[0x41], 0xFF);
    CHECK_EQ(cpu->iram[0x42], 0x11);
    CHECK_EQ(cpu->iram[0x43], 0x0F);
    CHECK_EQ(cpu->iram[4], 0x80);
    CHECK_EQ(cpu->iram[5], 0x7F);
    CHECK_EQ(PSW(cpu) & 0xC4, MCS51_PSW_CY);  /* flags untouched */
    run(cpu, 1);
    CHECK_EQ(tk_step_cycles(cpu), 2);         /* INC DPTR: 2 cycles */
    CHECK_EQ(SFR(cpu, MCS51_DPH), 0x00);
    CHECK_EQ(SFR(cpu, MCS51_DPL), 0x00);
}

/* PUSH increments SP then writes; POP reads then decrements. Calls push the
 * return address low byte first. */
static void test_stack_and_calls(void)
{
    static const uint8_t code[] = {
        0x75, 0x81, 0x40, /* 0000 MOV SP,#40h */
        0xC0, 0xE0,       /* 0003 PUSH ACC */
        0xC0, 0xF0,       /* 0005 PUSH B */
        0xD0, 0x30,       /* 0007 POP 30h   (B) */
        0xD0, 0x31,       /* 0009 POP 31h   (ACC) */
        0x12, 0x00, 0x20, /* 000B LCALL 0020h */
        0x80, 0xFE,       /* 000E SJMP $ */
    };
    mcs51 *cpu = tk_cpu(code, sizeof code);
    static const uint8_t sub[] = {0xE5, 0x81, 0x22}; /* 0020 MOV A,SP ; RET */
    mcs51_load_code(cpu, 0x20, sub, sizeof sub);
    ACC(cpu) = 0x11;
    SFR(cpu, MCS51_B) = 0x22;
    run(cpu, 3);
    CHECK_EQ(SFR(cpu, MCS51_SP), 0x42);
    CHECK_EQ(cpu->iram[0x41], 0x11);
    CHECK_EQ(cpu->iram[0x42], 0x22);
    run(cpu, 2);
    CHECK_EQ(cpu->iram[0x30], 0x22);
    CHECK_EQ(cpu->iram[0x31], 0x11);
    CHECK_EQ(SFR(cpu, MCS51_SP), 0x40);
    run(cpu, 1);                       /* LCALL */
    CHECK_EQ(cpu->pc, 0x0020);
    CHECK_EQ(SFR(cpu, MCS51_SP), 0x42);
    CHECK_EQ(cpu->iram[0x41], 0x0E);   /* return address low byte first */
    CHECK_EQ(cpu->iram[0x42], 0x00);
    run(cpu, 1);
    CHECK_EQ(ACC(cpu), 0x42);
    run(cpu, 1);                       /* RET */
    CHECK_EQ(cpu->pc, 0x000E);
    CHECK_EQ(SFR(cpu, MCS51_SP), 0x40);
}

/* The stack may grow into the upper 128 bytes of internal RAM (8052 style). */
static void test_stack_upper_ram(void)
{
    static const uint8_t code[] = {0x75, 0x81, 0x7F, 0xC0, 0xE0, 0xC0, 0xE0};
    mcs51 *cpu = tk_cpu(code, sizeof code);
    ACC(cpu) = 0x5A;
    run(cpu, 3);
    CHECK_EQ(cpu->iram[0x80], 0x5A);
    CHECK_EQ(cpu->iram[0x81], 0x5A);
    CHECK_EQ(SFR(cpu, MCS51_P0), 0xFF); /* SFRs at 80h/81h untouched */
    CHECK_EQ(SFR(cpu, MCS51_SP), 0x81);
}

static void test_relative_and_absolute_jumps(void)
{
    mcs51 *cpu = tk_cpu(NULL, 0);
    /* SJMP backwards and forwards */
    static const uint8_t sjmp[] = {0x80, 0x80}; /* at 0100h: SJMP 0100h+2-128 */
    mcs51_load_code(cpu, 0x0100, sjmp, sizeof sjmp);
    cpu->pc = 0x0100;
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x0082);
    static const uint8_t fwd[] = {0x80, 0x7F};
    mcs51_load_code(cpu, 0x0200, fwd, sizeof fwd);
    cpu->pc = 0x0200;
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x0281);

    /* AJMP at 07FEh: the next instruction is at 0800h, so the target lies in
     * the 2 KiB page 0800h-0FFFh, not in the page of the AJMP itself. */
    static const uint8_t ajmp[] = {0xE1, 0x23}; /* AJMP page + 723h */
    mcs51_load_code(cpu, 0x07FE, ajmp, sizeof ajmp);
    cpu->pc = 0x07FE;
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x0F23);

    /* ACALL 1123h from 1000h */
    static const uint8_t acall[] = {0x31, 0x23};
    mcs51_load_code(cpu, 0x1000, acall, sizeof acall);
    cpu->pc = 0x1000;
    SFR(cpu, MCS51_SP) = 0x07;
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x1123);
    CHECK_EQ(cpu->iram[0x08], 0x02);
    CHECK_EQ(cpu->iram[0x09], 0x10);

    /* JMP @A+DPTR */
    static const uint8_t jmp[] = {0x73};
    mcs51_load_code(cpu, 0x2000, jmp, sizeof jmp);
    cpu->pc = 0x2000;
    SFR(cpu, MCS51_DPH) = 0x30;
    SFR(cpu, MCS51_DPL) = 0xF0;
    ACC(cpu) = 0x20;
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x3110);

    /* PC wraps from FFFFh to 0000h */
    cpu->code[0xFFFF] = 0x00;
    cpu->pc = 0xFFFF;
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x0000);
}

static void test_conditional_branches(void)
{
    static const struct {
        uint8_t op;
        uint8_t acc, psw;
        bool taken;
    } cases[] = {
        {0x60, 0x00, 0, true},  {0x60, 0x01, 0, false},  /* JZ */
        {0x70, 0x01, 0, true},  {0x70, 0x00, 0, false},  /* JNZ */
        {0x40, 0, MCS51_PSW_CY, true}, {0x40, 0, 0, false}, /* JC */
        {0x50, 0, 0, true},  {0x50, 0, MCS51_PSW_CY, false}, /* JNC */
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t code[] = {cases[i].op, 0x10};
        mcs51 *cpu = tk_cpu(code, sizeof code);
        ACC(cpu) = cases[i].acc;
        PSW(cpu) = cases[i].psw;
        CHECK_EQ(tk_step_cycles(cpu), 2);
        CHECK_EQ(cpu->pc, cases[i].taken ? 0x12 : 0x02);
    }
}

static void test_bit_branches(void)
{
    static const uint8_t code[] = {
        0x20, 0x00, 0x05, /* 0000 JB 20h.0,+5   (not taken) */
        0x30, 0x00, 0x05, /* 0003 JNB 20h.0,+5  -> 000Bh */
    };
    mcs51 *cpu = tk_cpu(code, sizeof code);
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x0003);
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x000B);

    /* JBC jumps and clears the bit only when it was set */
    static const uint8_t jbc[] = {0x10, 0x08, 0x10, 0x10, 0x08, 0x10};
    cpu = tk_cpu(jbc, sizeof jbc);
    cpu->iram[0x21] = 0x01;              /* bit 08h = 21h.0 */
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x0013);
    CHECK_EQ(cpu->iram[0x21], 0x00);
    cpu->pc = 0x0003;
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x0006);
}

static void test_djnz_and_cjne_variants(void)
{
    static const uint8_t code[] = {
        0x7F, 0x03,       /* 0000 MOV R7,#3 */
        0xDF, 0xFE,       /* 0002 DJNZ R7,$ */
        0x75, 0x40, 0x02, /* 0004 MOV 40h,#2 */
        0xD5, 0x40, 0xFD, /* 0007 DJNZ 40h,$ */
        0x78, 0x50,       /* 000A MOV R0,#50h */
        0xB6, 0x80, 0x10, /* 000C CJNE @R0,#80h,+10h  (50h holds 7Fh: jump, CY = 1) */
    };
    mcs51 *cpu = tk_cpu(code, sizeof code);
    cpu->iram[0x50] = 0x7F;
    mcs51_step(cpu);
    unsigned cycles = 0;
    for (int i = 0; i < 3; i++)
        cycles += tk_step_cycles(cpu);
    CHECK_EQ(cycles, 6);
    CHECK_EQ(cpu->pc, 0x0004);
    CHECK_EQ(cpu->iram[0x07], 0);
    run(cpu, 3);
    CHECK_EQ(cpu->pc, 0x000A);
    CHECK_EQ(cpu->iram[0x40], 0);
    run(cpu, 2);
    CHECK_EQ(cpu->pc, 0x001F);
    CHECK_EQ(PSW(cpu) & MCS51_PSW_CY, MCS51_PSW_CY);

    static const uint8_t cjne[] = {
        0xBA, 0x05, 0x10, /* CJNE R2,#5,+10h   (R2 = 5: no jump, CY = 0) */
        0xB5, 0x40, 0x10, /* CJNE A,40h,+10h   (A = 9, 40h = 3: jump, CY = 0) */
    };
    cpu = tk_cpu(cjne, sizeof cjne);
    cpu->iram[2] = 5;
    cpu->iram[0x40] = 3;
    ACC(cpu) = 9;
    PSW(cpu) = MCS51_PSW_CY;
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x0003);
    CHECK_EQ(PSW(cpu) & MCS51_PSW_CY, 0);
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x0016);
    CHECK_EQ(PSW(cpu) & MCS51_PSW_CY, 0);
}

static void test_movc(void)
{
    static const uint8_t code[] = {
        0x74, 0x02,       /* 0000 MOV A,#2 */
        0x83,             /* 0002 MOVC A,@A+PC   reads 0003h + 2 = 0005h */
        0x80, 0x03,       /* 0003 SJMP 0008h */
        0xAA, 0xBB, 0xCC, /* 0005 table */
        0x90, 0x00, 0x05, /* 0008 MOV DPTR,#0005h */
        0x74, 0x01,       /* 000B MOV A,#1 */
        0x93,             /* 000D MOVC A,@A+DPTR */
    };
    mcs51 *cpu = tk_cpu(code, sizeof code);
    run(cpu, 2);
    CHECK_EQ(ACC(cpu), 0xAA);
    run(cpu, 4);
    CHECK_EQ(ACC(cpu), 0xBB);
}

typedef struct xmem {
    uint8_t mem[0x10000];
    uint16_t last_addr;
} xmem;

static uint8_t xmem_read(void *ctx, uint16_t addr)
{
    xmem *x = ctx;
    x->last_addr = addr;
    return x->mem[addr];
}

static void xmem_write(void *ctx, uint16_t addr, uint8_t value)
{
    xmem *x = ctx;
    x->last_addr = addr;
    x->mem[addr] = value;
}

/* MOVX @DPTR uses the 16-bit DPTR; MOVX @Ri takes A15-A8 from the P2 latch.
 * Every MOVX overwrites the P0 latch with FFh (multiplexed bus). */
static void test_movx(void)
{
    static xmem x;
    static const uint8_t code[] = {
        0x75, 0x80, 0x00, /* MOV P0,#00h */
        0x90, 0x12, 0x34, /* MOV DPTR,#1234h */
        0x74, 0x5A,       /* MOV A,#5Ah */
        0xF0,             /* MOVX @DPTR,A */
        0x75, 0xA0, 0x7F, /* MOV P2,#7Fh */
        0x79, 0x80,       /* MOV R1,#80h */
        0x74, 0xA5,       /* MOV A,#0A5h */
        0xF3,             /* MOVX @R1,A    -> 7F80h */
        0xE4,             /* CLR A */
        0xE0,             /* MOVX A,@DPTR */
        0xE3,             /* MOVX A,@R1 */
    };
    memset(&x, 0, sizeof x);
    mcs51 *cpu = tk_cpu(code, sizeof code);
    cpu->io.xdata_read = xmem_read;
    cpu->io.xdata_write = xmem_write;
    cpu->io.xdata_ctx = &x;
    run(cpu, 3);
    CHECK_EQ(SFR(cpu, MCS51_P0), 0x00);
    CHECK_EQ(tk_step_cycles(cpu), 2);
    CHECK_EQ(x.mem[0x1234], 0x5A);
    CHECK_EQ(SFR(cpu, MCS51_P0), 0xFF);
    run(cpu, 4);
    CHECK_EQ(x.last_addr, 0x7F80);
    CHECK_EQ(x.mem[0x7F80], 0xA5);
    run(cpu, 2);
    CHECK_EQ(ACC(cpu), 0x5A);
    run(cpu, 1);
    CHECK_EQ(ACC(cpu), 0xA5);
    CHECK_EQ(SFR(cpu, MCS51_P2), 0x7F); /* P2 latch is not changed by MOVX */
}

/* Without a bus device the data bus floats high. */
static void test_movx_open_bus(void)
{
    static const uint8_t code[] = {0xE0};
    mcs51 *cpu = tk_cpu(code, sizeof code);
    ACC(cpu) = 0;
    mcs51_step(cpu);
    CHECK_EQ(ACC(cpu), 0xFF);
}

typedef struct pins {
    uint8_t level[4];
    unsigned writes;
    uint8_t last[4];
} pins;

static uint8_t pins_in(void *ctx, unsigned port)
{
    return ((pins *)ctx)->level[port];
}

static void pins_out(void *ctx, unsigned port, uint8_t latch)
{
    pins *p = ctx;
    p->writes++;
    p->last[port] = latch;
}

/* Instructions that read a port read the pins; read-modify-write
 * instructions read the latch. */
static void test_port_read_modify_write(void)
{
    static pins p;
    static const uint8_t code[] = {
        0xE5, 0x90,       /* MOV A,P1          pins: FEh */
        0xB2, 0x91,       /* CPL P1.1          latch FF -> FD (bit 0 stays 1) */
        0x05, 0x90,       /* INC P1            latch FD -> FE */
        0xA2, 0x90,       /* MOV C,P1.0        pin 0 is pulled low: C = 0 */
        0x43, 0x90, 0x01, /* ORL P1,#01h       latch FE -> FF */
        0x20, 0x90, 0x10, /* JB P1.0,+10h      pin still low: no jump */
    };
    memset(&p, 0, sizeof p);
    memset(p.level, 0xFF, sizeof p.level);
    p.level[1] = 0xFE; /* a switch pulls P1.0 to ground */
    mcs51 *cpu = tk_cpu(code, sizeof code);
    cpu->io.port_in = pins_in;
    cpu->io.port_out = pins_out;
    cpu->io.port_ctx = &p;
    PSW(cpu) = MCS51_PSW_CY;
    mcs51_step(cpu);
    CHECK_EQ(ACC(cpu), 0xFE);
    mcs51_step(cpu);
    CHECK_EQ(SFR(cpu, MCS51_P1), 0xFD);
    CHECK_EQ(p.last[1], 0xFD);
    mcs51_step(cpu);
    CHECK_EQ(SFR(cpu, MCS51_P1), 0xFE);
    mcs51_step(cpu);
    CHECK_EQ(PSW(cpu) & MCS51_PSW_CY, 0);
    mcs51_step(cpu);
    CHECK_EQ(SFR(cpu, MCS51_P1), 0xFF);
    mcs51_step(cpu);
    CHECK_EQ(cpu->pc, 0x000E);
    CHECK_EQ(p.writes, 3);
    CHECK_EQ(mcs51_port_pins(cpu, 1), 0xFE);
}

/* Unimplemented SFR addresses read FFh and ignore writes; PSW.P cannot be
 * written because it is driven from the accumulator. */
static void test_unimplemented_sfr_and_parity_flag(void)
{
    static const uint8_t code[] = {
        0x75, 0xC8, 0x12, /* MOV 0C8h,#12h   (T2CON of the 8052: not present) */
        0xE5, 0xC8,       /* MOV A,0C8h */
        0x74, 0x01,       /* MOV A,#1 */
        0xC2, 0xD0,       /* CLR P          (ignored: P follows A) */
    };
    mcs51 *cpu = tk_cpu(code, sizeof code);
    run(cpu, 2);
    CHECK_EQ(ACC(cpu), 0xFF);
    run(cpu, 2);
    CHECK_EQ(PSW(cpu) & MCS51_PSW_P, MCS51_PSW_P);
    CHECK_EQ(mcs51_read_direct(cpu, 0xC8), 0xFF);
}

/* POP into SP: SP is decremented before the destination is written. */
static void test_pop_sp(void)
{
    static const uint8_t code[] = {0xD0, 0x81}; /* POP SP */
    mcs51 *cpu = tk_cpu(code, sizeof code);
    SFR(cpu, MCS51_SP) = 0x40;
    cpu->iram[0x40] = 0x55;
    mcs51_step(cpu);
    CHECK_EQ(SFR(cpu, MCS51_SP), 0x55);
}

/* A5h is reserved: nothing executes and no time passes. */
static void test_reserved_opcode(void)
{
    static const uint8_t code[] = {0x00, 0xA5, 0x00};
    mcs51 *cpu = tk_cpu(code, sizeof code);
    CHECK_EQ(mcs51_step(cpu), MCS51_OK);
    CHECK_EQ(mcs51_step(cpu), MCS51_ILLEGAL);
    CHECK_EQ(cpu->pc, 0x0001);
    CHECK_EQ(cpu->cycles, 1);
    CHECK_EQ(mcs51_run(cpu, 100), MCS51_ILLEGAL);
}

/* Jump-to-self halts only when no interrupt could ever leave the loop. */
static void test_halt_detection(void)
{
    static const uint8_t sjmp[] = {0x80, 0xFE};
    mcs51 *cpu = tk_cpu(sjmp, sizeof sjmp);
    CHECK_EQ(mcs51_step(cpu), MCS51_HALT);
    CHECK_EQ(cpu->cycles, 2);

    cpu = tk_cpu(sjmp, sizeof sjmp);
    SFR(cpu, MCS51_IE) = MCS51_IE_EA | MCS51_IE_ET0;
    CHECK_EQ(mcs51_run(cpu, 100), MCS51_CYCLE_LIMIT);
    CHECK_EQ(cpu->cycles, 100);

    static const uint8_t ljmp[] = {0x02, 0x00, 0x00};
    cpu = tk_cpu(ljmp, sizeof ljmp);
    CHECK_EQ(mcs51_step(cpu), MCS51_HALT);
    static const uint8_t ajmp[] = {0x01, 0x00};
    cpu = tk_cpu(ajmp, sizeof ajmp);
    CHECK_EQ(mcs51_step(cpu), MCS51_HALT);

    /* idle mode with interrupts disabled can never end */
    static const uint8_t idle[] = {0x43, 0x87, 0x01, 0x00}; /* ORL PCON,#1 */
    cpu = tk_cpu(idle, sizeof idle);
    CHECK_EQ(mcs51_step(cpu), MCS51_HALT);
    CHECK(cpu->idle);
}

static void test_power_down(void)
{
    static const uint8_t code[] = {0x75, 0x87, 0x02, 0x00}; /* MOV PCON,#02h */
    mcs51 *cpu = tk_cpu(code, sizeof code);
    CHECK_EQ(mcs51_step(cpu), MCS51_POWER_DOWN);
    uint64_t cycles = cpu->cycles;
    CHECK_EQ(mcs51_step(cpu), MCS51_POWER_DOWN);
    CHECK_EQ(cpu->cycles, cycles);
    mcs51_reset(cpu); /* only a reset leaves power down */
    CHECK(!cpu->power_down);
    CHECK_EQ(SFR(cpu, MCS51_PCON), 0x00);
    CHECK_EQ(cpu->pc, 0);
}

static void test_reset_values(void)
{
    mcs51 *cpu = tk_cpu(NULL, 0);
    CHECK_EQ(cpu->pc, 0);
    CHECK_EQ(SFR(cpu, MCS51_SP), 0x07);
    CHECK_EQ(SFR(cpu, MCS51_P0), 0xFF);
    CHECK_EQ(SFR(cpu, MCS51_P1), 0xFF);
    CHECK_EQ(SFR(cpu, MCS51_P2), 0xFF);
    CHECK_EQ(SFR(cpu, MCS51_P3), 0xFF);
    CHECK_EQ(SFR(cpu, MCS51_PSW), 0x00);
    CHECK_EQ(SFR(cpu, MCS51_IE), 0x00);
    CHECK_EQ(cpu->code[0x1234], 0xFF);
    cpu->iram[0x30] = 0x99;
    cpu->pc = 0x1234;
    mcs51_reset(cpu);
    CHECK_EQ(cpu->pc, 0);
    CHECK_EQ(cpu->iram[0x30], 0x99); /* reset keeps the RAM contents */
}

int main(void)
{
    TK_RUN(test_mov_addressing_modes);
    TK_RUN(test_register_banks);
    TK_RUN(test_bit_addressing);
    TK_RUN(test_logic_on_direct);
    TK_RUN(test_exchange_instructions);
    TK_RUN(test_inc_dec_variants);
    TK_RUN(test_stack_and_calls);
    TK_RUN(test_stack_upper_ram);
    TK_RUN(test_relative_and_absolute_jumps);
    TK_RUN(test_conditional_branches);
    TK_RUN(test_bit_branches);
    TK_RUN(test_djnz_and_cjne_variants);
    TK_RUN(test_movc);
    TK_RUN(test_movx);
    TK_RUN(test_movx_open_bus);
    TK_RUN(test_port_read_modify_write);
    TK_RUN(test_unimplemented_sfr_and_parity_flag);
    TK_RUN(test_pop_sp);
    TK_RUN(test_reserved_opcode);
    TK_RUN(test_halt_detection);
    TK_RUN(test_power_down);
    TK_RUN(test_reset_values);
    return tk_summary();
}
