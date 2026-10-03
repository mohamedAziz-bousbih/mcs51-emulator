/*
 * test_opcode_table.c - instruction lengths and machine cycles of all 256
 * opcodes.
 *
 * The golden matrices below are transcribed from the opcode map of the Intel
 * MCS-51 instruction set (row = high nibble, column = low nibble; 0 marks
 * the reserved opcode A5h). They are compared with the core's table, and
 * every opcode is executed to check that it fetches exactly its length and
 * takes exactly its cycle count.
 */
#include <string.h>

#include "testkit.h"

static const char *const golden_bytes[16] = {
    "1231121111111111", /* 0x */
    "3231121111111111", /* 1x */
    "3211221111111111", /* 2x */
    "3211221111111111", /* 3x */
    "2223221111111111", /* 4x */
    "2223221111111111", /* 5x */
    "2223221111111111", /* 6x */
    "2221232222222222", /* 7x */
    "2221132222222222", /* 8x */
    "3221221111111111", /* 9x */
    "2221102222222222", /* Ax */
    "2221333333333333", /* Bx */
    "2221121111111111", /* Cx */
    "2221131122222222", /* Dx */
    "1211121111111111", /* Ex */
    "1211121111111111", /* Fx */
};

static const char *const golden_cycles[16] = {
    "1221111111111111", /* 0x */
    "2221111111111111", /* 1x */
    "2221111111111111", /* 2x */
    "2221111111111111", /* 3x */
    "2212111111111111", /* 4x */
    "2212111111111111", /* 5x */
    "2212111111111111", /* 6x */
    "2222121111111111", /* 7x */
    "2222422222222222", /* 8x */
    "2222111111111111", /* 9x */
    "2212402222222222", /* Ax */
    "2211222222222222", /* Bx */
    "2211111111111111", /* Cx */
    "2211121122222222", /* Dx */
    "2222111111111111", /* Ex */
    "2222111111111111", /* Fx */
};

static void test_table_matches_intel_opcode_map(void)
{
    unsigned defined = 0;
    for (unsigned op = 0; op < 256; op++) {
        unsigned bytes = (unsigned)(golden_bytes[op >> 4][op & 15] - '0');
        unsigned cycles = (unsigned)(golden_cycles[op >> 4][op & 15] - '0');
        const mcs51_opinfo *info = &mcs51_opcodes[op];
        if (bytes == 0) {
            CHECK(info->mnemonic == NULL);
            continue;
        }
        defined++;
        if (info->mnemonic == NULL || info->bytes != bytes || info->cycles != cycles) {
            fprintf(stderr, "opcode %02X: table %s %u bytes %u cycles, Intel %u bytes %u cycles\n", op,
                    info->mnemonic ? info->mnemonic : "(reserved)", info->bytes, info->cycles, bytes, cycles);
            tk_failures++;
        }
    }
    CHECK_EQ(defined, 255);
}

/* Execute every opcode at 1000h with zero operand bytes. A relative offset
 * of 0 makes taken and untaken branches end at the same address, so for all
 * but the jumps with absolute targets the next PC must be 1000h + length. */
static void test_execution_matches_table(void)
{
    mcs51 *cpu = tk_cpu(NULL, 0);
    for (unsigned op = 0; op < 256; op++) {
        const mcs51_opinfo *info = &mcs51_opcodes[op];
        if (info->mnemonic == NULL)
            continue;
        mcs51_init(cpu);
        cpu->code[0x1000] = (uint8_t)op;
        cpu->code[0x1001] = 0x00;
        cpu->code[0x1002] = 0x00;
        cpu->pc = 0x1000;
        /* RET/RETI find 1001h on the stack; JMP @A+DPTR jumps to 1001h. */
        MCS51_SFR(cpu, MCS51_SP) = 0x40;
        cpu->iram[0x40] = 0x10;
        cpu->iram[0x3F] = 0x01;
        MCS51_SFR(cpu, MCS51_DPH) = 0x10;
        MCS51_SFR(cpu, MCS51_DPL) = 0x00;
        MCS51_SFR(cpu, MCS51_ACC) = 0x01;

        uint16_t expected = (uint16_t)(0x1000 + info->bytes);
        if ((op & 0x1F) == 0x01 || (op & 0x1F) == 0x11) /* AJMP/ACALL */
            expected = (uint16_t)(0x1000 | (op & 0xE0u) << 3);
        else if (op == 0x02 || op == 0x12) /* LJMP/LCALL 0000h */
            expected = 0x0000;

        /* AJMP 1000h jumps to itself, which the core reports as a halt
         * because no interrupt is enabled. */
        mcs51_status want = (op == 0x01) ? MCS51_HALT : MCS51_OK;
        mcs51_status status = mcs51_step(cpu);
        if (status != want || cpu->pc != expected || cpu->cycles != info->cycles) {
            fprintf(stderr, "opcode %02X (%s %s): status %s, PC %04X (expected %04X), %u cycles (expected %u)\n",
                    op, info->mnemonic, info->operands, mcs51_status_name(status), cpu->pc, expected,
                    (unsigned)cpu->cycles, info->cycles);
            tk_failures++;
        }
    }
}

/* Branch timing does not depend on whether the branch is taken. */
static void test_branch_cycles_taken_and_not_taken(void)
{
    static const struct { uint8_t code[3]; uint8_t acc; uint8_t psw; } cases[] = {
        {{0x60, 0x10, 0x00}, 0, 0}, {{0x60, 0x10, 0x00}, 1, 0},             /* JZ */
        {{0x40, 0x10, 0x00}, 0, MCS51_PSW_CY}, {{0x40, 0x10, 0x00}, 0, 0},  /* JC */
        {{0xB4, 0x05, 0x10}, 5, 0}, {{0xB4, 0x05, 0x10}, 6, 0},             /* CJNE */
        {{0x20, 0xE0, 0x10}, 1, 0}, {{0x20, 0xE0, 0x10}, 0, 0},             /* JB ACC.0 */
        {{0xD8, 0x10, 0x00}, 0, 0},                                         /* DJNZ R0 */
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        mcs51 *cpu = tk_cpu(cases[i].code, sizeof cases[i].code);
        MCS51_SFR(cpu, MCS51_ACC) = cases[i].acc;
        MCS51_SFR(cpu, MCS51_PSW) = cases[i].psw;
        CHECK_EQ(tk_step_cycles(cpu), 2);
    }
}

int main(void)
{
    TK_RUN(test_table_matches_intel_opcode_map);
    TK_RUN(test_execution_matches_table);
    TK_RUN(test_branch_cycles_taken_and_not_taken);
    return tk_summary();
}
