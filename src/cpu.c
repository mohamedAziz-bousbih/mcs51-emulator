/*
 * cpu.c - MCS-51 instruction execution, SFR access and interrupt system.
 *
 * Port reads follow the Intel rule: read-modify-write instructions (ANL, ORL,
 * XRL, JBC, CPL, INC, DEC, DJNZ, MOV bit,C, CLR bit, SETB bit) read the port
 * latch, every other instruction reads the pins.
 */
#include <string.h>

#include "core.h"

#define ACC SFR(MCS51_ACC)
#define PSW SFR(MCS51_PSW)

/* ----------------------------------------------------- registers and flags */

static unsigned parity(uint8_t v)
{
    v ^= (uint8_t)(v >> 4);
    v ^= (uint8_t)(v >> 2);
    v ^= (uint8_t)(v >> 1);
    return v & 1u;
}

/* PSW.P is driven by hardware from the accumulator in every cycle. */
static void sync_parity(mcs51 *cpu)
{
    PSW = (uint8_t)((PSW & ~MCS51_PSW_P) | parity(ACC));
}

static bool carry(const mcs51 *cpu)
{
    return (PSW & MCS51_PSW_CY) != 0;
}

static void set_flag(mcs51 *cpu, uint8_t flag, bool on)
{
    PSW = on ? (uint8_t)(PSW | flag) : (uint8_t)(PSW & ~flag);
}

/* Rn of the bank selected by PSW.RS1:RS0. */
static uint8_t *reg(mcs51 *cpu, unsigned n)
{
    return &cpu->iram[(PSW & 0x18u) + (n & 7u)];
}

/* @R0 / @R1: indirect access reaches all 256 bytes of internal RAM. */
static uint8_t *indirect(mcs51 *cpu, unsigned i)
{
    return &cpu->iram[*reg(cpu, i & 1u)];
}

static uint16_t dptr(const mcs51 *cpu)
{
    return (uint16_t)(SFR(MCS51_DPH) << 8 | SFR(MCS51_DPL));
}

static void set_dptr(mcs51 *cpu, uint16_t value)
{
    SFR(MCS51_DPH) = (uint8_t)(value >> 8);
    SFR(MCS51_DPL) = (uint8_t)value;
}

/* ------------------------------------------------------- direct addressing */

static bool sfr_implemented(uint8_t addr)
{
    switch (addr) {
    case MCS51_P0: case MCS51_SP: case MCS51_DPL: case MCS51_DPH:
    case MCS51_PCON: case MCS51_TCON: case MCS51_TMOD: case MCS51_TL0:
    case MCS51_TL1: case MCS51_TH0: case MCS51_TH1: case MCS51_P1:
    case MCS51_SCON: case MCS51_SBUF: case MCS51_P2: case MCS51_IE:
    case MCS51_P3: case MCS51_IP: case MCS51_PSW: case MCS51_ACC:
    case MCS51_B:
        return true;
    default:
        return false;
    }
}

static bool is_port(uint8_t addr)
{
    return (addr & 0xCFu) == 0x80u; /* 80h, 90h, A0h, B0h */
}

static unsigned port_index(uint8_t addr)
{
    return (addr >> 4) & 3u;
}

/* latch = true for the read half of a read-modify-write instruction.
 * Unimplemented SFR addresses read as FFh and ignore writes (real silicon
 * returns undefined data there). */
static uint8_t read_direct(mcs51 *cpu, uint8_t addr, bool latch)
{
    if (addr < 0x80)
        return cpu->iram[addr];
    if (!sfr_implemented(addr))
        return 0xFF;
    if (addr == MCS51_SBUF)
        return cpu->serial.rx_sbuf;
    if (is_port(addr) && !latch)
        return mcs51_port_pins(cpu, port_index(addr));
    return SFR(addr);
}

static void write_port_latch(mcs51 *cpu, uint8_t addr, uint8_t value)
{
    SFR(addr) = value;
    if (cpu->io.port_out)
        cpu->io.port_out(cpu->io.port_ctx, port_index(addr), value);
}

