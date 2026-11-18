@ The fixture ROM's start (3ds/test/fixture_rom.c) for clang, in place of devkitARM's gba crt0: the cartridge header's
@ branch, room for the header (gbafix.py fills it), then main in Thumb. mGBA's HLE BIOS has set the stacks.
    .section .crt0, "ax"
    .arm
    .global _start
_start:
    b reset
    .fill 0xBC, 1, 0
reset:
    ldr r0, =main
    bx r0
    .pool
