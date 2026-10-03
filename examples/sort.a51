; sort.a51 - bubble sort of 16 unsigned bytes in internal RAM
;
; The input is copied from program memory to ARRAY (40h-4Fh) and sorted in
; ascending order. Each pass compares neighbours through the pointers R0 and
; R1 (indirect addressing) and swaps them with XCH; after pass k the last k
; elements are final, so the passes get shorter. A pass without a swap ends
; the sort early (flag F0 in the PSW).
;
; CJNE A,B,rel sets CY when A < B as unsigned numbers, so 80h sorts after 7Fh.
;
; Machine cycles (instruction set table), used by the end-to-end test to
; check the cycle counter against a model of the algorithm:
;   load phase            4 + 16 * 9 = 148, then MOV R6,#N-1: 1
;   one comparison        11 without swap, 14 with swap
;   pass overhead         7, plus 2 for DJNZ R6 when the pass swapped
;   final SJMP $          2

N       EQU   16
ARRAY   DATA  40h

        ORG   0000h
START:  MOV   DPTR, #INPUT
        MOV   R0, #ARRAY
        MOV   R7, #N
LOAD:   CLR   A
        MOVC  A, @A+DPTR
        MOV   @R0, A
        INC   R0
        INC   DPTR
        DJNZ  R7, LOAD

        MOV   R6, #N-1          ; comparisons in the first pass
PASS:   CLR   F0                ; no swap in this pass yet
        MOV   R0, #ARRAY
        MOV   R1, #ARRAY+1
        MOV   A, R6
        MOV   R7, A
COMPARE:
        MOV   A, @R0            ; a[i]
        MOV   B, @R1            ; a[i+1]
        CJNE  A, B, DIFFERENT
        SJMP  NEXT              ; equal: keep the order
DIFFERENT:
        JC    NEXT              ; a[i] < a[i+1]: in order
        XCH   A, @R1            ; a[i+1] = a[i], A = old a[i+1]
        MOV   @R0, A
        SETB  F0
NEXT:   INC   R0
        INC   R1
        DJNZ  R7, COMPARE
        JNB   F0, DONE          ; no swap: sorted
        DJNZ  R6, PASS
DONE:   SJMP  $

INPUT:  DB    5Ah, 03h, 0FFh, 7Fh, 80h, 00h, 3Ch, 3Ch
        DB    99h, 12h, 0E7h, 45h, 01h, 0C3h, 66h, 0A0h

        END
