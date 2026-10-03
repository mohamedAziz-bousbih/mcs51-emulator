; bcd_counter.a51 - decimal up-counter 00..99 with packed BCD arithmetic
;
; P1 shows the count as two packed BCD digits (for example for two BCD to
; 7-segment decoders), P2 drives a common-cathode 7-segment display with the
; units digit (segment pattern looked up with MOVC). After 99 the counter
; wraps to 00 with a decimal carry (CY = 1) and the program stops.
;
; ADD A,#1 is a binary addition: 09h + 1 = 0Ah. DA A corrects the result to
; packed BCD (0Ah -> 10h, 99h + 1 = 9Ah -> 00h with CY = 1), using the
; auxiliary carry AC and the carry CY produced by the ADD.
;
; On real hardware a delay would separate the counts; the emulator test
; records every port write instead.

        ORG   0000h
START:  MOV   DPTR, #SEGMENTS
        MOV   R2, #00h          ; packed BCD count
SHOW:   MOV   P1, R2
        MOV   A, R2
        ANL   A, #0Fh           ; units digit
        MOVC  A, @A+DPTR
        MOV   P2, A
        MOV   A, R2
        ADD   A, #1
        DA    A
        MOV   R2, A
        JNC   SHOW              ; decimal carry: 99 -> 00
        MOV   P1, R2            ; show 00
        SJMP  $

; Segment patterns 0-9, bit 0 = segment a ... bit 6 = segment g.
SEGMENTS:
        DB    3Fh, 06h, 5Bh, 4Fh, 66h, 6Dh, 7Dh, 07h, 7Fh, 6Fh

        END
