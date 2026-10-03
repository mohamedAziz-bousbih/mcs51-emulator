/*
 * ppi8255.c - Intel 8255 PPI, mode 0 and port C bit set/reset.
 */
#include "mcs51/ppi8255.h"

#include <string.h>

void mcs51_ppi_reset(mcs51_ppi *ppi)
{
    ppi->control = 0x9B;
    memset(ppi->latch, 0, sizeof ppi->latch);
    memset(ppi->pins, 0xFF, sizeof ppi->pins);
}

uint8_t mcs51_ppi_output_mask(const mcs51_ppi *ppi, unsigned port)
{
    uint8_t cw = ppi->control;
    switch (port) {
    case MCS51_PPI_A:
        return (cw & 0x10) ? 0x00 : 0xFF;
    case MCS51_PPI_B:
        return (cw & 0x02) ? 0x00 : 0xFF;
    case MCS51_PPI_C:
        return (uint8_t)(((cw & 0x08) ? 0x00 : 0xF0) | ((cw & 0x01) ? 0x00 : 0x0F));
    default:
        return 0x00;
    }
}

uint8_t mcs51_ppi_port(const mcs51_ppi *ppi, unsigned port)
{
    uint8_t out = mcs51_ppi_output_mask(ppi, port);
    return (uint8_t)((ppi->latch[port] & out) | (ppi->pins[port] & ~out));
}

static void notify(mcs51_ppi *ppi, unsigned port)
{
    if (ppi->port_out && mcs51_ppi_output_mask(ppi, port))
        ppi->port_out(ppi->ctx, port,
                      (uint8_t)(ppi->latch[port] | ~mcs51_ppi_output_mask(ppi, port)));
}

uint8_t mcs51_ppi_read(void *ctx, uint16_t reg)
{
    const mcs51_ppi *ppi = ctx;
    reg &= 3;
    /* The control register is write-only: nothing drives the data bus. */
    return reg == MCS51_PPI_CONTROL ? 0xFF : mcs51_ppi_port(ppi, reg);
}

void mcs51_ppi_write(void *ctx, uint16_t reg, uint8_t value)
{
    mcs51_ppi *ppi = ctx;
    reg &= 3;
    if (reg != MCS51_PPI_CONTROL) {
        /* A write to an input port only loads the (undriven) output latch. */
        ppi->latch[reg] = value;
        notify(ppi, reg);
        return;
    }
    if (value & 0x80) {
        ppi->control = value;
        memset(ppi->latch, 0, sizeof ppi->latch);
        for (unsigned port = 0; port < 3; port++)
            notify(ppi, port);
    } else {
        uint8_t mask = (uint8_t)(1u << ((value >> 1) & 7u));
        ppi->latch[MCS51_PPI_C] = (value & 1) ? (uint8_t)(ppi->latch[MCS51_PPI_C] | mask)
                                              : (uint8_t)(ppi->latch[MCS51_PPI_C] & ~mask);
        notify(ppi, MCS51_PPI_C);
    }
}
