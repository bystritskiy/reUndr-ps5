/* PlayStation wording for an unchanged disc. The replacements are this port's own
 * strings; the disc's text files are read at start and rewritten into the data
 * folder, and the path layer opens those copies instead. MIT. */
#include "ui_text.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct row { uint32_t key;const uint16_t *text; };
#include "ui_text.inc"

struct file { const char *name;const struct row *rows;size_t count;int is_locale; };
static const struct file FILES[]={
 {"LANGUAGES/LANGUAGE_ENGLISH.bin",LANGUAGE_ENGLISH,sizeof LANGUAGE_ENGLISH/sizeof *LANGUAGE_ENGLISH,0},
 {"MEMCARD/XBOX_LOCALE_ENGLISH.loc",LOCALE_ENGLISH,sizeof LOCALE_ENGLISH/sizeof *LOCALE_ENGLISH,1},
 {"MEMCARD/XBOX_LOCALE_FRENCH.loc",LOCALE_FRENCH,sizeof LOCALE_FRENCH/sizeof *LOCALE_FRENCH,1},
};
#define FILE_COUNT (sizeof FILES/sizeof *FILES)
static char sources[FILE_COUNT][512],copies[FILE_COUNT][512];

static uint32_t u32(const unsigned char *p) { uint32_t v;memcpy(&v,p,4);return v; }
static void put32(unsigned char *p,uint32_t v) { memcpy(p,&v,4); }
static size_t chars(const uint16_t *text) { size_t n=0;while(text[n])n++;return n; }

static const uint16_t *replacement(const struct file *f,uint32_t key)
{
 for(size_t i=0;i<f->count;i++)if(f->rows[i].key==key)return f->rows[i].text;
 return NULL;
}

/* Length of the UTF-16 text at p, or SIZE_MAX if it runs past end. */
static size_t text_length(const unsigned char *p,const unsigned char *end)
{
 for(size_t n=0;p+n*2+2<=end;n++)if(!p[n*2]&&!p[n*2+1])return n;
 return SIZE_MAX;
}

/* Chunk: id, size, count, count pairs of key and offset, then the strings. */
static unsigned char *patch_language(const struct file *f,const unsigned char *in,size_t size,size_t *out_size)
{
 if(size<12||u32(in)!=0x39000||(uint64_t)u32(in+4)+8!=size)return NULL;
 uint32_t count=u32(in+8);
 size_t base=12+(uint64_t)count*8;
 if(base>size)return NULL;
 size_t capacity=size;
 for(size_t i=0;i<f->count;i++)capacity+=(chars(f->rows[i].text)+1)*2;
 unsigned char *out=malloc(capacity+4);
 if(!out)return NULL;
 memcpy(out,in,base);
 size_t pos=base;
 for(uint32_t i=0;i<count;i++) {
  const unsigned char *entry=in+12+i*8;
  uint32_t offset=u32(entry+4);
  size_t length=offset<size-base?text_length(in+base+offset,in+size):SIZE_MAX;
  if(length==SIZE_MAX) { free(out);return NULL; }
  const uint16_t *text=replacement(f,u32(entry));
  size_t chars_out=text?chars(text):length;
  const void *from=text?(const void *)text:(const void *)(in+base+offset);
  if(pos+(chars_out+1)*2+4>capacity+4) { free(out);return NULL; }
  put32(out+12+i*8+4,(uint32_t)(pos-base));
  memcpy(out+pos,from,chars_out*2);
  out[pos+chars_out*2]=out[pos+chars_out*2+1]=0;
  pos+=(chars_out+1)*2;
 }
 while(pos%4)out[pos++]=0;
 put32(out+4,(uint32_t)(pos-8));
 *out_size=pos;
 return out;
}

/* LOCH and LOCI headers, then LOCL: id, size, zero, count, count+1 offsets from
 * the LOCL, and strings closed by two UTF-16 zeros, as tools/nfs_assets.py writes it. */