static void write_direct(mcs51 *cpu, uint8_t addr, uint8_t value)
{
    if (addr < 0x80) {
        cpu->iram[addr] = value;
        return;
    }
    if (!sfr_implemented(addr))
        return;
    if (is_port(addr)) {
        write_port_latch(cpu, addr, value);
        return;
    }
    switch (addr) {
    case MCS51_SBUF:
        mcs51__serial_write_sbuf(cpu, value);
        return;
    case MCS51_IE:
    case MCS51_IP:
        cpu->irq.blocked = true; /* one more instruction runs before a vector */
        break;
    case MCS51_PCON:
        if (value & MCS51_PCON_PD)
            cpu->power_down = true;
        else if (value & MCS51_PCON_IDL)
            cpu->idle = true;
        break;
    default:
        break;
    }
    SFR(addr) = value;
}

/* Bit addresses 00h-7Fh live in RAM bytes 20h-2Fh, 80h-FFh in the SFRs whose
 * address is a multiple of 8. */
static uint8_t bit_byte(uint8_t bit)
{
    return bit < 0x80 ? (uint8_t)(0x20 + (bit >> 3)) : (uint8_t)(bit & 0xF8);
}

static bool read_bit(mcs51 *cpu, uint8_t bit, bool latch)
{
    return (read_direct(cpu, bit_byte(bit), latch) >> (bit & 7)) & 1u;
}

static void write_bit(mcs51 *cpu, uint8_t bit, bool value)
{
    uint8_t addr = bit_byte(bit);
    uint8_t mask = (uint8_t)(1u << (bit & 7));
    uint8_t old = read_direct(cpu, addr, true);
    write_direct(cpu, addr, value ? (uint8_t)(old | mask) : (uint8_t)(old & ~mask));
}

/* -------------------------------------------------- fetch, stack, branches */

static uint8_t fetch(mcs51 *cpu)
{
    return cpu->code[cpu->pc++];
}

static uint16_t fetch16(mcs51 *cpu)
{
    uint8_t hi = fetch(cpu);
    return (uint16_t)(hi << 8 | fetch(cpu));
}

static void push(mcs51 *cpu, uint8_t value)
{
    cpu->iram[++SFR(MCS51_SP)] = value;
}

static uint8_t pop(mcs51 *cpu)
{
    return cpu->iram[SFR(MCS51_SP)--];
}

static void call(mcs51 *cpu, uint16_t target)
{
    push(cpu, (uint8_t)cpu->pc);
    push(cpu, (uint8_t)(cpu->pc >> 8));
    cpu->pc = target;
}

static uint16_t pop_pc(mcs51 *cpu)
{
    uint8_t hi = pop(cpu);
    return (uint16_t)(hi << 8 | pop(cpu));
}

/* PC-relative target; PC already points behind the instruction. */
static void jump_rel(mcs51 *cpu, uint8_t offset)
{
    cpu->pc = (uint16_t)(cpu->pc + offset - ((offset & 0x80u) ? 0x100u : 0u));
}

/* AJMP/ACALL: the upper five PC bits are kept, opcode bits 7-5 supply A10-A8. */
static uint16_t addr11(mcs51 *cpu, uint8_t op)
{
    uint8_t low = fetch(cpu);
    return (uint16_t)((cpu->pc & 0xF800u) | ((op & 0xE0u) << 3) | low);
}

/* ------------------------------------------------------------- MOVX bus */

/* Port 0 carries the multiplexed address/data bus during MOVX; the CPU
 * writes FFh into the P0 latch, so earlier P0 output data is lost. */
static void bus_cycle(mcs51 *cpu)
{
    write_port_latch(cpu, MCS51_P0, 0xFF);
}

static uint8_t xdata_read(mcs51 *cpu, uint16_t addr)
{
    bus_cycle(cpu);
    return cpu->io.xdata_read ? cpu->io.xdata_read(cpu->io.xdata_ctx, addr) : 0xFF;
}

static void xdata_write(mcs51 *cpu, uint16_t addr, uint8_t value)
{
    bus_cycle(cpu);
    if (cpu->io.xdata_write)
        cpu->io.xdata_write(cpu->io.xdata_ctx, addr, value);
}

/* MOVX @Ri drives the low address byte from Ri and the high byte from the P2
 * latch, so P2 selects a 256-byte page of external memory. */
static uint16_t paged_addr(mcs51 *cpu, uint8_t op)
{
    return (uint16_t)(SFR(MCS51_P2) << 8 | *reg(cpu, op & 1u));
}

/* ---------------------------------------------------------- arithmetic */

