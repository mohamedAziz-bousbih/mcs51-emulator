/*
 * periph.c - on-chip peripherals of the 8051, clocked once per machine cycle:
 * Timer/Counter 0 and 1, the serial port and the external interrupt inputs.
 *
 * Order of events inside one machine cycle (mcs51__tick):
 *   1. timers count (counter inputs use the edge detected in the previous
 *      cycle's pin sample, as on the real chip);
 *   2. the serial port is clocked (timer 1 overflow, fixed mode 2 clock or the
 *      mode 0 shift clock);
 *   3. the port 3 pins are sampled (S5P2): INT0/INT1 set IE0/IE1, falling
 *      edges on T0/T1 are remembered for the next cycle;
 *   4. the interrupt request flags are sampled for the next poll.
 */
#include "core.h"

/* ------------------------------------------------------------------ ports */

uint8_t mcs51_port_pins(mcs51 *cpu, unsigned port)
{
    uint8_t level = cpu->io.port_in ? cpu->io.port_in(cpu->io.port_ctx, port)
                                    : cpu->pins_in[port & 3];
    return (uint8_t)(SFR(MCS51_P0 + 0x10 * (port & 3)) & level);
}

/* ------------------------------------------------------------ serial port */

static unsigned serial_mode(const mcs51 *cpu)
{
    return SFR(MCS51_SCON) >> 6;
}

/* Frame length in bit times for the asynchronous modes: start, 8 data bits,
 * (TB8/RB8 in modes 2 and 3), stop. */
static int frame_bits(unsigned mode)
{
    return mode == 1 ? 10 : 11;
}

void mcs51__serial_write_sbuf(mcs51 *cpu, uint8_t value)
{
    unsigned tb8 = (SFR(MCS51_SCON) & MCS51_SCON_TB8) ? 1u : 0u;
    cpu->serial.tx_next = (uint16_t)(value | (tb8 << 8));
    cpu->serial.tx_request = true;
}

bool mcs51__serial_tx_busy(const mcs51 *cpu)
{
    return cpu->serial.tx_request || cpu->serial.tx_bit >= 0;
}

bool mcs51_serial_inject(mcs51 *cpu, uint16_t data)
{
    if (cpu->serial.q_count >= MCS51_SERIAL_QUEUE)
        return false;
    unsigned tail = (cpu->serial.q_head + cpu->serial.q_count) % MCS51_SERIAL_QUEUE;
    cpu->serial.queue[tail] = data & 0x1FF;
    cpu->serial.q_count++;
    return true;
}

static bool rx_dequeue(mcs51 *cpu, uint16_t *frame)
{
    if (cpu->serial.q_count == 0)
        return false;
    *frame = cpu->serial.queue[cpu->serial.q_head];
    cpu->serial.q_head = (uint16_t)((cpu->serial.q_head + 1) % MCS51_SERIAL_QUEUE);
    cpu->serial.q_count--;
    return true;
}

static void tx_complete(mcs51 *cpu)
{
    SFR(MCS51_SCON) |= MCS51_SCON_TI;
    if (cpu->io.serial_tx)
        cpu->io.serial_tx(cpu->io.serial_ctx, cpu->serial.tx_frame);
}

/* Receiver load condition of modes 1-3: RI must be clear, and with SM2 set
 * only frames whose ninth bit (mode 1: the stop bit) is 1 are accepted. */
static void rx_complete(mcs51 *cpu)
{
    uint8_t scon = SFR(MCS51_SCON);
    bool ninth = serial_mode(cpu) == 1 ? true : (cpu->serial.rx_frame & 0x100) != 0;
    if (scon & MCS51_SCON_RI) {
        cpu->serial.overruns++;
        return;
    }
    if ((scon & MCS51_SCON_SM2) && !ninth)
        return;
    cpu->serial.rx_sbuf = (uint8_t)cpu->serial.rx_frame;
    scon = ninth ? (uint8_t)(scon | MCS51_SCON_RB8) : (uint8_t)(scon & ~MCS51_SCON_RB8);
    SFR(MCS51_SCON) = (uint8_t)(scon | MCS51_SCON_RI);
}

/* One tick of the 16x baud clock in modes 1-3. The transmitter shifts on the
 * rollovers of a free-running divide-by-16 counter, so a frame starts at the
 * first rollover after the SBUF write and TI rises at the start of the stop
 * bit. The receiver restarts its own divide-by-16 at the start-bit edge and
 * takes the frame at the middle of the stop bit (bit samples 7, 8, 9). */
