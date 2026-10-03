/*
 * mcs51.h - cycle-counting Intel 8051 (MCS-51) CPU core.
 *
 * Timing model (details in docs/timing-model.md):
 *   - Time advances in machine cycles (1 machine cycle = 12 oscillator periods).
 *   - An instruction's architectural effect is applied at the start of its first
 *     machine cycle; timers, the serial port and the interrupt inputs are then
 *     clocked once per machine cycle for the instruction's documented cycle count.
 *   - Interrupt request flags are sampled at the end of every machine cycle and
 *     polled in the last cycle of each instruction, using the sample taken one
 *     cycle earlier (Intel's S5P2 rule). A request that appears during the final
 *     cycle of an instruction is therefore serviced one instruction later.
 *
 * The core is an 8051 with the 256-byte internal RAM of the 8052 (80h-FFh is
 * reachable through @R0/@R1 and the stack). Timer 2 is not modelled.
 */
#ifndef MCS51_MCS51_H
#define MCS51_MCS51_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCS51_CODE_SIZE 0x10000u
#define MCS51_CLOCKS_PER_CYCLE 12u
#define MCS51_SERIAL_QUEUE 256u

/* Special function register addresses (direct addressing 80h-FFh). */
enum {
    MCS51_P0 = 0x80, MCS51_SP = 0x81, MCS51_DPL = 0x82, MCS51_DPH = 0x83,
    MCS51_PCON = 0x87, MCS51_TCON = 0x88, MCS51_TMOD = 0x89, MCS51_TL0 = 0x8A,
    MCS51_TL1 = 0x8B, MCS51_TH0 = 0x8C, MCS51_TH1 = 0x8D, MCS51_P1 = 0x90,
    MCS51_SCON = 0x98, MCS51_SBUF = 0x99, MCS51_P2 = 0xA0, MCS51_IE = 0xA8,
    MCS51_P3 = 0xB0, MCS51_IP = 0xB8, MCS51_PSW = 0xD0, MCS51_ACC = 0xE0,
    MCS51_B = 0xF0
};

/* Raw storage of an SFR inside struct mcs51: no side effects and no pin logic.
 * Port entries hold the output latches. SBUF has no storage here: the receive
 * half lives in cpu->serial.rx_sbuf, the transmit half is write-only. */
#define MCS51_SFR(cpu, addr) ((cpu)->sfr[(addr) - 0x80])

/* PSW */
#define MCS51_PSW_CY 0x80u
#define MCS51_PSW_AC 0x40u
#define MCS51_PSW_F0 0x20u
#define MCS51_PSW_RS1 0x10u
#define MCS51_PSW_RS0 0x08u
#define MCS51_PSW_OV 0x04u
#define MCS51_PSW_P 0x01u
/* TCON */
#define MCS51_TCON_TF1 0x80u
#define MCS51_TCON_TR1 0x40u
#define MCS51_TCON_TF0 0x20u
#define MCS51_TCON_TR0 0x10u
#define MCS51_TCON_IE1 0x08u
#define MCS51_TCON_IT1 0x04u
#define MCS51_TCON_IE0 0x02u
#define MCS51_TCON_IT0 0x01u
/* TMOD (timer 0 in the low nibble, timer 1 in the high nibble) */
#define MCS51_TMOD_GATE 0x08u
#define MCS51_TMOD_CT 0x04u
/* SCON */
#define MCS51_SCON_SM0 0x80u
#define MCS51_SCON_SM1 0x40u
#define MCS51_SCON_SM2 0x20u
#define MCS51_SCON_REN 0x10u
#define MCS51_SCON_TB8 0x08u
#define MCS51_SCON_RB8 0x04u
#define MCS51_SCON_TI 0x02u
#define MCS51_SCON_RI 0x01u
/* IE / IP (IP uses the same bit positions for PX0..PS) */
#define MCS51_IE_EA 0x80u
#define MCS51_IE_ES 0x10u
#define MCS51_IE_ET1 0x08u
#define MCS51_IE_EX1 0x04u
#define MCS51_IE_ET0 0x02u
#define MCS51_IE_EX0 0x01u
/* PCON */
#define MCS51_PCON_SMOD 0x80u
#define MCS51_PCON_PD 0x02u
#define MCS51_PCON_IDL 0x01u
/* Alternate functions of port 3 */
#define MCS51_P3_RXD 0x01u
#define MCS51_P3_TXD 0x02u
#define MCS51_P3_INT0 0x04u
#define MCS51_P3_INT1 0x08u
#define MCS51_P3_T0 0x10u
#define MCS51_P3_T1 0x20u

