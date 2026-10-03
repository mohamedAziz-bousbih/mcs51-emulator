/*
 * test_alu.c - arithmetic and flag behaviour.
 *
 * The exhaustive tests compare the core against reference models written
 * differently from the implementation: overflow from the signed result
 * range, auxiliary carry from nibble sums, DA A from decimal arithmetic.
 * The worked examples come from the instruction descriptions in the Intel
 * MCS-51 instruction set manual.
 */
#include "testkit.h"

#define ACC(c) MCS51_SFR(c, MCS51_ACC)
#define PSW(c) MCS51_SFR(c, MCS51_PSW)
#define B(c) MCS51_SFR(c, MCS51_B)

static unsigned flag(const mcs51 *cpu, uint8_t mask)
{
    return (PSW(cpu) & mask) ? 1u : 0u;
}

static int signed8(unsigned v)
{
    return (v & 0x80u) ? (int)v - 256 : (int)v;
}

static unsigned bcd(unsigned n)
{
    return (n / 10) << 4 | (n % 10);
}

/* ADD A,#data and ADDC A,#data for every A, operand and carry-in. */
static void test_add_addc_exhaustive(void)
{
    static const uint8_t code[] = {0x24, 0x00};
    mcs51 *cpu = tk_cpu(code, sizeof code);
    for (unsigned with_carry = 0; with_carry < 2; with_carry++) {
        cpu->code[0] = with_carry ? 0x34 : 0x24;
        for (unsigned a = 0; a < 256; a++) {
            for (unsigned v = 0; v < 256; v++) {
                for (unsigned c = 0; c < 2; c++) {
                    cpu->code[1] = (uint8_t)v;
                    cpu->pc = 0;
                    ACC(cpu) = (uint8_t)a;
                    PSW(cpu) = c ? MCS51_PSW_CY : 0;
                    mcs51_step(cpu);
                    unsigned cin = with_carry ? c : 0;
                    unsigned sum = a + v + cin;
                    int ssum = signed8(a) + signed8(v) + (int)cin;
                    unsigned r = sum & 0xFF;
                    CHECK_CASE(ACC(cpu) == r && flag(cpu, MCS51_PSW_CY) == (sum > 0xFF) &&
                                   flag(cpu, MCS51_PSW_AC) == ((a & 15) + (v & 15) + cin > 15) &&
                                   flag(cpu, MCS51_PSW_OV) == (ssum < -128 || ssum > 127) &&
                                   flag(cpu, MCS51_PSW_P) == tk_parity((uint8_t)r) && cpu->pc == 2,
                               "%s A=%02X #%02X CY=%u -> A=%02X PSW=%02X", with_carry ? "ADDC" : "ADD",
                               a, v, c, ACC(cpu), PSW(cpu));
                }
            }
        }
    }
}

/* SUBB A,#data: CY is the borrow, AC the borrow out of the low nibble,
 * OV the signed overflow of A - data - CY. */
static void test_subb_exhaustive(void)
{
    static const uint8_t code[] = {0x94, 0x00};
    mcs51 *cpu = tk_cpu(code, sizeof code);
    for (unsigned a = 0; a < 256; a++) {
        for (unsigned v = 0; v < 256; v++) {
            for (unsigned c = 0; c < 2; c++) {
                cpu->code[1] = (uint8_t)v;
                cpu->pc = 0;
                ACC(cpu) = (uint8_t)a;
                PSW(cpu) = c ? MCS51_PSW_CY : 0;
                mcs51_step(cpu);
                int diff = (int)a - (int)v - (int)c;
                int sdiff = signed8(a) - signed8(v) - (int)c;
                unsigned r = (unsigned)diff & 0xFF;
                CHECK_CASE(ACC(cpu) == r && flag(cpu, MCS51_PSW_CY) == (diff < 0) &&
                               flag(cpu, MCS51_PSW_AC) == ((a & 15) < (v & 15) + c) &&
                               flag(cpu, MCS51_PSW_OV) == (sdiff < -128 || sdiff > 127) &&
                               flag(cpu, MCS51_PSW_P) == tk_parity((uint8_t)r),
                           "SUBB A=%02X #%02X CY=%u -> A=%02X PSW=%02X", a, v, c, ACC(cpu), PSW(cpu));
            }
        }
    }
}

