bits 32
section .multiboot
    align 4
    dd 0x1BADB002             ; Magic Number
    dd 0x00                   ; Flags
    dd - (0x1BADB002 + 0x00)  ; Checksum

section .text
global start
extern kernel_main

start:
    cli
    mov esp, stack_top
    call kernel_main
    hlt

section .bss
align 16
stack_bottom:
    resb 16384
stack_top: