ENTRY(_start)
MEMORY { rom : ORIGIN = 0x08000000, LENGTH = 32M  ewram : ORIGIN = 0x02000000, LENGTH = 256K }
SECTIONS {
    .text : { KEEP(*(.crt0)) *(.text*) } > rom
    .rodata : { *(.rodata*) } > rom
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } > ewram
    /DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.data*) }
}
