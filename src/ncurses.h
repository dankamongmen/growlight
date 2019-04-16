#ifndef GROWLIGHT_SRC_UI_NCURSES
#define GROWLIGHT_SRC_UI_NCURSES

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

#if defined(HAVE_NCURSESW_H) || defined(HAVE_NCURSESW)
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
struct panel_state;

void locked_diag(const char *,...);

// Scrolling single select form
void raise_form(const char *,void (*)(const char *),struct form_option *,
			int,int,const char *);

// Single-entry string entry form with command-line editing
void raise_str_form(const char *,void (*)(const char *),
			const char *,const char *);

// Multiselect form with side panel
void raise_multiform(const char *,void (*)(const char *,char **,int,int),
	struct form_option *,int,int,int,char **,int,const char *,int);

struct panel_state *show_splash(const wchar_t *);
void kill_splash(struct panel_state *);

// get protection against bad arguments to mvwprintw()...
extern int mvwprintw(WINDOW *,int,int,const char *,...)
	__attribute__ ((format (printf,4,5)));

// get protection against bad arguments to wprintw()...
extern int wprintw(WINDOW *,const char *,...)
	__attribute__ ((format (printf,2,3)));

#ifdef __cplusplus
}
#endif

#endif