/* ADD/ADDC: CY = carry out of bit 7, AC = carry out of bit 3,
 * OV = carry out of bit 6 XOR carry out of bit 7. */
static void add(mcs51 *cpu, uint8_t value, unsigned c)
{
    uint8_t a = ACC;
    unsigned sum = (unsigned)a + value + c;
    unsigned c3 = ((a & 0x0Fu) + (value & 0x0Fu) + c) >> 4;
    unsigned c6 = ((a & 0x7Fu) + (value & 0x7Fu) + c) >> 7;
    unsigned c7 = sum >> 8;
    set_flag(cpu, MCS51_PSW_CY, c7 != 0);
    set_flag(cpu, MCS51_PSW_AC, c3 != 0);
    set_flag(cpu, MCS51_PSW_OV, (c6 ^ c7) != 0);
    ACC = (uint8_t)sum;
}

/* SUBB: the flags report borrows the same way ADD reports carries. */
static void subb(mcs51 *cpu, uint8_t value)
{
    uint8_t a = ACC;
    unsigned c = carry(cpu) ? 1u : 0u;
    bool b3 = (a & 0x0Fu) < (value & 0x0Fu) + c;
    bool b6 = (a & 0x7Fu) < (value & 0x7Fu) + c;
    bool b7 = a < value + c;
    set_flag(cpu, MCS51_PSW_CY, b7);
    set_flag(cpu, MCS51_PSW_AC, b3);
    set_flag(cpu, MCS51_PSW_OV, b6 != b7);
    ACC = (uint8_t)(a - value - c);
}

/* DA A: decimal adjust after ADD/ADDC. Sets CY on a decimal carry but never
 * clears it; AC and OV are unaffected. */
static void decimal_adjust(mcs51 *cpu)
{
    unsigned a = ACC;
    bool cy = carry(cpu);
    if ((a & 0x0Fu) > 9 || (PSW & MCS51_PSW_AC)) {
        a += 0x06;
        if (a > 0xFF)
            cy = true;
        a &= 0xFF;
    }
    if ((a >> 4) > 9 || cy) {
        a += 0x60;
        if (a > 0xFF)
            cy = true;
        a &= 0xFF;
    }
    ACC = (uint8_t)a;
    set_flag(cpu, MCS51_PSW_CY, cy);
}

static void mul(mcs51 *cpu)
{
    unsigned product = (unsigned)ACC * SFR(MCS51_B);
    ACC = (uint8_t)product;
    SFR(MCS51_B) = (uint8_t)(product >> 8);
    set_flag(cpu, MCS51_PSW_CY, false);
    set_flag(cpu, MCS51_PSW_OV, product > 0xFF);
}

/* Division by zero: Intel leaves A and B undefined; this core keeps them. */
static void divide(mcs51 *cpu)
{
    uint8_t divisor = SFR(MCS51_B);
    set_flag(cpu, MCS51_PSW_CY, false);
    set_flag(cpu, MCS51_PSW_OV, divisor == 0);
    if (divisor == 0)
        return;
    uint8_t dividend = ACC;
    ACC = (uint8_t)(dividend / divisor);
    SFR(MCS51_B) = (uint8_t)(dividend % divisor);
}

static void cjne(mcs51 *cpu, uint8_t x, uint8_t y, uint8_t offset)
{
    set_flag(cpu, MCS51_PSW_CY, x < y);
    if (x != y)
        jump_rel(cpu, offset);
}

/* Second operand of the arithmetic/logic rows (columns 4-F):
 * #data, direct, @R0, @R1, R0..R7. */
static uint8_t alu_source(mcs51 *cpu, uint8_t op)
{
    unsigned column = op & 0x0Fu;
    if (column == 4)
        return fetch(cpu);
    if (column == 5)
        return read_direct(cpu, fetch(cpu), false);
    if (column < 8)
        return *indirect(cpu, column);
    return *reg(cpu, column);
}

/* ------------------------------------------------------------- execution */

#define CASE_RI(op) case (op): case (op) + 1
#define CASE_RN(op)                                                          \
    case (op): case (op) + 1: case (op) + 2: case (op) + 3:                  \
    case (op) + 4: case (op) + 5: case (op) + 6: case (op) + 7
