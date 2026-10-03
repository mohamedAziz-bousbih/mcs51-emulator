# External bus, memory map and address decoding

This document describes the lab board that `mcs51_board_init()` builds
(`src/board.c`) and the `MOVX` bus model behind it (`src/xbus.c`). The board
is the classic single-chip 8051 system extended with external data memory and
memory-mapped peripherals, the way it is drawn in microprocessor-technology
lectures. It exists only in software: nothing here has been built in
hardware.

## Memory map

| Address range | Device | Selected by | Device address lines | Mirrors |
|---|---|---|---|---|
| `0000h`-`7FFFh` | 62256 SRAM, 32 KiB | `A15 = 0` | `A14`-`A0` | 1 |
| `8000h`-`BFFFh` | not decoded | - | - | - |
| `C000h`-`DFFFh` | 8255 PPI (ports A, B, C, control) | 74HC138 output `Y6` (`A15 A14 A13 = 110`) | `A1`-`A0` | 2048 per register |
| `E000h`-`FFFFh` | 74HC574 output latch, write-only | 74HC138 output `Y7` (`A15 A14 A13 = 111`) and `/WR` | none | 8192 |

`mcs51 map` prints the same table from the decoder configuration that the
emulator actually uses:

```
range        device          A15...........A0  mirrors
0000h-7FFFh  SRAM 62256      0aaaaaaaaaaaaaaa  1
C000h-DFFFh  PPI 8255        110xxxxxxxxxxxaa  2048
E000h-FFFFh  Latch 74HC574   111xxxxxxxxxxxxx  8192
1/0 = decoded address line, a = line wired to the device, x = not connected
(each x doubles the number of mirror addresses); other addresses read FFh
```

Program memory is separate (Harvard architecture): `MOVC` and instruction
fetches read the 64 KiB code space, `MOVX` reaches only the data space above.

## The external bus of the 8051

During `MOVX` the 8051 turns two of its ports into a bus:

- **P0** carries the low address byte `A7`-`A0` first and then the data byte
  (multiplexed address/data bus). An address latch (74HC573) holds the low
  address byte while `ALE` is high, so the memory sees a stable address
  while P0 switches to data.
- **P2** carries the high address byte `A15`-`A8` for `MOVX @DPTR`. For
  `MOVX @R0/@R1` the 8051 drives only the low byte from `Ri`; `A15`-`A8` are
  whatever is in the P2 latch, so P2 selects a 256-byte page.
- **P3.6** is `/WR` and **P3.7** is `/RD`.

Each `MOVX` takes two machine cycles; the `/RD` or `/WR` strobe falls into
the second one, during which the CPU skips one `ALE` pulse.

Two consequences are modelled because programs can observe them:

1. After every `MOVX` the P0 latch holds `FFh`: the CPU writes `FFh` to P0 to
   float the bus, so P0 cannot be used as an output port in a system with
   external memory (`tests/test_instructions.c`, `test_movx`).
2. `MOVX @Ri` uses the P2 *latch* as the page, so a program sets the page
   with `MOV P2,#page` once and then walks through 256 bytes with `INC Ri`
   (`examples/xmem_copy.a51`, step 4).

What is not modelled: the individual `ALE`, `/RD`, `/WR` and `/PSEN`
waveforms, the P2 pins showing `DPH` during `MOVX @DPTR`, and setup/hold
times. The emulator resolves each access as one atomic read or write at the
start of the instruction.

## Decoding logic

The decoder equations, with active-high signals for readability:

```
SRAM_CS    = NOT A15                              -> 62256 /CE (active low)
PPI_CS     = A15 AND A14 AND (NOT A13)            -> 74HC138 Y6 -> 8255 /CS
LATCH_CLK  = A15 AND A14 AND A13 AND WR           -> 74HC138 Y7 OR /WR -> 74HC574 CLK
```

