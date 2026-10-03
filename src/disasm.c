/*
 * disasm.c - one-instruction disassembler in Intel ASM51 syntax.
 *
 * Numbers are printed in hexadecimal with an 'h' suffix and a leading 0 when
 * they would start with a letter (0A5h). Direct addresses use SFR names,
 * bit addresses use the predefined bit names or the NAME.n / 2xh.n form.
 * Branch targets are printed as absolute code addresses.
 */
#include <stdio.h>
#include <string.h>

#include "mcs51/mcs51.h"

static const char *sfr_name(uint8_t addr)
{
    switch (addr) {
    case MCS51_P0: return "P0";
    case MCS51_SP: return "SP";
    case MCS51_DPL: return "DPL";
    case MCS51_DPH: return "DPH";
    case MCS51_PCON: return "PCON";
    case MCS51_TCON: return "TCON";
    case MCS51_TMOD: return "TMOD";
    case MCS51_TL0: return "TL0";
    case MCS51_TL1: return "TL1";
    case MCS51_TH0: return "TH0";
    case MCS51_TH1: return "TH1";
    case MCS51_P1: return "P1";
    case MCS51_SCON: return "SCON";
    case MCS51_SBUF: return "SBUF";
    case MCS51_P2: return "P2";
    case MCS51_IE: return "IE";
    case MCS51_P3: return "P3";
    case MCS51_IP: return "IP";
    case MCS51_PSW: return "PSW";
    case MCS51_ACC: return "ACC";
    case MCS51_B: return "B";
    default: return NULL;
    }
}

/* Named bits of the bit-addressable SFRs (ports use the Pn.b form). */
static const char *bit_name(uint8_t bit)
{
    static const char *const tcon[8] = {"IT0", "IE0", "IT1", "IE1", "TR0", "TF0", "TR1", "TF1"};
    static const char *const scon[8] = {"RI", "TI", "RB8", "TB8", "REN", "SM2", "SM1", "SM0"};
    static const char *const ie[8] = {"EX0", "ET0", "EX1", "ET1", "ES", NULL, NULL, "EA"};
    static const char *const ip[8] = {"PX0", "PT0", "PX1", "PT1", "PS", NULL, NULL, NULL};
    static const char *const psw[8] = {"P", NULL, "OV", "RS0", "RS1", "F0", "AC", "CY"};
    unsigned n = bit & 7u;
    switch (bit & 0xF8u) {
    case MCS51_TCON: return tcon[n];
    case MCS51_SCON: return scon[n];
    case MCS51_IE: return ie[n];
    case MCS51_IP: return ip[n];
    case MCS51_PSW: return psw[n];
    default: return NULL;
    }
}

/* Append to the output buffer, truncating safely. */
typedef struct {
    char *buf;
    size_t size, len;
} out_t;

static void put(out_t *o, const char *s)
{
    while (*s && o->len + 1 < o->size)
        o->buf[o->len++] = *s++;
    o->buf[o->len] = '\0';
}

static void put_hex(out_t *o, unsigned value, int digits)
{
    char tmp[8];
    /* Fixed-width formats keep GCC's -Wformat-truncation analysis exact. */
    snprintf(tmp, sizeof tmp, digits == 4 ? "%04X" : "%02X", value & (digits == 4 ? 0xFFFFu : 0xFFu));
    if (tmp[0] >= 'A')
        put(o, "0");
    put(o, tmp);
    put(o, "h");
}

static void put_direct(out_t *o, uint8_t addr)
{
    const char *name = sfr_name(addr);
    if (name)
        put(o, name);
    else
        put_hex(o, addr, 2);
}

static void put_bit(out_t *o, uint8_t bit)
{
    const char *name = bit_name(bit);
    char suffix[3] = {'.', (char)('0' + (bit & 7)), '\0'};
    if (name) {
        put(o, name);
        return;
    }
    if (bit < 0x80)
        put_hex(o, 0x20u + (bit >> 3), 2);
    else
        put_direct(o, (uint8_t)(bit & 0xF8));
    put(o, suffix);
}

unsigned mcs51_disasm(const uint8_t code[MCS51_CODE_SIZE], uint16_t addr,
                      char *out, size_t outsize)
{
    out_t o = {out, outsize, 0};
    uint8_t op = code[addr];
    const mcs51_opinfo *info = &mcs51_opcodes[op];
    uint8_t b1 = code[(uint16_t)(addr + 1)];
    uint8_t b2 = code[(uint16_t)(addr + 2)];
    uint16_t next = (uint16_t)(addr + info->bytes);

    if (outsize == 0)
        return info->bytes;
    out[0] = '\0';
    if (info->mnemonic == NULL) {
        put(&o, "DB    ");
        put_hex(&o, op, 2);
        return 1;
    }

    char mnemonic[8];
    snprintf(mnemonic, sizeof mnemonic, info->operands[0] ? "%-6s" : "%s", info->mnemonic);
    put(&o, mnemonic);

    for (const char *t = info->operands; *t; t++) {
        if (*t != '{') {
            char c[2] = {*t, '\0'};
            put(&o, c);
            continue;
        }
        char kind = t[1], pos = t[2];
        uint8_t byte = pos == '2' ? b2 : b1;
        t += (kind == 'W' || kind == 'A' || kind == 'L') ? 2 : 3; /* onto '}' */
        switch (kind) {
        case 'D':
            put_direct(&o, byte);
            break;
        case 'I':
            put(&o, "#");
            put_hex(&o, byte, 2);
            break;
        case 'W':
            put(&o, "#");
            put_hex(&o, (unsigned)(b1 << 8 | b2), 4);
            break;
        case 'B':
            put_bit(&o, byte);
            break;
        case 'N':
            put(&o, "/");
            put_bit(&o, byte);
            break;
        case 'R':
            put_hex(&o, (uint16_t)(next + byte - ((byte & 0x80u) ? 0x100u : 0u)), 4);
            break;
        case 'A':
            put_hex(&o, (unsigned)((next & 0xF800u) | ((op & 0xE0u) << 3) | b1), 4);
            break;
        case 'L':
            put_hex(&o, (unsigned)(b1 << 8 | b2), 4);
            break;
        }
    }
    return info->bytes;
}
