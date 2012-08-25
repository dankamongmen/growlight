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

void locked_diag(const char *,...);

#ifdef __cplusplus
}
#endif

#endif
