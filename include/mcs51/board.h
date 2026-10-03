/*
 * board.h - an 8051 lab board: CPU, 32 KiB external SRAM, an 8255 parallel
 * interface and a write-only output latch, connected through the address
 * decoder in xbus.h. See docs/memory-map.md for the decoding logic.
 *
 *   0000h-7FFFh  62256 SRAM, 32 KiB      /CE  = A15
 *   8000h-BFFFh  not decoded             reads FFh (pull-ups on P0)
 *   C000h-DFFFh  8255 PPI                /CS  = 74HC138 Y6 (A15 A14 A13 = 110),
 *                                        register select A1:A0, mirrored every 4 bytes
 *   E000h-FFFFh  74HC574 output latch    CLK  = 74HC138 Y7 (A15 A14 A13 = 111) and /WR,
 *                                        write-only, mirrored at every address
 */
#ifndef MCS51_BOARD_H
#define MCS51_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "mcs51/mcs51.h"
#include "mcs51/ppi8255.h"
#include "mcs51/xbus.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCS51_BOARD_XRAM_SIZE 0x8000u
#define MCS51_BOARD_PPI_BASE 0xC000u
#define MCS51_BOARD_LATCH_BASE 0xE000u

typedef struct mcs51_board {
    mcs51 cpu;
    mcs51_xbus bus;
    mcs51_ppi ppi;
    uint8_t xram[MCS51_BOARD_XRAM_SIZE];
    uint8_t latch;          /* last value clocked into the 74HC574 */
    uint32_t latch_writes;  /* number of writes to the latch */
} mcs51_board;

/* Power-on state of the whole board (CPU via mcs51_init, SRAM cleared, PPI
 * reset) with the memory map above. Install further CPU callbacks
 * (ports, serial) afterwards. Returns false if the memory map is rejected
 * by the decoder, which would be a bug in the board definition. */
bool mcs51_board_init(mcs51_board *board);

#ifdef __cplusplus
}
#endif

#endif /* MCS51_BOARD_H */
