ENTRY(start)

SECTIONS
{
    /* The kernel loads at 1MB */
    . = 1M;

    .text :
    {
        /* CRITICAL: Put the Multiboot header here, BEFORE the code */
        *(.multiboot)
        *(.text)
    }

    .data :
    {
        *(.data)
    }

    .bss :
    {
        *(.bss)
    }
}