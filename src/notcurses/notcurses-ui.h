#ifndef GROWLIGHT_SRC_UI_NOTCURSES
#define GROWLIGHT_SRC_UI_NOTCURSES

#ifdef __cplusplus
extern "C" {
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

#ifdef __cplusplus
}
#endif

#endif
