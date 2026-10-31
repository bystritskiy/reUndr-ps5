#include "ui_text.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void put(unsigned char *p,uint32_t v) { memcpy(p,&v,4); }
static uint32_t get(const unsigned char *p) { uint32_t v;memcpy(&v,p,4);return v; }
static size_t wide(unsigned char *out,const char *text)
{
 size_t n=strlen(text);
 for(size_t i=0;i<=n;i++) { out[i*2]=(unsigned char)text[i];out[i*2+1]=0; }
 return (n+1)*2;
}
static void write_file(const char *path,const void *data,size_t size)
{
 FILE *f=fopen(path,"wb");assert(f);assert(fwrite(data,1,size,f)==size);fclose(f);
}
static size_t read_file(const char *path,unsigned char *data,size_t size)
{
 FILE *f=fopen(path,"rb");assert(f);size_t n=fread(data,1,size,f);fclose(f);return n;
}
static int has(const unsigned char *data,size_t size,const char *text)
{
 unsigned char want[256];size_t n=wide(want,text)-2;
 for(size_t i=0;i+n<=size;i++)if(!memcmp(data+i,want,n))return 1;
 return 0;
}

int main(void)
{
 char temp[]="/tmp/nfsu1-text-XXXXXX";assert(mkdtemp(temp));
 char path[512],game[256],cache[256],copy[512];
 snprintf(game,sizeof game,"%s/game",temp);snprintf(cache,sizeof cache,"%s/cache",temp);
 mkdir(game,0777);mkdir(cache,0777);
 snprintf(path,sizeof path,"%s/LANGUAGES",game);mkdir(path,0777);
 snprintf(path,sizeof path,"%s/MEMCARD",game);mkdir(path,0777);

 unsigned char lang[256]={0},out[4096];
 put(lang,0x39000);put(lang+8,2);
 put(lang+12,0xEEEB248E);put(lang+16,0);
 size_t base=28,used=wide(lang+base,"Press START button");
 put(lang+20,0x11111111);put(lang+24,(uint32_t)used);
 used+=wide(lang+base+used,"Keep me");
 size_t size=(base+used+3)&~(size_t)3;
 put(lang+4,(uint32_t)size-8);
 snprintf(path,sizeof path,"%s/LANGUAGES/LANGUAGE_ENGLISH.bin",game);write_file(path,lang,size);

 unsigned char loc[512]={0};
 memcpy(loc,"LOCH",4);memcpy(loc+20,"LOCI",4);memcpy(loc+120,"LOCL",4);
 enum { ROWS=15 };
 put(loc+132,ROWS);
 size_t first=16+4*ROWS,at=first;
 for(unsigned i=0;i<ROWS;i++) { put(loc+136+i*4,(uint32_t)at);at+=wide(loc+120+at,i==14?"Unchanged":i?"Press A":"Xbox full")+2; }
 while(at%4)at++;
 put(loc+124,(uint32_t)at);
 snprintf(path,sizeof path,"%s/MEMCARD/XBOX_LOCALE_ENGLISH.loc",game);write_file(path,loc,120+at);

 nfsu1_ui_text_prepare(game,cache);

 snprintf(path,sizeof path,"%s/LANGUAGES/LANGUAGE_ENGLISH.bin",game);
 nfsu1_ui_text_redirect(path,sizeof path);
 snprintf(copy,sizeof copy,"%s/LANGUAGE_ENGLISH.bin",cache);assert(!strcmp(path,copy));
 size_t n=read_file(path,out,sizeof out);
 assert(get(out)==0x39000&&get(out+4)+8==n&&!(n%4)&&get(out+8)==2);
 assert(has(out,n,"Press OPTIONS button")&&has(out,n,"Keep me")&&!has(out,n,"Press START"));
 assert(has(out+28+get(out+24),7*2,"Keep me"));

 snprintf(path,sizeof path,"%s/memcard/xbox_locale_english.LOC",game);
 nfsu1_ui_text_redirect(path,sizeof path);
 snprintf(copy,sizeof copy,"%s/XBOX_LOCALE_ENGLISH.loc",cache);assert(!strcmp(path,copy));
 n=read_file(path,out,sizeof out);
 assert(!memcmp(out+120,"LOCL",4)&&get(out+132)==ROWS&&get(out+124)+120==n);
 assert(has(out,n,"Your PS5 has insufficient space")&&has(out,n,"Unchanged")&&has(out,n,"Unable to load %1. Press Cross")==1);
 assert(!has(out,n,"Xbox full"));

 snprintf(path,sizeof path,"%s/GLOBAL/other.bin",game);
 char before[512];strcpy(before,path);
 nfsu1_ui_text_redirect(path,sizeof path);assert(!strcmp(path,before));
 /* A file that is not what it should be stays the disc's. */
 snprintf(path,sizeof path,"%s/MEMCARD/XBOX_LOCALE_FRENCH.loc",game);write_file(path,"LOCH",4);
 nfsu1_ui_text_prepare(game,cache);
 strcpy(before,path);nfsu1_ui_text_redirect(path,sizeof path);assert(!strcmp(path,before));

 const char *files[]={"game/LANGUAGES/LANGUAGE_ENGLISH.bin","game/MEMCARD/XBOX_LOCALE_ENGLISH.loc","game/MEMCARD/XBOX_LOCALE_FRENCH.loc",
  "cache/LANGUAGE_ENGLISH.bin","cache/XBOX_LOCALE_ENGLISH.loc"};
 for(unsigned i=0;i<5;i++) { snprintf(path,sizeof path,"%s/%s",temp,files[i]);unlink(path); }
 const char *dirs[]={"game/LANGUAGES","game/MEMCARD","game","cache",""};
 for(unsigned i=0;i<5;i++) { snprintf(path,sizeof path,"%s/%s",temp,dirs[i]);rmdir(path); }
 puts("UI text: recipe applied to language and locale files, others untouched PASS");
}