/* ADD/ADDC followed by DA A must produce the decimal sum of two packed BCD
 * numbers (and the carry-in for ADDC), with CY as the decimal carry. */
static void test_da_decimal_sums(void)
{
    static const uint8_t code[] = {0x34, 0x00, 0xD4}; /* ADDC A,#bcd ; DA A */
    mcs51 *cpu = tk_cpu(code, sizeof code);
    for (unsigned with_carry = 0; with_carry < 2; with_carry++) {
        cpu->code[0] = with_carry ? 0x34 : 0x24;
        for (unsigned x = 0; x < 100; x++) {
            for (unsigned y = 0; y < 100; y++) {
                for (unsigned c = 0; c < 2; c++) {
                    cpu->code[1] = (uint8_t)bcd(y);
                    cpu->pc = 0;
                    ACC(cpu) = (uint8_t)bcd(x);
                    PSW(cpu) = c ? MCS51_PSW_CY : 0;
                    mcs51_step(cpu);
                    mcs51_step(cpu);
                    unsigned sum = x + y + (with_carry ? c : 0);
                    CHECK_CASE(ACC(cpu) == bcd(sum % 100) && flag(cpu, MCS51_PSW_CY) == (sum >= 100),
                               "%s %02u+%02u CY=%u, DA -> A=%02X CY=%u", with_carry ? "ADDC" : "ADD",
                               x, y, c, ACC(cpu), flag(cpu, MCS51_PSW_CY));
                }
            }
        }
    }
}

/* DA A never clears CY and never changes AC or OV. */
static void test_da_flag_rules(void)
{
    static const uint8_t code[] = {0xD4};
    mcs51 *cpu = tk_cpu(code, sizeof code);
    for (unsigned a = 0; a < 256; a++) {
        for (unsigned flags = 0; flags < 8; flags++) {
            uint8_t psw = (uint8_t)(((flags & 1) ? MCS51_PSW_CY : 0) | ((flags & 2) ? MCS51_PSW_AC : 0) |
                                    ((flags & 4) ? MCS51_PSW_OV : 0));
            cpu->pc = 0;
            ACC(cpu) = (uint8_t)a;
            PSW(cpu) = psw;
            mcs51_step(cpu);
            CHECK_CASE(!(flags & 1) || flag(cpu, MCS51_PSW_CY), "DA A=%02X cleared CY", a);
            CHECK_CASE((PSW(cpu) & (MCS51_PSW_AC | MCS51_PSW_OV)) == (psw & (MCS51_PSW_AC | MCS51_PSW_OV)),
                       "DA A=%02X changed AC/OV", a);
        }
    }
}

static void test_mul_exhaustive(void)
{
    static const uint8_t code[] = {0xA4};
    mcs51 *cpu = tk_cpu(code, sizeof code);
    for (unsigned a = 0; a < 256; a++) {
        for (unsigned b = 0; b < 256; b++) {
            cpu->pc = 0;
            ACC(cpu) = (uint8_t)a;
            B(cpu) = (uint8_t)b;
            PSW(cpu) = MCS51_PSW_CY | MCS51_PSW_AC;
            unsigned cycles = tk_step_cycles(cpu);
            unsigned p = a * b;
            CHECK_CASE(ACC(cpu) == (p & 0xFF) && B(cpu) == (p >> 8) && !flag(cpu, MCS51_PSW_CY) &&
                           flag(cpu, MCS51_PSW_OV) == (p > 0xFF) && flag(cpu, MCS51_PSW_AC) && cycles == 4,
                       "MUL %02X*%02X -> B:A=%02X%02X PSW=%02X", a, b, B(cpu), ACC(cpu), PSW(cpu));
        }
    }
}

/* DIV AB; division by zero sets OV and leaves A and B unchanged. */
static void test_div_exhaustive(void)
{
    static const uint8_t code[] = {0x84};
    mcs51 *cpu = tk_cpu(code, sizeof code);
    for (unsigned a = 0; a < 256; a++) {
        for (unsigned b = 0; b < 256; b++) {
            cpu->pc = 0;
            ACC(cpu) = (uint8_t)a;
            B(cpu) = (uint8_t)b;
            PSW(cpu) = MCS51_PSW_CY | (b ? MCS51_PSW_OV : 0);
            unsigned cycles = tk_step_cycles(cpu);
            unsigned q = b ? a / b : a, r = b ? a % b : b;
            CHECK_CASE(ACC(cpu) == q && B(cpu) == r && !flag(cpu, MCS51_PSW_CY) &&
                           flag(cpu, MCS51_PSW_OV) == (b == 0) && cycles == 4,
                       "DIV %02X/%02X -> A=%02X B=%02X PSW=%02X", a, b, ACC(cpu), B(cpu), PSW(cpu));
        }
    }
}

