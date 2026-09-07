/* arch/x86/64/boot.S — Multiboot2 → Long Mode trampoline */

/* ------------------------------------------------------------------ */
/* Multiboot2 header                                                 */
/* ------------------------------------------------------------------ */

.set MB2_HEADER_MAGIC, 0xE85250D6
.set MB2_ARCH,         0
.set MB2_BOOT_MAGIC,   0x36D76289

.section .multiboot, "a"
.align 8

mb2_header_start:
    .long MB2_HEADER_MAGIC
    .long MB2_ARCH
    .long mb2_header_end - mb2_header_start

    /*
     * Checksum:
     *
     * magic + architecture + header_length + checksum == 0
     * modulo 2^32
     */
    .long -(MB2_HEADER_MAGIC + MB2_ARCH + \
            (mb2_header_end - mb2_header_start))

/* ------------------------------------------------------------------ */
/* Information request tag                                            */
/* ------------------------------------------------------------------ */

.align 8
mb2_tag_info:

    .short 1              /* type = information request */
    .short 1              /* optional */
    .long 32              /* size */

    .long 4               /* basic memory info */
    .long 6               /* memory map */
    .long 8               /* framebuffer info */
    .long 9               /* ELF sections */
    .long 14              /* ACPI old RSDP */
    .long 15              /* ACPI new RSDP */


/* ------------------------------------------------------------------ */
/* Framebuffer preference                                             */
/* ------------------------------------------------------------------ */

.align 8
mb2_tag_fb:

    .short 5              /* type = framebuffer */
    .short 0              /* flags */
    .long 20              /* size */

    .long 1024            /* width */
    .long 768             /* height */
    .long 32              /* bpp */


/* ------------------------------------------------------------------ */
/* End tag                                                            */
/* ------------------------------------------------------------------ */

.align 8
mb2_tag_end:

    .short 0
    .short 0
    .long 8

mb2_header_end:

.set PAGE_PRESENT,      0x001
.set PAGE_WRITABLE,     0x002
.set PAGE_HUGE,         0x080

/* ------------------------------------------------------------------ */
/* Boot BSS                                                          */
/* ------------------------------------------------------------------ */

.section .boot.bss, "aw", @nobits
.align 4096

.global stack_top

pml4:
    .skip 4096

pdpt:
    .skip 4096

pdt:
    .skip 4096

.align 16
stack_bottom:
    .skip 16 * 1024         # 16 KB stack
stack_top:


/* ------------------------------------------------------------------ */
/* Boot data                                                         */
/* ------------------------------------------------------------------ */

.section .boot.data, "aw"
.align 8

/* Temporary GDT */

gdt64:
    .quad 0x0000000000000000       /* 0x00: null */
    .quad 0x00AF9A000000FFFF       /* 0x08: ring-0 64-bit code */
    .quad 0x00AF92000000FFFF       /* 0x10: ring-0 data */
gdt64_end:

gdt64_desc:
    .word (gdt64_end - gdt64 - 1)
    .long gdt64                       /* 32-bit base for lgdt in protected mode */

.global multiboot_info_ptr
multiboot_info_ptr:
    .quad 0


/* ------------------------------------------------------------------ */
/* 32-bit entry                                                      */
/* ------------------------------------------------------------------ */

.section .boot.text, "ax"
.code32

.global _start
.type _start, @function

_start:
    cli

    /* -------------------------------------------------------------- */
    /* Verify Multiboot2 magic                                        */
    /* -------------------------------------------------------------- */

    cmpl $MB2_BOOT_MAGIC, %eax
    jne .no_multiboot2

    /*
     * EBX contains the physical address of the Multiboot2 information
     * structure.
     *
     * Save it immediately.
     */
    movl %ebx, multiboot_info_ptr

    /* -------------------------------------------------------------- */
    /* Temporary boot stack                                            */
    /* -------------------------------------------------------------- */

    movl $stack_top, %esp

    /* -------------------------------------------------------------- */
    /* Clear page tables                                               */
    /* -------------------------------------------------------------- */

    movl $pml4, %edi
    xorl %eax, %eax
    movl $3072, %ecx
    rep stosl


    /* -------------------------------------------------------------- */
    /* Identity mapping                                                */
    /*                                                                */
    /* PML4[0] -> PDPT                                                 */
    /* PDPT[0] -> PDT                                                  */
    /* PDT[0..7] -> 16 MiB                                            */
    /* -------------------------------------------------------------- */

    movl $pdpt, %eax
    orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    movl %eax, pml4

    movl $pdt, %eax
    orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    movl %eax, pdpt


    /* -------------------------------------------------------------- */
    /* High-half mapping                                               */
    /*                                                                */
    /* 0xFFFFFFFF80000000 -> physical 0                                */
    /* -------------------------------------------------------------- */

    movl $pdpt, %eax
    orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    movl %eax, pml4 + (511 * 8)

    movl $pdt, %eax
    orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    movl %eax, pdpt + (510 * 8)


    /* -------------------------------------------------------------- */
    /* 16 MiB of 2 MiB identity/high-half mapped memory               */
    /* -------------------------------------------------------------- */

    movl $pdt, %edi
    xorl %eax, %eax
    movl $8, %ecx

