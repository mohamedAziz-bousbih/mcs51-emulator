; uart_echo.a51 - interrupt-driven serial echo with a ring buffer
;
; 9600 baud, mode 1, timer 1 as baud rate generator (11.0592 MHz, see
; uart_hello.a51). The serial interrupt service routine runs in register
; bank 1 so it does not disturb the main program's registers:
;   RI: store SBUF in a 16-byte ring buffer (RX_HEAD = write pointer)
;   TI: clear TI and set the software flag TX_READY
; RI and TI share one interrupt vector and are not cleared by hardware, so
; the ISR must clear them itself.
;
; The main program takes characters from the ring buffer (RX_TAIL = read
; pointer), converts lower-case letters to upper case and echoes them. A
; carriage return is echoed as CR LF and ends the program. The buffer has no
; overflow check: the echo runs at the same baud rate as the input.

BUF       EQU   60h             ; ring buffer 60h-6Fh
BUF_END   EQU   BUF + 16
RX_HEAD   DATA  30h
RX_TAIL   DATA  31h
TX_READY  BIT   20h.0

          ORG   0000h
          LJMP  MAIN
          ORG   0023h           ; serial interrupt vector (RI or TI)
          LJMP  SERIAL_ISR

          ORG   0030h
MAIN:     MOV   SP, #3Fh
          MOV   RX_HEAD, #BUF
          MOV   RX_TAIL, #BUF
          MOV   TMOD, #20h      ; timer 1: mode 2
          MOV   TH1, #0FDh      ; 9600 baud at 11.0592 MHz
          MOV   TL1, #0FDh
          SETB  TR1
          MOV   SCON, #50h      ; mode 1, receiver enabled
          SETB  ES
          SETB  EA

GETC:     MOV   A, RX_TAIL
          CJNE  A, RX_HEAD, AVAILABLE
          SJMP  GETC
AVAILABLE:
          MOV   R0, A
          MOV   A, @R0          ; next character
          INC   R0
          CJNE  R0, #BUF_END, NO_WRAP
          MOV   R0, #BUF
NO_WRAP:  MOV   RX_TAIL, R0
          CJNE  A, #'a', $+3    ; CY = (A < 'a')
          JC    ECHO
          CJNE  A, #'z'+1, $+3  ; CY = (A <= 'z')
          JNC   ECHO
          CLR   ACC.5           ; 'a'..'z' -> 'A'..'Z'
ECHO:     LCALL PUTCHAR
          CJNE  A, #0Dh, GETC
          MOV   A, #0Ah         ; CR -> CR LF, then stop
          LCALL PUTCHAR
          CLR   EA
          SJMP  $

; PUTCHAR: send A, wait until the ISR reports the end of the frame.
PUTCHAR:  CLR   TX_READY
          MOV   SBUF, A
          JNB   TX_READY, $
          RET

SERIAL_ISR:
          PUSH  ACC
          PUSH  PSW
          MOV   PSW, #08h       ; register bank 1
          JNB   RI, CHECK_TX
          CLR   RI
          MOV   R0, RX_HEAD
          MOV   @R0, SBUF
          INC   R0
          CJNE  R0, #BUF_END, STORE
          MOV   R0, #BUF
STORE:    MOV   RX_HEAD, R0
CHECK_TX: JNB   TI, ISR_DONE
          CLR   TI
          SETB  TX_READY
ISR_DONE: POP   PSW
          POP   ACC
          RETI

          END