/* Interrupt sources in hardware polling order; the value is also the bit
 * position in IE and IP. Vector address = 8 * source + 3. */
enum mcs51_irq {
    MCS51_IRQ_EX0 = 0,
    MCS51_IRQ_T0 = 1,
    MCS51_IRQ_EX1 = 2,
    MCS51_IRQ_T1 = 3,
    MCS51_IRQ_SERIAL = 4,
    MCS51_IRQ_COUNT = 5
};

typedef enum mcs51_status {
    MCS51_OK = 0,       /* an instruction, interrupt entry or idle cycle completed */
    MCS51_HALT,         /* jump-to-self (or idle) that no interrupt can leave */
    MCS51_ILLEGAL,      /* reserved opcode A5h at PC; nothing was executed */
    MCS51_POWER_DOWN,   /* PCON.PD was set; only a reset leaves this state */
    MCS51_CYCLE_LIMIT   /* mcs51_run(): the cycle budget is used up */
} mcs51_status;

/* Connections to the outside world. Every callback is optional. */
typedef struct mcs51_io {
    /* Level the outside world applies to the pins of port 0..3 (a 0 bit pulls
     * the pin low). The CPU sees (latch AND level): ports are modelled as
     * quasi-bidirectional with pull-ups. When NULL, cpu->pins_in[port] is used.
     * Port 3 is sampled once per machine cycle for INT0/INT1/T0/T1, so keep
     * this callback cheap. */
    uint8_t (*port_in)(void *ctx, unsigned port);
    /* Called for every write to a port latch, even if the value is unchanged. */
    void (*port_out)(void *ctx, unsigned port, uint8_t latch);
    void *port_ctx;

    /* External data memory (MOVX). NULL read callback: the bus floats (FFh). */
    uint8_t (*xdata_read)(void *ctx, uint16_t addr);
    void (*xdata_write)(void *ctx, uint16_t addr, uint8_t value);
    void *xdata_ctx;

    /* A serial frame has been shifted out (called when TI is set).
     * Bits 0-7: data; bit 8: TB8 in the 9-bit modes 2 and 3. */
    void (*serial_tx)(void *ctx, uint16_t data);
    void *serial_ctx;
} mcs51_io;