static void test_rotates_exhaustive(void)
{
    static const uint8_t code[] = {0x23}; /* RL A, RR A, RLC A, RRC A, SWAP A */
    static const uint8_t ops[] = {0x23, 0x03, 0x33, 0x13, 0xC4};
    mcs51 *cpu = tk_cpu(code, sizeof code);
    for (unsigned i = 0; i < sizeof ops; i++) {
        cpu->code[0] = ops[i];
        for (unsigned a = 0; a < 256; a++) {
            for (unsigned c = 0; c < 2; c++) {
                cpu->pc = 0;
                ACC(cpu) = (uint8_t)a;
                PSW(cpu) = c ? MCS51_PSW_CY : 0;
                mcs51_step(cpu);
                unsigned r = 0, cy = c;
                switch (ops[i]) {
                case 0x23: r = (a << 1 | a >> 7) & 0xFF; break;
                case 0x03: r = (a >> 1 | a << 7) & 0xFF; break;
                case 0x33: r = (a << 1 | c) & 0xFF; cy = a >> 7; break;
                case 0x13: r = a >> 1 | c << 7; cy = a & 1; break;
                case 0xC4: r = (a << 4 | a >> 4) & 0xFF; break;
                }
                CHECK_CASE(ACC(cpu) == r && flag(cpu, MCS51_PSW_CY) == cy,
                           "op %02X A=%02X CY=%u -> A=%02X CY=%u", ops[i], a, c, ACC(cpu),
                           flag(cpu, MCS51_PSW_CY));
            }
        }
    }
}

/* CJNE A,#data,rel: unsigned compare into CY, branch when different. */
static void test_cjne_exhaustive(void)
{
    static const uint8_t code[] = {0xB4, 0x00, 0x10};
    mcs51 *cpu = tk_cpu(code, sizeof code);
    for (unsigned a = 0; a < 256; a++) {
        for (unsigned v = 0; v < 256; v++) {
            cpu->code[1] = (uint8_t)v;
            cpu->pc = 0;
            ACC(cpu) = (uint8_t)a;
            PSW(cpu) = 0;
            unsigned cycles = tk_step_cycles(cpu);
            CHECK_CASE(cpu->pc == (a != v ? 0x13 : 0x03) && flag(cpu, MCS51_PSW_CY) == (a < v) &&
                           ACC(cpu) == a && cycles == 2,
                       "CJNE A=%02X,#%02X -> PC=%04X CY=%u", a, v, cpu->pc, flag(cpu, MCS51_PSW_CY));
        }
    }
}

/* INC/DEC never touch the flags; the parity flag follows the accumulator. */
static void test_inc_dec_and_parity(void)
{
    static const uint8_t code[] = {0x04, 0x14, 0x74, 0x00}; /* INC A; DEC A; MOV A,#v */
    mcs51 *cpu = tk_cpu(code, sizeof code);
    for (unsigned a = 0; a < 256; a++) {
        cpu->pc = 0;
        ACC(cpu) = (uint8_t)a;
        PSW(cpu) = MCS51_PSW_CY | MCS51_PSW_AC | MCS51_PSW_OV;
        mcs51_step(cpu);
        CHECK_CASE(ACC(cpu) == ((a + 1) & 0xFF) && (PSW(cpu) & 0xC4) == 0xC4 &&
                       flag(cpu, MCS51_PSW_P) == tk_parity((uint8_t)(a + 1)),
                   "INC A=%02X", a);
        mcs51_step(cpu);
        CHECK_CASE(ACC(cpu) == a && (PSW(cpu) & 0xC4) == 0xC4, "DEC A=%02X", a + 1);
        cpu->code[3] = (uint8_t)a;
        mcs51_step(cpu);
        CHECK_CASE(flag(cpu, MCS51_PSW_P) == tk_parity((uint8_t)a), "parity of %02X", a);
    }
}

