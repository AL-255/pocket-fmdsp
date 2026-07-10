#ifndef PFM_CONFIG_H_INCLUDED
#define PFM_CONFIG_H_INCLUDED

/*
 * pocket-fmdsp build configuration.
 *
 * Target: STM32F103V (ARM Cortex-M3, no FPU/SIMD/DSP), 512kB Flash / 64kB RAM.
 * Everything here is integer-only and freestanding (no libc beyond string.h).
 */

#include <stdint.h>
#include <stdbool.h>

/* PFM_HOT: place the per-sample render functions in the .ramfunc SRAM section
   (0 wait-states). Enabled only for the real hardware build (-DPFM_RAMFUNC),
   where flash is under-spec at the 96 MHz overclock; a no-op elsewhere. */
#ifdef PFM_RAMFUNC
#define PFM_HOT __attribute__((section(".ramfunc")))
/* Force a hot lookup table into .data (SRAM). Without this, GCC leaves a
   never-written static array in .rodata (flash) even after dropping const. */
#define PFM_RAMDATA __attribute__((section(".data")))
#else
#define PFM_HOT
#define PFM_RAMDATA
#endif

/* OPNA master clock (YM2608 on PC-98). */
#define PFM_OPNA_MASTER_CLOCK 7987200u

/*
 * Native mix rate = master / 144 = 55466.66.. Hz, interleaved stereo int16.
 * The whole engine (FM/SSG/drum) is generated at this rate; the driver's
 * Timer-B ticks are counted in these samples.
 */
#define PFM_MIX_RATE (PFM_OPNA_MASTER_CLOCK / 144u) /* 55466 */

/* SSG raw generation rate = master / 32 = 249600 Hz (resampled to PFM_MIX_RATE). */
#define PFM_SSG_RATE (PFM_OPNA_MASTER_CLOCK / 32u) /* 249600 */

/*
 * Feature switches. Kept off for the embedded target to save flash/RAM/cycles.
 * The reference upstream also disables the hi-res paths by default.
 */
#ifndef PFM_ENABLE_ADPCM
#define PFM_ENABLE_ADPCM 0 /* ADPCM-B needs 256kB sample RAM; stubbed for now. */
#endif

/* Small helpers usable everywhere. */
static inline int32_t pfm_clamp16(int32_t v) {
  if (v < -32768) return -32768;
  if (v > 32767) return 32767;
  return v;
}

static inline uint16_t pfm_read16le(const uint8_t *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

#endif /* PFM_CONFIG_H_INCLUDED */
