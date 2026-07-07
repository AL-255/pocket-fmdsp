#include "semihosting.h"

/* ARM semihosting operation numbers */
enum {
  SYS_OPEN = 0x01,
  SYS_CLOSE = 0x02,
  SYS_WRITE0 = 0x04,
  SYS_WRITE = 0x05,
  SYS_READ = 0x06,
  SYS_FLEN = 0x0C,
  SYS_EXIT = 0x18,
  SYS_EXIT_EXTENDED = 0x20,
};

static int sh_call(int op, void *arg) {
  register int r0 __asm__("r0") = op;
  register void *r1 __asm__("r1") = arg;
  __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
  return r0;
}

static size_t sh_strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

void sh_write0(const char *s) { sh_call(SYS_WRITE0, (void *)s); }

/* mode is the semihosting fopen index: 1='rb', 5='wb', 9='ab', ... */
int sh_open(const char *path, int mode) {
  uint32_t a[3] = {(uint32_t)path, (uint32_t)mode, (uint32_t)sh_strlen(path)};
  return sh_call(SYS_OPEN, a);
}

int sh_close(int fd) {
  uint32_t a[1] = {(uint32_t)fd};
  return sh_call(SYS_CLOSE, a);
}

int sh_write(int fd, const void *buf, size_t len) {
  uint32_t a[3] = {(uint32_t)fd, (uint32_t)buf, (uint32_t)len};
  return sh_call(SYS_WRITE, a); /* returns bytes NOT written */
}

int sh_read(int fd, void *buf, size_t len) {
  uint32_t a[3] = {(uint32_t)fd, (uint32_t)buf, (uint32_t)len};
  return sh_call(SYS_READ, a); /* returns bytes NOT read */
}

long sh_flen(int fd) {
  uint32_t a[1] = {(uint32_t)fd};
  return sh_call(SYS_FLEN, a);
}

void sh_exit(int code) {
  if (code == 0) {
    sh_call(SYS_EXIT, (void *)0x20026 /* ADP_Stopped_ApplicationExit */);
  } else {
    uint32_t a[2] = {0x20026, (uint32_t)code};
    sh_call(SYS_EXIT_EXTENDED, a);
  }
  for (;;) {
  }
}

static void put_uint(char *buf, uint32_t v, int base, int width) {
  static const char digits[] = "0123456789abcdef";
  char tmp[16];
  int n = 0;
  do {
    tmp[n++] = digits[v % base];
    v /= base;
  } while (v);
  while (n < width) tmp[n++] = '0';
  int i = 0;
  while (n) buf[i++] = tmp[--n];
  buf[i] = 0;
}

void sh_write_hex(uint32_t v) {
  char b[12] = "0x";
  put_uint(b + 2, v, 16, 8);
  sh_write0(b);
}

void sh_write_dec(int32_t v) {
  char b[16];
  int i = 0;
  uint32_t u = v < 0 ? (b[i++] = '-', (uint32_t)(-v)) : (uint32_t)v;
  put_uint(b + i, u, 10, 1);
  sh_write0(b);
}
