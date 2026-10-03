# Timing model

The emulator counts time in **machine cycles**. On the standard 8051 one
machine cycle is 12 oscillator periods (six states S1-S6 with two phases
each), so at 11.0592 MHz a machine cycle lasts 1.085 us and at 12 MHz exactly
1 us. `cpu->cycles` is the number of machine cycles since `mcs51_init()`;
the CLI converts it to seconds with `--clock`.

## Instructions

`mcs51_step()` executes one instruction in two parts:

1. The instruction's architectural effect (registers, RAM, SFRs, ports,
   `MOVX` bus access) is applied at once, at the start of its first machine
   cycle.
2. The on-chip peripherals are clocked once per machine cycle for the
   instruction's cycle count from the Intel instruction set table
   (`mcs51_opcodes[op].cycles`: 1, 2 or 4).

Branches take the same time whether they are taken or not, as on the real
chip. `tests/test_opcode_table.c` compares the length and cycle count of all
255 defined opcodes with matrices transcribed from the Intel opcode map, and
executes each opcode to check that it fetches exactly its length and consumes
exactly its cycles.

This granularity reproduces every cycle count a program can measure with a
timer or a counter loop, but it does not place an effect at the exact state
inside a multi-cycle instruction. For example, `MOV TL0,#data` (two cycles)
writes TL0 before both timer increments of the instruction, so a running
timer reads `data + 2` afterwards; on the real chip the write lands somewhere
inside the instruction, and that internal state timing is not modelled.
Programs that depend on the sub-cycle order of such events are outside the
model.

## Order of events in one machine cycle

`mcs51__tick()` (`src/periph.c`):

