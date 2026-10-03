; port_counter.a51 - count push-button presses and show the count on LEDs
;
; Hardware (any 8051 lab board):
;   P3.2  push button to ground, pull-up: reads 1 when released, 0 when pressed
;   P1    eight LEDs from Vcc through resistors to the port pins:
;         a 0 in the latch switches an LED on (the port sinks the current)
;
; The program polls the button and counts presses (high-to-low transitions).
; A mechanical contact bounces for a few milliseconds, so a level change is
; only accepted if it is still present after a debounce delay of about 5 ms;
; shorter pulses are ignored. The count is shown inverted on P1 (active-low
; LEDs). The program runs forever.

BUTTON  BIT   P3.2

        ORG   0000h
START:  MOV   R5, #0            ; press counter
        MOV   P1, #0FFh         ; all LEDs off
WAIT_PRESS:
        JB    BUTTON, WAIT_PRESS
        LCALL DEBOUNCE
        JB    BUTTON, WAIT_PRESS    ; released again: it was a glitch
        INC   R5
        MOV   A, R5
        CPL   A                 ; active-low LEDs
        MOV   P1, A
WAIT_RELEASE:
        JNB   BUTTON, WAIT_RELEASE
        LCALL DEBOUNCE
        JNB   BUTTON, WAIT_RELEASE  ; still bouncing: wait again
        SJMP  WAIT_PRESS

; DEBOUNCE: 4995 machine cycles including LCALL and RET (5 ms at 12 MHz):
;   2 + 1 + 10 * (1 + 248 * 2 + 2) + 2
DEBOUNCE:
        MOV   R6, #10
DB1:    MOV   R7, #248
        DJNZ  R7, $
        DJNZ  R6, DB1
        RET

        END
