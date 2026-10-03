/*
 * test_disasm.c - the disassembler against tests/vectors/all_opcodes.a51.
 *
 * all_opcodes.a51 holds one instruction per opcode, rendered from the
 * instruction table of the Python assembler; all_opcodes.hex is its
 * assembled form. Disassembling the hex file must reproduce every source
 * line (compared without whitespace), which cross-checks the C opcode table,
 * the operand templates and the disassembler against the assembler.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "mcs51/ihex.h"
#include "testkit.h"

static void normalise(const char *in, char *out, size_t size)
{
    size_t n = 0;
    for (; *in && *in != ';' && n + 1 < size; in++) {
        if (!isspace((unsigned char)*in))
            out[n++] = (char)toupper((unsigned char)*in);
    }
    out[n] = '\0';
}

static void test_all_opcodes_round_trip(void)
{
    static mcs51 cpu;
    mcs51_init(&cpu);
    mcs51_ihex_info info;
    CHECK_EQ(mcs51_ihex_load_file(tk_path("tests/vectors/all_opcodes.hex"), cpu.code, MCS51_CODE_SIZE, &info), 0);
    FILE *f = fopen(tk_path("tests/vectors/all_opcodes.a51"), "r");
    CHECK(f != NULL);
    if (!f)
        return;

    char line[256], expected[128], actual[128], text[64];
    uint16_t addr = 0;
    unsigned instructions = 0, opcode_seen[256] = {0};
    while (fgets(line, sizeof line, f)) {
        normalise(line, expected, sizeof expected);
        if (expected[0] == '\0' || !strcmp(expected, "END"))
            continue;
        if (!strncmp(expected, "ORG", 3)) {
            addr = (uint16_t)strtoul(expected + 3, NULL, 16);
            continue;
        }
        unsigned len = mcs51_disasm(cpu.code, addr, text, sizeof text);
        normalise(text, actual, sizeof actual);
        if (strcmp(expected, actual) != 0) {
            fprintf(stderr, "%04X: disassembled '%s', source '%s'\n", addr, text, line);
            tk_failures++;
        }
        opcode_seen[cpu.code[addr]]++;
        addr = (uint16_t)(addr + len);
        instructions++;
    }
    fclose(f);
    CHECK_EQ(instructions, 256);
    for (unsigned op = 0; op < 256; op++)
        CHECK_EQ(opcode_seen[op], 1);
    CHECK_EQ(addr, info.highest + 1);
}

static void test_formatting_details(void)
{
    static uint8_t code[MCS51_CODE_SIZE];
    char text[64];
    static const uint8_t bytes[] = {0x75, 0xC8, 0x0A, 0xD2, 0xD1, 0xA5, 0x80, 0xFE};
    memcpy(code, bytes, sizeof bytes);
    CHECK_EQ(mcs51_disasm(code, 0, text, sizeof text), 3);
    CHECK(!strcmp(text, "MOV   0C8h,#0Ah"));     /* no SFR at C8h: plain hex */
    CHECK_EQ(mcs51_disasm(code, 3, text, sizeof text), 2);
    CHECK(!strcmp(text, "SETB  PSW.1"));         /* unnamed bit of a named SFR */
    CHECK_EQ(mcs51_disasm(code, 5, text, sizeof text), 1);
    CHECK(!strcmp(text, "DB    0A5h"));          /* reserved opcode */
    CHECK_EQ(mcs51_disasm(code, 6, text, sizeof text), 2);
    CHECK(!strcmp(text, "SJMP  0006h"));
    /* a short buffer is truncated, never overrun */
    char tiny[6];
    memset(tiny, 'x', sizeof tiny);
    mcs51_disasm(code, 0, tiny, 5);
    CHECK_EQ(strlen(tiny), 4);
    CHECK_EQ(tiny[5], 'x');
}

int main(void)
{
    TK_RUN(test_all_opcodes_round_trip);
    TK_RUN(test_formatting_details);
    return tk_summary();
}