#define CASE_PAGES(op)                                                       \
    case (op): case (op) + 0x20: case (op) + 0x40: case (op) + 0x60:         \
    case (op) + 0x80: case (op) + 0xA0: case (op) + 0xC0: case (op) + 0xE0

/* Execute one instruction; the opcode byte has been fetched already. The
 * reserved opcode A5h never gets here (mcs51_step reports it). */
static void execute(mcs51 *cpu, uint8_t op)
{
    uint8_t d, v, offset;

    switch (op) {
    /* --- program branching --------------------------------------------- */
    case 0x00: /* NOP */
        break;
    CASE_PAGES(0x01): /* AJMP addr11 */
        cpu->pc = addr11(cpu, op);
        break;
    CASE_PAGES(0x11): /* ACALL addr11 */
        call(cpu, addr11(cpu, op));
        break;
    case 0x02: /* LJMP addr16 */
        cpu->pc = fetch16(cpu);
        break;
    case 0x12: /* LCALL addr16 */
        call(cpu, fetch16(cpu));
        break;
    case 0x22: /* RET */
        cpu->pc = pop_pc(cpu);
        break;
    case 0x32: /* RETI: also ends the highest priority level in progress */
        cpu->pc = pop_pc(cpu);
        cpu->irq.active &= (cpu->irq.active & 2u) ? 1u : 0u;
        cpu->irq.blocked = true;
        break;
    case 0x73: /* JMP @A+DPTR */
        cpu->pc = (uint16_t)(dptr(cpu) + ACC);
        break;
    case 0x80: /* SJMP rel */
        jump_rel(cpu, fetch(cpu));
        break;
    case 0x40: /* JC rel */
    case 0x50: /* JNC rel */
        offset = fetch(cpu);
        if (carry(cpu) == (op == 0x40))
            jump_rel(cpu, offset);
        break;
    case 0x60: /* JZ rel */
    case 0x70: /* JNZ rel */
        offset = fetch(cpu);
        if ((ACC == 0) == (op == 0x60))
            jump_rel(cpu, offset);
        break;
    case 0x10: /* JBC bit,rel (read-modify-write) */
        d = fetch(cpu);
        offset = fetch(cpu);
        if (read_bit(cpu, d, true)) {
            write_bit(cpu, d, false);
            jump_rel(cpu, offset);
        }
        break;
    case 0x20: /* JB bit,rel */
    case 0x30: /* JNB bit,rel */
        d = fetch(cpu);
        offset = fetch(cpu);
        if (read_bit(cpu, d, false) == (op == 0x20))
            jump_rel(cpu, offset);
        break;
    case 0xB4: /* CJNE A,#data,rel */
        v = fetch(cpu);
        offset = fetch(cpu);
        cjne(cpu, ACC, v, offset);
        break;
    case 0xB5: /* CJNE A,direct,rel */
        d = fetch(cpu);
        offset = fetch(cpu);
        cjne(cpu, ACC, read_direct(cpu, d, false), offset);
        break;
    CASE_RI(0xB6): /* CJNE @Ri,#data,rel */
        v = fetch(cpu);
        offset = fetch(cpu);
        cjne(cpu, *indirect(cpu, op), v, offset);
        break;
    CASE_RN(0xB8): /* CJNE Rn,#data,rel */
        v = fetch(cpu);
        offset = fetch(cpu);
        cjne(cpu, *reg(cpu, op), v, offset);
        break;
    case 0xD5: /* DJNZ direct,rel (read-modify-write) */
        d = fetch(cpu);
        offset = fetch(cpu);
        v = (uint8_t)(read_direct(cpu, d, true) - 1);
        write_direct(cpu, d, v);
        if (v != 0)
            jump_rel(cpu, offset);
        break;
    CASE_RN(0xD8): /* DJNZ Rn,rel */
        offset = fetch(cpu);
        if (--*reg(cpu, op) != 0)
            jump_rel(cpu, offset);
        break;

    /* --- arithmetic ------------------------------------------------------ */
    case 0x24: case 0x25: CASE_RI(0x26): CASE_RN(0x28): /* ADD A,src */
        add(cpu, alu_source(cpu, op), 0);
        break;
    case 0x34: case 0x35: CASE_RI(0x36): CASE_RN(0x38): /* ADDC A,src */
        add(cpu, alu_source(cpu, op), carry(cpu) ? 1u : 0u);
        break;
    case 0x94: case 0x95: CASE_RI(0x96): CASE_RN(0x98): /* SUBB A,src */
        subb(cpu, alu_source(cpu, op));
        break;
    case 0x04: /* INC A */
        ACC++;
        break;
    case 0x14: /* DEC A */
        ACC--;
        break;
    case 0x05: /* INC direct */
        d = fetch(cpu);
        write_direct(cpu, d, (uint8_t)(read_direct(cpu, d, true) + 1));
        break;
    case 0x15: /* DEC direct */
        d = fetch(cpu);
        write_direct(cpu, d, (uint8_t)(read_direct(cpu, d, true) - 1));
        break;
    CASE_RI(0x06): /* INC @Ri */
        ++*indirect(cpu, op);
        break;
    CASE_RI(0x16): /* DEC @Ri */
        --*indirect(cpu, op);
        break;
    CASE_RN(0x08): /* INC Rn */
        ++*reg(cpu, op);
        break;
    CASE_RN(0x18): /* DEC Rn */
        --*reg(cpu, op);
        break;
    case 0xA3: /* INC DPTR */
        set_dptr(cpu, (uint16_t)(dptr(cpu) + 1));
        break;
    case 0xA4: /* MUL AB */
        mul(cpu);
        break;
    case 0x84: /* DIV AB */
        divide(cpu);
        break;
    case 0xD4: /* DA A */
        decimal_adjust(cpu);
        break;

    /* --- logic ----------------------------------------------------------- */
    case 0x42: /* ORL direct,A */
        d = fetch(cpu);
        write_direct(cpu, d, (uint8_t)(read_direct(cpu, d, true) | ACC));
        break;
    case 0x43: /* ORL direct,#data */
        d = fetch(cpu);
        v = fetch(cpu);
        write_direct(cpu, d, (uint8_t)(read_direct(cpu, d, true) | v));
        break;
    case 0x44: case 0x45: CASE_RI(0x46): CASE_RN(0x48): /* ORL A,src */
        ACC |= alu_source(cpu, op);
        break;
    case 0x52: /* ANL direct,A */
        d = fetch(cpu);
        write_direct(cpu, d, (uint8_t)(read_direct(cpu, d, true) & ACC));
        break;
    case 0x53: /* ANL direct,#data */
        d = fetch(cpu);
        v = fetch(cpu);
        write_direct(cpu, d, (uint8_t)(read_direct(cpu, d, true) & v));
        break;
    case 0x54: case 0x55: CASE_RI(0x56): CASE_RN(0x58): /* ANL A,src */
        ACC &= alu_source(cpu, op);
        break;
    case 0x62: /* XRL direct,A */
        d = fetch(cpu);
        write_direct(cpu, d, (uint8_t)(read_direct(cpu, d, true) ^ ACC));
        break;
    case 0x63: /* XRL direct,#data */
        d = fetch(cpu);
        v = fetch(cpu);
        write_direct(cpu, d, (uint8_t)(read_direct(cpu, d, true) ^ v));
        break;
    case 0x64: case 0x65: CASE_RI(0x66): CASE_RN(0x68): /* XRL A,src */
        ACC ^= alu_source(cpu, op);
        break;
    case 0xE4: /* CLR A */
        ACC = 0;
        break;
    case 0xF4: /* CPL A */
        ACC = (uint8_t)~ACC;
        break;
    case 0x03: /* RR A */
        ACC = (uint8_t)(ACC >> 1 | ACC << 7);
        break;
    case 0x23: /* RL A */
        ACC = (uint8_t)(ACC << 1 | ACC >> 7);
        break;
    case 0x13: /* RRC A */
        v = ACC;
        ACC = (uint8_t)(v >> 1 | (carry(cpu) ? 0x80u : 0u));
        set_flag(cpu, MCS51_PSW_CY, (v & 1u) != 0);
        break;
    case 0x33: /* RLC A */
        v = ACC;
        ACC = (uint8_t)(v << 1 | (carry(cpu) ? 1u : 0u));
        set_flag(cpu, MCS51_PSW_CY, (v & 0x80u) != 0);
        break;
    case 0xC4: /* SWAP A */
        ACC = (uint8_t)(ACC << 4 | ACC >> 4);
        break;

    /* --- boolean (bit) operations --------------------------------------- */
    case 0xC3: /* CLR C */
        set_flag(cpu, MCS51_PSW_CY, false);
        break;
    case 0xD3: /* SETB C */
        set_flag(cpu, MCS51_PSW_CY, true);
        break;
    case 0xB3: /* CPL C */
        set_flag(cpu, MCS51_PSW_CY, !carry(cpu));
        break;
    case 0xC2: /* CLR bit */
        write_bit(cpu, fetch(cpu), false);
        break;
    case 0xD2: /* SETB bit */
        write_bit(cpu, fetch(cpu), true);
        break;
    case 0xB2: /* CPL bit */
        d = fetch(cpu);
        write_bit(cpu, d, !read_bit(cpu, d, true));
        break;
    case 0x72: /* ORL C,bit */
        set_flag(cpu, MCS51_PSW_CY, read_bit(cpu, fetch(cpu), false) || carry(cpu));
        break;
    case 0xA0: /* ORL C,/bit */
        set_flag(cpu, MCS51_PSW_CY, !read_bit(cpu, fetch(cpu), false) || carry(cpu));
        break;
    case 0x82: /* ANL C,bit */
        set_flag(cpu, MCS51_PSW_CY, read_bit(cpu, fetch(cpu), false) && carry(cpu));
        break;
    case 0xB0: /* ANL C,/bit */
        set_flag(cpu, MCS51_PSW_CY, !read_bit(cpu, fetch(cpu), false) && carry(cpu));
        break;
    case 0xA2: /* MOV C,bit */
        set_flag(cpu, MCS51_PSW_CY, read_bit(cpu, fetch(cpu), false));
        break;
    case 0x92: /* MOV bit,C (read-modify-write) */
        write_bit(cpu, fetch(cpu), carry(cpu));
        break;

    /* --- data transfer --------------------------------------------------- */
    case 0x74: /* MOV A,#data */
        ACC = fetch(cpu);
        break;
    case 0xE5: /* MOV A,direct */
        ACC = read_direct(cpu, fetch(cpu), false);
        break;
    CASE_RI(0xE6): /* MOV A,@Ri */
        ACC = *indirect(cpu, op);
        break;
    CASE_RN(0xE8): /* MOV A,Rn */
        ACC = *reg(cpu, op);
        break;
    case 0xF5: /* MOV direct,A */
        write_direct(cpu, fetch(cpu), ACC);
        break;
    CASE_RI(0xF6): /* MOV @Ri,A */
        *indirect(cpu, op) = ACC;
        break;
    CASE_RN(0xF8): /* MOV Rn,A */
        *reg(cpu, op) = ACC;
        break;
    case 0x75: /* MOV direct,#data */
        d = fetch(cpu);
        write_direct(cpu, d, fetch(cpu));
        break;
    CASE_RI(0x76): /* MOV @Ri,#data */
        *indirect(cpu, op) = fetch(cpu);
        break;
    CASE_RN(0x78): /* MOV Rn,#data */
        *reg(cpu, op) = fetch(cpu);
        break;
    case 0x85: /* MOV direct,direct: source address comes first */
        d = fetch(cpu);
        v = read_direct(cpu, d, false);
        write_direct(cpu, fetch(cpu), v);
        break;
    CASE_RI(0x86): /* MOV direct,@Ri */
        write_direct(cpu, fetch(cpu), *indirect(cpu, op));
        break;
    CASE_RN(0x88): /* MOV direct,Rn */
        write_direct(cpu, fetch(cpu), *reg(cpu, op));
        break;
    CASE_RI(0xA6): /* MOV @Ri,direct */
        *indirect(cpu, op) = read_direct(cpu, fetch(cpu), false);
        break;
    CASE_RN(0xA8): /* MOV Rn,direct */
        *reg(cpu, op) = read_direct(cpu, fetch(cpu), false);
        break;
    case 0x90: /* MOV DPTR,#data16 */
        set_dptr(cpu, fetch16(cpu));
        break;
    case 0x83: /* MOVC A,@A+PC (PC of the next instruction) */
        ACC = cpu->code[(uint16_t)(cpu->pc + ACC)];
        break;
    case 0x93: /* MOVC A,@A+DPTR */
        ACC = cpu->code[(uint16_t)(dptr(cpu) + ACC)];
        break;
    case 0xE0: /* MOVX A,@DPTR */
        ACC = xdata_read(cpu, dptr(cpu));
        break;
    CASE_RI(0xE2): /* MOVX A,@Ri */
        ACC = xdata_read(cpu, paged_addr(cpu, op));
        break;
    case 0xF0: /* MOVX @DPTR,A */
        xdata_write(cpu, dptr(cpu), ACC);
        break;
    CASE_RI(0xF2): /* MOVX @Ri,A */
        xdata_write(cpu, paged_addr(cpu, op), ACC);
        break;
    case 0xC0: /* PUSH direct */
        push(cpu, read_direct(cpu, fetch(cpu), false));
        break;
    case 0xD0: /* POP direct: SP is decremented before the destination is
                * written, so POP SP leaves the popped value in SP. */
        d = fetch(cpu);
        v = pop(cpu);
        write_direct(cpu, d, v);
        break;
    case 0xC5: /* XCH A,direct */
        d = fetch(cpu);
        v = read_direct(cpu, d, false);
        write_direct(cpu, d, ACC);
        ACC = v;
        break;
    CASE_RI(0xC6): /* XCH A,@Ri */
        v = *indirect(cpu, op);
        *indirect(cpu, op) = ACC;
        ACC = v;
        break;
    CASE_RN(0xC8): /* XCH A,Rn */
        v = *reg(cpu, op);
        *reg(cpu, op) = ACC;
        ACC = v;
        break;
    CASE_RI(0xD6): /* XCHD A,@Ri: exchange the low nibbles */
        v = *indirect(cpu, op);
        *indirect(cpu, op) = (uint8_t)((v & 0xF0u) | (ACC & 0x0Fu));
        ACC = (uint8_t)((ACC & 0xF0u) | (v & 0x0Fu));
        break;
    }
}

