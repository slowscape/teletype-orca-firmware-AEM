// Minimal syscall stubs for newlib on bare-metal AVR32
// These are needed because assert.h / stdio.h pull in symbols
// that newlib's libc expects from the OS.

int _isatty(int fd) {
    (void)fd;
    return 1;
}