/* Worked examples from the Intel instruction set descriptions. */
static void test_intel_examples(void)
{
    {   /* ADD A,R0 with A = C3h, R0 = AAh: A = 6Dh, AC = 0, CY = 1, OV = 1 */
        static const uint8_t code[] = {0x28};
        mcs51 *cpu = tk_cpu(code, sizeof code);
        ACC(cpu) = 0xC3;
        cpu->iram[0] = 0xAA;
        mcs51_step(cpu);
        CHECK_EQ(ACC(cpu), 0x6D);
        CHECK_EQ(PSW(cpu) & (MCS51_PSW_CY | MCS51_PSW_AC | MCS51_PSW_OV), MCS51_PSW_CY | MCS51_PSW_OV);
    }
    {   /* ADDC A,R0 with A = C3h, R0 = AAh, CY = 1: A = 6Eh, AC = 0, CY = 1, OV = 1 */
        static const uint8_t code[] = {0x38};
        mcs51 *cpu = tk_cpu(code, sizeof code);
        ACC(cpu) = 0xC3;
        PSW(cpu) = MCS51_PSW_CY;
        cpu->iram[0] = 0xAA;
        mcs51_step(cpu);
        CHECK_EQ(ACC(cpu), 0x6E);
        CHECK_EQ(PSW(cpu) & (MCS51_PSW_CY | MCS51_PSW_AC | MCS51_PSW_OV), MCS51_PSW_CY | MCS51_PSW_OV);
    }
    {   /* SUBB A,R2 with A = C9h, R2 = 54h, CY = 1: A = 74h, CY = 0, AC = 0, OV = 1 */
        static const uint8_t code[] = {0x9A};
        mcs51 *cpu = tk_cpu(code, sizeof code);
        ACC(cpu) = 0xC9;
        PSW(cpu) = MCS51_PSW_CY;
        cpu->iram[2] = 0x54;
        mcs51_step(cpu);
        CHECK_EQ(ACC(cpu), 0x74);
        CHECK_EQ(PSW(cpu) & (MCS51_PSW_CY | MCS51_PSW_AC | MCS51_PSW_OV), MCS51_PSW_OV);
    }
    {   /* ADDC A,R3 ; DA A with A = 56h, R3 = 67h, CY = 1: BEh, then 24h with CY = 1 */
        static const uint8_t code[] = {0x3B, 0xD4};
        mcs51 *cpu = tk_cpu(code, sizeof code);
        ACC(cpu) = 0x56;
        PSW(cpu) = MCS51_PSW_CY;
        cpu->iram[3] = 0x67;
        mcs51_step(cpu);
        CHECK_EQ(ACC(cpu), 0xBE);
        CHECK_EQ(PSW(cpu) & (MCS51_PSW_CY | MCS51_PSW_AC), 0);
        mcs51_step(cpu);
        CHECK_EQ(ACC(cpu), 0x24);
        CHECK_EQ(flag(cpu, MCS51_PSW_CY), 1);
    }
    {   /* ADD A,#99h ; DA A with A = 30h: decimal 30 - 1, A = 29h, CY = 1 */
        static const uint8_t code[] = {0x24, 0x99, 0xD4};
        mcs51 *cpu = tk_cpu(code, sizeof code);
        ACC(cpu) = 0x30;
        mcs51_step(cpu);
        mcs51_step(cpu);
        CHECK_EQ(ACC(cpu), 0x29);
        CHECK_EQ(flag(cpu, MCS51_PSW_CY), 1);
    }
    {   /* MUL AB with A = 50h, B = A0h: product 3200h, B = 32h, A = 00h, OV = 1 */
        static const uint8_t code[] = {0xA4};
        mcs51 *cpu = tk_cpu(code, sizeof code);
        ACC(cpu) = 0x50;
        B(cpu) = 0xA0;
        mcs51_step(cpu);
        CHECK_EQ(B(cpu), 0x32);
        CHECK_EQ(ACC(cpu), 0x00);
        CHECK_EQ(PSW(cpu) & (MCS51_PSW_CY | MCS51_PSW_OV), MCS51_PSW_OV);
    }
    {   /* DIV AB with A = 251, B = 18: A = 13, B = 17, CY = OV = 0 */
        static const uint8_t code[] = {0x84};
        mcs51 *cpu = tk_cpu(code, sizeof code);
        ACC(cpu) = 251;
        B(cpu) = 18;
        mcs51_step(cpu);
        CHECK_EQ(ACC(cpu), 13);
        CHECK_EQ(B(cpu), 17);
        CHECK_EQ(PSW(cpu) & (MCS51_PSW_CY | MCS51_PSW_OV), 0);
    }
    {   /* RLC A / RRC A / RL A / RR A / SWAP A on C5h with CY = 0 */
        static const struct { uint8_t op, result, carry; } rot[] = {
            {0x33, 0x8A, 1}, {0x13, 0x62, 1}, {0x23, 0x8B, 0}, {0x03, 0xE2, 0}, {0xC4, 0x5C, 0},
        };
        for (unsigned i = 0; i < sizeof rot / sizeof rot[0]; i++) {
            mcs51 *cpu = tk_cpu(&rot[i].op, 1);
            ACC(cpu) = 0xC5;
            mcs51_step(cpu);
            CHECK_EQ(ACC(cpu), rot[i].result);
            CHECK_EQ(flag(cpu, MCS51_PSW_CY), rot[i].carry);
        }
    }
}