/* -------------------------------------------------------- interrupt system */

static unsigned lowest_bit(uint8_t mask)
{
    unsigned n = 0;
    while (!(mask & 1u)) {
        mask >>= 1;
        n++;
    }
    return n;
}

/* Choose the source to vector to from a poll sample, or -1. A request is
 * ignored while an interrupt of equal or higher priority is in progress and
 * during the instruction after RETI or a write to IE/IP. Within one priority
 * level the fixed polling order IE0, TF0, IE1, TF1, RI|TI decides. */
static int8_t select_irq(const mcs51 *cpu, uint8_t poll)
{
    uint8_t ie = SFR(MCS51_IE);
    if (cpu->irq.blocked || !(ie & MCS51_IE_EA) || (cpu->irq.active & 2u))
        return -1;
    uint8_t requests = (uint8_t)(poll & ie & 0x1Fu);
    uint8_t high = (uint8_t)(requests & SFR(MCS51_IP));
    if (high)
        return (int8_t)lowest_bit(high);
    if (requests && !(cpu->irq.active & 1u))
        return (int8_t)lowest_bit(requests);
    return -1;
}

/* Hardware LCALL to the vector. Timer flags and edge-triggered external flags
 * are cleared here; level-triggered flags follow the pin and RI/TI must be
 * cleared by the service routine. */
