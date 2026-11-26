/* reUndr PS5, MIT. Optional replacement labels for port-owned UI rows, and
 * an optional second choice for text, voice and music (port_ui.c). */
#ifndef NFSU1_PORT_UI_H
#define NFSU1_PORT_UI_H
#include <stddef.h>
int nfsu1_port_ui_custom(void);
int nfsu1_port_ui_hide_trailers(void);
const char *nfsu1_port_ui_text(const char *text);
/* Kinds: 0 text, 1 voice, 2 music. Choices: 0 first, 1 second. */
int nfsu1_port_lang_count(void);
int nfsu1_port_lang_kind(int i);
const char *nfsu1_port_lang_name(int kind,int choice);
int nfsu1_port_lang_choice(int kind);
int nfsu1_port_lang_active(int kind);
const char *nfsu1_port_lang_kind_name(int kind);
void nfsu1_port_lang_choose(int kind,int choice,int at_start);
void nfsu1_port_lang_redirect(char *path,size_t size);
#endif