/* Named edge cases of the flag rules, one row each. A row executes `steps`
 * instructions; the flags not listed in `mask` must keep their input value. */
static void test_flag_edge_cases(void)
{
    enum { CY = MCS51_PSW_CY, AC = MCS51_PSW_AC, OV = MCS51_PSW_OV };
    static const struct {
        const char *name;
        uint8_t code[3], steps;
        uint8_t a, b, psw;          /* inputs */
        uint8_t a_out, b_out, flags; /* expected A, B and CY/AC/OV */
    } cases[] = {
        {"ADD 7Fh+01h: signed overflow, no carry", {0x24, 0x01}, 1, 0x7F, 0, 0, 0x80, 0, AC | OV},
        {"ADD FFh+01h: carry, no overflow", {0x24, 0x01}, 1, 0xFF, 0, 0, 0x00, 0, CY | AC},
        {"ADD 80h+80h: carry and overflow", {0x24, 0x80}, 1, 0x80, 0, 0, 0x00, 0, CY | OV},
        {"ADDC 0Fh+00h+CY: half carry from the carry-in", {0x34, 0x00}, 1, 0x0F, 0, CY, 0x10, 0, AC},
        {"SUBB 00h-01h: borrow", {0x94, 0x01}, 1, 0x00, 0, 0, 0xFF, 0, CY | AC},
        {"SUBB 80h-01h: overflow (-128 - 1)", {0x94, 0x01}, 1, 0x80, 0, 0, 0x7F, 0, AC | OV},
        {"SUBB 7Fh-FFh: borrow and overflow (127 + 1)", {0x94, 0xFF}, 1, 0x7F, 0, 0, 0x80, 0, CY | OV},
        {"SUBB 10h-00h-CY: borrow into bit 3", {0x94, 0x00}, 1, 0x10, 0, CY, 0x0F, 0, AC},
        {"SUBB 05h-05h-CY: borrow from the carry alone", {0x94, 0x05}, 1, 0x05, 0, CY, 0xFF, 0, CY | AC},
        {"ADD 09h+01h, DA A: 10", {0x24, 0x01, 0xD4}, 2, 0x09, 0, 0, 0x10, 0, 0},
        {"ADD 08h+08h, DA A: 16 (AC set by the ADD)", {0x24, 0x08, 0xD4}, 2, 0x08, 0, 0, 0x16, 0, AC},
        {"ADD 99h+01h, DA A: 00 with decimal carry", {0x24, 0x01, 0xD4}, 2, 0x99, 0, 0, 0x00, 0, CY},
        {"ADD 90h+90h, DA A: 80 with carry, OV kept", {0x24, 0x90, 0xD4}, 2, 0x90, 0, 0, 0x80, 0, CY | OV},
        {"MUL 10h*10h: 0100h sets OV, clears CY", {0xA4}, 1, 0x10, 0x10, CY, 0x00, 0x01, OV},
        {"MUL FFh*FFh: FE01h", {0xA4}, 1, 0xFF, 0xFF, 0, 0x01, 0xFE, OV},
        {"MUL 0Fh*11h: 00FFh, no OV", {0xA4}, 1, 0x0F, 0x11, CY | OV, 0xFF, 0x00, 0},
        {"DIV FFh/10h: 0Fh remainder 0Fh", {0x84}, 1, 0xFF, 0x10, CY | OV, 0x0F, 0x0F, 0},
        {"DIV 00h/FFh: 0 remainder 0", {0x84}, 1, 0x00, 0xFF, 0, 0x00, 0x00, 0},
        {"DIV 12h/00h: OV set, CY cleared, A and B kept", {0x84}, 1, 0x12, 0x00, CY, 0x12, 0x00, OV},
        {"RLC 80h: bit 7 into CY", {0x33}, 1, 0x80, 0, 0, 0x00, 0, CY},
        {"RRC 00h with CY: CY into bit 7", {0x13}, 1, 0x00, 0, CY, 0x80, 0, 0},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        mcs51 *cpu = tk_cpu(cases[i].code, sizeof cases[i].code);
        ACC(cpu) = cases[i].a;
        B(cpu) = cases[i].b;
        PSW(cpu) = cases[i].psw;
        for (unsigned s = 0; s < cases[i].steps; s++)
            mcs51_step(cpu);
        uint8_t flags = (uint8_t)(PSW(cpu) & (CY | AC | OV));
        if (ACC(cpu) != cases[i].a_out || B(cpu) != cases[i].b_out || flags != cases[i].flags ||
            flag(cpu, MCS51_PSW_P) != tk_parity(cases[i].a_out)) {
            fprintf(stderr, "%s: A=%02X B=%02X flags=%02X, expected A=%02X B=%02X flags=%02X\n",
                    cases[i].name, ACC(cpu), B(cpu), flags, cases[i].a_out, cases[i].b_out,
                    cases[i].flags);
            tk_failures++;
        }
    }
}

