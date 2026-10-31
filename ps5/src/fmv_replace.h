#ifndef NFSU1_FMV_REPLACE_H
#define NFSU1_FMV_REPLACE_H
#include <stdint.h>
typedef struct {
    const void *pack;
    int restart;
    uint32_t frame, width, height, bytes;
} NfsFmvFrame;
void nfsu1_fmv_movie(const char *xbox_path);
int nfsu1_fmv_find(uint32_t hash, uint32_t w, uint32_t h, NfsFmvFrame *out);
const void *nfsu1_fmv_load(const NfsFmvFrame *frame);
void nfsu1_fmv_capture(uint32_t hash, uint32_t w, uint32_t h, const void *pixels);
#endif