static unsigned char *patch_locale(const struct file *f,const unsigned char *in,size_t size,size_t *out_size)
{
 if(size<136||memcmp(in,"LOCH",4)||memcmp(in+20,"LOCI",4)||memcmp(in+120,"LOCL",4))return NULL;
 uint32_t count=u32(in+132);
 if((uint64_t)136+((uint64_t)count+1)*4>size)return NULL;
 size_t capacity=size+(count+2)*8;
 for(size_t i=0;i<f->count;i++)capacity+=(chars(f->rows[i].text)+2)*2;
 unsigned char *out=calloc(1,capacity);
 if(!out)return NULL;
 memcpy(out,in,136);
 size_t first=16+4*((size_t)count+1),pos=120+first;
 for(uint32_t i=0;i<count;i++) {
  /* The disc's table has no closing offset: the last string runs to the end. */
  uint32_t a=u32(in+136+i*4),b=i+1<count?u32(in+136+(i+1)*4):(uint32_t)(size-120);
  if(a>b||b>size-120) { free(out);return NULL; }
  const uint16_t *text=replacement(f,i);
  size_t length=text?chars(text):(b-a)/2;
  const void *from=text?(const void *)text:(const void *)(in+120+a);
  if(!text)while(length&&!in[120+a+length*2-2]&&!in[120+a+length*2-1])length--;
  if(pos+(length+2)*2>capacity) { free(out);return NULL; }
  put32(out+136+i*4,(uint32_t)(pos-120));
  memcpy(out+pos,from,length*2);
  pos+=(length+2)*2;
 }
 while((pos-120)%4)pos++;
 put32(out+136+(size_t)count*4,(uint32_t)(pos-120));
 put32(out+124,(uint32_t)(pos-120));
 *out_size=pos;
 return out;
}

static unsigned char *read_all(const char *path,size_t *size)
{
 FILE *f=fopen(path,"rb");
 if(!f)return NULL;
 unsigned char *data=NULL;
 long length;
 if(!fseek(f,0,SEEK_END)&&(length=ftell(f))>0&&length<(1<<24)&&!fseek(f,0,SEEK_SET)&&(data=malloc((size_t)length))) {
  if(fread(data,1,(size_t)length,f)==(size_t)length)*size=(size_t)length;
  else { free(data);data=NULL; }
 }
 fclose(f);
 return data;
}

static int write_all(const char *path,const unsigned char *data,size_t size)
{
 char part[540];
 snprintf(part,sizeof part,"%s.part",path);
 FILE *f=fopen(part,"wb");
 if(!f)return 0;
 int ok=fwrite(data,1,size,f)==size;
 if(fclose(f))ok=0;
 return (ok&&!rename(part,path))||(remove(part),0);
}

void nfsu1_ui_text_prepare(const char *game_dir,const char *cache_dir)
{
 for(size_t i=0;i<FILE_COUNT;i++) {
  const struct file *f=&FILES[i];
  char source[512],copy[512];
  const char *leaf=strrchr(f->name,'/')+1;
  snprintf(source,sizeof source,"%s/%s",game_dir,f->name);
  snprintf(copy,sizeof copy,"%s/%s",cache_dir,leaf);
  size_t size=0,out_size=0;
  unsigned char *in=read_all(source,&size);
  if(!in)continue;
  unsigned char *out=(f->is_locale?patch_locale:patch_language)(f,in,size,&out_size);
  free(in);
  if(out&&write_all(copy,out,out_size)) {
   memcpy(sources[i],source,sizeof source);memcpy(copies[i],copy,sizeof copy);
  }
  else fprintf(stderr,"[UI] Keeping the disc text of %s\n",f->name);
  free(out);
 }
}

void nfsu1_ui_text_redirect(char *path,size_t size)
{
 for(size_t i=0;i<FILE_COUNT;i++)
  if(sources[i][0]&&!strcasecmp(path,sources[i])) { snprintf(path,size,"%s",copies[i]);return; }
}
