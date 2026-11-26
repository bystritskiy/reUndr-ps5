/* reUndr PS5, MIT. Labels from <game>/port_ui.txt: "label<TAB>replacement"
 * lines in the game font's 8-bit encoding, '#' comments, and the directive
 * "!hide-trailers". Without the file the rows keep the port's own labels.
 *
 * Second language: <game>/lang/lang.txt has "text<TAB>first<TAB>second",
 * "voice<TAB>first<TAB>second" and "music<TAB>first<TAB>second" lines in
 * the same encoding, and the second choice's files sit in lang/text/,
 * lang/voice/ and lang/music/ at their game paths. The choice in force at
 * start stays until the next start; the second text also turns the label
 * table off. */
#include "port_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#define MAX_ROWS 64
#define KINDS 3
static struct {char *from,*to;} s_rows[MAX_ROWS];
static int s_count=-1,s_hide_trailers;
static char *s_text;
static const char *const s_kind[KINDS]={"text","voice","music"};
static char *s_lang_text,*s_lang_name[KINDS][2];
static int s_lang=-1,s_lang_kinds[KINDS],s_active[KINDS],s_chosen[KINDS];
static char *read_text(const char *name)
{
    const char *dir=getenv("NFSU2_GAME_DIR");char path[1024];
    if(!dir)return NULL;
    snprintf(path,sizeof(path),"%s/%s",dir,name);
    FILE *f=fopen(path,"rb");if(!f)return NULL;
    fseek(f,0,SEEK_END);long size=ftell(f);fseek(f,0,SEEK_SET);
    char *text=NULL;
    if(size>0&&size<=65536&&(text=malloc((size_t)size+1))){size_t got=fread(text,1,(size_t)size,f);text[got]=0;}
    fclose(f);return text;
}
static void load(void)
{
    if(s_count>=0)return;
    s_count=0;
    if(!(s_text=read_text("port_ui.txt")))return;
    for(char *line=strtok(s_text,"\r\n");line;line=strtok(NULL,"\r\n")){
        if(line[0]=='#')continue;
        if(!strcmp(line,"!hide-trailers")){s_hide_trailers=1;continue;}
        char *tab=strchr(line,'\t');
        if(!tab||tab==line||!tab[1]||s_count==MAX_ROWS)continue;
        *tab=0;s_rows[s_count].from=line;s_rows[s_count].to=tab+1;s_count++;
    }
}
static void lang_load(void)
{
    if(s_lang>=0)return;
    s_lang=0;
    if(!(s_lang_text=read_text("lang/lang.txt")))return;
    char *save=NULL;
    for(char *line=strtok_r(s_lang_text,"\r\n",&save);line;line=strtok_r(NULL,"\r\n",&save)){
        char *first=strchr(line,'\t'),*second=first?strchr(first+1,'\t'):NULL;
        if(line[0]=='#'||!second||second==first+1||!second[1])continue;
        *first=*second=0;
        for(int k=0;k<KINDS;k++)
            if(!strcmp(line,s_kind[k])&&!s_lang_name[k][0]){s_lang_name[k][0]=first+1;s_lang_name[k][1]=second+1;}
    }
    for(int k=0;k<KINDS;k++)if(s_lang_name[k][0])s_lang_kinds[s_lang++]=k;
}
int nfsu1_port_ui_custom(void){load();return s_count>0;}
/* With a language screen the trailers entry opens it instead of hiding. */
int nfsu1_port_ui_hide_trailers(void){load();return s_hide_trailers&&!nfsu1_port_lang_count();}
const char *nfsu1_port_ui_text(const char *text)
{
    load();
    if(s_active[0])return text;
    for(int i=0;i<s_count;i++)
        if(!strcmp(text,s_rows[i].from))return s_rows[i].to;
    return text;
}
int nfsu1_port_lang_count(void){lang_load();return s_lang;}
int nfsu1_port_lang_kind(int i){lang_load();return i>=0&&i<s_lang?s_lang_kinds[i]:-1;}
const char *nfsu1_port_lang_name(int kind,int choice)
{
    lang_load();
    return kind>=0&&kind<KINDS&&s_lang_name[kind][0]?s_lang_name[kind][choice!=0]:"";
}
int nfsu1_port_lang_choice(int kind){return kind>=0&&kind<KINDS?s_chosen[kind]:0;}
int nfsu1_port_lang_active(int kind){return kind>=0&&kind<KINDS?s_active[kind]:0;}
const char *nfsu1_port_lang_kind_name(int kind){return kind>=0&&kind<KINDS?s_kind[kind]:"";}
void nfsu1_port_lang_choose(int kind,int choice,int at_start)
{
    lang_load();
    if(kind<0||kind>=KINDS||!s_lang_name[kind][0])return;
    s_chosen[kind]=choice!=0;
    if(at_start)s_active[kind]=s_chosen[kind];
}
void nfsu1_port_lang_redirect(char *path,size_t size)
{
    const char *dir=getenv("NFSU2_GAME_DIR");
    if(!dir||(!s_active[0]&&!s_active[1]&&!s_active[2]))return;
    size_t n=strlen(dir);
    if(strncasecmp(path,dir,n)||path[n]!='/'||!strncasecmp(path+n+1,"lang/",5))return;
    for(int k=0;k<KINDS;k++){
        char alt[1024];
        if(!s_active[k])continue;
        if((size_t)snprintf(alt,sizeof alt,"%s/lang/%s/%s",dir,s_kind[k],path+n+1)>=sizeof alt)continue;
        if(!access(alt,F_OK)&&strlen(alt)<size){memcpy(path,alt,strlen(alt)+1);return;}
    }
}
