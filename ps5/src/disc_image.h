/* Unpacks an Xbox disc image (XDVDFS) on the console. MIT. */
#ifndef NFSU1_DISC_IMAGE_H
#define NFSU1_DISC_IMAGE_H

enum {
 NFSU1_IMAGE_OK,
 NFSU1_IMAGE_NOT_A_DISC,
 NFSU1_IMAGE_WRONG_DISC,
 NFSU1_IMAGE_DAMAGED,
 NFSU1_IMAGE_WRITE
};

/* Copies the disc into target. accept sees default.xbe before anything else is
 * copied and may refuse it; progress gets each new percentage. */
int nfsu1_image_extract(const char *image,const char *target,int (*accept)(const char *xbe),void (*progress)(unsigned));

#endif