static void vector(mcs51 *cpu, unsigned source)
{
    uint8_t tcon = SFR(MCS51_TCON);
    switch (source) {
    case MCS51_IRQ_EX0:
        if (tcon & MCS51_TCON_IT0)
            tcon &= (uint8_t)~MCS51_TCON_IE0;
        break;
    case MCS51_IRQ_T0:
        tcon &= (uint8_t)~MCS51_TCON_TF0;
        break;
    case MCS51_IRQ_EX1:
        if (tcon & MCS51_TCON_IT1)
            tcon &= (uint8_t)~MCS51_TCON_IE1;
        break;
    case MCS51_IRQ_T1:
        tcon &= (uint8_t)~MCS51_TCON_TF1;
        break;
    default:
        break;
    }
    SFR(MCS51_TCON) = tcon;
    cpu->irq.active |= (uint8_t)(1u << ((SFR(MCS51_IP) >> source) & 1u));
    call(cpu, (uint16_t)(8 * source + 3));
}

static bool interrupts_possible(const mcs51 *cpu)
{
    uint8_t ie = SFR(MCS51_IE);
    return (ie & MCS51_IE_EA) && (ie & 0x1Fu);
}

/* ----------------------------------------------------------------- public */

void mcs51_reset(mcs51 *cpu)
{
    memset(cpu->sfr, 0, sizeof cpu->sfr);
    SFR(MCS51_P0) = SFR(MCS51_P1) = SFR(MCS51_P2) = SFR(MCS51_P3) = 0xFF;
    SFR(MCS51_SP) = 0x07;
    cpu->pc = 0;
    cpu->irq.sample = 0;
    cpu->irq.active = 0;
    cpu->irq.pending = -1;
    cpu->irq.blocked = false;
    cpu->p3_sample = 0xFF;
    cpu->count_edge = 0;
    cpu->idle = false;
    cpu->power_down = false;
    memset(&cpu->serial, 0, sizeof cpu->serial);
    cpu->serial.tx_bit = -1;
    cpu->serial.rx_tick = -1;
}

