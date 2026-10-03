/*
 * test_xbus.c - address decoder, 8255 PPI, output latch and the lab board's
 * memory map, accessed directly and through MOVX instructions.
 */
#include <string.h>

#include "mcs51/board.h"
#include "testkit.h"

static uint8_t dummy_read(void *ctx, uint16_t local)
{
    (void)ctx;
    return (uint8_t)local;
}

static void test_decoder_rejects_invalid_maps(void)
{
    mcs51_xbus bus;
    mcs51_xbus_init(&bus);
    mcs51_xbus_device ram = {"ram", 0x8000, 0x0000, 0x7FFF, dummy_read, NULL, NULL};
    CHECK_EQ(mcs51_xbus_map(&bus, &ram), MCS51_XBUS_OK);

    /* 4000h-7FFFh is already selected by the RAM */
    mcs51_xbus_device clash = {"clash", 0xC000, 0x4000, 0x00FF, dummy_read, NULL, NULL};
    CHECK_EQ(mcs51_xbus_map(&bus, &clash), MCS51_XBUS_OVERLAP);
    /* a fully decoded single address inside the RAM also overlaps */
    mcs51_xbus_device port = {"port", 0xFFFF, 0x1234, 0x0000, dummy_read, NULL, NULL};
    CHECK_EQ(mcs51_xbus_map(&bus, &port), MCS51_XBUS_OVERLAP);
    /* match bits on undecoded lines can never be compared */
    mcs51_xbus_device bad = {"bad", 0xC000, 0x8001, 0x0001, dummy_read, NULL, NULL};
    CHECK_EQ(mcs51_xbus_map(&bus, &bad), MCS51_XBUS_BAD_MATCH);
    /* A13 cannot both select the chip and address a register */
    mcs51_xbus_device lines = {"lines", 0xE000, 0x8000, 0x2003, dummy_read, NULL, NULL};
    CHECK_EQ(mcs51_xbus_map(&bus, &lines), MCS51_XBUS_LINE_CONFLICT);

    mcs51_xbus_device io = {"io", 0xC000, 0x8000, 0x00FF, dummy_read, NULL, NULL};
    CHECK_EQ(mcs51_xbus_map(&bus, &io), MCS51_XBUS_OK);
    for (unsigned i = 2; i < MCS51_XBUS_MAX_DEVICES; i++) {
        mcs51_xbus_device d = {"d", 0xFFFF, (uint16_t)(0xFF00 + i), 0, dummy_read, NULL, NULL};
        CHECK_EQ(mcs51_xbus_map(&bus, &d), MCS51_XBUS_OK);
    }
    mcs51_xbus_device more = {"more", 0xFFFF, 0xFFF0, 0, dummy_read, NULL, NULL};
    CHECK_EQ(mcs51_xbus_map(&bus, &more), MCS51_XBUS_FULL);
    CHECK(strcmp(mcs51_xbus_error_name(MCS51_XBUS_OVERLAP), "overlaps an existing device") == 0);

    /* the io device answers at all 64 mirrors of each of its 256 registers */
    CHECK_EQ(mcs51_xbus_read(&bus, 0x8042), 0x42);
    CHECK_EQ(mcs51_xbus_read(&bus, 0xBF42), 0x42);
    CHECK_EQ(mcs51_xbus_mirrors(&io), 64);
}

/* The overlap rule against brute force: two selects overlap iff some address
 * in the 64 KiB space selects both. Pseudo-random decoder pairs, with masks
 * thinned out so that both outcomes occur often. */
static void test_overlap_rule_matches_brute_force(void)
{
    uint32_t seed = 12345;
    unsigned overlaps = 0, disjoint = 0;
    for (unsigned n = 0; n < 400; n++) {
        uint16_t mask[2], match[2];
        for (unsigned k = 0; k < 2; k++) {
            seed = seed * 1103515245u + 12345u;
            mask[k] = (uint16_t)((seed >> 8) & (seed >> 16) & 0xFFFFu);
            seed = seed * 1103515245u + 12345u;
            match[k] = (uint16_t)((seed >> 12) & mask[k]);
        }
        bool expected = false;
        for (uint32_t addr = 0; addr <= 0xFFFF && !expected; addr++)
            expected = (addr & mask[0]) == match[0] && (addr & mask[1]) == match[1];
        mcs51_xbus bus;
        mcs51_xbus_init(&bus);
        mcs51_xbus_device a = {"a", mask[0], match[0], 0, dummy_read, NULL, NULL};
        mcs51_xbus_device b = {"b", mask[1], match[1], 0, dummy_read, NULL, NULL};
        CHECK_EQ(mcs51_xbus_map(&bus, &a), MCS51_XBUS_OK);
        CHECK_CASE(mcs51_xbus_map(&bus, &b) == (expected ? MCS51_XBUS_OVERLAP : MCS51_XBUS_OK),
                   "mask %04X match %04X vs mask %04X match %04X: overlap %d", mask[0], match[0],
                   mask[1], match[1], expected);
        if (expected)
            overlaps++;
        else
            disjoint++;
    }
    CHECK(overlaps > 50);
    CHECK(disjoint > 50);
}

