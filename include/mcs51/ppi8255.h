/*
 * ppi8255.h - Intel 8255 programmable peripheral interface (parallel I/O).
 *
 * Registers (A1:A0): 0 = port A, 1 = port B, 2 = port C, 3 = control word.
 * Control word with bit 7 = 1 (mode set):
 *   bit 6-5 group A mode, bit 4 port A input, bit 3 port C upper input,
 *   bit 2 group B mode, bit 1 port B input, bit 0 port C lower input.
 * A mode set clears all output latches. Control word with bit 7 = 0 is a
 * bit set/reset command for port C: bits 3-1 select the bit, bit 0 = level.
 *
 * Only mode 0 (basic I/O) is modelled. Mode 1/2 words are accepted and set
 * the port directions, but the strobe/handshake lines on port C are not
 * emulated.
 */
#ifndef MCS51_PPI8255_H
#define MCS51_PPI8255_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MCS51_PPI_A = 0, MCS51_PPI_B = 1, MCS51_PPI_C = 2, MCS51_PPI_CONTROL = 3 };

typedef struct mcs51_ppi {
    uint8_t control;  /* last mode-set word */
    uint8_t latch[3]; /* output latches of ports A, B, C */
    uint8_t pins[3];  /* levels the outside world drives onto input bits */
    /* Optional: called when the level on output bits of a port may have
     * changed; value has the output bits from the latch, input bits as 1. */
    void (*port_out)(void *ctx, unsigned port, uint8_t value);
    void *ctx;
} mcs51_ppi;

/* State after the RESET pin: control word 9Bh (all ports input). */
void mcs51_ppi_reset(mcs51_ppi *ppi);

/* Bit mask of the port's bits that are configured as outputs. */
uint8_t mcs51_ppi_output_mask(const mcs51_ppi *ppi, unsigned port);

/* Level on the port pins: output bits from the latch, input bits from pins. */
uint8_t mcs51_ppi_port(const mcs51_ppi *ppi, unsigned port);

/* Register access with the signature of an xbus device (ctx = mcs51_ppi *). */
uint8_t mcs51_ppi_read(void *ctx, uint16_t reg);
void mcs51_ppi_write(void *ctx, uint16_t reg, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* MCS51_PPI8255_H */
