/*
 * xbus.h - external data bus (MOVX) with a configurable address decoder.
 *
 * Each device is selected the way a 74HC138/GAL decoder would select it: a set
 * of address lines (mask) must carry a fixed pattern (match). The device
 * itself only sees the lines in local_mask. Address lines that are neither
 * decoded nor wired to the device are "don't care", so the device appears at
 * several mirror addresses (partial decoding), exactly like on real hardware.
 */
#ifndef MCS51_XBUS_H
#define MCS51_XBUS_H

#include <stdbool.h>
#include <stdint.h>

#include "mcs51/mcs51.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCS51_XBUS_MAX_DEVICES 8u

typedef struct mcs51_xbus_device {
    const char *name;
    uint16_t mask;       /* address lines compared by the decoder */
    uint16_t match;      /* level those lines must have to select the device */
    uint16_t local_mask; /* address lines wired to the device */
    /* NULL read: write-only device, the bus floats (FFh) on a read.
     * NULL write: read-only device, writes are ignored. */
    uint8_t (*read)(void *ctx, uint16_t local);
    void (*write)(void *ctx, uint16_t local, uint8_t value);
    void *ctx;
} mcs51_xbus_device;

typedef enum mcs51_xbus_error {
    MCS51_XBUS_OK = 0,
    MCS51_XBUS_FULL,          /* no free decoder output */
    MCS51_XBUS_BAD_MATCH,     /* match has bits outside mask */
    MCS51_XBUS_LINE_CONFLICT, /* a local address line is also decoded */
    MCS51_XBUS_OVERLAP        /* two chip selects could be active at once */
} mcs51_xbus_error;

typedef struct mcs51_xbus {
    mcs51_xbus_device dev[MCS51_XBUS_MAX_DEVICES];
    unsigned count;
    /* Optional bus monitor, called after every access; dev is NULL when no
     * device was selected. */
    void (*monitor)(void *ctx, uint16_t addr, uint8_t value, bool write,
                    const mcs51_xbus_device *dev);
    void *monitor_ctx;
} mcs51_xbus;

void mcs51_xbus_init(mcs51_xbus *bus);

/* Add a device; rejects configurations that real decoding logic could not
 * implement without bus contention. */
mcs51_xbus_error mcs51_xbus_map(mcs51_xbus *bus, const mcs51_xbus_device *dev);

/* Device selected by an address, or NULL. */
const mcs51_xbus_device *mcs51_xbus_decode(const mcs51_xbus *bus, uint16_t addr);

/* First and last address of the decoded block, and the number of addresses
 * at which each device location is mirrored (1 = fully decoded). */
uint16_t mcs51_xbus_first(const mcs51_xbus_device *dev);
uint16_t mcs51_xbus_last(const mcs51_xbus_device *dev);
uint32_t mcs51_xbus_mirrors(const mcs51_xbus_device *dev);

uint8_t mcs51_xbus_read(mcs51_xbus *bus, uint16_t addr);
void mcs51_xbus_write(mcs51_xbus *bus, uint16_t addr, uint8_t value);

/* Route the CPU's MOVX accesses through this bus. */
void mcs51_xbus_attach(mcs51_xbus *bus, mcs51 *cpu);

const char *mcs51_xbus_error_name(mcs51_xbus_error error);

#ifdef __cplusplus
}
#endif

#endif /* MCS51_XBUS_H */
