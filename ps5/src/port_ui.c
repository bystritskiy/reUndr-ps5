/* reUndr PS5, MIT. Labels from <game>/port_ui.txt: "label<TAB>replacement"
 * lines in the game font's 8-bit encoding, '#' comments, and the directive
 * "!hide-trailers". Without the file the rows keep the port's own labels. */
#include "port_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_ROWS 64
static struct {char *from,*to;} s_rows[MAX_ROWS];
static int s_count=-1,s_hide_trailers;
static char *s_text;
static void load(void)
{
    if(s_count>=0)return;
    s_count=0;
    const char *dir=getenv("NFSU2_GAME_DIR");char path[1024];
    if(!dir)return;
    snprintf(path,sizeof(path),"%s/port_ui.txt",dir);
    FILE *f=fopen(path,"rb");if(!f)return;
    fseek(f,0,SEEK_END);long size=ftell(f);fseek(f,0,SEEK_SET);
    if(size<=0||size>65536||!(s_text=malloc((size_t)size+1))){fclose(f);return;}
    size_t got=fread(s_text,1,(size_t)size,f);fclose(f);s_text[got]=0;
    for(char *line=strtok(s_text,"\r\n");line;line=strtok(NULL,"\r\n")){
        if(line[0]=='#')continue;
        if(!strcmp(line,"!hide-trailers")){s_hide_trailers=1;continue;}
        char *tab=strchr(line,'\t');
        if(!tab||tab==line||!tab[1]||s_count==MAX_ROWS)continue;
        *tab=0;s_rows[s_count].from=line;s_rows[s_count].to=tab+1;s_count++;
    }
}
int nfsu1_port_ui_custom(void){load();return s_count>0;}
int nfsu1_port_ui_hide_trailers(void){load();return s_hide_trailers;}
const char *nfsu1_port_ui_text(const char *text)
{
    load();
    for(int i=0;i<s_count;i++)
        if(!strcmp(text,s_rows[i].from))return s_rows[i].to;
    return text;
}
