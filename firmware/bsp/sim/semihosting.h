#ifndef PFM_SEMIHOSTING_H
#define PFM_SEMIHOSTING_H
/*
 * ARM semihosting (BKPT 0xAB) — the QEMU/sim I/O channel. Lets the firmware use
 * the host's console and filesystem while running under QEMU
 * (`-semihosting-config enable=on`). Not used on real hardware.
 */
#include <stdint.h>
#include <stddef.h>

/* Console */
void sh_write0(const char *s);        /* write NUL-terminated string to stdout */
void sh_write_hex(uint32_t v);
void sh_write_dec(int32_t v);

/* Host files. mode: 0=r("rb"), 4=w("wb"), 8=a. Returns handle or -1. */
int sh_open(const char *path, int mode);
int sh_close(int fd);
/* Return bytes NOT written / read (semihosting convention); 0 == full success. */
int sh_write(int fd, const void *buf, size_t len);
int sh_read(int fd, void *buf, size_t len);
long sh_flen(int fd);                 /* file length in bytes, or -1 */

void sh_exit(int code);               /* terminate QEMU */

#endif /* PFM_SEMIHOSTING_H */