/* The other ADD source operands (direct, @Ri, Rn) feed the same adder. */
static void test_alu_operand_sources(void)
{
    static const uint8_t code[] = {
        0x25, 0x40,   /* ADD A,40h */
        0x37,         /* ADDC A,@R1 */
        0x9D,         /* SUBB A,R5 */
        0x45, 0x40,   /* ORL A,40h */
        0x56,         /* ANL A,@R0 */
        0x6F,         /* XRL A,R7 */
    };
    mcs51 *cpu = tk_cpu(code, sizeof code);
    cpu->iram[0x40] = 0x0F;
    cpu->iram[0x41] = 0xF0;
    cpu->iram[0] = 0x41;  /* R0 -> 41h */
    cpu->iram[1] = 0x41;  /* R1 -> 41h */
    cpu->iram[5] = 0x01;  /* R5 */
    cpu->iram[7] = 0x3C;  /* R7 */
    ACC(cpu) = 0x01;
    mcs51_step(cpu);
    CHECK_EQ(ACC(cpu), 0x10);                       /* 01 + 0F, AC */
    CHECK_EQ(flag(cpu, MCS51_PSW_AC), 1);
    mcs51_step(cpu);
    CHECK_EQ(ACC(cpu), 0x00);                       /* 10 + F0 = 100h */
    CHECK_EQ(flag(cpu, MCS51_PSW_CY), 1);
    mcs51_step(cpu);
    CHECK_EQ(ACC(cpu), 0xFE);                       /* 00 - 01 - 1 */
    mcs51_step(cpu);
    CHECK_EQ(ACC(cpu), 0xFF);
    mcs51_step(cpu);
    CHECK_EQ(ACC(cpu), 0xF0);
    mcs51_step(cpu);
    CHECK_EQ(ACC(cpu), 0xCC);
    CHECK_EQ(cpu->pc, sizeof code);
}

int main(void)
{
    TK_RUN(test_add_addc_exhaustive);
    TK_RUN(test_subb_exhaustive);
    TK_RUN(test_da_decimal_sums);
    TK_RUN(test_da_flag_rules);
    TK_RUN(test_mul_exhaustive);
    TK_RUN(test_div_exhaustive);
    TK_RUN(test_rotates_exhaustive);
    TK_RUN(test_cjne_exhaustive);
    TK_RUN(test_inc_dec_and_parity);
    TK_RUN(test_intel_examples);
    TK_RUN(test_flag_edge_cases);
    TK_RUN(test_alu_operand_sources);
    return tk_summary();
}
