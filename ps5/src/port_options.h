/* reUndr PS5, MIT. The options the port menu shows (port_menu.cpp), kept by
 * recomp_manual.c with the title's own Options rows in nfsu1x_options.txt.
 * Values are indexes into each option's steps; every change applies at once. */
#ifndef NFSU1_PORT_OPTIONS_H
#define NFSU1_PORT_OPTIONS_H

enum {
    NFSU1_OPT_SCALE,        /* 1x 1.5x 2x 2.25x 2.5x 3x 4.5x */
    NFSU1_OPT_FXAA,         /* off on */
    NFSU1_OPT_ANISO,        /* off 2x 4x 8x 16x */
    NFSU1_OPT_SQUARE,       /* off on */
    NFSU1_OPT_REFLECTIONS,  /* off on */
    NFSU1_OPT_LOD,          /* 1x 2x 3x 4.5x 6x 8x */
    NFSU1_OPT_FPS,          /* 30 60 120 (on a 119.88 Hz output) */
    NFSU1_OPT_OVERLAY,      /* off on */
    NFSU1_OPT_PAUSE,        /* off on: the port menu holds the title */
    NFSU1_OPT_TEXT_LANG,    /* lang.txt's two text choices; from the next start */
    NFSU1_OPT_VOICE_LANG,   /* lang.txt's two voice choices; from the next start */
    NFSU1_OPT_MUSIC,        /* lang.txt's two music choices; from the next start */
    NFSU1_OPT_SPEAKERS,     /* off on: music and menus on every speaker (PS5) */
    NFSU1_OPT_SPEAKER_TEST, /* off, eight channels, all in turn; never kept */
    NFSU1_OPT_BLUR,         /* motion blur: off low original */
    NFSU1_OPT_VIBRATION,    /* off 10% .. 100% (PS5) */
    NFSU1_OPT_COUNT
};

#ifdef __cplusplus
extern "C" {
#endif
int nfsu1_opt_get(int id);
int nfsu1_opt_steps(int id);
void nfsu1_opt_set(int id, int value);
int nfsu1_opt_shown(int id);    /* this build and data have the option */
/* A language or music choice's name in UTF-8, marked while it waits for a restart. */
const char *nfsu1_opt_lang_text(int id, int value, char *buf, unsigned cap);
void nfsu1_opt_save(void);
extern int nfsu1_port_menu_seen;   /* kept with the options */
#ifdef __cplusplus
}
#endif
#endif