typedef struct mcs51 {
    /* Architectural state ------------------------------------------------ */
    uint8_t code[MCS51_CODE_SIZE]; /* program memory, FFh where unprogrammed */
    uint8_t iram[256];             /* internal RAM; 80h-FFh only via @R0/@R1/SP */
    uint8_t sfr[128];              /* SFR storage, index = address - 80h */
    uint16_t pc;
    uint64_t cycles;               /* machine cycles since mcs51_init() */

    mcs51_io io;
    uint8_t pins_in[4];            /* pin levels used when io.port_in is NULL */

    /* Internal state: readable for diagnostics, modified only by the core. */
    struct {
        uint8_t sample;  /* request flags sampled at the end of the last cycle */
        uint8_t active;  /* bit 0: low-level ISR in progress, bit 1: high-level */
        int8_t pending;  /* source to vector to on the next step, -1 = none */
        bool blocked;    /* current instruction is RETI or wrote IE/IP */
    } irq;
    uint8_t p3_sample;   /* port 3 pin levels sampled at the end of the last cycle */
    uint8_t count_edge;  /* bit 0/1: falling edge seen on T0/T1 in that sample */
    bool idle;
    bool power_down;
    struct {
        uint8_t rx_sbuf;        /* receive half of SBUF */
        bool tx_request;        /* SBUF written, frame not started yet */
        uint16_t tx_next;       /* frame waiting to start (data | TB8 << 8) */
        uint16_t tx_frame;      /* frame being shifted out */
        int8_t tx_bit;          /* bit time of the frame in progress, -1 = idle */
        uint8_t div16;          /* free-running divide-by-16 of the baud clock */
        bool t1_half;           /* divide-by-2 of timer 1 overflows (SMOD = 0) */
        int16_t rx_tick;        /* baud ticks into the frame being received, -1 = idle */
        uint16_t rx_frame;
        uint16_t queue[MCS51_SERIAL_QUEUE]; /* frames waiting on the RXD line */
        uint16_t q_head, q_count;
        uint32_t overruns;      /* frames lost because RI was still set */
    } serial;
} mcs51;

/* Power-on: clear RAM, fill code memory with FFh, reset all registers.
 * io callbacks are cleared, so install them afterwards. */
void mcs51_init(mcs51 *cpu);

/* Hardware reset (RST pin): SFRs to their reset values, PC = 0000h.
 * Code memory, internal RAM, the cycle counter and io are kept. */
void mcs51_reset(mcs51 *cpu);

/* Copy len bytes to program memory at addr (wrapping at 64 KiB). */
void mcs51_load_code(mcs51 *cpu, uint16_t addr, const uint8_t *data, size_t len);

/* Execute one instruction, one interrupt entry (hardware LCALL) or, in idle
 * mode, one machine cycle. */
mcs51_status mcs51_step(mcs51 *cpu);

/* Step until a status other than MCS51_OK occurs or cpu->cycles reaches
 * max_cycles (the instruction that crosses the limit is completed). */
mcs51_status mcs51_run(mcs51 *cpu, uint64_t max_cycles);

/* Queue a frame on the RXD line (bit 8 = ninth bit in modes 2/3). The frame
 * starts once the receiver is enabled (REN = 1) and the line is free.
 * Returns false if the queue is full. */
bool mcs51_serial_inject(mcs51 *cpu, uint16_t data);

/* Read/write a direct address exactly like MOV A,direct / MOV direct,A:
 * ports read pin levels, writes trigger peripheral side effects. */
uint8_t mcs51_read_direct(mcs51 *cpu, uint8_t addr);
void mcs51_write_direct(mcs51 *cpu, uint8_t addr, uint8_t value);

/* Register Rn of the currently selected bank. */
uint8_t mcs51_reg(const mcs51 *cpu, unsigned n);

/* Pin levels of a port as an instruction would read them (latch AND input). */
uint8_t mcs51_port_pins(mcs51 *cpu, unsigned port);

const char *mcs51_status_name(mcs51_status status);

/* Static opcode information as listed in the Intel MCS-51 instruction set.
 * operands is a template for the disassembler (see disasm.c). */
typedef struct mcs51_opinfo {
    const char *mnemonic; /* NULL for the reserved opcode A5h */
    const char *operands;
    uint8_t bytes;
    uint8_t cycles;       /* machine cycles */
} mcs51_opinfo;

extern const mcs51_opinfo mcs51_opcodes[256];

/* Disassemble the instruction at addr into out (Intel syntax, hexadecimal
 * with an 'h' suffix, SFR and bit names where they exist). The output is
 * accepted unchanged by tools/asm51.py.
 * Returns the instruction length in bytes (1 for the reserved opcode). */
unsigned mcs51_disasm(const uint8_t code[MCS51_CODE_SIZE], uint16_t addr,
                      char *out, size_t outsize);

#ifdef __cplusplus
}
#endif

#endif /* MCS51_MCS51_H */
