/* FatFs R0.15 configuration for pocket-fmdsp: read-only, LFN, Shift-JIS names,
   one volume, minimal footprint. */
#define FFCONF_DEF	80386

/* Function config */
#define FF_FS_READONLY   1   /* read-only: we only browse and load songs */
#define FF_FS_MINIMIZE   1   /* keep opendir/readdir/open/read/lseek; drop write/manage */
#define FF_USE_FIND      0
#define FF_USE_MKFS      0
#define FF_USE_FASTSEEK  0
#define FF_USE_EXPAND    0
#define FF_USE_CHMOD     0
#define FF_USE_LABEL     0
#define FF_USE_FORWARD   0
#define FF_USE_STRFUNC   0

/* Locale / namespace */
#define FF_CODE_PAGE     932 /* Shift-JIS (Japanese) — matches our gfx font */
#define FF_USE_LFN       1   /* long file names, static working buffer */
#define FF_MAX_LFN       128
#define FF_LFN_UNICODE   0   /* OEM (Shift-JIS) API strings */
#define FF_LFN_BUF       128
#define FF_SFN_BUF       12
#define FF_FS_RPATH      0   /* absolute paths only (browser builds full paths) */

/* Volume / drive config */
#define FF_VOLUMES       1
#define FF_STR_VOLUME_ID 0
#define FF_MULTI_PARTITION 0
#define FF_MIN_SS        512
#define FF_MAX_SS        512
#define FF_LBA64         0
#define FF_MIN_GPT       0x10000000
#define FF_USE_TRIM      0

/* System config */
#define FF_FS_TINY       0
#define FF_FS_EXFAT      0
#define FF_FS_NORTC      1   /* read-only: no timestamps */
#define FF_NORTC_MON     1
#define FF_NORTC_MDAY    1
#define FF_NORTC_YEAR    2024
#define FF_FS_NOFSINFO   0
#define FF_FS_LOCK       0
#define FF_FS_REENTRANT  0   /* single-threaded: only the app task uses FatFs */
#define FF_FS_TIMEOUT    1000