.fill_pdt:
    movl %eax, %edx
    orl $(PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE), %edx
    movl %edx, (%edi)

    addl $0x200000, %eax
    addl $8, %edi

    loop .fill_pdt

    /* -------------------------------------------------------------- */
    /* Load GDT                                                        */
    /* -------------------------------------------------------------- */

    lgdtl gdt64_desc

    /* -------------------------------------------------------------- */
    /* Enable PAE                                                      */
    /* CR4.PAE = bit 5                                                 */
    /* -------------------------------------------------------------- */

    .set CR4_PAE,          0x20
    .set CR4_OSFXSR,       0x200
    .set CR4_OSXMMEXCPT,   0x400

    movl %cr4, %eax
    orl $(CR4_PAE | CR4_OSFXSR | CR4_OSXMMEXCPT), %eax
    movl %eax, %cr4

    /* -------------------------------------------------------------- */
    /* Enable Long Mode                                                 */
    /* EFER.LME = bit 8                                                */
    /* -------------------------------------------------------------- */

    movl $0xC0000080, %ecx        /* IA32_EFER */
    rdmsr
    orl $(0x100 | 0x800), %eax    /* EFER.LME | EFER.NXE */
    wrmsr

    /* -------------------------------------------------------------- */
    /* Load PML4 into CR3                                               */
    /* -------------------------------------------------------------- */

    movl $pml4, %eax
    movl %eax, %cr3

    /* -------------------------------------------------------------- */
    /* Enable paging                                                    */
    /* CR0.PG = bit 31                                                  */
    /* -------------------------------------------------------------- */

    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0

    /*
     * At this point:
     *
     *   CR0.PG  = 1
     *   CR4.PAE = 1
     *   EFER.LME = 1
     *
     * The CPU is ready to enter long mode through a far jump.
     */

    ljmp $0x08, $_start64


.no_multiboot2:
    movl $0xDEADBEEF, %eax

.halt:
    hlt
    jmp .halt


/* ------------------------------------------------------------------ */
/* 64-bit entry                                                       */
/* ------------------------------------------------------------------ */

.code64
_start64:

    /*
     * The far jump loaded CS = 0x08.
     *
     * We are now executing in 64-bit long mode.
     */

    /* -------------------------------------------------------------- */
    /* Set up 64-bit stack                                             */
    /* -------------------------------------------------------------- */

    movabs $stack_top, %rsp

    /*
     * SysV AMD64 ABI:
     *
     * Before CALL:
     *     RSP must be 16-byte aligned.
     *
     * CALL pushes an 8-byte return address, therefore the callee
     * observes RSP % 16 == 8.
     *
     * Align explicitly rather than relying on the linker layout.
     */

    andq $-16, %rsp

    /* -------------------------------------------------------------- */
    /* kernel_early_init(magic, mbi)                                  */
    /* -------------------------------------------------------------- */

    /*
     * SysV AMD64 argument registers:
     *
     *   RDI = arg1
     *   RSI = arg2
     */

    movl $MB2_BOOT_MAGIC, %edi

    movl multiboot_info_ptr, %esi

    call kernel_early_init

    /* -------------------------------------------------------------- */
    /* pmm_init()                                                      */
    /* -------------------------------------------------------------- */

    call pmm_init


    testl %eax, %eax
    jnz .pmm_fail

    /* -------------------------------------------------------------- */
    /* Enter high-half kernel                                          */
    /* -------------------------------------------------------------- */

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

.pmm_fail:
    movl $0xBAD01, %eax

.pmm_halt:
    hlt
    jmp .pmm_halt