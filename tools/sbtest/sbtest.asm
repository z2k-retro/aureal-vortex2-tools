; SBTEST.COM - minimal Sound Blaster check for tracing the Vortex 2's SB Pro
; emulation. Assumes A220 I5 D1 (the Vortex default). Steps, each reported:
;   1. DSP reset (expects 0AAh)   2. DSP version (E1h)
;   3. one second of 8-bit 11 kHz single-cycle DMA (cmd 14h) on DMA 1,
;      waiting up to ~3 s for IRQ 5      4. mixer master volume read
; Prints a marker before each step so the trace can be lined up.
org 100h
BASE equ 220h

start:
    mov dx, msg_hello
    call puts

; ---- 1. reset
    mov dx, msg_reset
    call puts
    mov dx, BASE+6
    mov al, 1
    out dx, al
    mov cx, 100
.d1: in al, dx
    loop .d1
    xor al, al
    out dx, al
    mov cx, 0FFFFh
.w1: mov dx, BASE+0Eh
    in al, dx
    test al, 80h
    jnz .r1
    loop .w1
    mov dx, msg_fail
    call puts
    jmp version
.r1: mov dx, BASE+0Ah
    in al, dx
    call hexbyte
    call crlf

; ---- 2. version
version:
    mov dx, msg_ver
    call puts
    mov al, 0E1h
    call dspw
    call dspr
    call hexbyte
    mov al, '.'
    call putc
    call dspr
    call hexbyte
    call crlf

; ---- 3. DMA playback
    mov dx, msg_dma
    call puts
    ; fill buffer: 220 Hz square wave, 8-bit unsigned
    mov di, buf
    mov cx, BUFLEN
    xor bx, bx
.f1: mov al, 060h
    cmp bx, 25
    jb .f2
    mov al, 0A0h
.f2: stosb
    inc bx
    cmp bx, 50
    jb .f3
    xor bx, bx
.f3: loop .f1
    ; physical address
    mov ax, ds
    mov dx, ax
    shl ax, 4
    shr dx, 12
    add ax, buf
    adc dx, 0
    mov [physlo], ax
    mov [physpg], dl
    ; would it cross a 64K page? then give up on this step
    mov bx, ax
    add bx, BUFLEN-1
    jnc .nocross
    mov dx, msg_cross
    call puts
    jmp mixer
.nocross:
    ; hook IRQ 5 (INT 0Dh)
    mov ax, 350Dh
    int 21h
    mov [oldoff], bx
    mov [oldseg], es
    mov dx, isr
    mov ax, 250Dh
    int 21h
    in al, 21h
    mov [oldmask], al
    and al, 0DFh
    out 21h, al
    ; speaker on, time constant for 11025 Hz
    mov al, 0D1h
    call dspw
    mov al, 40h
    call dspw
    mov al, 165
    call dspw
    ; DMA channel 1: single, read (memory->device)
    mov al, 5
    out 0Ah, al
    xor al, al
    out 0Ch, al
    mov al, 49h
    out 0Bh, al
    mov ax, [physlo]
    out 02h, al
    mov al, ah
    out 02h, al
    mov al, [physpg]
    out 83h, al
    mov ax, BUFLEN-1
    out 03h, al
    mov al, ah
    out 03h, al
    mov al, 1
    out 0Ah, al
    ; start: 8-bit single-cycle output
    mov byte [gotirq], 0
    mov al, 14h
    call dspw
    mov ax, BUFLEN-1
    call dspw
    mov al, ah
    call dspw
    ; wait up to ~3 s (55 BIOS ticks)
    push ds
    xor ax, ax
    mov es, ax
    mov bx, [es:46Ch]
.wt: cmp byte [gotirq], 0
    jne .done
    mov ax, [es:46Ch]
    sub ax, bx
    cmp ax, 55
    jb .wt
.done:
    pop ds
    cmp byte [gotirq], 0
    jne .ok
    mov dx, msg_noirq
    call puts
    jmp .rest
.ok: mov dx, msg_irq
    call puts
.rest:
    ; DMA count left
    mov dx, msg_left
    call puts
    xor al, al
    out 0Ch, al
    in al, 03h
    mov bl, al
    in al, 03h
    call hexbyte
    mov al, bl
    call hexbyte
    call crlf
    mov al, 0D3h
    call dspw
    mov al, [oldmask]
    out 21h, al
    push ds
    mov dx, [oldoff]
    mov ax, [oldseg]
    mov ds, ax
    mov ax, 250Dh
    int 21h
    pop ds

; ---- 4. mixer
mixer:
    mov dx, msg_mix
    call puts
    mov dx, BASE+4
    mov al, 22h
    out dx, al
    inc dx
    in al, dx
    call hexbyte
    call crlf
    mov dx, msg_end
    call puts
    mov ax, 4C00h
    int 21h

isr:
    push ax
    push dx
    mov dx, BASE+0Eh
    in al, dx
    mov byte [cs:gotirq], 1
    mov al, 20h
    out 20h, al
    pop dx
    pop ax
    iret

dspw:                   ; write AL to the DSP
    push dx
    push ax
    mov dx, BASE+0Ch
    mov cx, 0FFFFh
.l: in al, dx
    test al, 80h
    jz .w
    loop .l
.w: pop ax
    out dx, al
    pop dx
    ret
dspr:                   ; read a DSP byte into AL (0FFh on timeout)
    push dx
    mov dx, BASE+0Eh
    mov cx, 0FFFFh
.l: in al, dx
    test al, 80h
    jnz .r
    loop .l
    mov al, 0FFh
    pop dx
    ret
.r: mov dx, BASE+0Ah
    in al, dx
    pop dx
    ret
hexbyte:
    push ax
    shr al, 4
    call hexdig
    pop ax
    and al, 0Fh
hexdig:
    add al, '0'
    cmp al, '9'
    jbe .p
    add al, 7
.p: jmp putc
putc:
    push dx
    mov dl, al
    mov ah, 2
    int 21h
    pop dx
    ret
crlf:
    mov dx, msg_crlf
puts:
    mov ah, 9
    int 21h
    ret

msg_hello db 'SBTEST: Sound Blaster at 220h, IRQ 5, DMA 1',13,10,'$'
msg_reset db 'STEP 1 DSP reset: $'
msg_fail  db 'no response',13,10,'$'
msg_ver   db 'STEP 2 DSP version: $'
msg_dma   db 'STEP 3 DMA playback, 1 s at 11 kHz...',13,10,'$'
msg_cross db '  buffer crosses a 64K page, skipped',13,10,'$'
msg_irq   db '  IRQ 5 received',13,10,'$'
msg_noirq db '  no IRQ within 3 s',13,10,'$'
msg_left  db '  DMA count left: $'
msg_mix   db 'STEP 4 mixer master volume: $'
msg_end   db 'SBTEST done',13,10,'$'
msg_crlf  db 13,10,'$'
gotirq  db 0
oldmask db 0
oldoff  dw 0
oldseg  dw 0
physlo  dw 0
physpg  db 0
BUFLEN equ 11025
buf:
