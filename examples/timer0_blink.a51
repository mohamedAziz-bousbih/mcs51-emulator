; timer0_blink.a51 - blink an LED from the Timer 0 interrupt
;
; Timer 0 runs in mode 1 (16-bit timer) and overflows every 50000 machine
; cycles; the interrupt service routine toggles P1.0. At 12 MHz this is a
; 50 ms half period, so the LED blinks at 10 Hz. Between interrupts the CPU
; sleeps in idle mode (PCON.IDL): the timers keep running, the next
; interrupt wakes the CPU up.
;
; Exact period: the timer keeps counting after the overflow while the
; interrupt is being entered, so the ISR does not overwrite TH0:TL0 but adds
; the reload value to the count that has accumulated. The timer is stopped
; for the 7 machine cycles of that 16-bit addition (CLR TR0 ... MOV TH0,A);
; the reload value includes those 7 counts:
;
;   period = 7 + (65536 - RELOAD) = 50000   ->   RELOAD = 65536 - 50000 + 7

PERIOD  EQU   50000
INIT    EQU   65536 - PERIOD    ; first period: timer started from scratch
RELOAD  EQU   65536 - PERIOD + 7

        ORG   0000h
        LJMP  MAIN
        ORG   000Bh             ; Timer 0 interrupt vector
        LJMP  T0_ISR

        ORG   0030h
MAIN:   MOV   SP, #2Fh
        MOV   TMOD, #01h        ; timer 0: mode 1, timer, no gate
        MOV   TH0, #HIGH(INIT)
        MOV   TL0, #LOW(INIT)
        SETB  ET0
        SETB  EA
        SETB  TR0
SLEEP:  ORL   PCON, #01h        ; idle until the next interrupt
        SJMP  SLEEP

T0_ISR: PUSH  ACC
        PUSH  PSW
        CLR   TR0               ; timer stopped ...
        MOV   A, TL0
        ADD   A, #LOW(RELOAD)
        MOV   TL0, A
        MOV   A, TH0
        ADDC  A, #HIGH(RELOAD)
        MOV   TH0, A
        SETB  TR0               ; ... for 7 machine cycles
        CPL   P1.0
        POP   PSW
        POP   ACC
        RETI

        END
