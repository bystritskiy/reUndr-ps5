#include "port_ui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
static void put(const char *dir,const char *name,const char *text){
 char path[512];snprintf(path,sizeof(path),"%s/%s",dir,name);FILE *f=fopen(path,"wb");assert(f);fputs(text,f);fclose(f);
}
static void child(const char *self,const char *mode){
 int status;pid_t pid=fork();assert(pid>=0);if(!pid){execl(self,self,mode,NULL);_exit(99);}waitpid(pid,&status,0);assert(status==0);
}
int main(int argc,char **argv){
 if(argc>1){
  const char *dir=getenv("NFSU2_GAME_DIR");char path[1024];
  if(!strcmp(argv[1],"table")){
   assert(nfsu1_port_ui_custom()&&nfsu1_port_ui_hide_trailers());
   assert(!strcmp(nfsu1_port_ui_text("On"),"Activ\xe9"));
   assert(!strcmp(nfsu1_port_ui_text("All Speakers:"),"Tous les haut-parleurs"));
   assert(!strcmp(nfsu1_port_ui_text("8x"),"8x"));
   assert(!nfsu1_port_lang_count());
  }else if(!strcmp(argv[1],"lang")){
   /* A language screen keeps the trailers entry; voice only redirects. */
   assert(nfsu1_port_ui_custom()&&!nfsu1_port_ui_hide_trailers());
   assert(nfsu1_port_lang_count()==3&&nfsu1_port_lang_kind(0)==0&&nfsu1_port_lang_kind(1)==1&&nfsu1_port_lang_kind(2)==2);
   assert(!strcmp(nfsu1_port_lang_kind_name(2),"music")&&!strcmp(nfsu1_port_lang_name(2,1),"Original"));
   assert(!strcmp(nfsu1_port_lang_name(0,0),"Fran\xe7""ais")&&!strcmp(nfsu1_port_lang_name(1,1),"English"));
   nfsu1_port_lang_choose(1,1,1);
   assert(!strcmp(nfsu1_port_ui_text("On"),"Activ\xe9"));
   snprintf(path,sizeof(path),"%s/SOUND/a.ast",dir);nfsu1_port_lang_redirect(path,sizeof(path));
   char want[1024];snprintf(want,sizeof(want),"%s/lang/voice/SOUND/a.ast",dir);assert(!strcmp(path,want));
   snprintf(path,sizeof(path),"%s/SOUND/b.ast",dir);nfsu1_port_lang_redirect(path,sizeof(path));
   snprintf(want,sizeof(want),"%s/SOUND/b.ast",dir);assert(!strcmp(path,want));
   snprintf(path,sizeof(path),"%s/LANGUAGES/t.bin",dir);nfsu1_port_lang_redirect(path,sizeof(path));
   snprintf(want,sizeof(want),"%s/LANGUAGES/t.bin",dir);assert(!strcmp(path,want));
   /* Music has its own folder and choice. */
   char music[1024];snprintf(music,sizeof(music),"%s/SOUND/m.ast",dir);nfsu1_port_lang_redirect(music,sizeof(music));
   snprintf(want,sizeof(want),"%s/SOUND/m.ast",dir);assert(!strcmp(music,want));
   nfsu1_port_lang_choose(2,1,1);nfsu1_port_lang_redirect(music,sizeof(music));
   snprintf(want,sizeof(want),"%s/lang/music/SOUND/m.ast",dir);assert(!strcmp(music,want));
   snprintf(want,sizeof(want),"%s/LANGUAGES/t.bin",dir);
   /* A choice in the menu waits for the next start. */
   nfsu1_port_lang_choose(0,1,0);assert(nfsu1_port_lang_choice(0)==1);
   assert(!strcmp(nfsu1_port_ui_text("On"),"Activ\xe9"));
   nfsu1_port_lang_redirect(path,sizeof(path));assert(!strcmp(path,want));
   nfsu1_port_lang_choose(0,1,1);
   assert(!strcmp(nfsu1_port_ui_text("On"),"On"));
   nfsu1_port_lang_redirect(path,sizeof(path));
   snprintf(want,sizeof(want),"%s/lang/text/LANGUAGES/t.bin",dir);assert(!strcmp(path,want));
  }else{
   assert(!nfsu1_port_ui_custom()&&!nfsu1_port_ui_hide_trailers());assert(!strcmp(nfsu1_port_ui_text("On"),"On"));
   assert(!nfsu1_port_lang_count()&&!*nfsu1_port_lang_name(0,1));
   snprintf(path,sizeof(path),"%s/SOUND/a.ast",dir);nfsu1_port_lang_choose(1,1,1);nfsu1_port_lang_redirect(path,sizeof(path));
   assert(!strstr(path,"/lang/"));
  }
  return 0;
 }
 char dir[]="/tmp/reundr-ui-XXXXXX",path[256];assert(mkdtemp(dir));
 setenv("NFSU2_GAME_DIR",dir,1);
 child(argv[0],"none");
 put(dir,"port_ui.txt","# test\r\n!hide-trailers\nOn\tActiv\xe9\r\nAll Speakers:\tTous les haut-parleurs\nbroken line\n\tno label\n");
 child(argv[0],"table");
 const char *made[]={"lang","lang/voice","lang/voice/SOUND","lang/text","lang/text/LANGUAGES","lang/music","lang/music/SOUND"};
 for(unsigned i=0;i<sizeof made/sizeof *made;i++){snprintf(path,sizeof(path),"%s/%s",dir,made[i]);assert(!mkdir(path,0700));}
 put(dir,"lang/lang.txt","# kind, first, second\nvoice\tFran\xe7""ais\tEnglish\r\ntext\tFran\xe7""ais\tEnglish\nmusic\tUncut\tOriginal\nbroken\tline\n");
 put(dir,"lang/voice/SOUND/a.ast","x");put(dir,"lang/text/LANGUAGES/t.bin","x");put(dir,"lang/music/SOUND/m.ast","x");
 child(argv[0],"lang");
 const char *gone[]={"lang/music/SOUND/m.ast","lang/music/SOUND","lang/music","lang/text/LANGUAGES/t.bin","lang/voice/SOUND/a.ast","lang/lang.txt","lang/text/LANGUAGES","lang/text","lang/voice/SOUND","lang/voice","lang","port_ui.txt"};
 for(unsigned i=0;i<sizeof gone/sizeof *gone;i++){snprintf(path,sizeof(path),"%s/%s",dir,gone[i]);assert(!remove(path));}
 rmdir(dir);puts("port UI: default labels, replacement table and second choices passed");return 0;
}
