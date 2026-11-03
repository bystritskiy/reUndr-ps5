/* reUndr PS5, MIT. Optional replacement labels for port-owned UI rows, and
 * an optional second language for text and voice (port_ui.c). */
#ifndef NFSU1_PORT_UI_H
#define NFSU1_PORT_UI_H
#include <stddef.h>
int nfsu1_port_ui_custom(void);
int nfsu1_port_ui_hide_trailers(void);
const char *nfsu1_port_ui_text(const char *text);
/* Kinds: 0 text, 1 voice. Choices: 0 first, 1 second language. */
int nfsu1_port_lang_count(void);
int nfsu1_port_lang_kind(int i);
const char *nfsu1_port_lang_name(int kind,int choice);
int nfsu1_port_lang_choice(int kind);
void nfsu1_port_lang_choose(int kind,int choice,int at_start);
void nfsu1_port_lang_redirect(char *path,size_t size);
#endif
