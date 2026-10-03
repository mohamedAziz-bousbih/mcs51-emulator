; subroutine.a51 - subroutine calls, parameter passing and the stack
;
; Computes n! for n = 0..8 with a recursive subroutine and stores the 16-bit
; results big-endian in internal RAM 30h-41h (1, 1, 2, 6, 24, 120, 720,
; 5040, 40320). The C equivalent of FACT is
;
;     uint16_t fact(uint8_t n) { return n == 0 ? 1 : n * fact(n - 1); }
;
; Calling convention: argument in A, result in R2 (high) : R3 (low);
; FACT changes A, B, R2, R3 and R4. The caller's n is kept on the stack
; across the recursive call.
;
; Cost at the machine level (cycles from the instruction set table):
;   FACT(0)          6 cycles (JNZ 2, MOV 1, MOV 1, RET 2)
;   FACT(n), n > 0   28 cycles + FACT(n - 1), so FACT(n) = 6 + 28 n
;   stack            2 bytes return address + 1 byte saved n per level:
;                    the outer call of FACT(8) needs 2 + 8 * 3 = 26 bytes,
;                    so SP climbs from 5Fh to 79h.
;   main loop        12 + FACT(n) = 18 + 28 n cycles for argument n,
;   whole program    4 (setup) + sum(n = 0..8) (18 + 28 n) + 2 (SJMP $) = 1176.

RESULT  DATA  30h               ; 9 results x 2 bytes

        ORG   0000h
START:  MOV   SP, #5Fh          ; stack above the result table
        MOV   R0, #RESULT
        MOV   R5, #0            ; n
NEXT:   MOV   A, R5
        LCALL FACT
        MOV   A, R2             ; store high byte, then low byte
        MOV   @R0, A
        INC   R0
        MOV   A, R3
        MOV   @R0, A
        INC   R0
        INC   R5
        CJNE  R5, #9, NEXT
        SJMP  $

; FACT: R2:R3 = A!  (valid for A <= 8, the result fits in 16 bits)
FACT:   JNZ   RECURSE
        MOV   R2, #0            ; 0! = 1
        MOV   R3, #1
        RET
RECURSE:
        PUSH  ACC               ; keep n for after the call
        DEC   A
        LCALL FACT              ; R2:R3 = (n-1)!
        POP   ACC               ; A = n
        MOV   R4, A
        MOV   B, R3             ; low byte times n
        MUL   AB                ; B:A = R3 * n
        MOV   R3, A
        MOV   A, R4
        XCH   A, B              ; A = carry into the high byte, B = n
        XCH   A, R2             ; A = old high byte, R2 = carry
        MUL   AB                ; A = R2_old * n (fits in 8 bits for n <= 8)
        ADD   A, R2
        MOV   R2, A
        RET

        END