1. Timer 0 and timer 1 count (timer mode: every cycle; counter mode: if a
   falling edge was detected on T0/T1 in the previous cycle's pin sample).
2. The serial port is clocked: by timer 1 overflows in modes 1 and 3, by the
   fixed fosc/64 or fosc/32 clock in mode 2, once per cycle in mode 0.
3. The port 3 pins are sampled (S5P2 on the real chip): `INT0`/`INT1` set
   `IE0`/`IE1` (edge or level triggered), falling edges on `T0`/`T1` are
   remembered for the next cycle.
4. The interrupt request flags are sampled for the next poll.

A consequence of steps 1 and 3: in counter mode a high/low/high sequence must
span at least two samples, so the maximum count rate is fosc/24
(`test_counter_mode`).

## Interrupts

The 8051 samples the interrupt flags at S5P2 of every machine cycle and polls
the samples in the following cycle. The emulator follows these rules
(`src/cpu.c`, `select_irq()`):

- The poll happens in the **last cycle of each instruction** and uses the
  sample taken at the end of the cycle before it. A flag that is set during
  the last cycle of an instruction is therefore seen one instruction later
  (`test_request_during_multicycle_instruction`).
- No interrupt is vectored when the instruction just polled is `RETI` or a
  write to `IE` or `IP`; at least one more instruction executes first
  (`test_ie_write_blocks_one_instruction`, and the RETI case in
  `test_priorities_and_nesting`).
- A request is ignored while an interrupt of the same or a higher priority
  level is in progress. A high-priority request (`IP` bit set) interrupts a
  low-priority service routine; nothing interrupts a high-priority one.
- Among requests of one level the fixed order `IE0`, `TF0`, `IE1`, `TF1`,
  `RI|TI` decides (`test_polling_order`).
- The vector is a hardware `LCALL` that takes two machine cycles. It clears
  `TF0`/`TF1` and edge-triggered `IE0`/`IE1`; level-triggered `IE0`/`IE1`
  follow the pin, and `RI`/`TI` must be cleared by the service routine
  (`test_external_edge_and_level`, `test_serial_flags_not_cleared`).
- `RETI` ends the highest priority level in progress. A routine that ends
  with `RET` leaves its level marked as active, and further requests of that
  level are never served (`test_ret_instead_of_reti`).

With these rules the shortest response is: flag set in cycle 1, polled in
cycle 2, `LCALL` in cycles 3 and 4, so the first instruction of the service
routine starts at cycle count 4, three complete machine cycles after the
request (`test_minimum_latency`). That matches the minimum of three complete
machine cycles stated in the Intel manual. Longer responses come from the
instruction in progress (up to 3 extra cycles for a `MUL` or `DIV`) and from
the blocking rules (a `RETI` or an `IE`/`IP` write followed by a `MUL` or
`DIV`); Intel bounds the response of a single-interrupt system at less than
9 machine cycles.

## Timers

Timer/Counter 0 and 1 count in every machine cycle in which
`TRx AND (NOT GATE OR INTx)` is true, using the pin sample of the previous
cycle for `INTx`.

| Mode | Counter | Overflow |
|---|---|---|
| 0 | 13 bits: `THx` and `TLx[4:0]`; `TLx[7:5]` are left unchanged | `1FFFh -> 0000h` |
| 1 | 16 bits: `THx:TLx` | `FFFFh -> 0000h` |
| 2 | 8 bits `TLx`, reloaded from `THx` | `FFh -> THx` |
| 3 | timer 0: `TL0` uses `TR0`/`TF0`, `TH0` is an 8-bit timer that uses `TR1`/`TF1`; timer 1 holds its count | |

While timer 0 is in mode 3, timer 1 keeps counting and still clocks the
serial port, but its overflow can no longer set `TF1`.

`examples/timer0_blink.a51` shows what the cycle model means for a program:
the timer keeps counting while the interrupt is entered, so the service
routine adds the reload value to the count instead of overwriting it, and
compensates for the 7 cycles in which it stops the timer. The end-to-end test
checks that P1.0 toggles every 50000 machine cycles exactly, 20 times.

## Serial port

Modes 1 and 3 derive the 16x baud clock from timer 1 overflows, divided by 2
when `PCON.SMOD = 0`. With timer 1 in mode 2 and `TH1 = FDh`, timer 1
overflows every 3 machine cycles, the 16x clock ticks every 6, and one bit
takes 96 machine cycles: 9600 baud at 11.0592 MHz
(`11059200 / 12 / 96 = 9600`).

- **Transmit.** Writing `SBUF` requests a frame. It starts at the next
  rollover of a free-running divide-by-16 counter, so the delay from the
  write to the start bit varies by up to one bit time, as on the real chip.
  `TI` rises at the start of the stop bit (after 9 bit times in modes 1,
  10 in modes 2/3). Each completed frame is passed to `io.serial_tx`; the
  CLI writes it to stdout.
- **Receive.** `mcs51_serial_inject()` queues frames on the RXD line. A frame
  starts when `REN = 1` and the line is free; the receiver restarts its
  divide-by-16 at the start bit and takes the byte at the middle of the stop
  bit. `RI` is only set if it was clear (otherwise the frame is counted as an
  overrun and lost) and, with `SM2 = 1`, only if the ninth bit (mode 1: the
  stop bit) is 1. Frames are injected as whole bytes; the bit-level
  majority vote on RXD is not modelled.
- **Mode 2** uses a fixed fosc/64 (fosc/32 with `SMOD = 1`).
- **Mode 0** shifts 8 bits at one bit per machine cycle in both directions;
  reception starts whenever `REN = 1` and `RI = 0` and shifts in `FFh` when
  nothing is queued.

`examples/uart_hello.a51` sends 14 characters; the first `TI` is at cycle
968 and the following ones exactly 960 cycles apart (10 bits of 96 cycles),
which `tests/test_examples.c` checks.

## Idle, power-down and halt

- `ORL PCON,#01h` (IDL) stops instruction fetch; timers, serial port and
  interrupts keep running, and each `mcs51_step()` call advances one machine
  cycle. An interrupt ends idle mode; after its `RETI` execution continues
  behind the instruction that set IDL.
- `PCON.PD` stops everything; only `mcs51_reset()` leaves power-down.
- `mcs51_step()` reports `MCS51_HALT` for a jump to itself (`SJMP $`,
  `AJMP`/`LJMP` to its own address) or idle mode when no interrupt is enabled
  and the transmitter has finished its last frame: nothing could ever change
  the program flow again. This lets example programs end with `SJMP $` and
  still terminate in tests and on the command line.
- The reserved opcode `A5h` is not executed: `mcs51_step()` returns
  `MCS51_ILLEGAL` with the PC still pointing at it and no time consumed, and
  the CLI exits with status 2.