static void test_board_memory_map(void)
{
    static mcs51_board board;
    CHECK(mcs51_board_init(&board));
    CHECK_EQ(board.bus.count, 3);

    const mcs51_xbus_device *ram = mcs51_xbus_decode(&board.bus, 0x7FFF);
    const mcs51_xbus_device *ppi = mcs51_xbus_decode(&board.bus, 0xC000);
    const mcs51_xbus_device *latch = mcs51_xbus_decode(&board.bus, 0xFFFF);
    CHECK(ram && ppi && latch);
    if (!ram || !ppi || !latch)
        return;
    CHECK_EQ(mcs51_xbus_first(ram), 0x0000);
    CHECK_EQ(mcs51_xbus_last(ram), 0x7FFF);
    CHECK_EQ(mcs51_xbus_mirrors(ram), 1);
    CHECK_EQ(mcs51_xbus_first(ppi), 0xC000);
    CHECK_EQ(mcs51_xbus_last(ppi), 0xDFFF);
    CHECK_EQ(mcs51_xbus_mirrors(ppi), 2048);
    CHECK_EQ(mcs51_xbus_first(latch), 0xE000);
    CHECK_EQ(mcs51_xbus_mirrors(latch), 8192);
    CHECK(mcs51_xbus_decode(&board.bus, 0x8000) == NULL);
    CHECK(mcs51_xbus_decode(&board.bus, 0xBFFF) == NULL);
    CHECK(mcs51_xbus_decode(&board.bus, 0xDFFF) == ppi);
    CHECK(mcs51_xbus_decode(&board.bus, 0xE123) == latch);

    /* gap: nothing drives the bus */
    mcs51_xbus_write(&board.bus, 0x9000, 0x12);
    CHECK_EQ(mcs51_xbus_read(&board.bus, 0x9000), 0xFF);
    /* the latch is write-only and sees every address in its block */
    mcs51_xbus_write(&board.bus, 0xF123, 0x5A);
    CHECK_EQ(board.latch, 0x5A);
    CHECK_EQ(board.latch_writes, 1);
    CHECK_EQ(mcs51_xbus_read(&board.bus, 0xE000), 0xFF);
    /* SRAM */
    mcs51_xbus_write(&board.bus, 0x7FFF, 0xA5);
    CHECK_EQ(board.xram[0x7FFF], 0xA5);
    CHECK_EQ(mcs51_xbus_read(&board.bus, 0x7FFF), 0xA5);
}

typedef struct ppi_log {
    unsigned calls;
    uint8_t last[3];
} ppi_log;

static void ppi_out(void *ctx, unsigned port, uint8_t value)
{
    ppi_log *log = ctx;
    log->calls++;
    log->last[port] = value;
}

static void test_ppi8255(void)
{
    static ppi_log log;
    mcs51_ppi ppi;
    mcs51_ppi_reset(&ppi);
    memset(&log, 0, sizeof log);
    ppi.port_out = ppi_out;
    ppi.ctx = &log;

    /* after reset all ports are inputs and read the pins */
    ppi.pins[MCS51_PPI_A] = 0x3C;
    CHECK_EQ(ppi.control, 0x9B);
    CHECK_EQ(mcs51_ppi_read(&ppi, MCS51_PPI_A), 0x3C);
    mcs51_ppi_write(&ppi, MCS51_PPI_A, 0x55); /* only loads the latch */
    CHECK_EQ(mcs51_ppi_read(&ppi, MCS51_PPI_A), 0x3C);
    CHECK_EQ(log.calls, 0);

    /* mode word 89h: A and B outputs, C (both halves) inputs; latches cleared */
    mcs51_ppi_write(&ppi, MCS51_PPI_CONTROL, 0x89);
    CHECK_EQ(mcs51_ppi_output_mask(&ppi, MCS51_PPI_A), 0xFF);
    CHECK_EQ(mcs51_ppi_output_mask(&ppi, MCS51_PPI_B), 0xFF);
    CHECK_EQ(mcs51_ppi_output_mask(&ppi, MCS51_PPI_C), 0x00);
    CHECK_EQ(mcs51_ppi_read(&ppi, MCS51_PPI_A), 0x00);
    CHECK_EQ(log.calls, 2);
    mcs51_ppi_write(&ppi, MCS51_PPI_A, 0xA5);
    CHECK_EQ(mcs51_ppi_read(&ppi, MCS51_PPI_A), 0xA5);
    CHECK_EQ(log.last[MCS51_PPI_A], 0xA5);
    ppi.pins[MCS51_PPI_C] = 0x81;
    CHECK_EQ(mcs51_ppi_read(&ppi, MCS51_PPI_C), 0x81);

    /* mode word 88h: C upper half input, C lower half output */
    mcs51_ppi_write(&ppi, MCS51_PPI_CONTROL, 0x88);
    CHECK_EQ(mcs51_ppi_output_mask(&ppi, MCS51_PPI_C), 0x0F);
    /* bit set/reset: 0000 011 1 sets PC3, 0000 000 1 sets PC0, 0000 011 0 clears PC3 */
    mcs51_ppi_write(&ppi, MCS51_PPI_CONTROL, 0x07);
    mcs51_ppi_write(&ppi, MCS51_PPI_CONTROL, 0x01);
    CHECK_EQ(ppi.latch[MCS51_PPI_C], 0x09);
    mcs51_ppi_write(&ppi, MCS51_PPI_CONTROL, 0x06);
    CHECK_EQ(ppi.latch[MCS51_PPI_C], 0x01);
    CHECK_EQ(ppi.control, 0x88); /* BSR does not change the mode */
    CHECK_EQ(mcs51_ppi_read(&ppi, MCS51_PPI_C), 0x81); /* upper from pins, lower from latch */
    CHECK_EQ(log.last[MCS51_PPI_C], 0xF1);             /* input bits reported as 1 */
    /* the control register cannot be read back */
    CHECK_EQ(mcs51_ppi_read(&ppi, MCS51_PPI_CONTROL), 0xFF);
}