void mcs51_init(mcs51 *cpu)
{
    memset(cpu, 0, sizeof *cpu);
    memset(cpu->code, 0xFF, sizeof cpu->code);
    memset(cpu->pins_in, 0xFF, sizeof cpu->pins_in);
    mcs51_reset(cpu);
}

void mcs51_load_code(mcs51 *cpu, uint16_t addr, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
        cpu->code[(uint16_t)(addr + i)] = data[i];
}

mcs51_status mcs51_step(mcs51 *cpu)
{
    if (cpu->power_down)
        return MCS51_POWER_DOWN;

    sync_parity(cpu);
    cpu->irq.blocked = false;

    unsigned cycles;
    bool self_jump = false;
    if (cpu->irq.pending >= 0) {
        vector(cpu, (unsigned)cpu->irq.pending);
        cpu->irq.pending = -1;
        cycles = 2;
    } else if (cpu->idle) {
        cycles = 1;
    } else {
        uint16_t start = cpu->pc;
        uint8_t op = cpu->code[start];
        if (mcs51_opcodes[op].mnemonic == NULL)
            return MCS51_ILLEGAL;
        cpu->pc++;
        execute(cpu, op);
        cycles = mcs51_opcodes[op].cycles;
        self_jump = cpu->pc == start && (op == 0x80 || op == 0x02 || (op & 0x1Fu) == 0x01);
    }

    /* The poll in the last cycle sees the sample taken one cycle earlier. */
    uint8_t poll = cpu->irq.sample;
    for (unsigned i = 0; i < cycles; i++) {
        if (i + 1 == cycles)
            poll = cpu->irq.sample;
        mcs51__tick(cpu);
    }
    sync_parity(cpu);

    if (cpu->power_down)
        return MCS51_POWER_DOWN;

    cpu->irq.pending = select_irq(cpu, poll);
    if (cpu->irq.pending >= 0 && cpu->idle) {
        cpu->idle = false;
        SFR(MCS51_PCON) &= (uint8_t)~MCS51_PCON_IDL;
    }
    if ((self_jump || cpu->idle) && !interrupts_possible(cpu) && !mcs51__serial_tx_busy(cpu))
        return MCS51_HALT;
    return MCS51_OK;
}

mcs51_status mcs51_run(mcs51 *cpu, uint64_t max_cycles)
{
    while (cpu->cycles < max_cycles) {
        mcs51_status status = mcs51_step(cpu);
        if (status != MCS51_OK)
            return status;
    }
    return MCS51_CYCLE_LIMIT;
}

uint8_t mcs51_read_direct(mcs51 *cpu, uint8_t addr)
{
    return read_direct(cpu, addr, false);
}

void mcs51_write_direct(mcs51 *cpu, uint8_t addr, uint8_t value)
{
    write_direct(cpu, addr, value);
    sync_parity(cpu);
}

uint8_t mcs51_reg(const mcs51 *cpu, unsigned n)
{
    return cpu->iram[(SFR(MCS51_PSW) & 0x18u) + (n & 7u)];
}

const char *mcs51_status_name(mcs51_status status)
{
    switch (status) {
    case MCS51_OK:
        return "ok";
    case MCS51_HALT:
        return "halted";
    case MCS51_ILLEGAL:
        return "illegal opcode";
    case MCS51_POWER_DOWN:
        return "power down";
    case MCS51_CYCLE_LIMIT:
        return "cycle limit";
    }
    return "unknown";
}
