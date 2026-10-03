/*
 * board.c - lab board: CPU, SRAM, 8255 and output latch on the MOVX bus.
 */
#include "mcs51/board.h"

#include <string.h>

static uint8_t sram_read(void *ctx, uint16_t local)
{
    const mcs51_board *board = ctx;
    return board->xram[local];
}

static void sram_write(void *ctx, uint16_t local, uint8_t value)
{
    mcs51_board *board = ctx;
    board->xram[local] = value;
}

static void latch_write(void *ctx, uint16_t local, uint8_t value)
{
    mcs51_board *board = ctx;
    (void)local; /* no address lines reach the latch */
    board->latch = value;
    board->latch_writes++;
}

bool mcs51_board_init(mcs51_board *board)
{
    mcs51_init(&board->cpu);
    memset(board->xram, 0, sizeof board->xram);
    mcs51_ppi_reset(&board->ppi);
    board->ppi.port_out = NULL;
    board->ppi.ctx = NULL;
    board->latch = 0;
    board->latch_writes = 0;

    const mcs51_xbus_device map[] = {
        {"SRAM 62256", 0x8000, 0x0000, 0x7FFF, sram_read, sram_write, board},
        {"PPI 8255", 0xE000, 0xC000, 0x0003, mcs51_ppi_read, mcs51_ppi_write, &board->ppi},
        {"Latch 74HC574", 0xE000, 0xE000, 0x0000, NULL, latch_write, board},
    };
    mcs51_xbus_init(&board->bus);
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (mcs51_xbus_map(&board->bus, &map[i]) != MCS51_XBUS_OK)
            return false;
    }
    mcs51_xbus_attach(&board->bus, &board->cpu);
    return true;
}
