/*
 * opcodes.c - MCS-51 opcode map: mnemonic, operand template, length, cycles.
 *
 * Operand template placeholders (expanded by disasm.c):
 *   {D1} {D2}  direct address in operand byte 1 / 2
 *   {I1} {I2}  8-bit immediate in operand byte 1 / 2
 *   {W}        16-bit immediate in bytes 1-2 (high byte first)
 *   {B1}       bit address in byte 1        {N1}  complemented bit (/bit)
 *   {R1} {R2}  relative target from byte 1 / 2
 *   {A}        11-bit absolute address (AJMP/ACALL)
 *   {L}        16-bit code address in bytes 1-2
 *
 * Note the operand order of MOV direct,direct (85h): the source address is
 * encoded first, so the template is "{D2},{D1}".
 */
#include "mcs51/mcs51.h"

#define OP(m, o, b, c) {m, o, b, c}
/* Eight consecutive register opcodes Rn = R0..R7. */
#define RN(m, pre, post, b, c)                                               \
    OP(m, pre "R0" post, b, c), OP(m, pre "R1" post, b, c),                  \
    OP(m, pre "R2" post, b, c), OP(m, pre "R3" post, b, c),                  \
    OP(m, pre "R4" post, b, c), OP(m, pre "R5" post, b, c),                  \
    OP(m, pre "R6" post, b, c), OP(m, pre "R7" post, b, c)
/* Two consecutive indirect opcodes @R0, @R1. */
#define RI(m, pre, post, b, c) OP(m, pre "@R0" post, b, c), OP(m, pre "@R1" post, b, c)

