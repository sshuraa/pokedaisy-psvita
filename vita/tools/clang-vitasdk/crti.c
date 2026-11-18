/* GCC's crti.o / crtn.o for the clang toolchain: newlib's __libc_init_array calls _init, __libc_fini_array _fini;
   constructors run from .init_array, so both are empty. */
void _init(void) {}
void _fini(void) {}