static void serial_clock16(mcs51 *cpu)
{
    int bits = frame_bits(serial_mode(cpu));

    cpu->serial.div16 = (uint8_t)((cpu->serial.div16 + 1) & 15);
    if (cpu->serial.div16 == 0) {
        if (cpu->serial.tx_bit >= 0) {
            cpu->serial.tx_bit++;
            if (cpu->serial.tx_bit == bits - 1)
                tx_complete(cpu);
            else if (cpu->serial.tx_bit >= bits)
                cpu->serial.tx_bit = -1;
        }
        if (cpu->serial.tx_bit < 0 && cpu->serial.tx_request) {
            cpu->serial.tx_request = false;
            cpu->serial.tx_frame = cpu->serial.tx_next;
            cpu->serial.tx_bit = 0; /* start bit */
        }
    }

    if (cpu->serial.rx_tick < 0) {
        if ((SFR(MCS51_SCON) & MCS51_SCON_REN) && rx_dequeue(cpu, &cpu->serial.rx_frame))
            cpu->serial.rx_tick = 0;
        return;
    }
    cpu->serial.rx_tick++;
    if (cpu->serial.rx_tick == 16 * (bits - 1) + 9)
        rx_complete(cpu);
    else if (cpu->serial.rx_tick >= 16 * bits)
        cpu->serial.rx_tick = -1;
}

/* Mode 0: synchronous shift register clocked at fosc/12, one bit per machine
 * cycle. Transmission starts in the cycle after the SBUF write, reception
 * whenever REN = 1 and RI = 0; either sets its flag after eight bit times. */
static void serial_mode0_cycle(mcs51 *cpu)
{
    if (cpu->serial.tx_bit >= 0) {
        if (++cpu->serial.tx_bit == 8) {
            cpu->serial.tx_bit = -1;
            tx_complete(cpu);
        }
    } else if (cpu->serial.tx_request) {
        cpu->serial.tx_request = false;
        cpu->serial.tx_frame = cpu->serial.tx_next & 0xFF;
        cpu->serial.tx_bit = 0;
    }

    if (cpu->serial.rx_tick >= 0) {
        if (++cpu->serial.rx_tick == 8) {
            cpu->serial.rx_tick = -1;
            cpu->serial.rx_sbuf = (uint8_t)cpu->serial.rx_frame;
            SFR(MCS51_SCON) |= MCS51_SCON_RI;
        }
    } else if ((SFR(MCS51_SCON) & (MCS51_SCON_REN | MCS51_SCON_RI)) == MCS51_SCON_REN) {
        /* With nothing queued the RXD line stays high and the port shifts in FFh. */
        uint16_t frame;
        cpu->serial.rx_frame = rx_dequeue(cpu, &frame) ? (uint16_t)(frame & 0xFF) : 0xFF;
        cpu->serial.rx_tick = 0;
    }
}

static void serial_timer1_overflow(mcs51 *cpu)
{
    /* SMOD = 0 inserts a divide-by-2 between timer 1 and the 16x baud clock. */
    if (!(SFR(MCS51_PCON) & MCS51_PCON_SMOD)) {
        cpu->serial.t1_half = !cpu->serial.t1_half;
        if (cpu->serial.t1_half)
            return;
    }
    unsigned mode = serial_mode(cpu);
    if (mode == 1 || mode == 3)
        serial_clock16(cpu);
}

/* ------------------------------------------------------------------ timers */

/* Count once in mode 0, 1 or 2; returns true on overflow. */
static bool timer_count(uint8_t *tl, uint8_t *th, unsigned mode)
{
    switch (mode) {
    case 0: {
        /* 13 bits: TH (8) and TL[4:0] (5). TL[7:5] are not part of the counter. */
        uint8_t low = (uint8_t)((*tl + 1) & 0x1F);
        *tl = (uint8_t)((*tl & 0xE0) | low);
        return low == 0 && ++*th == 0;
    }
    case 1:
        return ++*tl == 0 && ++*th == 0;
    case 2:
        if (++*tl != 0)
            return false;
        *tl = *th; /* auto-reload */
        return true;
    default:
        return false;
    }
}