const mcs51_opinfo mcs51_opcodes[256] = {
    /* 00 */ OP("NOP", "", 1, 1), OP("AJMP", "{A}", 2, 2), OP("LJMP", "{L}", 3, 2),
    OP("RR", "A", 1, 1), OP("INC", "A", 1, 1), OP("INC", "{D1}", 2, 1),
    RI("INC", "", "", 1, 1), RN("INC", "", "", 1, 1),
    /* 10 */ OP("JBC", "{B1},{R2}", 3, 2), OP("ACALL", "{A}", 2, 2), OP("LCALL", "{L}", 3, 2),
    OP("RRC", "A", 1, 1), OP("DEC", "A", 1, 1), OP("DEC", "{D1}", 2, 1),
    RI("DEC", "", "", 1, 1), RN("DEC", "", "", 1, 1),
    /* 20 */ OP("JB", "{B1},{R2}", 3, 2), OP("AJMP", "{A}", 2, 2), OP("RET", "", 1, 2),
    OP("RL", "A", 1, 1), OP("ADD", "A,{I1}", 2, 1), OP("ADD", "A,{D1}", 2, 1),
    RI("ADD", "A,", "", 1, 1), RN("ADD", "A,", "", 1, 1),
    /* 30 */ OP("JNB", "{B1},{R2}", 3, 2), OP("ACALL", "{A}", 2, 2), OP("RETI", "", 1, 2),
    OP("RLC", "A", 1, 1), OP("ADDC", "A,{I1}", 2, 1), OP("ADDC", "A,{D1}", 2, 1),
    RI("ADDC", "A,", "", 1, 1), RN("ADDC", "A,", "", 1, 1),
    /* 40 */ OP("JC", "{R1}", 2, 2), OP("AJMP", "{A}", 2, 2), OP("ORL", "{D1},A", 2, 1),
    OP("ORL", "{D1},{I2}", 3, 2), OP("ORL", "A,{I1}", 2, 1), OP("ORL", "A,{D1}", 2, 1),
    RI("ORL", "A,", "", 1, 1), RN("ORL", "A,", "", 1, 1),
    /* 50 */ OP("JNC", "{R1}", 2, 2), OP("ACALL", "{A}", 2, 2), OP("ANL", "{D1},A", 2, 1),
    OP("ANL", "{D1},{I2}", 3, 2), OP("ANL", "A,{I1}", 2, 1), OP("ANL", "A,{D1}", 2, 1),
    RI("ANL", "A,", "", 1, 1), RN("ANL", "A,", "", 1, 1),
    /* 60 */ OP("JZ", "{R1}", 2, 2), OP("AJMP", "{A}", 2, 2), OP("XRL", "{D1},A", 2, 1),
    OP("XRL", "{D1},{I2}", 3, 2), OP("XRL", "A,{I1}", 2, 1), OP("XRL", "A,{D1}", 2, 1),
    RI("XRL", "A,", "", 1, 1), RN("XRL", "A,", "", 1, 1),
    /* 70 */ OP("JNZ", "{R1}", 2, 2), OP("ACALL", "{A}", 2, 2), OP("ORL", "C,{B1}", 2, 2),
    OP("JMP", "@A+DPTR", 1, 2), OP("MOV", "A,{I1}", 2, 1), OP("MOV", "{D1},{I2}", 3, 2),
    RI("MOV", "", ",{I1}", 2, 1), RN("MOV", "", ",{I1}", 2, 1),
    /* 80 */ OP("SJMP", "{R1}", 2, 2), OP("AJMP", "{A}", 2, 2), OP("ANL", "C,{B1}", 2, 2),
    OP("MOVC", "A,@A+PC", 1, 2), OP("DIV", "AB", 1, 4), OP("MOV", "{D2},{D1}", 3, 2),
    RI("MOV", "{D1},", "", 2, 2), RN("MOV", "{D1},", "", 2, 2),
    /* 90 */ OP("MOV", "DPTR,{W}", 3, 2), OP("ACALL", "{A}", 2, 2), OP("MOV", "{B1},C", 2, 2),
    OP("MOVC", "A,@A+DPTR", 1, 2), OP("SUBB", "A,{I1}", 2, 1), OP("SUBB", "A,{D1}", 2, 1),
    RI("SUBB", "A,", "", 1, 1), RN("SUBB", "A,", "", 1, 1),
    /* A0 */ OP("ORL", "C,{N1}", 2, 2), OP("AJMP", "{A}", 2, 2), OP("MOV", "C,{B1}", 2, 1),
    OP("INC", "DPTR", 1, 2), OP("MUL", "AB", 1, 4), OP(NULL, NULL, 1, 0),
    RI("MOV", "", ",{D1}", 2, 2), RN("MOV", "", ",{D1}", 2, 2),
    /* B0 */ OP("ANL", "C,{N1}", 2, 2), OP("ACALL", "{A}", 2, 2), OP("CPL", "{B1}", 2, 1),
    OP("CPL", "C", 1, 1), OP("CJNE", "A,{I1},{R2}", 3, 2), OP("CJNE", "A,{D1},{R2}", 3, 2),
    RI("CJNE", "", ",{I1},{R2}", 3, 2), RN("CJNE", "", ",{I1},{R2}", 3, 2),
    /* C0 */ OP("PUSH", "{D1}", 2, 2), OP("AJMP", "{A}", 2, 2), OP("CLR", "{B1}", 2, 1),
    OP("CLR", "C", 1, 1), OP("SWAP", "A", 1, 1), OP("XCH", "A,{D1}", 2, 1),
    RI("XCH", "A,", "", 1, 1), RN("XCH", "A,", "", 1, 1),
    /* D0 */ OP("POP", "{D1}", 2, 2), OP("ACALL", "{A}", 2, 2), OP("SETB", "{B1}", 2, 1),
    OP("SETB", "C", 1, 1), OP("DA", "A", 1, 1), OP("DJNZ", "{D1},{R2}", 3, 2),
    RI("XCHD", "A,", "", 1, 1), RN("DJNZ", "", ",{R1}", 2, 2),
    /* E0 */ OP("MOVX", "A,@DPTR", 1, 2), OP("AJMP", "{A}", 2, 2), OP("MOVX", "A,@R0", 1, 2),
    OP("MOVX", "A,@R1", 1, 2), OP("CLR", "A", 1, 1), OP("MOV", "A,{D1}", 2, 1),
    RI("MOV", "A,", "", 1, 1), RN("MOV", "A,", "", 1, 1),
    /* F0 */ OP("MOVX", "@DPTR,A", 1, 2), OP("ACALL", "{A}", 2, 2), OP("MOVX", "@R0,A", 1, 2),
    OP("MOVX", "@R1,A", 1, 2), OP("CPL", "A", 1, 1), OP("MOV", "{D1},A", 2, 1),
    RI("MOV", "", ",A", 1, 1), RN("MOV", "", ",A", 1, 1),
};
