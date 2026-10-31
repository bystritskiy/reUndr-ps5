#include "fmv_replace.h"
#include "fmv_video.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdint.h>
#include <unistd.h>

#ifndef NFS_HD_ROOT
#error Test root required
#endif
#define AV1_FRAMES 120
#define AV1_SIZE 64
#define AV1_HASH(frame) (0x10000u+(frame))
#define AV1_SOURCE 16
#define MOVIE(name) "\\Device\\CdRom0\\MOVIES\\" name

/* The movie is grey, luma 30+frame; frame 12 repeats the hash of frame 11. */
static void write_pack(const char *path,const char *magic,uint32_t version,uint32_t base)
{
    FILE *in=fopen(NFS_HD_ROOT "/test.ivf","rb");assert(in);
    FILE *o=fopen(path,"wb");assert(o);
    const uint32_t header[8]={0,version,AV1_SOURCE,AV1_SOURCE,AV1_SIZE,AV1_SIZE,AV1_FRAMES,AV1_SIZE*AV1_SIZE*4};
    const uint64_t data_offset=40+AV1_FRAMES*8;
    fwrite(header,sizeof header,1,o);fwrite(&data_offset,sizeof data_offset,1,o);
    fseek(o,0,SEEK_SET);fwrite(magic,4,1,o);fseek(o,40,SEEK_SET);
    for (uint32_t frame=0;frame<AV1_FRAMES;frame++) {
        const uint32_t entry[2]={base+(frame==12 ? 11 : frame),frame};
        fwrite(entry,sizeof entry,1,o);
    }
    unsigned char ivf_header[32];
    assert(fread(ivf_header,sizeof ivf_header,1,in)==1);
    int c;
    while ((c=fgetc(in))!=EOF) fputc(c,o);
    fclose(in);fclose(o);
}

static uint32_t show(uint32_t hash)
{
    NfsFmvFrame frame;
    assert(nfsu1_fmv_find(hash,AV1_SOURCE,AV1_SOURCE,&frame));
    assert(frame.width==AV1_SIZE && frame.bytes==AV1_SIZE*AV1_SIZE*4);
    const unsigned char *pixels=nfsu1_fmv_load(&frame);
    assert(pixels);
    const int expected=((30+(int)frame.frame-16)*298+128)>>8;
    int blue=pixels[0],grey=pixels[AV1_SIZE*2+1];
    assert(blue-expected<5 && expected-blue<5 && grey-expected<5 && expected-grey<5 && pixels[3]==255);
    return frame.frame;
}

static void test_movie(void)
{
    nfsu1_fmv_movie(MOVIE("VIDEO.mad"));
    for (uint32_t frame=0;frame<=4;frame++) assert(show(AV1_HASH(frame))==frame);
    assert(show(AV1_HASH(3))==4);                 /* uploaded again: stays on screen */
    assert(show(AV1_HASH(4))==4);
    assert(show(AV1_HASH(11))==11);               /* frames the game skipped */
    assert(show(AV1_HASH(11))==12);               /* the repeat moves on */
    assert(show(AV1_HASH(13))==13);
    for (uint32_t frame=14;frame<AV1_FRAMES;frame++) assert(show(AV1_HASH(frame))==frame);
    assert(show(AV1_HASH(0))==119);               /* a late upload does not rewind */
    nfsu1_fmv_movie(MOVIE("video.mad"));
    assert(show(AV1_HASH(0))==0);                 /* opened again, it starts over */
    assert(show(AV1_HASH(1))==1);
    nfsu1_fmv_video_stop();
    assert(show(AV1_HASH(2))==2);                 /* a stopped decoder comes back */
    nfsu1_fmv_movie(MOVIE("video.mad"));
    NfsFmvFrame end;
    assert(!nfsu1_fmv_find(AV1_HASH(100),AV1_SOURCE,AV1_SOURCE,&end));  /* a far frame does not open it */
    assert(show(AV1_HASH(2))==2);                 /* the opening still does */
    assert(show(AV1_HASH(3))==3);
    nfsu1_fmv_video_stop();
    puts("fmv movie ok");
}

int main(int argc,char **argv)
{
    int invalid=argc>1 && !strcmp(argv[1],"invalid");
    mkdir(NFS_HD_ROOT "/fmv",0777);
    mkdir(NFS_HD_ROOT "/outside",0777);
    mkdir(NFS_HD_ROOT "/outside/fmv",0777);
    write_pack(NFS_HD_ROOT "/outside/fmv/outside.fmv","NFMV",2,0x20000);
    setenv("NFSU1_HD_DIR",NFS_HD_ROOT "/outside",1);
    write_pack(NFS_HD_ROOT "/fmv/old.fmv","NFMV",1,0x30000);
    if (invalid) write_pack(NFS_HD_ROOT "/fmv/video.fmv","BAD!",2,AV1_HASH(0));
    else write_pack(NFS_HD_ROOT "/fmv/video.fmv","NFMV",2,AV1_HASH(0));
    NfsFmvFrame frame;
    if (invalid) {
        nfsu1_fmv_movie(MOVIE("video.mad"));
        assert(!nfsu1_fmv_find(AV1_HASH(0),AV1_SOURCE,AV1_SOURCE,&frame));
        puts("fmv invalid pack ignored");
        return 0;
    }
    assert(!nfsu1_fmv_find(AV1_HASH(0),AV1_SOURCE,AV1_SOURCE,&frame));  /* no movie open yet */
    nfsu1_fmv_movie("\\Device\\CdRom0\\GLOBAL\\video.bun");
    assert(!nfsu1_fmv_find(AV1_HASH(0),AV1_SOURCE,AV1_SOURCE,&frame));
    nfsu1_fmv_movie(MOVIE("other.mad"));
    assert(!nfsu1_fmv_find(AV1_HASH(0),AV1_SOURCE,AV1_SOURCE,&frame));
    nfsu1_fmv_movie(MOVIE("old.mad"));
    assert(!nfsu1_fmv_find(0x30000,AV1_SOURCE,AV1_SOURCE,&frame));      /* an old BC7 pack */
    nfsu1_fmv_movie(MOVIE("video.mad"));
    assert(!nfsu1_fmv_find(0x3333,AV1_SOURCE,AV1_SOURCE,&frame));
    assert(!nfsu1_fmv_find(AV1_HASH(0),8,8,&frame));
    assert(!nfsu1_fmv_find(0x20000,AV1_SOURCE,AV1_SOURCE,&frame));      /* another movie's pack */
    test_movie();
    nfsu1_fmv_movie(MOVIE("outside.mad"));
    assert(nfsu1_fmv_find(0x20000,AV1_SOURCE,AV1_SOURCE,&frame));       /* a pack kept outside the app */
    assert(nfsu1_fmv_load(&frame));
    nfsu1_fmv_video_stop();
    const uint32_t texels[64*64]={[64*63]=0xAABBCCDD};
    setenv("NFSU1_FMV_CAPTURE",NFS_HD_ROOT "/capture.txt",1);
    nfsu1_fmv_capture(0x1234,64,64,texels);
    FILE *c=fopen(NFS_HD_ROOT "/capture.txt","r");assert(c);
    char line[1024];assert(fgets(line,sizeof line,c));fclose(c);
    assert(!strncmp(line,"00001234 64 64 00000000 ",24));
    assert(strstr(line,"AABBCCDD\n") && strlen(line)==15+64*9);
    puts("fmv ok");
    return 0;
}
