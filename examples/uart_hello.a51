; uart_hello.a51 - "Hello, 8051!" on the serial port
;
; Serial port mode 1 (8 data bits, 1 stop bit, no parity) at 9600 baud with
; an 11.0592 MHz crystal. Timer 1 in mode 2 (8-bit auto-reload) generates the
; baud rate:
;
;   baud = 2^SMOD / 32 * fosc / (12 * (256 - TH1))
;        = 1 / 32 * 11059200 / (12 * 3) = 9600          (SMOD = 0, TH1 = 0FDh)
;
; One bit lasts 96 machine cycles, one frame (start + 8 data + stop) 960.
; The characters are sent by polling TI; the program then stops.

        ORG   0000h
START:  MOV   TMOD, #20h        ; timer 1: mode 2, timer
        MOV   TH1, #0FDh        ; reload value for 9600 baud
        MOV   TL1, #0FDh
        MOV   SCON, #50h        ; mode 1, receiver enabled
        SETB  TR1
        MOV   DPTR, #TEXT
NEXT:   CLR   A
        MOVC  A, @A+DPTR
        JZ    DONE              ; 0 terminates the string
        LCALL PUTCHAR
        INC   DPTR
        SJMP  NEXT
DONE:   SJMP  $

; PUTCHAR: send A and wait until the frame is out (TI rises at the stop bit).
PUTCHAR:
        MOV   SBUF, A
        JNB   TI, $
        CLR   TI
        RET

TEXT:   DB    'Hello, 8051!', 0Dh, 0Ah, 0

        END
