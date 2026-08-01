.section .text
.global _start
.type _start, @function

_start:
    xor %rbp, %rbp
    and $-16, %rsp
    call neutrino_crt_start
    ud2

.size _start, . - _start

.section .note.GNU-stack,"",@progbits
