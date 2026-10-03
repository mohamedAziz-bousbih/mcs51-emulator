/*
 * test_ihex.c - Intel HEX parsing and error reporting.
 */
#include <string.h>

#include "mcs51/ihex.h"
#include "testkit.h"

static uint8_t mem[0x10000];

static int parse(const char *text, mcs51_ihex_info *info)
{
    memset(mem, 0xFF, sizeof mem);
    return mcs51_ihex_parse(text, strlen(text), mem, sizeof mem, info);
}

static void test_valid_file(void)
{
    mcs51_ihex_info info;
    const char *text = ":03000000020030CB\r\n"
                       "\r\n"
                       ":0400300075815f2255\r\n" /* lower-case digits */
                       ":0400000300000000F9\r\n" /* start address: ignored */
                       ":00000001FF\r\n"
                       "anything after the end record is ignored\n";
    CHECK_EQ(parse(text, &info), 0);
    CHECK_EQ(info.bytes, 7);
    CHECK_EQ(info.lowest, 0x0000);
    CHECK_EQ(info.highest, 0x0033);
    CHECK_EQ(mem[0x0000], 0x02);
    CHECK_EQ(mem[0x0002], 0x30);
    CHECK_EQ(mem[0x0030], 0x75);
    CHECK_EQ(mem[0x0033], 0x22);
    CHECK_EQ(mem[0x0034], 0xFF);
    CHECK_EQ(info.error[0], '\0');
}

static void test_extended_addresses(void)
{
    mcs51_ihex_info info;
    /* linear base 0 is fine for a 64 KiB program memory */
    CHECK_EQ(parse(":020000040000FA\n:01001000AA45\n:00000001FF\n", &info), 0);
    CHECK_EQ(mem[0x0010], 0xAA);
    /* segment base 0100h * 16 = 1000h */
    CHECK_EQ(parse(":020000020100FB\n:01001000AA45\n:00000001FF\n", &info), 0);
    CHECK_EQ(mem[0x1010], 0xAA);
    /* linear base 10000h is beyond the memory */
    CHECK_EQ(parse(":020000040001F9\n:01001000AA45\n:00000001FF\n", &info), -1);
    CHECK_EQ(info.line, 2);
    CHECK(strstr(info.error, "beyond") != NULL);
    /* offsets wrap inside the 64 KiB segment (I16HEX) */
    CHECK_EQ(parse(":02FFFF001122CD\n:00000001FF\n", &info), 0);
    CHECK_EQ(mem[0xFFFF], 0x11);
    CHECK_EQ(mem[0x0000], 0x22);
}

static void test_errors(void)
{
    static const struct {
        const char *text;
        unsigned line;
        const char *message;
    } cases[] = {
        {":03000000020030CC\n:00000001FF\n", 1, "checksum"},
        {":03000000020030CB\n03000000020030CB\n", 2, "':'"},
        {":0300000002G030CB\n", 1, "hexadecimal"},
        {":04000000020030CA\n", 1, "byte count"},
        {":0000\n", 1, "too short"},
        {":00000006FA\n", 1, "record type"},
        {":0100000401FA\n", 1, "2 bytes"},
        {":03000000020030CB\n", 0, "end-of-file"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        mcs51_ihex_info info;
        CHECK_EQ(parse(cases[i].text, &info), -1);
        CHECK_EQ(info.line, cases[i].line);
        if (!strstr(info.error, cases[i].message)) {
            fprintf(stderr, "case %u: message '%s' lacks '%s'\n", i, info.error, cases[i].message);
            tk_failures++;
        }
    }
}

static void test_load_file(void)
{
    mcs51_ihex_info info;
    memset(mem, 0xFF, sizeof mem);
    CHECK_EQ(mcs51_ihex_load_file(tk_path("examples/delay.hex"), mem, sizeof mem, &info), 0);
    CHECK(info.bytes > 0);
    CHECK_EQ(mem[0], 0x7D); /* MOV R5,#10 */
    CHECK_EQ(mem[1], 10);
    CHECK_EQ(mcs51_ihex_load_file(tk_path("examples/no-such-file.hex"), mem, sizeof mem, &info), -1);
    CHECK(strstr(info.error, "open") != NULL);
}

int main(void)
{
    TK_RUN(test_valid_file);
    TK_RUN(test_extended_addresses);
    TK_RUN(test_errors);
    TK_RUN(test_load_file);
    return tk_summary();
}
