.set MB2_MAGIC,    0xE85250D6
.set MB2_ARCH,     0
.set MB2_LENGTH,   (mb2_header_end - mb2_header_start)
.set MB2_CHECKSUM, -(MB2_MAGIC + MB2_ARCH + MB2_LENGTH)   
.set KERNEL_VIRT_BASE, 0xC0000000
.set PAGE_PRESENT,     0x001
.set PAGE_WRITABLE,    0x002

.section .multiboot, "a"
.align 8
mb2_header_start:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long MB2_LENGTH
    .long MB2_CHECKSUM  /* checksum */

/* Information request tag */
.align 8
mb2_tag_info:
    .short 1        /* type = information request */
    .short 1        /* flags: bit 0 = optional — don't hard-fail boot
                       if any requested tag can't be satisfied */
    .long 32        /* size = 8 + 4*num_requests */
    .long 4         /* basic memory info */
    .long 6         /* memory map */
    .long 8         /* framebuffer info */
    .long 9         /* ELF sections */
    .long 14        /* ACPI old RSDP */
    .long 15        /* ACPI new RSDP */
    
/* Skip for now, GRUB handles ELF kernels fine */
/* Optional: ask the bootloader to set a preferred mode.
 * GRUB2 will try to honor this on BIOS; on UEFI it usually
 * gives you the current GOP mode regardless. */
.align 8
mb2_tag_fb:
    .short 5        /* type = framebuffer */
    .short 0
    .long 20
    .long 1024      /* preferred width */
    .long 768       /* preferred height */
    .long 32        /* preferred bpp */

/* Null terminator */
.align 8
mb2_tag_end:
    .short 0
    .short 0
    .long 8
mb2_header_end:

.section .boot.bss, "aw", @nobits

.align 16
stack_bottom:
    .skip 16 * 1024         # 16 KB stack
stack_top:

.section .boot.bss
.align 4096
boot_page_directory:
    .skip 4096
boot_page_table0:
    .skip 4096
boot_page_table1:
    .skip 4096
boot_page_table2:
    .skip 4096
boot_page_table3:
    .skip 4096

.section .boot.text, "ax"
.code32

.section .boot.data, "aw"
.align 4
.global multiboot_info_ptr
multiboot_info_ptr:
    .long 0

.section .boot.text, "ax"
.global _start
.type _start, @function

_start:
    cli

    /* Verify Multiboot2 magic */
    cmpl $0x36d76289, %eax
    jne .no_multiboot2

    /* Preserve the Multiboot2 handoff before using scratch registers. */
    movl %eax, %ebp
    movl %ebx, %esi

        /* Use the physical address of the high-linked bootstrap stack. */
        movl $stack_top, %esp

        /* Build a temporary identity map for the first 16 MiB. */
        movl $boot_page_directory, %edi
        xorl %eax, %eax
        movl $1024, %ecx
        rep stosl

        movl $boot_page_table0, %edi
        xorl %ebx, %ebx
        movl $4, %edx
    .fill_pt:
        movl $1024, %ecx
    .fill_pte:
        movl %ebx, %eax
        orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
        stosl
        addl $4096, %ebx
        loop .fill_pte
        addl $4096, %edi
        decl %edx
        jnz .fill_pt

        movl $boot_page_table0, %eax
        orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
        movl %eax, boot_page_directory
        movl $boot_page_table1, %eax
        orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
        movl %eax, boot_page_directory + 4
        movl $boot_page_table2, %eax
        orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
        movl %eax, boot_page_directory + 8
        movl $boot_page_table3, %eax
        orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
        movl %eax, boot_page_directory + 12

        /* Alias physical 0-16 MiB at 0xC0000000 for the kernel. */
        movl $boot_page_table0, %eax
        orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
        movl %eax, boot_page_directory + (768 * 4)
        movl $boot_page_table1, %eax
        orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
        movl %eax, boot_page_directory + (769 * 4)
        movl $boot_page_table2, %eax
        orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
        movl %eax, boot_page_directory + (770 * 4)
        movl $boot_page_table3, %eax
        orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
        movl %eax, boot_page_directory + (771 * 4)

        movl $boot_page_directory, %eax
        movl %eax, %cr3
        movl %cr0, %eax
        orl $0x80000000, %eax
        movl %eax, %cr0

        /* Run early initialization while the low identity map remains. */
        pushl %esi
        pushl %ebp
        call kernel_early_init
        addl $8, %esp
        call pmm_init
        testl %eax, %eax
        jnz .pmm_fail

        /* Enter the high-linked C entry point. */
        movl $kernel_main, %eax
        jmp *%eax

.pmm_fail:
    movl $0xBAD01, %eax
    jmp halt

.no_multiboot2:
    /* Can't printk yet, but we can hang with a known EAX value */
    movl $0xDEADBEEF, %eax
halt:
    hlt
    jmp halt 