/* A program on the board reaches the PPI through mirror addresses and uses
 * MOVX @R0 with the page in P2. */
static void test_movx_through_decoder(void)
{
    static const uint8_t code[] = {
        0x90, 0xDF, 0xFF, /* MOV DPTR,#0DFFFh   PPI control (A1:A0 = 11) */
        0x74, 0x80,       /* MOV A,#80h         mode 0, all outputs */
        0xF0,             /* MOVX @DPTR,A */
        0x90, 0xC0, 0x04, /* MOV DPTR,#0C004h   port A through a mirror */
        0x74, 0x3C,       /* MOV A,#3Ch */
        0xF0,             /* MOVX @DPTR,A */
        0x75, 0xA0, 0x12, /* MOV P2,#12h */
        0x78, 0x34,       /* MOV R0,#34h */
        0x74, 0x77,       /* MOV A,#77h */
        0xF2,             /* MOVX @R0,A         SRAM 1234h */
        0x75, 0xA0, 0xE7, /* MOV P2,#0E7h */
        0xF2,             /* MOVX @R0,A         latch (E734h) */
        0x80, 0xFE,       /* SJMP $ */
    };
    static mcs51_board board;
    CHECK(mcs51_board_init(&board));
    mcs51_load_code(&board.cpu, 0, code, sizeof code);
    CHECK_EQ(mcs51_run(&board.cpu, 1000), MCS51_HALT);
    CHECK_EQ(board.ppi.control, 0x80);
    CHECK_EQ(mcs51_ppi_port(&board.ppi, MCS51_PPI_A), 0x3C);
    CHECK_EQ(board.xram[0x1234], 0x77);
    CHECK_EQ(board.latch, 0x77);
    CHECK_EQ(MCS51_SFR(&board.cpu, MCS51_P0), 0xFF);
}

typedef struct access_log {
    unsigned reads, writes, unmapped;
} access_log;

static void monitor(void *ctx, uint16_t addr, uint8_t value, bool write, const mcs51_xbus_device *dev)
{
    access_log *log = ctx;
    (void)addr;
    (void)value;
    if (write)
        log->writes++;
    else
        log->reads++;
    if (!dev)
        log->unmapped++;
}

static void test_bus_monitor(void)
{
    static mcs51_board board;
    static access_log log;
    memset(&log, 0, sizeof log);
    CHECK(mcs51_board_init(&board));
    board.bus.monitor = monitor;
    board.bus.monitor_ctx = &log;
    mcs51_xbus_write(&board.bus, 0x0000, 1);
    mcs51_xbus_read(&board.bus, 0x0000);
    mcs51_xbus_read(&board.bus, 0xA000);
    CHECK_EQ(log.writes, 1);
    CHECK_EQ(log.reads, 2);
    CHECK_EQ(log.unmapped, 1);
}

int main(void)
{
    TK_RUN(test_decoder_rejects_invalid_maps);
    TK_RUN(test_overlap_rule_matches_brute_force);
    TK_RUN(test_board_memory_map);
    TK_RUN(test_ppi8255);
    TK_RUN(test_movx_through_decoder);
    TK_RUN(test_bus_monitor);
    return tk_summary();
}
