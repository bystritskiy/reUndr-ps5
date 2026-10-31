/* PlayStation wording for an unchanged disc. MIT. */
#ifndef NFSU1_UI_TEXT_H
#define NFSU1_UI_TEXT_H
#include <stddef.h>

/* Writes the changed text files into cache_dir; a file it cannot change stays the disc's. */
void nfsu1_ui_text_prepare(const char *game_dir,const char *cache_dir);
/* Points a translated host path at the changed copy, when there is one. */
void nfsu1_ui_text_redirect(char *path,size_t size);

#endif
