# arch/x86/64/gdt.S - Global Descriptor Table (32-bit Long Mode)
# Author:   amity
# Date:     Mon Aug 31 12:19:51 2026
# Copyright © 2026 OwlyNest

# ==============================
# Alignment instructions:

# Instructions               begin at col 5
# First operand              begins at col 10
# Second operand             begins at col 20 (if possible)

# Header comments            begin at col 5
# In-line comments           begin at col 40
# Labels                     begin at col 1, end with ':'
# Section dividers           use 30 '=' characters
# (Sub)Section titles        use 3 '=' characters before and after

.intel_syntax noprefix

.global gdt_install
.global tss_set_rsp0

.global gdt_start
.global gdt_start_64
.global tss_struct
.global tss_end

/* ==============================
 * Selectors
 * ============================== */

.set SEL_KCODE, 0x08
.set SEL_KDATA, 0x10
.set SEL_UCODE, 0x18
.set SEL_UDATA, 0x20
.set SEL_TSS,   0x28
.set SEL_TSS_HI, 0x30

/* ==============================
 * TSS
 * ============================== */

/*
 * x86-64 TSS is 104 bytes.
 * The descriptor itself is 16 bytes.
 */

.align 4
tss_struct:
    .long 0                  /* reserved / previous task link */
    .quad 0x90000            /* rsp0 */
    .quad 0                  /* rsp1 */
    .quad 0                  /* rsp2 */
    .quad 0                  /* reserved */
    .quad stack_top          /* ist1 */
    .quad 0                  /* ist2 */
    .quad 0                  /* ist3 */
    .quad 0                  /* ist4 */
    .quad 0                  /* ist5 */
    .quad 0                  /* ist6 */
    .quad 0                  /* ist7 */
    .quad 0                  /* reserved */
    .word 0                  /* reserved */
    .word 0x68               /* I/O map base */

tss_end:

/* ==============================
 * GDT
 * ============================== */

.align 4
gdt_start:

    /* 0: Null descriptor */
    .quad 0x0000000000000000

    /* 1: Kernel code (0x08) */
    .word 0xFFFF
    .word 0x0000
    .byte 0x00
    .byte 0x9A
    .byte 0xAF
    .byte 0x00

    /* 2: Kernel data (0x10) */
    .word 0xFFFF
    .word 0x0000
    .byte 0x00
    .byte 0x92
    .byte 0xCF
    .byte 0x00

    /* 3: User code (0x18) */
    .word 0xFFFF
    .word 0x0000
    .byte 0x00
    .byte 0xFA
    .byte 0xAF
    .byte 0x00

    /* 4: User data (0x20) */
    .word 0xFFFF
    .word 0x0000
    .byte 0x00
    .byte 0xF2
    .byte 0xCF
    .byte 0x00
    /* TSS low (8 bytes) — filled by C code */
    .quad 0x0000000000000000   /* 0x28: TSS low */
gdt_start_64:
    .quad 0x0000000000000000   /* 0x30: high 32 bits of TSS base */

gdt_end:

gdt_ptr:
    .word gdt_end - gdt_start - 1
    .quad gdt_start

/* ==============================
 * gdt_install
 * ============================== */

gdt_install:

    /*
     * Build the 16-byte TSS descriptor.
     *
     * Descriptor layout:
     *
     *   +0  limit[15:0]
     *   +2  base[15:0]
     *   +4  base[23:16]
     *   +5  type/access = 0x89
     *   +6  limit[19:16] + flags
     *   +7  base[31:24]
     *
     * High 32 bits:
     *
     *   +8  base[63:32]
     *   +12 reserved = 0
     */

    lea rax, tss_struct

    /* limit = 104 - 1 = 0x67 */
    mov word ptr [gdt_start + SEL_TSS + 0], 0x67

    /* base[15:0] */
    mov word ptr [gdt_start + SEL_TSS + 2], ax

    /* base[23:16] */
    shr rax, 16
    mov byte ptr [gdt_start + SEL_TSS + 4], al

    /* type = available 64-bit TSS */
    mov byte ptr [gdt_start + SEL_TSS + 5], 0x89

    /* limit[19:16] = 0, flags = 0 */
    mov byte ptr [gdt_start + SEL_TSS + 6], 0

    /* base[31:24] */
    shr rax, 8
    mov byte ptr [gdt_start + SEL_TSS + 7], al

    /* base[63:32] */
    shr rax, 8
    mov dword ptr [gdt_start + SEL_TSS + 8], eax

    /* reserved */
    mov dword ptr [gdt_start + SEL_TSS + 12], 0

    /*
     * Load GDT.
     */

    lgdt [gdt_ptr]

    /*
     * Load TSS.
     */

    mov ax, SEL_TSS
    ltr ax

    /*
     * Reload data segments.
     */

    mov ax, SEL_KDATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    /*
     * Reload CS.
     *
     * GAS does not accept:
     *
     *     jmp SEL_KCODE:flush_cs
     *
     * in 64-bit mode.
     *
     * A far return gives us the same result.
     */

    lea rax, flush_cs
    push SEL_KCODE
    push rax

    cli
    rex.w retf

flush_cs:
    ret

/* ==============================
 * tss_set_rsp0
 * ============================== */

/* void tss_set_rsp0(uint64_t rsp) */

tss_set_rsp0:
    mov qword ptr [tss_struct + 4], rdi
    ret