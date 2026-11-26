/* reUndr PS5, MIT. Second language: <game>/lang/lang.txt has
 * "text<TAB>first<TAB>second", "voice<TAB>first<TAB>second" and
 * "music<TAB>first<TAB>second" lines in the game font's 8-bit encoding, and
 * the second choice's files sit in lang/text/, lang/voice/ and lang/music/ at
 * their game paths. The port menu's Language tab picks one; the choice in
 * force at start stays until the next start. */
#include "port_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#define KINDS 3
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
