; delay.a51 - software delay loop with an exact cycle count
;
; Toggles P1.0 ten times with exactly 1000 machine cycles between the
; toggles: 1.000 ms at 12 MHz (1 machine cycle = 12 clocks = 1 us), i.e. a
; 500 Hz square wave for five periods. Then the program stops.
;
; Cycle budget of one loop iteration (cycles per instruction from the Intel
; MCS-51 instruction set table):
;
;   LOOP:  CPL   P1.0                    1
;          LCALL DELAY                   2
;          DELAY: MOV R6,#2              1
;                 D1: MOV R7,#246        1          x 2
;                     DJNZ R7,$          2 x 246    x 2
;                     DJNZ R6,D1         2          x 2
;                 NOP, NOP               2
;                 RET                    2
;          DJNZ  R5,LOOP                 2
;   ---------------------------------------------------
;   1 + 2 + 1 + 2 * (1 + 492 + 2) + 2 + 2 + 2  =  1000
;
; Whole program: MOV R5,#10 (1) + 10 * 1000 + SJMP $ (2) = 10003 cycles.

        ORG   0000h
START:  MOV   R5, #10           ; ten half periods
LOOP:   CPL   P1.0
        LCALL DELAY
        DJNZ  R5, LOOP
        SJMP  $                 ; done (interrupts are disabled: the emulator halts)

; DELAY: 995 machine cycles from its first instruction up to and including RET.
DELAY:  MOV   R6, #2
D1:     MOV   R7, #246
D2:     DJNZ  R7, D2
        DJNZ  R6, D1
        NOP
        NOP
        RET

        END
