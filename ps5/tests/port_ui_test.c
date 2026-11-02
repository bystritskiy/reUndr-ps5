#include "port_ui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
int main(int argc,char **argv){
 if(argc>1){
  if(!strcmp(argv[1],"table")){
   assert(nfsu1_port_ui_custom()&&nfsu1_port_ui_hide_trailers());
   assert(!strcmp(nfsu1_port_ui_text("On"),"Activ\xe9"));
   assert(!strcmp(nfsu1_port_ui_text("All Speakers:"),"Tous les haut-parleurs"));
   assert(!strcmp(nfsu1_port_ui_text("8x"),"8x"));
  }else{assert(!nfsu1_port_ui_custom()&&!nfsu1_port_ui_hide_trailers());assert(!strcmp(nfsu1_port_ui_text("On"),"On"));}
  return 0;
 }
 char dir[]="/tmp/reundr-ui-XXXXXX",path[256];assert(mkdtemp(dir));
 setenv("NFSU2_GAME_DIR",dir,1);
 pid_t pid=fork();assert(pid>=0);if(!pid){execl(argv[0],argv[0],"none",NULL);_exit(99);}int status;waitpid(pid,&status,0);assert(status==0);
 snprintf(path,sizeof(path),"%s/port_ui.txt",dir);FILE *f=fopen(path,"wb");assert(f);
 fputs("# test\r\n!hide-trailers\nOn\tActiv\xe9\r\nAll Speakers:\tTous les haut-parleurs\nbroken line\n\tno label\n",f);fclose(f);
 pid=fork();assert(pid>=0);if(!pid){execl(argv[0],argv[0],"table",NULL);_exit(99);}waitpid(pid,&status,0);assert(status==0);
 unlink(path);rmdir(dir);puts("port UI: default labels and replacement table passed");return 0;
}
