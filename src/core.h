/*
 * core.h - internal interface between the instruction core (cpu.c) and the
 * on-chip peripherals (periph.c). Not part of the public API.
 */
#ifndef MCS51_CORE_H
#define MCS51_CORE_H

#include "mcs51/mcs51.h"

#define SFR(addr) MCS51_SFR(cpu, addr)

/* Advance timers, serial port and interrupt inputs by one machine cycle and
 * take the end-of-cycle interrupt sample. */
void mcs51__tick(mcs51 *cpu);

/* Interrupt request flags currently set, one bit per enum mcs51_irq source. */
uint8_t mcs51__irq_requests(const mcs51 *cpu);

/* Software wrote SBUF: queue a transmit frame (TB8 is latched now). */
void mcs51__serial_write_sbuf(mcs51 *cpu, uint8_t value);

/* True while a written frame has not been completely shifted out. */
bool mcs51__serial_tx_busy(const mcs51 *cpu);

#endif /* MCS51_CORE_H */
