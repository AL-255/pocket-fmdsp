/* FatFs <-> bit-banged SD-SPI glue (read-only). */
#include "ff.h"
#include "diskio.h"

extern int sd_init(void);
extern int sd_status(void);
extern int sd_read_blocks(unsigned lba, unsigned char *buf, unsigned count);
extern int sd_write_blocks(unsigned lba, const unsigned char *buf, unsigned count);

DSTATUS disk_status(BYTE pdrv) {
  (void)pdrv;
  return sd_status() ? STA_NOINIT : 0;
}

DSTATUS disk_initialize(BYTE pdrv) {
  (void)pdrv;
  return sd_init() ? STA_NOINIT : 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
  (void)pdrv;
  return sd_read_blocks((unsigned)sector, (unsigned char *)buff, count) ? RES_ERROR
                                                                        : RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
  (void)pdrv;
  return sd_write_blocks((unsigned)sector, (const unsigned char *)buff, count) ? RES_ERROR
                                                                               : RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  (void)pdrv;
  (void)buff;
  if (cmd == CTRL_SYNC) return RES_OK; /* writes block until the card is done */
  return RES_PARERR;
}
