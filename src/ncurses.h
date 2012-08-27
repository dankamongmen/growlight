#ifndef GROWLIGHT_SRC_UI_NCURSES
#define GROWLIGHT_SRC_UI_NCURSES

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

#ifdef HAVE_NCURSESW_H
#include <term.h>
#include <panel.h>
#include <ncurses.h>
#else
#ifdef HAVE_NCURSESW_CURSES_H
#include <ncursesw/term.h>
#include <ncursesw/panel.h>
#include <ncursesw/curses.h>
#else
#error "Couldn't find working cursesw headers"
#endif
#endif

struct form_option;

void locked_diag(const char *,...);

// Scrolling single select form
void raise_form(const char *,void (*)(const char *),struct form_option *,int,int);

// Multiselect form with side panel
void raise_multiform(const char *,void (*)(const char *,int *),
			struct form_option *,int,int,int *);

#ifdef __cplusplus
}
#endif

#endif
