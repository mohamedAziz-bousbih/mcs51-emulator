/*
 * ihex.c - Intel HEX loader.
 *
 * Record layout: ':' LL AAAA TT DD... CC, all fields as hexadecimal byte
 * pairs; CC makes the sum of all record bytes 0 modulo 256.
 */
#include "mcs51/ihex.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(mcs51_ihex_info *info, unsigned line, const char *message)
{
    info->line = line;
    if (line)
        snprintf(info->error, sizeof info->error, "line %u: %s", line, message);
    else
        snprintf(info->error, sizeof info->error, "%s", message);
    return -1;
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

int mcs51_ihex_parse(const char *text, size_t len, uint8_t *mem, size_t mem_size,
                     mcs51_ihex_info *info)
{
    memset(info, 0, sizeof *info);
    uint32_t base = 0;
    unsigned line = 0;
    size_t pos = 0;

    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n')
            end++;
        size_t stop = end;
        while (stop > pos && (text[stop - 1] == '\r' || text[stop - 1] == ' ' || text[stop - 1] == '\t'))
            stop--;
        line++;
        const char *rec = text + pos;
        size_t n = stop - pos;
        pos = end + 1;
        if (n == 0)
            continue;

        if (rec[0] != ':')
            return fail(info, line, "record does not start with ':'");
        if (n < 11 || (n - 1) % 2 != 0)
            return fail(info, line, "record too short or odd number of digits");

        uint8_t bytes[261];
        size_t count = (n - 1) / 2;
        if (count > sizeof bytes)
            return fail(info, line, "record too long");
        for (size_t i = 0; i < count; i++) {
            int hi = nibble(rec[1 + 2 * i]), lo = nibble(rec[2 + 2 * i]);
            if (hi < 0 || lo < 0)
                return fail(info, line, "invalid hexadecimal digit");
            bytes[i] = (uint8_t)(hi << 4 | lo);
        }
        unsigned data_len = bytes[0];
        if (count != data_len + 5u)
            return fail(info, line, "byte count does not match record length");
        uint8_t sum = 0;
        for (size_t i = 0; i < count; i++)
            sum = (uint8_t)(sum + bytes[i]);
        if (sum != 0)
            return fail(info, line, "checksum mismatch");

        uint32_t offset = (uint32_t)(bytes[1] << 8 | bytes[2]);
        const uint8_t *data = &bytes[4];
        switch (bytes[3]) {
        case 0x00:
            for (unsigned i = 0; i < data_len; i++) {
                uint32_t addr = base + ((offset + i) & 0xFFFFu);
                if (addr >= mem_size)
                    return fail(info, line, "data beyond the end of memory");
                mem[addr] = data[i];
                if (info->bytes == 0 || addr < info->lowest)
                    info->lowest = addr;
                if (info->bytes == 0 || addr > info->highest)
                    info->highest = addr;
                info->bytes++;
            }
            break;
        case 0x01:
            return 0;
        case 0x02:
        case 0x04:
            if (data_len != 2)
                return fail(info, line, "address record must hold 2 bytes");
            base = (uint32_t)(data[0] << 8 | data[1]);
            base = bytes[3] == 0x02 ? base << 4 : base << 16;
            break;
        case 0x03:
        case 0x05:
            break;
        default:
            return fail(info, line, "unknown record type");
        }
    }
    return fail(info, 0, "missing end-of-file record");
}

int mcs51_ihex_load_file(const char *path, uint8_t *mem, size_t mem_size,
                         mcs51_ihex_info *info)
{
    memset(info, 0, sizeof *info);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(info->error, sizeof info->error, "cannot open file");
        return -1;
    }
    size_t cap = 4096, len = 0;
    char *text = malloc(cap);
    bool ok = text != NULL;
    while (ok) {
        if (len == cap) {
            char *bigger = realloc(text, cap * 2);
            if (!bigger) {
                ok = false;
                break;
            }
            text = bigger;
            cap *= 2;
        }
        size_t got = fread(text + len, 1, cap - len, f);
        len += got;
        if (got == 0)
            break;
    }
    ok = ok && !ferror(f);
    fclose(f);
    if (!ok) {
        free(text);
        snprintf(info->error, sizeof info->error, "cannot read file");
        return -1;
    }
    int result = mcs51_ihex_parse(text, len, mem, mem_size, info);
    free(text);
    return result;
}
