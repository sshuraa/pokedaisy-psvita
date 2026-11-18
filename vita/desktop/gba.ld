ENTRY(_start)
SECTIONS {
    . = 0x08000000;
    .text : { KEEP(*(.crt0)) *(.text*) }
    .rodata : { *(.rodata*) }
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) }
}
