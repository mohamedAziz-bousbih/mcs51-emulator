; xmem_copy.a51 - external data memory and memory-mapped I/O through the
; address decoder of the lab board (memory map in docs/memory-map.md):
;
;   0000h-7FFFh  62256 SRAM                    selected by A15 = 0
;   C000h-DFFFh  8255 PPI, register = A1:A0    A15..A13 = 110, mirrored every 4 bytes
;   E000h-FFFFh  74HC574 output latch          A15..A13 = 111, write-only
;
; 1. copy a 64-byte block from program memory (MOVC) to SRAM 0000h
; 2. copy SRAM 0000h-003Fh to SRAM 4000h-403Fh with a single data pointer:
;    source and destination differ only in the high byte, so DPL is the
;    index and DPH is switched between the two pages
; 3. verify the copy against program memory and form a 16-bit checksum
; 4. fill the page 7F00h-7FFFh with MOVX @R0: R0 supplies A7..A0, the P2
;    latch supplies A15..A8
; 5. report through the peripherals: the 8255 is set to mode 0 with all ports
;    as outputs; port A = checksum high byte, port B = checksum low byte,
;    port C = number of mismatches, written through the mirror address C006h
;    (A2 is not decoded, so C006h selects the same register as C002h);
;    the latch shows 0A5h if the copy verified and 0EEh otherwise.
;
; Note: every MOVX writes FFh into the P0 latch, because P0 carries the
; multiplexed address/data bus; P0 cannot be used as an I/O port here.

LEN     EQU   64
SRC     XDATA 0000h
DST     XDATA 4000h
PAGE    EQU   7Fh
PPI_A   XDATA 0C000h
PPI_B   XDATA 0C001h
PPI_C_MIRROR XDATA 0C006h
PPI_CW  XDATA 0C003h
LATCH   XDATA 0E000h

        ORG   0000h
        ; 1. program memory -> SRAM 0000h
        MOV   R6, #0
COPY1:  MOV   DPTR, #BLOCK
        MOV   A, R6
        MOVC  A, @A+DPTR
        MOV   DPH, #HIGH(SRC)
        MOV   DPL, R6           ; LOW(SRC) = 0
        MOVX  @DPTR, A
        INC   R6
        CJNE  R6, #LEN, COPY1

        ; 2. SRAM 0000h -> SRAM 4000h
        MOV   DPTR, #SRC
        MOV   R7, #LEN
COPY2:  MOVX  A, @DPTR
        MOV   DPH, #HIGH(DST)
        MOVX  @DPTR, A
        MOV   DPH, #HIGH(SRC)
        INC   DPTR
        DJNZ  R7, COPY2

        ; 3. verify: R3:R4 = sum of the copied bytes, R5 = mismatches
        MOV   R3, #0
        MOV   R4, #0
        MOV   R5, #0
        MOV   R6, #0
VERIFY: MOV   DPTR, #BLOCK
        MOV   A, R6
        MOVC  A, @A+DPTR
        MOV   B, A              ; expected byte
        MOV   DPH, #HIGH(DST)
        MOV   DPL, R6
        MOVX  A, @DPTR
        CJNE  A, B, MISMATCH
        SJMP  ACCUMULATE
MISMATCH:
        INC   R5
ACCUMULATE:
        ADD   A, R4
        MOV   R4, A
        CLR   A
        ADDC  A, R3
        MOV   R3, A
        INC   R6
        CJNE  R6, #LEN, VERIFY

        ; 4. paged access with MOVX @R0
        MOV   P2, #PAGE
        MOV   R0, #0
FILL:   MOV   A, R0
        CPL   A
        MOVX  @R0, A            ; address = P2:R0
        INC   R0
        CJNE  R0, #0, FILL

        ; 5. results
        MOV   DPTR, #PPI_CW
        MOV   A, #80h           ; mode set: mode 0, ports A, B and C outputs
        MOVX  @DPTR, A
        MOV   DPTR, #PPI_A
        MOV   A, R3
        MOVX  @DPTR, A
        MOV   DPTR, #PPI_B
        MOV   A, R4
        MOVX  @DPTR, A
        MOV   DPTR, #PPI_C_MIRROR
        MOV   A, R5
        MOVX  @DPTR, A
        MOV   DPTR, #LATCH
        MOV   A, #0A5h
        CJNE  R5, #0, FAILED
        SJMP  REPORT
FAILED: MOV   A, #0EEh
REPORT: MOVX  @DPTR, A
        SJMP  $

        ORG   0100h
BLOCK:  DB    '8051 MOVX demo - SRAM 0000h-7FFFh, PPI 8255 C000h, latch E000h'
        DB    0Dh, 0Ah

        END
