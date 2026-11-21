#ifndef NFSU1_HD_TEXTURES_H
#define NFSU1_HD_TEXTURES_H
#include <stdint.h>
typedef struct {
    uint32_t color, source_w, source_h, source_bytes, sample_hash;
    uint32_t width, height, hash_lo, hash_hi;
    char file[48];
} NfsHdTexture;
/* What nfsu1_hd_load returned: color 0 = BGRA8, level 0 only (NHD1);
 * 0x0C/0x0E/0x0F = BC1/BC2/BC3 blocks of `levels` mips, largest first (NHD2). */
typedef struct { uint32_t color, levels, bytes; } NfsHdPixels;
const NfsHdTexture *nfsu1_hd_find(const void *source, uint32_t color,
                                uint32_t w, uint32_t h, uint32_t bytes,
                                uint32_t sample_hash);
/* P8 (0x0B), only with NFSU1_HD_P8=1, else NULL. The record's strong key is
 * FNV-1a over the w*h indices then the palette in use (source_bytes = both),
 * so the same indices under a changed palette (paint, blinking) never match;
 * sample_hash is bytes_hash() of the indices alone. */
const NfsHdTexture *nfsu1_hd_find_p8(const void *indices, uint32_t w, uint32_t h,
                                     const void *palette, uint32_t palette_bytes,
                                     uint32_t sample_hash);
/* malloc'd pixels (the caller frees them), or NULL. */
void *nfsu1_hd_load(const NfsHdTexture *texture, NfsHdPixels *info);
/* NHD2 files held in RAM so far (NFSU1_HD_PRELOAD=1). */
uint32_t nfsu1_hd_preloaded(void);
#endif
