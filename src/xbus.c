/*
 * xbus.c - external data bus and address decoder.
 */
#include "mcs51/xbus.h"

#include <string.h>

void mcs51_xbus_init(mcs51_xbus *bus)
{
    memset(bus, 0, sizeof *bus);
}

mcs51_xbus_error mcs51_xbus_map(mcs51_xbus *bus, const mcs51_xbus_device *dev)
{
    if (bus->count >= MCS51_XBUS_MAX_DEVICES)
        return MCS51_XBUS_FULL;
    if (dev->match & ~dev->mask)
        return MCS51_XBUS_BAD_MATCH;
    if (dev->local_mask & dev->mask)
        return MCS51_XBUS_LINE_CONFLICT;
    /* Two selects overlap iff some address satisfies both: on the lines both
     * decoders look at, the required levels must agree. */
    for (unsigned i = 0; i < bus->count; i++) {
        const mcs51_xbus_device *other = &bus->dev[i];
        if (((other->match ^ dev->match) & other->mask & dev->mask) == 0)
            return MCS51_XBUS_OVERLAP;
    }
    bus->dev[bus->count++] = *dev;
    return MCS51_XBUS_OK;
}

const mcs51_xbus_device *mcs51_xbus_decode(const mcs51_xbus *bus, uint16_t addr)
{
    for (unsigned i = 0; i < bus->count; i++) {
        if ((addr & bus->dev[i].mask) == bus->dev[i].match)
            return &bus->dev[i];
    }
    return NULL;
}

uint16_t mcs51_xbus_first(const mcs51_xbus_device *dev)
{
    return dev->match;
}

uint16_t mcs51_xbus_last(const mcs51_xbus_device *dev)
{
    return (uint16_t)(dev->match | (uint16_t)~dev->mask);
}

uint32_t mcs51_xbus_mirrors(const mcs51_xbus_device *dev)
{
    uint16_t dont_care = (uint16_t)~(dev->mask | dev->local_mask);
    uint32_t mirrors = 1;
    for (; dont_care; dont_care &= (uint16_t)(dont_care - 1))
        mirrors *= 2;
    return mirrors;
}

uint8_t mcs51_xbus_read(mcs51_xbus *bus, uint16_t addr)
{
    const mcs51_xbus_device *dev = mcs51_xbus_decode(bus, addr);
    uint8_t value = (dev && dev->read) ? dev->read(dev->ctx, addr & dev->local_mask) : 0xFF;
    if (bus->monitor)
        bus->monitor(bus->monitor_ctx, addr, value, false, dev);
    return value;
}

void mcs51_xbus_write(mcs51_xbus *bus, uint16_t addr, uint8_t value)
{
    const mcs51_xbus_device *dev = mcs51_xbus_decode(bus, addr);
    if (dev && dev->write)
        dev->write(dev->ctx, addr & dev->local_mask, value);
    if (bus->monitor)
        bus->monitor(bus->monitor_ctx, addr, value, true, dev);
}

static uint8_t bus_read(void *ctx, uint16_t addr)
{
    return mcs51_xbus_read(ctx, addr);
}

static void bus_write(void *ctx, uint16_t addr, uint8_t value)
{
    mcs51_xbus_write(ctx, addr, value);
}

void mcs51_xbus_attach(mcs51_xbus *bus, mcs51 *cpu)
{
    cpu->io.xdata_read = bus_read;
    cpu->io.xdata_write = bus_write;
    cpu->io.xdata_ctx = bus;
}

const char *mcs51_xbus_error_name(mcs51_xbus_error error)
{
    switch (error) {
    case MCS51_XBUS_OK:
        return "ok";
    case MCS51_XBUS_FULL:
        return "decoder full";
    case MCS51_XBUS_BAD_MATCH:
        return "match pattern uses undecoded lines";
    case MCS51_XBUS_LINE_CONFLICT:
        return "local address line is also decoded";
    case MCS51_XBUS_OVERLAP:
        return "overlaps an existing device";
    }
    return "unknown";
}