static void timers_tick(mcs51 *cpu)
{
    uint8_t tmod = SFR(MCS51_TMOD);
    uint8_t tcon = SFR(MCS51_TCON);
    unsigned mode0 = tmod & 3u, mode1 = (tmod >> 4) & 3u;
    bool int0 = (cpu->p3_sample & MCS51_P3_INT0) != 0;
    bool int1 = (cpu->p3_sample & MCS51_P3_INT1) != 0;

    /* Control = TRx AND (NOT GATE OR INTx). The counter input is either the
     * machine cycle (timer) or a falling edge on the Tx pin (counter). */
    bool run0 = (tcon & MCS51_TCON_TR0) && (!(tmod & MCS51_TMOD_GATE) || int0);
    if (run0 && (!(tmod & MCS51_TMOD_CT) || (cpu->count_edge & 1u))) {
        bool overflow = mode0 == 3 ? ++SFR(MCS51_TL0) == 0
                                   : timer_count(&SFR(MCS51_TL0), &SFR(MCS51_TH0), mode0);
        if (overflow)
            tcon |= MCS51_TCON_TF0;
    }
    /* Timer 0 mode 3: TH0 is an 8-bit timer that borrows TR1 and TF1. */
    if (mode0 == 3 && (tcon & MCS51_TCON_TR1) && ++SFR(MCS51_TH0) == 0)
        tcon |= MCS51_TCON_TF1;

    /* Timer 1 holds its count in mode 3. While timer 0 is in mode 3 it keeps
     * running (typically as baud rate generator) but cannot set TF1. */
    bool run1 = mode1 != 3 && (tcon & MCS51_TCON_TR1) && (!((tmod >> 4) & MCS51_TMOD_GATE) || int1);
    if (run1 && (!((tmod >> 4) & MCS51_TMOD_CT) || (cpu->count_edge & 2u))) {
        if (timer_count(&SFR(MCS51_TL1), &SFR(MCS51_TH1), mode1)) {
            if (mode0 != 3)
                tcon |= MCS51_TCON_TF1;
            serial_timer1_overflow(cpu);
        }
    }
    SFR(MCS51_TCON) = tcon;
}

/* ------------------------------------------------ pins and interrupt sample */

static void sample_pins(mcs51 *cpu)
{
    uint8_t now = mcs51_port_pins(cpu, 3);
    uint8_t fell = (uint8_t)(cpu->p3_sample & ~now);
    uint8_t tcon = SFR(MCS51_TCON);

    /* IT = 1: edge triggered, the flag is latched on a high-to-low transition
     * between two samples. IT = 0: level triggered, the flag follows the pin. */
    if (tcon & MCS51_TCON_IT0) {
        if (fell & MCS51_P3_INT0)
            tcon |= MCS51_TCON_IE0;
    } else {
        tcon = (now & MCS51_P3_INT0) ? (uint8_t)(tcon & ~MCS51_TCON_IE0)
                                     : (uint8_t)(tcon | MCS51_TCON_IE0);
    }
    if (tcon & MCS51_TCON_IT1) {
        if (fell & MCS51_P3_INT1)
            tcon |= MCS51_TCON_IE1;
    } else {
        tcon = (now & MCS51_P3_INT1) ? (uint8_t)(tcon & ~MCS51_TCON_IE1)
                                     : (uint8_t)(tcon | MCS51_TCON_IE1);
    }
    SFR(MCS51_TCON) = tcon;

    cpu->count_edge = (uint8_t)(((fell & MCS51_P3_T0) ? 1u : 0u) | ((fell & MCS51_P3_T1) ? 2u : 0u));
    cpu->p3_sample = now;
}

uint8_t mcs51__irq_requests(const mcs51 *cpu)
{
    uint8_t tcon = SFR(MCS51_TCON);
    uint8_t req = 0;
    if (tcon & MCS51_TCON_IE0)
        req |= 1u << MCS51_IRQ_EX0;
    if (tcon & MCS51_TCON_TF0)
        req |= 1u << MCS51_IRQ_T0;
    if (tcon & MCS51_TCON_IE1)
        req |= 1u << MCS51_IRQ_EX1;
    if (tcon & MCS51_TCON_TF1)
        req |= 1u << MCS51_IRQ_T1;
    if (SFR(MCS51_SCON) & (MCS51_SCON_RI | MCS51_SCON_TI))
        req |= 1u << MCS51_IRQ_SERIAL;
    return req;
}

void mcs51__tick(mcs51 *cpu)
{
    cpu->cycles++;
    timers_tick(cpu);

    unsigned mode = serial_mode(cpu);
    if (mode == 0) {
        serial_mode0_cycle(cpu);
    } else if (mode == 2) {
        /* Mode 2 baud rate is fosc/64 (SMOD = 0) or fosc/32 (SMOD = 1): the
         * 16x clock ticks 3 or 6 times per machine cycle. */
        unsigned ticks = (SFR(MCS51_PCON) & MCS51_PCON_SMOD) ? 6u : 3u;
        for (unsigned i = 0; i < ticks; i++)
            serial_clock16(cpu);
    }

    sample_pins(cpu);
    cpu->irq.sample = mcs51__irq_requests(cpu);
}