- The SRAM needs no decoder chip: `A15` drives its `/CE` directly, `/RD`
  drives `/OE` and `/WR` drives `/WE`. `A14`-`A0` address the 32 KiB.
- The 74HC138 gets `A13` on input A, `A14` on B and `A15` on C, so its outputs
  each cover 8 KiB. `Y0`-`Y3` lie in the SRAM range and stay unused; `Y4` and
  `Y5` (`8000h`-`BFFFh`) are free for expansion; `Y6` selects the 8255 and
  `Y7` the latch.
- The 8255 only sees `A1:A0` (register select: 0 = port A, 1 = port B,
  2 = port C, 3 = control). `A12`-`A2` are not connected, so each register
  appears at 2^11 = 2048 addresses in its 8 KiB block. `C006h` selects the
  same register as `C002h` (port C); `examples/xmem_copy.a51` uses that mirror
  on purpose and `tests/test_examples.c` checks that the decoder routed it to
  the 8255.
- The 74HC574 is an edge-triggered register without address inputs. Its
  clock is `/Y7 OR /WR` (a 74HC32 gate): the clock rises at the end of a
  write to `E000h`-`FFFFh`, when P0 still holds the data. It cannot be read:
  nothing drives the bus during a read in that range.

Reads from addresses that select no device (the gap at `8000h`-`BFFFh`, the
write-only latch) return `FFh` in the emulator, which corresponds to pull-up
resistors on the P0 bus lines. On a board without pull-ups the value would be
undefined.

## The decoder model in software

`mcs51_xbus` holds up to eight devices. Each is described the way a decoder
is wired:

| Field | Meaning |
|---|---|
| `mask` | address lines the decoder compares |
| `match` | level those lines must have to select the device |
| `local_mask` | address lines wired to the device's own address inputs |

A device is selected when `(addr & mask) == match`; it receives
`addr & local_mask`. Lines in neither mask are not connected, and each of
them doubles the number of mirror addresses
(`mcs51_xbus_mirrors()` = 2^(number of unconnected lines)).

`mcs51_xbus_map()` rejects configurations that real decoding logic could not
implement without bus contention:

- `match` bits outside `mask` (a comparison on a line the decoder does not
  see),
- an address line that is both decoded and wired to the device,
- two devices that can be selected by the same address. Two selects overlap
  exactly when their required levels agree on every line both decoders look
  at: `((match1 ^ match2) & mask1 & mask2) == 0`.

An optional bus monitor sees every access with the selected device (or none).
The CLI uses it for `--trace`; an excerpt from
`mcs51 run examples/xmem_copy.hex --trace` (cycle count, PC, bytes,
instruction, registers before the instruction):

```
[      5011] 005B  F0        MOVX  @DPTR,A              A=0F PSW=00 SP=07
[      5011] PPI port A = 0Fh
[      5011] MOVX write C000h = 0Fh  (PPI 8255)
[      5013] 005C  90 C0 01  MOV   DPTR,#0C001h         A=0F PSW=00 SP=07
[      5015] 005F  EC        MOV   A,R4                 A=0F PSW=00 SP=07
[      5016] 0060  F0        MOVX  @DPTR,A              A=D3 PSW=01 SP=07
[      5016] PPI port B = D3h
[      5016] MOVX write C001h = D3h  (PPI 8255)
```

## The 8255 model

`src/ppi8255.c` implements the parts of the Intel 8255 that a mode-0 program
uses:

- control word with bit 7 = 1 (mode set): port directions for A, B, upper C
  and lower C; a mode set clears all output latches, as on the real chip;
- control word with bit 7 = 0: bit set/reset of a single port C bit;
- reads return the output latch for output bits and the pin level for input
  bits; the control register reads as `FFh` (it is write-only);
- after reset the control word is `9Bh` (all ports input).

Modes 1 and 2 are accepted (the direction bits take effect) but their
handshake lines on port C are not emulated.
