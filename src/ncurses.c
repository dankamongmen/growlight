#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <pthread.h>

#include "fs.h"
#include "mbr.h"
#include "config.h"
#include "health.h"
#include "ptable.h"
#include "ptypes.h"
#include "growlight.h"

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

#define KEY_ESC 27

// Our color pairs
enum {
	BORDER_COLOR = 1,		// Main window
	HEADER_COLOR,
	FOOTER_COLOR,
	UHEADING_COLOR,
	UBORDER_COLOR,
	PBORDER_COLOR,
	PHEADING_COLOR,
	SUBDISPLAY_COLOR,
	OPTICAL_COLOR,
	ROTATE_COLOR,
	VIRTUAL_COLOR,
	SSD_COLOR,
	FS_COLOR,
	EMPTY_COLOR,			// Empty sectors
	MDADM_COLOR,
	ZPOOL_COLOR,
	PARTITION_COLOR,
	ZONE_COLOR,			// Selected zone
	FORMBORDER_COLOR,
	FORMTEXT_COLOR,
	INPUT_COLOR,			// Form input color
};

static pthread_mutex_t bfl = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

struct panel_state {
	PANEL *p;
	int ysize;		      // number of lines of *text* (not win)
};

#define PANEL_STATE_INITIALIZER { .p = NULL, .ysize = -1, }
#define SUBDISPLAY_ATTR (COLOR_PAIR(SUBDISPLAY_COLOR) | A_BOLD)
#define SUBDISPLAY_INVAL_ATTR (COLOR_PAIR(SUBDISPLAY_COLOR))

static struct panel_state *active;
static struct panel_state maps = PANEL_STATE_INITIALIZER;
static struct panel_state help = PANEL_STATE_INITIALIZER;
static struct panel_state diags = PANEL_STATE_INITIALIZER;
static struct panel_state details = PANEL_STATE_INITIALIZER;
static struct panel_state environment = PANEL_STATE_INITIALIZER;

static struct form_state *actform;

static void screen_update(void);
static int update_diags(struct panel_state *);

struct adapterstate;

struct partobj;

typedef struct zobj {
	unsigned zoneno;		// in-order, but not monotonic growth (skip empties)
	uintmax_t fsector,lsector;	// first and last logical sector, inclusive
	device *p;			// partition/block device, NULL for empty space
	struct zobj *prev,*next;
} zobj;

typedef struct blockobj {
	struct blockobj *next,*prev;
	device *d;
	// Zones refer to sets of contiguously allocated or unallocated sectors
	// on a block object. Each partition is a zone. Each empty space
	// between partitions is a zone. Empty space at the beginning or end of
	// the disk is a zone. The entire disk is one zone if it has not yet
	// been partitioned. When a blockobj is selected, one of its zones is
	// always selected. The first time a blockobj is selected, zone 0 is
	// selected. The selected zone is preserved across de- and reselection
	// of the block device. Zones are indexed by 0, obviously.
	unsigned zones;
	zobj *zchain,*zone;
} blockobj;

typedef struct reelbox {
	WINDOW *win;
	PANEL *panel;
	struct reelbox *next,*prev;
	struct adapterstate *as;
	int scrline,selline;
	blockobj *selected;
	struct partobj *pselected;
} reelbox;

typedef struct adapterstate {
	controller *c;
	unsigned devdisps,devs;
	enum {
		EXPANSION_NONE,
		EXPANSION_FULL,
	} expansion;
	struct adapterstate *next,*prev;
	blockobj *bobjs;
	reelbox *rb;
} adapterstate;

#define EXPANSION_MAX EXPANSION_FULL

static char statusmsg[73];
static unsigned count_adapters;
// dequeue + single selection
static reelbox *current_adapter,*top_reelbox,*last_reelbox;

#define START_COL 1		// Room to leave for borders
#define PAD_COLS(cols) ((cols) - START_COL * 2)

static int
selection_active(void){
	if(current_adapter == NULL){
		return 0;
	}
	if(current_adapter->selected == NULL){
		return 0;
	}
	return 1;
}

static int
bevel_bottom(WINDOW *w){
	static const cchar_t bchr[] = {
		{ .attr = 0, .chars = L"└", },
		{ .attr = 0, .chars = L"┘", },
		{ .attr = 0, .chars = L"─", },
		{ .attr = 0, .chars = L"│", },
	};
	int rows,cols,z;

	getmaxyx(w,rows,cols);
	assert(mvwadd_wch(w,rows - 1,0,&bchr[0]) != ERR);
	for(z = 1 ; z < cols - 1 ; ++z){
		assert(mvwadd_wch(w,rows - 1,z,&bchr[2]) != ERR);
	}
	assert(mvwins_wch(w,rows - 1,cols - 1,&bchr[1]) != ERR);
	for(z = 0 ; z < rows - 1 ; ++z){
		mvwadd_wch(w,z,0,&bchr[3]);
		mvwins_wch(w,z,cols - 1,&bchr[3]);
	}
	return OK;
}

static int
bevel_top(WINDOW *w){
	static const cchar_t bchr[] = {
		{ .attr = 0, .chars = L"┌", },
		{ .attr = 0, .chars = L"┐", },
		{ .attr = 0, .chars = L"─", },
		{ .attr = 0, .chars = L"│", },
	};
	int rows,cols,z;

	getmaxyx(w,rows,cols);
	assert(rows && cols);
	assert(mvwadd_wch(w,0,0,&bchr[0]) != ERR);
	for(z = 1 ; z < cols - 1 ; ++z){
		assert(mvwadd_wch(w,0,z,&bchr[2]) != ERR);
	}
	assert(mvwins_wch(w,0,cols - 1,&bchr[1]) != ERR);
	for(z = 1 ; z < rows ; ++z){
		mvwadd_wch(w,z,0,&bchr[3]);
		mvwins_wch(w,z,cols - 1,&bchr[3]);
	}
	return OK;
}

static int
bevel(WINDOW *w){
	static const cchar_t bchr[] = {
		{ .attr = 0, .chars = L"┌", },
		{ .attr = 0, .chars = L"┐", },
		{ .attr = 0, .chars = L"└", },
		{ .attr = 0, .chars = L"┘", },
		{ .attr = 0, .chars = L"│", },
		{ .attr = 0, .chars = L"─", },
	};
	int rows,cols,z;

	getmaxyx(w,rows,cols);
	assert(rows && cols);
	// called as one expects: 'mvwadd_wch(w,rows - 1,cols - 1,&bchr[3]);'
	// we get ERR returned. this is known behavior: fuck ncurses. instead,
	// we use mvwins_wch, which doesn't update the cursor position.
	// see http://lists.gnu.org/archive/html/bug-ncurses/2007-09/msg00001.ht
	assert(mvwadd_wch(w,0,0,&bchr[0]) != ERR);
	for(z = 1 ; z < cols - 1 ; ++z){
		assert(mvwadd_wch(w,0,z,&bchr[5]) != ERR);
	}
	for(z = rows - 2 ; z > 0 ; --z){
		assert(mvwadd_wch(w,z,0,&bchr[4]) != ERR);
		assert(mvwins_wch(w,z,cols - 1,&bchr[4]) != ERR);
	}
	assert(mvwins_wch(w,0,cols - 1,&bchr[1]) != ERR);
	assert(mvwadd_wch(w,rows - 1,0,&bchr[2]) != ERR);
	for(z = 1 ; z < cols - 1 ; ++z){
		assert(mvwadd_wch(w,rows - 1,z,&bchr[5]) != ERR);
	}
	assert(mvwins_wch(w,rows - 1,cols - 1,&bchr[3]) != ERR);
	return OK;
}

static int
draw_main_window(WINDOW *w){
	int rows,cols,scol;

	getmaxyx(w,rows,cols);
	assert(cols >= 80);
	assert(wattrset(w,A_DIM | COLOR_PAIR(BORDER_COLOR)) != ERR);
	if(bevel(w) != OK){
		goto err;
	}
	scol = START_COL * 4;
	assert(mvwprintw(w,0,scol,"[") != ERR);
	assert(wattron(w,A_BOLD | COLOR_PAIR(HEADER_COLOR)) != ERR);
	assert(wprintw(w,"%s %s | %d adapter%s",PACKAGE,VERSION,
			count_adapters,count_adapters == 1 ? "" : "s") != ERR);
	assert(wattrset(w,COLOR_PAIR(BORDER_COLOR)) != ERR);
	assert(wprintw(w,"]") != ERR);
	assert(wattron(w,A_BOLD | COLOR_PAIR(FOOTER_COLOR)) != ERR);
	// addstr() doesn't interpret format strings, so this is safe. It will
	// fail, however, if the string can't fit on the window, which will for
	// instance happen if there's an embedded newline.
	assert(mvwaddstr(w,rows - 1,START_COL * 2,statusmsg) != ERR);
	assert(wattroff(w,A_BOLD | COLOR_PAIR(FOOTER_COLOR)) != ERR);
	return OK;

err:
	return ERR;
}

static void
locked_diag(const char *fmt,...){
	va_list v;
	char *nl;

	va_start(v,fmt);
	vsnprintf(statusmsg,sizeof(statusmsg),fmt,v);
	va_end(v);
	if( (nl = strchr(statusmsg,'\n')) ){
		*nl = '\0';
	}
	draw_main_window(stdscr);
	if(diags.p){
		update_diags(&diags);
	}
	screen_update();
}

// -------------------------------------------------------------------------
// -- begin form API
// -------------------------------------------------------------------------
// Forms are modal. They take over the keyboard UI and sit atop everything
// else. Subwindows sit atop the hardware elements of the UI, but do not seize
// any of the input UI. A form and subwindow can coexist.

struct form_option {
	char *option;			// option key (the string passed to cb)
	char *desc;			// longer description
};

struct form_input {
	const char *prompt;		// short prompt. currently aliases boxstr
	char *longprompt;		// longer prompt, not currently used
	char *buffer;			// input buffer, initialized to ""
};

typedef enum {
	FORM_SELECT,			// form_option[]
	FORM_STRING_INPUT,		// form_input
} form_enum;

struct form_state {
	PANEL *p;
	int ysize;			// number of lines of *text* (not win)
	void (*fxn)(const char *);	// callback once form is done
	int idx;			// selection index, [0..ysize)
	int longop;			// length of longest op
	char *boxstr;			// string for box label
	form_enum formtype;		// type of form
	union {
		struct form_option *ops;// form_option array for *this instance*
		struct form_input inp;	// form_input state for this instance
	};
};

// -------------------------------------------------------------------------
// -- form creation
// -------------------------------------------------------------------------
static struct form_state *
create_form(const char *str,void (*fxn)(const char *),form_enum ftype){
	struct form_state *fs;

	if( (fs = malloc(sizeof(*fs))) ){
		memset(fs,0,sizeof(*fs));
		if((fs->boxstr = strdup(str)) == NULL){
			locked_diag("Couldn't create input dialog (%s?)",strerror(errno));
			free(fs);
			return NULL;
		}
		fs->formtype = ftype;
		fs->fxn = fxn;
	}else{
		locked_diag("Couldn't create input dialog (%s?)",strerror(errno));
	}
	return fs;
}

static void
destroy_form_locked(struct form_state *fs){
	if(fs){
		WINDOW *fsw;
		int z;

		assert(fs == actform);
		fsw = panel_window(fs->p);
		hide_panel(fs->p);
		assert(del_panel(fs->p) == OK);
		fs->p = NULL;
		assert(delwin(fsw) == OK);
		switch(fs->formtype){
			case FORM_SELECT:
				for(z = 0 ; z < fs->ysize ; ++z){
					free(fs->ops[z].option);
					free(fs->ops[z].desc);
				}
				free(fs->ops);
				fs->ops = NULL;
				break;
			case FORM_STRING_INPUT:
				free(fs->inp.buffer);
				free(fs->inp.longprompt);
				fs->inp.longprompt = NULL;
				fs->inp.prompt = NULL;
				fs->inp.buffer = NULL;
				break;
		}
		fs->ysize = -1;
		actform = NULL;
	}
}

static void
free_form(struct form_state *fs){
	if(fs){
		free(fs->boxstr);
		destroy_form_locked(fs);
		free(fs);
	}
}

static void
form_options(struct form_state *fs){
	const struct form_option *opstrs = fs->ops;
	WINDOW *fsw = panel_window(fs->p);
	const int ops = fs->ysize;
	int z,cols;

	if(fs->formtype != FORM_SELECT){
		return;
	}
	cols = getmaxx(fsw);
	wattron(fsw,A_BOLD);
	for(z = 0 ; z < ops ; ++z){
		wcolor_set(fsw,FORMTEXT_COLOR,NULL);
		mvwprintw(fsw,z + 1,START_COL,"%-*.*s ",
			fs->longop,fs->longop,opstrs[z].option);
		if(z == fs->idx){
			wattron(fsw,A_REVERSE);
		}
		wcolor_set(fsw,INPUT_COLOR,NULL);
		wprintw(fsw,"%-*.*s",cols - fs->longop - 1 - START_COL * 2,
			cols - fs->longop - 1 - START_COL * 2,opstrs[z].desc);
		if(z == fs->idx){
			wattroff(fsw,A_REVERSE);
		}
	}
}

// -------------------------------------------------------------------------
// - select type form, for single choice from among a set
// -------------------------------------------------------------------------
static void
raise_form(const char *str,void (*fxn)(const char *),struct form_option *opstrs,int ops){
	size_t longop,longdesc;
	struct form_state *fs;
	WINDOW *fsw;
	int cols;
	int x,y;

	if(opstrs == NULL || !ops){
		locked_diag("Passed empty %u-option string table",ops);
		return;
	}
	if(actform){
		locked_diag("An input dialog is already active");
		return;
	}
	if((fs = create_form(str,fxn,FORM_SELECT)) == NULL){
		return;
	}
	longdesc = longop = 0;
	for(x = 0 ; x < ops ; ++x){
		if(strlen(opstrs[x].option) > longop){
			longop = strlen(opstrs[x].option);
		}
		if(strlen(opstrs[x].desc) > longdesc){
			longdesc = strlen(opstrs[x].desc);
		}
	}
	cols = longdesc + longop + 1;
	getmaxyx(stdscr,y,x);
	assert(x >= cols + START_COL * 2);
	assert(y >= ops + 3);
	assert( (fsw = newwin(ops + 2,cols + START_COL * 2,y - ops - 4,5)) );
	if(fsw == NULL){
		free_form(fs);
		return;
	}
	assert((fs->p = new_panel(fsw)));
	assert(top_panel(fs->p) != ERR);
	if(fs->p == NULL){
		delwin(fsw);
		free_form(fs);
		return;
	}
	fs->ysize = ops;
	wattroff(fsw,A_BOLD);
	wcolor_set(fsw,FORMBORDER_COLOR,NULL);
	bevel(fsw);
	wattron(fsw,A_BOLD);
	mvwprintw(fsw,0,cols - strlen(fs->boxstr),fs->boxstr);
	mvwprintw(fsw,fs->ysize + 1,cols - strlen("⎋esc returns"),"⎋esc returns");
	wattroff(fsw,A_BOLD);
	fs->longop = longop;
	fs->ops = opstrs;
	form_options(fs);
	actform = fs;
	locked_diag(fs->boxstr); // calls screen_update()
}

// -------------------------------------------------------------------------
// - string type form, for generic input
// -------------------------------------------------------------------------
static void
form_string_options(struct form_state *fs){
	WINDOW *fsw = panel_window(fs->p);
	int cols;

	if(fs->formtype != FORM_STRING_INPUT){
		return;
	}
	cols = getmaxx(fsw);
	wattrset(fsw,A_BOLD);
	wcolor_set(fsw,FORMTEXT_COLOR,NULL);
	mvwprintw(fsw,1,START_COL,"%-*.*s: ",
		fs->longop,fs->longop,fs->inp.prompt);
	wattron(fsw,A_REVERSE);
	wcolor_set(fsw,INPUT_COLOR,NULL);
	wprintw(fsw,"%-*.*s",cols - fs->longop - 2 - START_COL * 2,
		cols - fs->longop - 2 - START_COL * 2,fs->inp.buffer);
	wattroff(fsw,A_BOLD);
}

static void
raise_str_form(const char *str,void (*fxn)(const char *)){
	struct form_state *fs;
	WINDOW *fsw;
	int cols;
	int x,y;

	assert(str && fxn);
	if(actform){
		locked_diag("An input dialog is already active");
		return;
	}
	if((fs = create_form(str,fxn,FORM_STRING_INPUT)) == NULL){
		return;
	}
	fs->longop = strlen(str);
	cols = fs->longop + 40 + 1; // FIXME? 40 for input currently
	getmaxyx(stdscr,y,x);
	assert(x >= cols + START_COL * 2);
	assert(y >= 3);
	assert( (fsw = newwin(3,cols + START_COL * 2,y - 5,5)) );
	if(fsw == NULL){
		free_form(fs);
		return;
	}
	assert((fs->p = new_panel(fsw)));
	assert(top_panel(fs->p) != ERR);
	if(fs->p == NULL){
		delwin(fsw);
		free_form(fs);
		return;
	}
	fs->ysize = 1;
	wattroff(fsw,A_BOLD);
	wcolor_set(fsw,FORMBORDER_COLOR,NULL);
	bevel(fsw);
	wattron(fsw,A_BOLD);
	mvwprintw(fsw,0,cols - strlen(fs->boxstr),fs->boxstr);
	mvwprintw(fsw,fs->ysize + 1,cols - strlen("⎋esc returns"),"⎋esc returns");
	fs->inp.prompt = fs->boxstr;
	fs->inp.buffer = strdup("");
	form_string_options(fs);
	actform = fs;
	locked_diag(fs->boxstr); // calls screen_update()
}

// -------------------------------------------------------------------------
// - filesystem type form, for new filesystem creation
// -------------------------------------------------------------------------
static void
fs_callback(const char *fs){
	blockobj *b;

	if(fs == NULL){ // user cancelled
		locked_diag("Filesystem creation cancelled by the user");
		return;
	}
	if(!current_adapter || !(b = current_adapter->selected)){
		locked_diag("Lost selection while choosing filesystem type");
		return;
	}
	b = current_adapter->selected;
	if(b->zone->p == NULL){
		make_filesystem(b->d,fs);
		return;
	}else if(b->zone->p->layout != LAYOUT_PARTITION){
		locked_diag("%s is not a partition, aborting.\n",b->zone->p->name);
		return;
	}else{
		make_filesystem(b->zone->p,fs);
		return;
	}
	locked_diag("I'm confused. Aborting.\n");
}

// -------------------------------------------------------------------------
// -- end filesystem type form
// -------------------------------------------------------------------------

static struct form_option *
ptype_table(int *count){
	struct form_option *fo = NULL,*tmp;
	const ptype *pt;

	*count = 0;
	for(pt = ptypes ; pt->name ; ++pt){
		const size_t KEYSIZE = 5; // 4 hex digit code
		char *key,*desc;

		if((key = malloc(KEYSIZE)) == NULL){
			goto err;
		}
		if((desc = strdup(pt->name)) == NULL){
			free(key);
			goto err;
		}
		if(snprintf(key,KEYSIZE,"%04x",pt->code) >= (int)KEYSIZE){
			locked_diag("Couldn't convert key 0x%x",pt->code);
			free(key);
			free(desc);
			goto err;
		}
		if((tmp = realloc(fo,sizeof(*fo) * (*count + 1))) == NULL){
			free(key);
			free(desc);
			goto err;
		}
		fo = tmp;
		fo[*count].option = key;
		fo[*count].desc = desc;
		++*count;
	}
	return fo;

err:
	while(*count--){
		free(fo[*count].option);
		free(fo[*count].desc);
	}
	free(fo);
	return NULL;
}

// -------------------------------------------------------------------------
// - partition name form, for new partition creation
// -------------------------------------------------------------------------
static void ptype_callback(const char *);

static unsigned long pending_ptype; // set when waiting for name callback

static void
ptype_name_callback(const char *name){
	unsigned long pt;
	blockobj *b;

	pt = pending_ptype;
	pending_ptype = 0;
	if(!current_adapter || !(b = current_adapter->selected)){
		locked_diag("Lost selection while choosing partition type");
		return;
	}
	if(name == NULL){ // go back to partition type
		struct form_option *ops_ptype;
		int opcount;

		if((ops_ptype = ptype_table(&opcount)) == NULL){
			return;
		}
		raise_form("select a partition type",ptype_callback,ops_ptype,opcount);
		return;
	}
	// FIXME this won't necessarily map to the selected zone!
	// FIXME use name! convert to wchar_t
	add_partition(b->d,L"FIXME",0,pt);
}

// -------------------------------------------------------------------------
// - partition type form, for new partition creation
// -------------------------------------------------------------------------
static void
ptype_callback(const char *ptype){
	unsigned long pt;
	blockobj *b;
	char *pend;

	if(ptype == NULL){ // user cancelled
		locked_diag("Partition creation cancelled by the user");
		return;
	}
	if(!current_adapter || !(b = current_adapter->selected)){
		locked_diag("Lost selection while choosing partition type");
		return;
	}
	if(((pt = strtoul(ptype,&pend,16)) == ULONG_MAX && errno == ERANGE) || *pend){
		locked_diag("Bad partition type selection: %s",ptype);
		return;
	}
	if(partitions_named_p(b->d)){
		pending_ptype = pt;
		raise_str_form("enter partition name",ptype_name_callback);
		return;
	}
	// FIXME this won't necessarily map to the selected zone!
	add_partition(b->d,NULL,0,pt);
}

// -------------------------------------------------------------------------
// -- end partition type form
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// - partition tabletype form, for new partition table creation
// -------------------------------------------------------------------------
static void
pttype_callback(const char *pttype){
	blockobj *b;

	if(pttype == NULL){ // user cancelled
		locked_diag("Partition table creation cancelled by the user");
		return;
	}
	if(!current_adapter || !(b = current_adapter->selected)){
		locked_diag("Lost selection while choosing table type");
		return;
	}
	b = current_adapter->selected;
	make_partition_table(b->d,pttype);
}

// -------------------------------------------------------------------------
// -- end partition table type form
// -------------------------------------------------------------------------
// -------------------------------------------------------------------------
// -- end form API
// -------------------------------------------------------------------------

static inline void
screen_update(void){
	if(active){
		assert(top_panel(active->p) != ERR);
	}
	if(actform){
		assert(top_panel(actform->p) != ERR);
	}
	update_panels();
	assert(doupdate() == OK);
}

// This is the number of l we'd have in an optimal world; we might have
// fewer available to us on this screen at this time.
static int
lines_for_adapter(const struct adapterstate *as){
	int l = 2;

	if(as->expansion != EXPANSION_NONE){
		l += as->devdisps * 3;
		l += as->devs;
	}
	return l;
}

static int
device_lines(int expa,const blockobj *bo){
	int l = 0;

	if(expa != EXPANSION_NONE){
		if(bo->d->size){
			l += 3;
		}
		++l;
	}
	return l;
}

static zobj *
create_zobj(zobj *prev,unsigned zno,uintmax_t fsector,uintmax_t lsector,device *p){
	zobj *z;

	if( (z = malloc(sizeof(*z))) ){
		z->zoneno = zno;
		z->fsector = fsector;
		z->lsector = lsector;
		z->p = p;
		if( (z->prev = prev) ){
			prev->next = z;
		}
		z->next = NULL;
	}
	return z;
}

static inline int
adapter_lines_bounded(const adapterstate *as,int rows){
	int l = lines_for_adapter(as);

	if(l > rows - 2){ // top and bottom border
		l = rows - 2;
	}
	return l;
}

static inline int
adapter_lines_unbounded(const adapterstate *is){
	return adapter_lines_bounded(is,INT_MAX);
}

// Is the adapter window entirely visible? We can't draw it otherwise, as it
// will obliterate the global bounding box.
static int
adapter_wholly_visible_p(int rows,const reelbox *rb){
	const adapterstate *as = rb->as;

	if(rb->scrline + adapter_lines_bounded(as,rows) >= rows){
		return 0;
	}else if(rb->scrline < 1){
		return 0;
	}else if(rb->scrline == 1 && adapter_lines_bounded(as,rows) != getmaxy(rb->win)){
		return 0;
	}
	return 1;
}

// Returns the amount of space available at the bottom.
static inline int
bottom_space_p(int rows){
	if(!last_reelbox){
		return rows - 1;
	}
	if(getmaxy(last_reelbox->win) + getbegy(last_reelbox->win) >= rows - 2){
		return 0;
	}
	return (rows - 1) - (getmaxy(last_reelbox->win) + getbegy(last_reelbox->win));
}

static int
ncurses_cleanup(WINDOW **w){
	int ret = 0;

	if(*w){
		if(delwin(*w) != OK){
			ret = -1;
		}
		*w = NULL;
	}
	if(stdscr){
		if(delwin(stdscr) != OK){
			ret = -2;
		}
		stdscr = NULL;
	}
	if(endwin() != OK){
		ret = -3;
	}
	switch(ret){
	case -3: fprintf(stderr,"Couldn't end main window\n"); break;
	case -2: fprintf(stderr,"Couldn't delete main window\n"); break;
	case -1: fprintf(stderr,"Couldn't delete main pad\n"); break;
	case 0: break;
	default: fprintf(stderr,"Couldn't cleanup ncurses\n"); break;
	}
	return ret;
}

static int
setup_colors(void){
	assert(init_pair(BORDER_COLOR,COLOR_GREEN,-1) == OK);
	assert(init_pair(HEADER_COLOR,COLOR_BLUE,-1) == OK);
	assert(init_pair(FOOTER_COLOR,COLOR_YELLOW,-1) == OK);
	assert(init_pair(UHEADING_COLOR,COLOR_BLUE,-1) == OK);
	assert(init_pair(UBORDER_COLOR,COLOR_CYAN,-1) == OK);
	assert(init_pair(PBORDER_COLOR,COLOR_YELLOW,-1) == OK);
	assert(init_pair(PHEADING_COLOR,COLOR_RED,-1) == OK);
	assert(init_pair(SUBDISPLAY_COLOR,COLOR_WHITE,-1) == OK);
	assert(init_pair(OPTICAL_COLOR,COLOR_YELLOW,-1) == OK);
	assert(init_pair(ROTATE_COLOR,COLOR_WHITE,-1) == OK);
	assert(init_pair(VIRTUAL_COLOR,COLOR_WHITE,-1) == OK);
	assert(init_pair(SSD_COLOR,COLOR_CYAN,-1) == OK);
	assert(init_pair(FS_COLOR,COLOR_GREEN,-1) == OK);
	assert(init_pair(EMPTY_COLOR,COLOR_GREEN,-1) == OK);
	assert(init_pair(MDADM_COLOR,COLOR_MAGENTA,-1) == OK);
	assert(init_pair(ZPOOL_COLOR,COLOR_MAGENTA,-1) == OK);
	assert(init_pair(PARTITION_COLOR,COLOR_BLUE,-1) == OK);
	assert(init_pair(ZONE_COLOR,COLOR_BLACK,-1) == OK);
	assert(init_pair(FORMBORDER_COLOR,COLOR_RED,-1) == OK);
	assert(init_pair(FORMTEXT_COLOR,COLOR_MAGENTA,-1) == OK);
	assert(init_pair(INPUT_COLOR,COLOR_CYAN,-1) == OK);
	return 0;
}

static WINDOW *
ncurses_setup(void){
	const char *errstr = NULL;
	WINDOW *w = NULL;

	if(initscr() == NULL){
		fprintf(stderr,"Couldn't initialize ncurses\n");
		return NULL;
	}
	w = stdscr;
	if(cbreak() != OK){
		errstr = "Couldn't disable input buffering\n";
		goto err;
	}
	if(noecho() != OK){
		errstr = "Couldn't disable input echoing\n";
		goto err;
	}
	if(intrflush(stdscr,TRUE) != OK){
		errstr = "Couldn't set flush-on-interrupt\n";
		goto err;
	}
	if(scrollok(stdscr,FALSE) != OK){
		errstr = "Couldn't disable scrolling\n";
		goto err;
	}
	if(nonl() != OK){
		errstr = "Couldn't disable nl translation\n";
		goto err;
	}
	if(start_color() != OK){
		errstr = "Couldn't initialize ncurses color\n";
		goto err;
	}
	if(use_default_colors()){
		errstr = "Couldn't initialize ncurses colordefs\n";
		goto err;
	}
	ESCDELAY = 100;
	keypad(stdscr,TRUE);
	if(nodelay(stdscr,FALSE) != OK){
		errstr = "Couldn't set blocking input\n";
		goto err;
	}
	if(setup_colors() != OK){
		errstr = "Couldn't set up colors\n";
		goto err;
	}
	if(curs_set(0) == ERR){
		errstr = "Couldn't disable cursor\n";
		goto err;
	}
	if(draw_main_window(w) != OK){
		errstr = "Couldn't draw main window\n";
		goto err;
	}
	refresh();
	return w;

err:
	ncurses_cleanup(&w);
	fprintf(stderr,"%s",errstr);
	return NULL;
}

// Caller needs set up: next, prev
static reelbox *
create_reelbox(adapterstate *as,int rows,int scrline,int cols){
	reelbox *ret;
	int l;

	l = adapter_lines_bounded(as,rows);
	if(l >= rows - scrline){
		l = rows - scrline - 1;
	}
	if( (ret = malloc(sizeof(*ret))) ){
		if((ret->win = newwin(l,PAD_COLS(cols),scrline,START_COL)) == NULL){
			exit(0);
		}
		assert( (ret->panel = new_panel(ret->win)) );
		ret->scrline = scrline;
		ret->selected = NULL;
		ret->selline = -1;
		ret->as = as;
		as->rb = ret;
	}
	return ret;
}

static const controller *
get_current_adapter(void){
	if(current_adapter){
		return current_adapter->as->c;
	}
	return NULL;
}

// Abovetop: lines hidden at the top of the screen
// Belowend: lines hidden at the bottom of the screen
static void
adapter_box(const adapterstate *as,WINDOW *w,unsigned abovetop,
						unsigned belowend){
	int current = as->rb == current_adapter;
	int bcolor,hcolor,rows,cols;
	int attrs;

	getmaxyx(w,rows,cols);
	bcolor = UBORDER_COLOR;
	hcolor = UHEADING_COLOR;
	attrs = current ? A_REVERSE : A_BOLD;
	assert(wattrset(w,attrs | COLOR_PAIR(bcolor)) == OK);
	if(abovetop == 0){
		if(belowend == 0){
			assert(bevel(w) == OK);
		}else{
			assert(bevel_top(w) == OK);
		}
	}else{
		if(belowend == 0){
			assert(bevel_bottom(w) == OK);
		} // otherwise it has no top or bottom visible
	}
	assert(wattroff(w,A_REVERSE) == OK);

	if(abovetop == 0){
		if(current){
			assert(wattron(w,A_BOLD) == OK);
		}
		assert(mvwprintw(w,0,5,"[") != ERR);
		assert(wcolor_set(w,hcolor,NULL) == OK);
		if(current){
			assert(wattron(w,A_BOLD) == OK);
		}else{
			assert(wattroff(w,A_BOLD) == OK);
		}
		assert(waddstr(w,as->c->ident) != ERR);
		if(as->c->bandwidth){
			char buf[PREFIXSTRLEN + 1];

			wprintw(w," (%sbps)",qprefix(as->c->bandwidth,1,buf,sizeof(buf),1));
		}
		assert(wcolor_set(w,bcolor,NULL) != ERR);
		if(current){
			assert(wattron(w,A_BOLD) == OK);
		}
		assert(wprintw(w,"]") != ERR);
		assert(wmove(w,0,cols - 4) != ERR);
		assert(wattron(w,A_BOLD) == OK);
		waddwstr(w,as->expansion != EXPANSION_MAX ? L"[+]" : L"[-]");
		assert(wattron(w,attrs) != ERR);
		assert(wattroff(w,A_REVERSE) != ERR);
	}
	if(belowend == 0){
		if(as->c->bus == BUS_PCIe){
			assert(mvwprintw(w,rows - 1,2,"[") != ERR);
			assert(wcolor_set(w,hcolor,NULL) != ERR);
			if(current){
				assert(wattron(w,A_BOLD) == OK);
			}else{
				assert(wattroff(w,A_BOLD) == OK);
			}
			if(as->c->pcie.lanes_neg == 0){
				wprintw(w,"Southbridge device %04hx:%02x.%02x.%x",
					as->c->pcie.domain,as->c->pcie.bus,
					as->c->pcie.dev,as->c->pcie.func);
			}else{
				wprintw(w,"PCI Express device %04hx:%02x.%02x.%x (x%u, gen %s)",
						as->c->pcie.domain,as->c->pcie.bus,
						as->c->pcie.dev,as->c->pcie.func,
						as->c->pcie.lanes_neg,pcie_gen(as->c->pcie.gen));
			}
			assert(wcolor_set(w,bcolor,NULL) != ERR);
			if(current){
				assert(wattron(w,A_BOLD) == OK);
			}
			assert(wprintw(w,"]") != ERR);
		}
	}
}

/*static void
print_fs(const device *d,WINDOW *w,int *line,int rows,unsigned cols,
					unsigned endp,int selected){
	char buf[PREFIXSTRLEN + 1];

	if(selected){
		assert(wattrset(w,A_BOLD|A_REVERSE|COLOR_PAIR(FS_COLOR)) == OK);
	}else{
		assert(wattrset(w,A_BOLD|COLOR_PAIR(FS_COLOR)) == OK);
	}
	if(*line >= rows - !endp){
		return;
	}
	if(d->mnttype){
		mvwprintw(w,*line,START_COL,"  %-*.*s %-5.5s %-36.36s " PREFIXFMT "%-*.*s",
				FSLABELSIZ,FSLABELSIZ,
				d->label ? d->label : "n/a",
				d->mnttype,
				d->uuid ? d->uuid : "n/a",
				qprefix(d->mntsize,1,buf,sizeof(buf),0),
				cols - 72,cols - 72,"");
		if(++*line >= rows - !endp){
			return;
		}
	}
	if(d->swapprio != SWAP_INVALID){
		mvwprintw(w,*line,START_COL,"  %-*.*s %-5.5s %-36.36s " PREFIXFMT "%-*.*s",
				FSLABELSIZ,FSLABELSIZ,
				d->label ? d->label : "n/a",
				d->mnttype,
				d->uuid ? d->uuid : "n/a",
				qprefix(d->mntsize,1,buf,sizeof(buf),0),
				cols - 72,cols - 72,"");
		if(++*line >= rows - !endp){
			return;
		}
	}
	if(d->mnt){
		char buf[cols + 1];

		snprintf(buf,sizeof(buf),"%s %s",d->mnt,d->mntops);
		mvwprintw(w,*line,START_COL,"   %-*.*s",
					cols - (START_COL * 4) - 1,
					cols - (START_COL * 4) - 1,
					buf);
		if(++*line >= rows - !endp){
			return;
		}
	}
	if(d->target){
		char buf[cols + 1];

		snprintf(buf,sizeof(buf),"%s %s",d->target->path,d->target->ops);
		mvwprintw(w,*line,START_COL,"   %-*.*s",
					cols - (START_COL * 4) - 1,
					cols - (START_COL * 4) - 1,
					buf);
		if(++*line >= rows - !endp){
			return;
		}
	}
}

static void
print_empty(WINDOW *w,int *line,int rows,unsigned cols,
			unsigned endp,int selected,
			uint64_t fsect,uint64_t lsect,
			size_t sectsize){
	char buf[BPREFIXSTRLEN + 1];

	if(*line >= rows - !endp){
		return;
	}
#define STR " Empty space "
	mvwprintw(w,*line,START_COL,"%s%-*.*s",STR,
			cols - (START_COL * 4) - __builtin_strlen(STR) + 2,
			cols - (START_COL * 4) - __builtin_strlen(STR) + 2,
			bprefix((lsect - fsect) * sectsize,1,buf,sizeof(buf),1));
#undef STR
	++*line;
}*/

static inline unsigned
sectpos(const device *d,uintmax_t sec,unsigned sx,unsigned ex,unsigned *sectpos){
	unsigned u = ((sec * d->logsec) / (float)d->size) * (ex - sx - 1) + sx;
		       
	if(u > ++*sectpos){
		*sectpos = u;
	}
	if(*sectpos >= ex){
		*sectpos = ex - 1;
	}
	return *sectpos;
}

// Print the contents of the block device in a horizontal bar of arbitrary size
static void
print_blockbar(WINDOW *w,const blockobj *bo,int y,int sx,int ex,int selected){
	char pre[PREFIXSTRLEN + 1];
	unsigned off = sx - 1,ch;
	const device *d = bo->d;
	const zobj *z;

	int PNUMFIXME = '0';
	if(d->mnttype){
		if(selected){
			assert(wattrset(w,A_BOLD|A_REVERSE|COLOR_PAIR(FS_COLOR)) == OK);
		}else{
			assert(wattrset(w,A_BOLD|COLOR_PAIR(FS_COLOR)) == OK);
		}
		mvwprintw(w,y,sx,"%*.*s",ex - sx,ex - sx,"");
		mvwprintw(w,y,sx,"%s%s%s%s filesystem%s%s",
				d->mntsize ? qprefix(d->mntsize,1,pre,sizeof(pre),1) : "",
				d->mntsize ? " " : "",
				d->label ? "" : "unlabeled ", d->mnttype,
				d->label ? " named " : "",
				d->label ? d->label : "");
		return;
	}else if(d->layout == LAYOUT_NONE && d->blkdev.unloaded){
		if(selected){
			assert(wattrset(w,A_BOLD|A_REVERSE|COLOR_PAIR(OPTICAL_COLOR)) == OK);
		}else{
			assert(wattrset(w,COLOR_PAIR(OPTICAL_COLOR)) == OK);
		}
		mvwprintw(w,y,sx,"%-*.*s",ex - sx,ex - sx,"No media detected in drive");
		return;
	}else if(d->layout == LAYOUT_NONE && d->blkdev.pttable == NULL){
		if(selected){
			assert(wattrset(w,A_BOLD|A_REVERSE|COLOR_PAIR(EMPTY_COLOR)) == OK);
		}else{
			assert(wattrset(w,COLOR_PAIR(EMPTY_COLOR)) == OK);
		}
		mvwprintw(w,y,sx,"%*.*s",ex - sx,ex - sx,"");
		mvwprintw(w,y,sx,"%s %s",
				qprefix(d->size,1,pre,sizeof(pre),1),
				"unpartitioned space");
		return;
	}
	if((z = bo->zchain) == NULL){
		return;
	}
	do{
		if(z->p == NULL){
			if(selected && z != bo->zone){
				assert(wattrset(w,A_REVERSE|COLOR_PAIR(EMPTY_COLOR)) == OK);
			}else if(selected){
				assert(wattrset(w,A_BOLD|A_REVERSE|COLOR_PAIR(EMPTY_COLOR)) == OK);
			}else{
				assert(wattrset(w,COLOR_PAIR(EMPTY_COLOR)) == OK);
			}
			mvwaddch(w,y,sectpos(d,z->fsector,sx,ex,&off),'E');
			ch = ((z->lsector - z->fsector) / ((float)(d->size / d->logsec) / (ex - sx - 1)));
			while(ch--){
				if(++off >= (unsigned)ex){
					break;
				}
				mvwaddch(w,y,off,L'E');
			}
		}else{
			if(selected && z != bo->zone){
				assert(wattrset(w,A_BOLD|A_REVERSE|COLOR_PAIR(PARTITION_COLOR)) == OK);
			}else if(selected){
				assert(wattrset(w,A_BOLD|A_REVERSE|COLOR_PAIR(ZONE_COLOR)) == OK);
			}else{
				assert(wattrset(w,A_BOLD|COLOR_PAIR(PARTITION_COLOR)) == OK);
			}
			mvwaddch(w,y,sectpos(d,z->fsector,sx,ex,&off),PNUMFIXME);
			ch = ((z->lsector - z->fsector) / ((float)(d->size / d->logsec) / (ex - sx - 1)));
			while(ch--){
				if(++off >= (unsigned)ex){
					break;
				}
				mvwaddch(w,y,off,PNUMFIXME);
			}
			++PNUMFIXME;
		}
		//fprintf(stderr,"%s] %ju %ju\n",d->name,z->fsector,z->lsector);
	}while((z = z->next) != bo->zchain);
}

static void
print_dev(const reelbox *rb,const blockobj *bo,int line,int rows,
			unsigned cols,unsigned topp,unsigned endp){
	char buf[PREFIXSTRLEN + 1];
	const char *rolestr;
	int selected;

	if(line >= rows - !endp){
		return;
	}
	rolestr = NULL;
	selected = line == rb->selline;
	switch(bo->d->layout){
case LAYOUT_NONE:
	if(bo->d->blkdev.realdev){
		if(bo->d->blkdev.removable){
			if(selected){
				assert(wattrset(rb->win,A_BOLD|A_REVERSE|COLOR_PAIR(OPTICAL_COLOR)) == OK);
			}else{
				assert(wattrset(rb->win,A_BOLD|COLOR_PAIR(OPTICAL_COLOR)) == OK);
			}
			rolestr = "removable";
		}else if(bo->d->blkdev.rotate){
			if(selected){
				assert(wattrset(rb->win,A_REVERSE|COLOR_PAIR(ROTATE_COLOR)) == OK);
			}else{
				assert(wattrset(rb->win,COLOR_PAIR(ROTATE_COLOR)) == OK);
			}
			rolestr = "ferromag";
		}else{
			if(selected){
				assert(wattrset(rb->win,A_BOLD|A_REVERSE|COLOR_PAIR(SSD_COLOR)) == OK);
			}else{
				assert(wattrset(rb->win,A_BOLD|COLOR_PAIR(SSD_COLOR)) == OK);
			}
			rolestr = "solidstate";
		}
	}else{
		if(selected){
			assert(wattrset(rb->win,A_REVERSE|COLOR_PAIR(VIRTUAL_COLOR)) == OK);
		}else{
			assert(wattrset(rb->win,COLOR_PAIR(VIRTUAL_COLOR)) == OK);
		}
		rolestr = "virtual";
	}
	if(line + topp >= 1){
	mvwprintw(rb->win,line,START_COL,"%-11.11s %-16.16s %4.4s " PREFIXFMT " %4uB %-6.6s%-16.16s %-4.4s %-*.*s",
				bo->d->name,
				bo->d->model ? bo->d->model : "n/a",
				bo->d->revision ? bo->d->revision : "n/a",
				qprefix(bo->d->size,1,buf,sizeof(buf),0),
				bo->d->physsec,
				bo->d->blkdev.pttable ? bo->d->blkdev.pttable : "none",
				bo->d->wwn ? bo->d->wwn : "n/a",
				bo->d->blkdev.realdev ? transport_str(bo->d->blkdev.transport) : "n/a",
				cols - 78,cols - 78,"");
	}
		break;
case LAYOUT_MDADM:
	rolestr = "mdraid";
	if(selected){
		assert(wattrset(rb->win,A_BOLD|A_REVERSE|COLOR_PAIR(MDADM_COLOR)) == OK);
	}else{
		assert(wattrset(rb->win,A_BOLD|COLOR_PAIR(MDADM_COLOR)) == OK);
	}
	if(line + topp >= 1){
	mvwprintw(rb->win,line,START_COL,"%-11.11s %-16.16s %4.4s " PREFIXFMT " %4uB %-6.6s%-16.16s %-4.4s %-*.*s",
				bo->d->name,
				bo->d->model ? bo->d->model : "n/a",
				bo->d->revision ? bo->d->revision : "n/a",
				qprefix(bo->d->size,1,buf,sizeof(buf),0),
				bo->d->physsec,
				"n/a",
				bo->d->wwn ? bo->d->wwn : "n/a",
				transport_str(bo->d->mddev.transport),
				cols - 78,cols - 78,"");
	}
		break;
case LAYOUT_DM:
	rolestr = "dm";
	if(selected){
		assert(wattrset(rb->win,A_BOLD|A_REVERSE|COLOR_PAIR(MDADM_COLOR)) == OK);
	}else{
		assert(wattrset(rb->win,A_BOLD|COLOR_PAIR(MDADM_COLOR)) == OK);
	}
	if(line + topp >= 1){
	mvwprintw(rb->win,line,START_COL,"%-11.11s %-16.16s %4.4s " PREFIXFMT " %4uB %-6.6s%-16.16s %-4.4s %-*.*s",
				bo->d->name,
				bo->d->model ? bo->d->model : "n/a",
				bo->d->revision ? bo->d->revision : "n/a",
				qprefix(bo->d->size,1,buf,sizeof(buf),0),
				bo->d->physsec,
				"n/a",
				bo->d->wwn ? bo->d->wwn : "n/a",
				transport_str(bo->d->dmdev.transport),
				cols - 78,cols - 78,"");
	}
		break;
case LAYOUT_PARTITION:
		break;
case LAYOUT_ZPOOL:
	rolestr = "zpool";
	if(selected){
		assert(wattrset(rb->win,A_BOLD|A_REVERSE|COLOR_PAIR(ZPOOL_COLOR)) == OK);
	}else{
		assert(wattrset(rb->win,A_BOLD|COLOR_PAIR(ZPOOL_COLOR)) == OK);
	}
	if(line + topp >= 1){
	mvwprintw(rb->win,line,START_COL,"%-11.11s %-16.16s %4ju " PREFIXFMT " %4uB %-6.6s%-16.16s %-4.4s %-*.*s",
				bo->d->name,
				bo->d->model ? bo->d->model : "n/a",
				(uintmax_t)bo->d->zpool.zpoolver,
				qprefix(bo->d->size,1,buf,sizeof(buf),0),
				bo->d->physsec,
				"spa",
				bo->d->wwn ? bo->d->wwn : "n/a",
				transport_str(bo->d->zpool.transport),
				cols - 78,cols - 78,"");
		break;
	}
	}
	if(++line >= rows - !endp){
		return;
	}
	if(bo->d->size == 0){
		return;
	}

	if(line + topp >= 1){
		mvwprintw(rb->win,line,START_COL,"%11.11s",bo->d->name);
		if(line - 1 >= 1){
			mvwprintw(rb->win,line - 1,START_COL,"%11.11s","");
		}
	}
	// Print summary below device name, in the same color
	if(line + 1 < rows - !endp && line + topp + 1 >= 1){
		if(rolestr){
			mvwprintw(rb->win,line + 1,START_COL,"%11.11s",rolestr);
		}
	}

	if(selected){
		assert(wattrset(rb->win,A_BOLD|A_REVERSE|COLOR_PAIR(PARTITION_COLOR)) == OK);
	}else{
		assert(wattrset(rb->win,A_BOLD|COLOR_PAIR(PARTITION_COLOR)) == OK);
	}
	if(line + topp >= 1){
		mvwaddch(rb->win,line,START_COL + 10 + 1,ACS_ULCORNER);
		mvwhline(rb->win,line,START_COL + 2 + 10,ACS_HLINE,cols - START_COL * 2 - 2 - 10);
		mvwaddch(rb->win,line,cols - START_COL * 2,ACS_URCORNER);
	}
	if(++line >= rows - !endp){
		return;
	}
	if(line + topp >= 1){
		mvwaddch(rb->win,line,START_COL + 10 + 1,ACS_VLINE);
		print_blockbar(rb->win,bo,line,START_COL + 10 + 2,
					cols - START_COL - 1,selected);
	}
	if(selected){
		assert(wattrset(rb->win,A_BOLD|A_REVERSE|COLOR_PAIR(PARTITION_COLOR)) == OK);
	}else{
		assert(wattrset(rb->win,A_BOLD|COLOR_PAIR(PARTITION_COLOR)) == OK);
	}
	if(line + topp >= 1){
		mvwaddch(rb->win,line,cols - START_COL * 2,ACS_VLINE);
	}
	if(++line >= rows - !endp){
		return;
	}
	if(line + topp >= 1){
		mvwaddch(rb->win,line,START_COL + 10 + 1,ACS_LLCORNER);
		mvwhline(rb->win,line,START_COL + 2 + 10,ACS_HLINE,cols - START_COL * 2 - 2 - 10);
		mvwaddch(rb->win,line,cols - START_COL * 2,ACS_LRCORNER);
	}
	++line;
}

static void
print_adapter_devs(const adapterstate *as,int rows,int cols,
				unsigned topp,unsigned endp){
	// If the interface is down, we don't lead with the summary line
	const blockobj *cur;
	const reelbox *rb;
	long line;

	if((rb = as->rb) == NULL){
		return;
	}
	if(as->expansion == EXPANSION_NONE){
		return;
	}
	// First, print the selected device (if there is one), and those above
	cur = rb->selected;
	line = rb->selline;
	while(cur && line + (long)device_lines(as->expansion,cur) >= !!topp){
		print_dev(rb,cur,line,rows,cols,topp,endp);
		// here we traverse, then account...
		if( (cur = cur->prev) ){
			line -= device_lines(as->expansion,cur);
		}
	}
	line = rb->selected ? (rb->selline +
		(long)device_lines(as->expansion,rb->selected)) : -(long)topp + 1;
	cur = (rb->selected ? rb->selected->next : as->bobjs);
	while(cur && line < rows){
		print_dev(rb,cur,line,rows,cols,topp,endp);
		// here, we account before we traverse. this is correct.
		line += device_lines(as->expansion,cur);
		cur = cur->next;
	}
}

static int
redraw_adapter(const reelbox *rb){
	const adapterstate *as = rb->as;
	int scrrows,scrcols,rows,cols;
	unsigned topp,endp;

	if(panel_hidden(rb->panel)){
		return OK;
	}
	getmaxyx(stdscr,scrrows,scrcols);
	assert(scrcols); // FIXME
	if(adapter_wholly_visible_p(scrrows,rb) || active){ // completely vasible
		topp = endp = 0;
	}else if(getbegy(rb->win) == 1){ // no top
		topp = adapter_lines_unbounded(as) - getmaxy(rb->win);
		endp = 0;
	}else{
		topp = 0;
		endp = 1; // no bottom FIXME
	}
	getmaxyx(rb->win,rows,cols);
	assert(cols); // FIXME
	assert(werase(rb->win) != ERR);
	adapter_box(as,rb->win,topp,endp);
	print_adapter_devs(as,rows,cols,topp,endp);
	return OK;
}

static int
select_adapter_dev(reelbox *rb,blockobj *bo,int delta){
	assert(bo != rb->selected);
	if((rb->selected = bo) == NULL){
		rb->selline = -1;
	}else{
		rb->selline += delta;
	}
	return redraw_adapter(rb);
}

// Positive delta moves down, negative delta moves up, except for l2 == NULL
// where we always move to -1 (and delta is ignored).
static void
select_adapter_node(reelbox *rb,struct blockobj *bo,int delta){
	assert(bo != rb->selected);
	if((rb->selected = bo) == NULL){
		rb->selline = -1;
	}else{
		rb->selline += delta;
	}
	redraw_adapter(rb);
}

static void
deselect_adapter_locked(void){
	reelbox *rb;

	if((rb = current_adapter) == NULL){
		return;
	}
	if(rb->selected == NULL){
		return;
	}
	select_adapter_node(rb,NULL,0); // calls redraw_adapter()
}

// Move this adapter, possibly hiding it. Negative delta indicates movement
// up, positive delta moves down. rows and cols describe the containing window.
static void
move_adapter(reelbox *rb,int targ,int rows,int cols,int delta){
	const adapterstate *as;
	int nlines,rr;

	as = rb->as;
	//fprintf(stderr,"  moving %s (%d) from %d to %d (%d)\n",is->adapter->name,
	//	      adapter_lines_bounded(is,rows),getbegy(rb->win),targ,delta);
	assert(rb->as);
	assert(rb->as->rb == rb);
	assert(werase(rb->win) != ERR);
	screen_update();
	if(adapter_wholly_visible_p(rows,rb)){
		assert(move_panel(rb->panel,targ,1) != ERR);
		if(getmaxy(rb->win) != adapter_lines_bounded(as,rows)){
			assert(wresize(rb->win,adapter_lines_bounded(as,rows),PAD_COLS(cols)) == OK);
			if(panel_hidden(rb->panel)){
				assert(show_panel(rb->panel) == OK);
			}
		}
		assert(redraw_adapter(rb) == OK);
		return;
	}
	rr = getmaxy(rb->win);
	if(delta > 0){ // moving down
		if(targ >= rows - 1){
			assert(hide_panel(rb->panel) != ERR);
			return;
		}
		nlines = rows - targ - 1; // sans-bottom partial
	}else{
		if((rr + getbegy(rb->win)) <= -delta){
			assert(hide_panel(rb->panel) != ERR);
			return;
		}
		if(targ < 1){
			nlines = rr + (targ - 1);
			targ = 1;
		}else{
			nlines = adapter_lines_bounded(as,rows - targ + 1);
		}
	}
	if(nlines < 1){
		assert(hide_panel(rb->panel) != ERR);
		return;
	}else if(nlines > rr){
		assert(move_panel(rb->panel,targ,1) == OK);
		assert(wresize(rb->win,nlines,PAD_COLS(cols)) == OK);
	}else if(nlines < rr){
		assert(wresize(rb->win,nlines,PAD_COLS(cols)) == OK);
		assert(move_panel(rb->panel,targ,1) == OK);
	}else{
		assert(move_panel(rb->panel,targ,1) == OK);
	}
	assert(redraw_adapter(rb) == OK);
	assert(show_panel(rb->panel) == OK);
	return;
}

static inline void
move_adapter_generic(reelbox *rb,int rows,int cols,int delta){
	move_adapter(rb,rb->scrline,rows,cols,delta);
	rb->scrline = getbegy(rb->win);
}

static void
free_reelbox(reelbox *rb){
	if(rb){
		assert(rb->as);
		assert(rb->as->rb == rb);

		rb->as->rb = NULL;
		assert(delwin(rb->win) == OK);
		assert(del_panel(rb->panel) == OK);
		free(rb);
	}
}

// An adapter (pusher) has had its bottom border moved up or down (positive or
// negative delta, respectively). Update the adapters below it on the screen
// (all those up until those actually displayed above it on the screen). Should
// be called before pusher->scrline has been updated.
static void
push_adapters_below(reelbox *pusher,int rows,int cols,int delta){
	reelbox *rb;

	if(delta <= 0){
		return;
	}
	//fprintf(stderr,"pushing down %d from %s@%d\n",delta,pusher ? pusher->as ? pusher->as->adapter->name : "destroyed" : "all",
	//	      pusher ? pusher->scrline : 0);
	rb = last_reelbox;
	while(rb){
		if(rb == pusher){
			break;
		}
		rb->scrline += delta;
		move_adapter_generic(rb,rows,cols,delta);
		if(panel_hidden(rb->panel)){
			if((last_reelbox = rb->prev) == NULL){
				top_reelbox = NULL;
			}else{
				last_reelbox->next = NULL;
			}
			free_reelbox(rb);
			rb = last_reelbox;
		}else{
			rb = rb->prev;
		}
	}
	// Now, if our delta was negative, see if we pulled any down below us
	// FIXME pull_adapters_down();
}

static void
detail_fs(WINDOW *hw,const device *d,int row){
	char buf[BPREFIXSTRLEN + 1];

	if(d->mnttype){
		mvwprintw(hw,row,START_COL,BPREFIXFMT "B %s filesystem",
			bprefix(d->mntsize,1,buf,sizeof(buf),1),
				d->mnttype);
	}
}

static int
update_details(WINDOW *hw){
	const controller *c = get_current_adapter();
	char buf[PREFIXSTRLEN + 1];
	const blockobj *b;
	const device *d;
	int cols,rows,n;

	if(c == NULL){
		return 0; // FIXME hide thyself!
	}
	getmaxyx(hw,rows,cols);
	if(cols < START_COL * 2){
		return 0;
	}
	if(rows == 0){
		return 0;
	}
	for(n = 1 ; n < rows - 1 ; ++n){
		mvwhline(hw,n,START_COL,' ',cols - 2);
	}
	assert(wattrset(hw,SUBDISPLAY_ATTR) != ERR);
	assert(mvwprintw(hw,1,START_COL,"%-*.*s",cols - 2,cols - 2,c->name) != ERR);
	if(rows == 1){
		return 0;
	}
	if(c->bus == BUS_VIRTUAL){
		assert(mvwprintw(hw,2,START_COL,"%-*.*s",cols - 2,cols - 2,"No details available") != ERR);
	}else{
		assert(mvwprintw(hw,2,START_COL,"Firmware: %s BIOS: %s",
					c->fwver ? c->fwver : "Unknown",
					c->biosver ? c->biosver : "Unknown") != ERR);
	}
	if((b = current_adapter->selected) == NULL){
		return 0;
	}
	d = b->d;
	if(d->layout == LAYOUT_NONE){
		mvwprintw(hw,3,START_COL,"%s: %s %s (%s) S/N: %s",d->name,
					d->model ? d->model : "n/a",
					d->revision ? d->revision : "n/a",
					qprefix(d->size,1,buf,sizeof(buf),0),
					d->blkdev.serial ? d->blkdev.serial : "n/a");
		mvwprintw(hw,4,START_COL,"Logical/physical/total sectors: %zuB/%zuB/%ju Transport: %s",
					d->logsec,d->physsec,
					d->size / (d->logsec ? d->logsec : 1),
					transport_str(d->blkdev.transport));
	}else{
		mvwprintw(hw,3,START_COL,"%s: %s %s (%s)",d->name,
					d->model,d->revision,
					qprefix(d->size,1,buf,sizeof(buf),0));
		mvwprintw(hw,4,START_COL,"Logical/physical/total sectors: %zuB/%zuB/%ju",
					d->logsec,d->physsec,
					d->size / (d->logsec ? d->logsec : 1));
	}
	mvwprintw(hw,5,START_COL,"I/O scheduler: %s",d->sched);
	if(b->zone){
		char buf[BPREFIXSTRLEN + 1];

		if(b->zone->p){
			switch(b->zone->p->layout){
			case LAYOUT_NONE:
			case LAYOUT_MDADM:
			case LAYOUT_ZPOOL:
			case LAYOUT_DM:
			mvwprintw(hw,6,START_COL,BPREFIXFMT "B LBA %u→%u: %s",
					bprefix(d->logsec * (b->zone->lsector - b->zone->fsector + 1),1,buf,sizeof(buf),1),
					b->zone->fsector,b->zone->lsector,
					b->zone->p->name);
			detail_fs(hw,b->zone->p,7);
			break;
			case LAYOUT_PARTITION:
			mvwprintw(hw,6,START_COL,BPREFIXFMT "B P%02x LBA %u→%u: %s (%ls) type %04x",
					bprefix(d->logsec * (b->zone->lsector - b->zone->fsector + 1),1,buf,sizeof(buf),1),
					b->zone->p->partdev.pnumber,
					b->zone->fsector,b->zone->lsector,
					b->zone->p->name,
					b->zone->p->partdev.pname ?
					 b->zone->p->partdev.pname :
					 L"unnamed",b->zone->p->partdev.ptype);
			detail_fs(hw,b->zone->p,7);
			break;
			}
		}else{
			mvwprintw(hw,6,START_COL,BPREFIXFMT "B LBA %u→%u: unpartitioned space",
					bprefix(d->logsec * (b->zone->lsector - b->zone->fsector + 1),1,buf,sizeof(buf),1),
					b->zone->fsector,b->zone->lsector);
		}
	}else{
		mvwprintw(hw,6,START_COL,"Media is not loaded");
	}
	return 0;
}

static int
update_details_cond(WINDOW *w){
	if(details.p){
		return update_details(w);
	}
	return 0;
}

static void pull_adapters_down(reelbox *,int,int,int);

// Pass a NULL puller to move all adapters up
static void
pull_adapters_up(reelbox *puller,int rows,int cols,int delta){
	reelbox *rb;

	if(delta <= 0){
		return;
	}
	rb = puller ? puller->next : top_reelbox;
	while(rb){
		rb->scrline -= delta;
		move_adapter_generic(rb,rows,cols,-delta);
		if(panel_hidden(rb->panel)){
			if((top_reelbox = rb->next) == NULL){
				last_reelbox = NULL;
			}else{
				top_reelbox->prev = NULL;
			}
			free_reelbox(rb);
			rb = top_reelbox;
		}else{
			rb = rb->next;
		}
	}
	while(last_reelbox){
		struct adapterstate *i;
		int scrline;

		if((scrline = last_reelbox->scrline + getmaxy(last_reelbox->win)) >= rows - 2){
			return;
		}
		i = last_reelbox->as->next;
		if(i->rb){
			return; // next adapter is already visible
		}
		if((rb = create_reelbox(i,rows,scrline + 1,cols)) == NULL){
			return;
		}
		rb->prev = last_reelbox;
		last_reelbox->next = rb;
		rb->next = NULL;
		last_reelbox = rb;
		redraw_adapter(rb);
	}
}


static inline int
gap_above(reelbox *rb){
	if(!rb->prev){
		return 0;
	}
	return getbegy(rb->win) - (getmaxy(rb->prev->win) + getbegy(rb->prev->win)) - 1;
}

static inline int
gap_below(reelbox *rb){
	if(!rb->next){
		return 0;
	}
	return getbegy(rb->next->win) - (getmaxy(rb->win) + getbegy(rb->win)) - 1;
}

// An adapter (pusher) has had its top border moved up or down (positive or
// negative delta, respectively). Update the adapters above it on the screen
// (all those up until those actually displayed below it on the screen).
//
// If an adapter is being brought onto the bottom of the screen, ensure that
// last_reelbox has been updated to point to it, and top_reelbox has been
// updated if the adapter came from the top of the screen. Any other updates
// to reelboxes will be made by this function. Otherwise....
//
//     Update before: pusher->scrline, current_adapter
//     Updated after: reelbox pointers, affected scrlines.
//
static void
push_adapters_above(reelbox *pusher,int rows,int cols,int delta){
	reelbox *rb;

	assert(delta < 0);
	//fprintf(stderr,"pushing up %d from %s@%d\n",delta,pusher ? pusher->as ? pusher->as->adapter->name : "destroyed" : "all",
	//	      pusher ? pusher->scrline : rows);
	rb = top_reelbox;
	while(rb){
		if(rb == pusher){
			break;
		}
		rb->scrline += delta;
		move_adapter_generic(rb,rows,cols,delta);
		if(panel_hidden(rb->panel)){
			if((top_reelbox = rb->next) == NULL){
				last_reelbox = NULL;
			}else{
				top_reelbox->prev = NULL;
			}
			free_reelbox(rb);
			rb = top_reelbox;
		}else{
			rb = rb->next;
		}
	}
	// Now, if our delta was negative, see if we pulled any down below us
	// FIXME pull_adapters_up(pusher,rows,cols,delta);
}

// Upon entry, the display might not have been updated to reflect a change in
// the adapter's data. If so, the adapter panel is resized (subject to the
// containing window's constraints) and other panels are moved as necessary.
// The adapter's display is synchronized via redraw_adapter() whether a resize
// is performed or not (unless it's invisible). The display ought be partially
// visible -- ie, if we ought be invisible, we ought be already and this is not
// going to make us so. We do not redraw -- that's the callers job (we
// can't redraw, since we might not yet have been moved).
static int
resize_adapter(reelbox *rb){
	const controller *i,*curi = get_current_adapter();
	int rows,cols,subrows;
	adapterstate *is;

	i = rb->as->c;
	if(panel_hidden(rb->panel)){ // resize upon becoming visible
		return OK;
	}
	is = rb->as;
	getmaxyx(stdscr,rows,cols);
	const int nlines = adapter_lines_bounded(is,rows);
	subrows = getmaxy(rb->win);
	if(nlines < subrows){ // Shrink the adapter
		werase(rb->win);
		// Without screen_update(), the werase() doesn't take effect,
		// even if wclear() is used.
		screen_update();
		wresize(rb->win,nlines,PAD_COLS(cols));
		replace_panel(rb->panel,rb->win);
		if(rb->scrline < current_adapter->scrline){
			rb->scrline += subrows - nlines;
			move_panel(rb->panel,rb->scrline,1);
			pull_adapters_down(rb,rows,cols,subrows - nlines);
		}else{
			pull_adapters_up(rb,rows,cols,subrows - nlines);
		}
		return OK;
	}else if(nlines == subrows){ // otherwise, expansion
		return OK;
	}
	// The current adapter grows in both directions and never becomes a
	// partial adapter. We don't try to make it one here, and
	// move_adapter() will refuse to perform a move resulting in one.
	if(i == curi){
		// We can't already occupy the screen, or the nlines == subrows
		// check would have thrown us out. There *is* space to grow.
		if(rb->scrline + subrows < rows - 1){ // can we grow down?
			int delta = (rows - 1) - (rb->scrline + subrows);

			if(delta + subrows > nlines){
				delta = nlines - subrows;
			}
			push_adapters_below(rb,rows,cols,delta);
			subrows += delta;
		}
		if(nlines > subrows){ // can we grow up?
			int delta = rb->scrline - 1;

			if(delta + subrows > nlines){
				delta = nlines - subrows;
			}
			delta = -delta;
			rb->scrline += delta;
			push_adapters_above(rb,rows,cols,delta);
			move_panel(rb->panel,rb->scrline,1);
		}
		wresize(rb->win,nlines,PAD_COLS(cols));
		replace_panel(rb->panel,rb->win);
	}else{ // we're not the current adapter
		int delta;

		if( (delta = bottom_space_p(rows)) ){ // always occupy free rows
			if(delta > nlines - subrows){
				delta = nlines - subrows;
			}
			delta -= gap_below(rb); // FIXME questionable
			push_adapters_below(rb,rows,cols,delta);
			subrows += delta;
		}
		if(nlines > subrows){
			if(rb->scrline > current_adapter->scrline){ // only down
				delta = (rows - 1) - (rb->scrline + subrows);
				if(delta > nlines - subrows){
					delta = nlines - subrows;
				}
				delta -= gap_below(rb);
				if(delta > 0){
					push_adapters_below(rb,rows,cols,delta);
				}
			}else{ // only up
				delta = rb->scrline - 1;
				if(delta > nlines - subrows){
					delta = nlines - subrows;
				}
				delta -= gap_above(rb);
				if(delta){
					push_adapters_above(rb,rows,cols,-delta);
					rb->scrline -= delta;
					move_adapter_generic(rb,rows,cols,-delta);
				}
			}
			subrows += delta;
			if(nlines > subrows){
				if( (delta = gap_below(rb)) ){
					subrows += delta > (nlines - subrows) ?
						nlines - subrows : delta;
				}
			}
			if(nlines > subrows){
				if( (delta = gap_above(rb)) ){
					subrows += delta > (nlines - subrows) ?
						nlines - subrows : delta;
				}
			}
		}
		if(subrows != getmaxy(rb->win)){
			assert(wresize(rb->win,subrows,PAD_COLS(cols)) != ERR);
			assert(replace_panel(rb->panel,rb->win) != ERR);
		}
	}
	return OK;
}

// Pull the adapters above the puller down to fill unused space. Move from
// the puller out, as we might need make visible some unknown number of
// adapters (and the space has already been made).
//
// If the puller is being removed, it ought already have been spliced out of
// the reelbox list, and all reelbox state updated, but it obviously must not
// yet have been freed. Its ->as pointer must still be valid (though
// ->as->adapter is no longer valid). Its ->next and ->prev pointers ought not
// have been altered.
static void
pull_adapters_down(reelbox *puller,int rows,int cols,int delta){
	reelbox *rb;

	if(delta <= 0){
		return;
	}
	rb = puller ? puller->prev : last_reelbox;
	while(rb){
		int before = getmaxy(rb->win);

		if(adapter_lines_bounded(rb->as,rows) > before){
			assert(rb == top_reelbox);
			resize_adapter(rb);
			if((delta -= (getmaxy(rb->win) - before)) == 0){
				return;
			}
		}
		rb->scrline += delta;
		move_adapter_generic(rb,rows,cols,delta);
		if(panel_hidden(rb->panel)){
			if((last_reelbox = rb->prev) == NULL){
				top_reelbox = NULL;
			}else{
				last_reelbox->next = NULL;
			}
			free_reelbox(rb);
			rb = last_reelbox;
		}else{
			rb = rb->prev;
		}
	}
	while(top_reelbox){
		struct adapterstate *i;
		int maxl,nl;

		if(top_reelbox->scrline <= 2){
			return;
		}
		i = top_reelbox->as->prev;
		if(i->rb){
			return; // already visible
		}
		nl = adapter_lines_bounded(i,top_reelbox->scrline);
		maxl = top_reelbox->scrline;
		if((rb = create_reelbox(i,maxl,top_reelbox->scrline - 1 - nl,cols)) == NULL){
			return;
		}
		rb->next = top_reelbox;
		top_reelbox->prev = rb;
		rb->prev = NULL;
		top_reelbox = rb;
		redraw_adapter(rb);
	}
}

static inline int
top_space_p(int rows){
	if(!top_reelbox){
		return rows - 2;
	}
	return getbegy(top_reelbox->win) - 1;
}

// Selecting the previous or next adapter (this doesn't apply to an arbitrary
// repositioning): There are two phases to be considered.
//
//  1. There's not enough data to fill the screen. In this case, none will lose
//     or gain visibility, but they might be rotated.
//  2. There's a screen's worth, but not much more than that. An adapter might
//     be split across the top/bottom boundaries. adapters can be caused to
//     lose or gain visibility.
static void
use_next_controller(WINDOW *w,struct panel_state *ps){
	int rows,cols,delta;
	reelbox *oldrb;
	reelbox *rb;

	if(!current_adapter || current_adapter->as->next == current_adapter->as){
		return;
	}
	// fprintf(stderr,"Want next adapter (%s->%s)\n",current_adapter->as->if
	//	      current_adapter->as->next->adapter->name);
	getmaxyx(w,rows,cols);
	oldrb = current_adapter;
	deselect_adapter_locked();
	// Don't redraw the old inteface yet; it might have been moved/hidden
	if(current_adapter->next == NULL){
		adapterstate *is = current_adapter->as->next;

		if(is->rb == NULL){ // it's off-screen
			int delta;

			if((is->rb = create_reelbox(is,rows,(rows - 1) - adapter_lines_bounded(is,rows),cols)) == NULL){
				return; // FIXME
			}
			current_adapter = is->rb;
			delta = -adapter_lines_bounded(is,rows);
			if(getbegy(last_reelbox->win) + getmaxy(last_reelbox->win) >= rows - 1){
				--delta;
			}
			push_adapters_above(NULL,rows,cols,delta);
			if((current_adapter->prev = last_reelbox) == NULL){
				top_reelbox = current_adapter;
			}else{
				last_reelbox->next = current_adapter;
			}
			current_adapter->next = NULL;
			last_reelbox = current_adapter;
			if( (delta = top_space_p(rows)) ){
				pull_adapters_up(NULL,rows,cols,delta);
			}
			redraw_adapter(is->rb);
			if(ps->p){
				update_details(panel_window(ps->p));
			}
			return;
		}
		current_adapter = is->rb; // it's at the top
	}else{
		current_adapter = current_adapter->next; // it's below us
	}
	rb = current_adapter;
	// If the newly-selected adapter is wholly visible, we'll not need
	// change visibility of any adapters. If it's above us, we'll need
	// rotate the adapters 1 unit, moving all. Otherwise, none change
	// position. Redraw all affected adapters.
	if(adapter_wholly_visible_p(rows,rb)){
		if(rb->scrline > oldrb->scrline){ // new is below old
			assert(redraw_adapter(oldrb) == OK);
			assert(redraw_adapter(rb) == OK);
		}else{ // we were at the bottom (rotate)
			if(top_reelbox->next){
				top_reelbox->next->prev = NULL;
				top_reelbox = top_reelbox->next;
			}else{
				top_reelbox = last_reelbox;
			}
			pull_adapters_up(rb,rows,cols,getmaxy(rb->win) + 1);
			if(last_reelbox){
				rb->scrline = last_reelbox->scrline + getmaxy(last_reelbox->win) + 1;
			}else{
				rb->scrline = 1;
			}
			rb->prev = last_reelbox;
			last_reelbox->next = rb;
			rb->next = NULL;
			last_reelbox = rb;
			move_adapter_generic(rb,rows,cols,rb->scrline - getbegy(rb->win));
		}
	}else{ // new is partially visible...
		if(rb->scrline > oldrb->scrline){ // ...at the bottom
			adapterstate *is = current_adapter->as;
			int delta = getmaxy(rb->win) - adapter_lines_bounded(is,rows);

			rb->scrline = rows - (adapter_lines_bounded(is,rows) + 1);
			push_adapters_above(rb,rows,cols,delta);
			move_adapter_generic(rb,rows,cols,getbegy(rb->win) - rb->scrline);
			assert(wresize(rb->win,adapter_lines_bounded(rb->as,rows),PAD_COLS(cols)) == OK);
			assert(replace_panel(rb->panel,rb->win) != ERR);
			assert(redraw_adapter(rb) == OK);
		}else{ // ...at the top (rotate)
			int delta;

			assert(top_reelbox == rb);
			rb->scrline = rows - 1 - adapter_lines_bounded(rb->as,rows);
			top_reelbox->next->prev = NULL;
			top_reelbox = top_reelbox->next;
			delta = -adapter_lines_bounded(rb->as,rows);
			if(getbegy(last_reelbox->win) + getmaxy(last_reelbox->win) >= (rows - 1)){
				--delta;
			}
			push_adapters_above(NULL,rows,cols,delta);
			rb->next = NULL;
			if( (rb->prev = last_reelbox) ){
				last_reelbox->next = rb;
			}else{
				top_reelbox = rb;
			}
			last_reelbox = rb;
			move_adapter_generic(rb,rows,cols,rb->scrline);
			assert(wresize(rb->win,adapter_lines_bounded(rb->as,rows),PAD_COLS(cols)) == OK);
			assert(replace_panel(rb->panel,rb->win) != ERR);
			assert(redraw_adapter(rb) == OK);
		}
	}
	if( (delta = top_space_p(rows)) ){
		pull_adapters_up(NULL,rows,cols,delta);
	}
	if(ps->p){
		update_details(panel_window(ps->p));
	}
}

static void
use_prev_zone(blockobj *b){
	if(b->zone){
		b->zone = b->zone->prev;
	}
}

static void
use_next_zone(blockobj *b){
	if(b->zone){
		b->zone = b->zone->next;
	}
}

static void
use_prev_controller(WINDOW *w,struct panel_state *ps){
	reelbox *oldrb,*rb;
	int rows,cols;

	if(!current_adapter || current_adapter->as->next == current_adapter->as){
		return;
	}
	getmaxyx(w,rows,cols);
	oldrb = current_adapter;
	deselect_adapter_locked();
	// Don't redraw the old adapter yet; it might have been moved/hidden
	if(current_adapter->prev){
		current_adapter = current_adapter->prev;
	}else{
		adapterstate *as = current_adapter->as->prev;

		if(as->rb){
			current_adapter = as->rb;
		}else{
			if((as->rb = create_reelbox(as,rows,1,cols)) == NULL){
				return; // FIXME
			}
			current_adapter = as->rb;
			push_adapters_below(NULL,rows,cols,adapter_lines_bounded(as,rows) + 1);
			if((current_adapter->next = top_reelbox) == NULL){
				last_reelbox = current_adapter;
			}else{
				top_reelbox->prev = current_adapter;
			}
			current_adapter->prev = NULL;
			top_reelbox = current_adapter;
			redraw_adapter(current_adapter);
			if(ps->p){
				update_details(panel_window(ps->p));
			}
			return;
		}
	}
	rb = current_adapter;
	// If the newly-selected adapter is wholly visible, we'll not need
	// change visibility of any adapters. If it's below us, we'll need
	// rotate the adapters 1 unit, moving all. Otherwise, none need change
	// position. Redraw all affected adapters.
	if(adapter_wholly_visible_p(rows,rb)){
		if(rb->scrline < oldrb->scrline){ // new is above old
			assert(redraw_adapter(oldrb) == OK);
			assert(redraw_adapter(rb) == OK);
		}else{ // we were at the top
			// Selecting the previous adapter is simpler -- we
			// take one from the bottom, and stick it in row 1.
			if(last_reelbox->prev){
				last_reelbox->prev->next = NULL;
				last_reelbox = last_reelbox->prev;
			}else{
				last_reelbox = top_reelbox;
			}
			pull_adapters_down(rb,rows,cols,getmaxy(rb->win) + 1);
			rb->scrline = 1;
			rb->next = top_reelbox;
			top_reelbox->prev = rb;
			rb->prev = NULL;
			top_reelbox = rb;
			move_adapter_generic(rb,rows,cols,getbegy(rb->win) - rb->scrline);
		}
	}else{ // partially visible...
		adapterstate *is = current_adapter->as;

		if(rb->scrline < oldrb->scrline){ // ... at the top
			rb->scrline = 1;
			push_adapters_below(rb,rows,cols,-(getmaxy(rb->win) - adapter_lines_bounded(is,rows)));
			assert(wresize(rb->win,adapter_lines_bounded(rb->as,rows),PAD_COLS(cols)) == OK);
			assert(replace_panel(rb->panel,rb->win) != ERR);
			assert(redraw_adapter(rb) == OK);
		}else{ // at the bottom
			if(last_reelbox->prev){
				last_reelbox->prev->next = NULL;
				last_reelbox = last_reelbox->prev;
			}else{
				last_reelbox = top_reelbox;
			}
			push_adapters_below(NULL,rows,cols,adapter_lines_bounded(is,rows) + 1);
			rb->scrline = 1;
			if( (rb->next = top_reelbox) ){
				top_reelbox->prev = rb;
			}else{
				last_reelbox = rb;
			}
			rb->prev = NULL;
			top_reelbox = rb;
			move_adapter_generic(rb,rows,cols,getbegy(rb->win) - rb->scrline);
			assert(wresize(rb->win,adapter_lines_bounded(rb->as,rows),PAD_COLS(cols)) == OK);
			assert(replace_panel(rb->panel,rb->win) != ERR);
			assert(redraw_adapter(rb) == OK);
		}
	}
	if(ps->p){
		update_details(panel_window(ps->p));
	}
}

static void
use_prev_device(void){
	reelbox *rb;
	int delta;

	if((rb = current_adapter) == NULL){
		return;
	}
	if(rb->selected == NULL || rb->selected->prev == NULL){
		return;
	}
	delta = -device_lines(rb->as->expansion,rb->selected->prev);
	if(rb->selline + delta <= 1){ // FIXME verify
		delta = 1 - rb->selline;
	}
	select_adapter_dev(rb,rb->selected->prev,delta);
}

static void
use_next_device(void){
	reelbox *rb;
	int delta;

	if((rb = current_adapter) == NULL){
		return;
	}
	if(rb->selected == NULL || rb->selected->next == NULL){
		return;
	}
	delta = device_lines(rb->as->expansion,rb->selected);
	if(rb->selline + delta + device_lines(rb->as->expansion,rb->selected->next) >= getmaxy(rb->win) - 1){
		delta = (getmaxy(rb->win) - 1 - device_lines(rb->as->expansion,rb->selected->next))
				- rb->selline;
	}
	select_adapter_dev(rb,rb->selected->next,delta);
}

// Create a panel at the bottom of the window, referred to as the "subdisplay".
// Only one can currently be active at a time. Window decoration and placement
// is managed here; only the rows needed for display ought be provided.
static int
new_display_panel(WINDOW *w,struct panel_state *ps,int rows,int cols,const wchar_t *hstr){
	const wchar_t crightstr[] = L"http://nick-black.com/dankwiki/index.php/Growlight";
	const int crightlen = wcslen(crightstr);
	WINDOW *psw;
	int x,y;

	getmaxyx(w,y,x);
	if(cols == 0){
		cols = x - START_COL * 2; // indent 2 on the left, 0 on the right
	}else{
		assert(x >= cols + START_COL * 2);
	}
	assert(y >= rows + 3);
	assert((x >= crightlen + START_COL * 2));
	// Six up from the bottom, so it looks good with our logo in the
	// installer, heh
	assert( (psw = newwin(rows + 2,cols,y - (rows + 9),x - cols)) );
	if(psw == NULL){
		return ERR;
	}
	assert((ps->p = new_panel(psw)));
	assert(top_panel(ps->p) != ERR);
	if(ps->p == NULL){
		delwin(psw);
		return ERR;
	}
	ps->ysize = rows;
	// memory leaks follow if we're compiled with NDEBUG! FIXME
	assert(wattron(psw,A_BOLD) != ERR);
	assert(wcolor_set(psw,PBORDER_COLOR,NULL) == OK);
	assert(bevel(psw) == OK);
	assert(wattroff(psw,A_BOLD) != ERR);
	assert(wcolor_set(psw,PHEADING_COLOR,NULL) == OK);
	assert(mvwaddwstr(psw,0,START_COL * 2,hstr) != ERR);
	assert(mvwaddwstr(psw,rows + 1,cols - (crightlen + START_COL * 2),crightstr) != ERR);
	return OK;
}

// When this text is being displayed, the help window is the active window.
// Thus we refer to other window commands as "viewing", while 'H' here is
// described as "toggling". When other windows come up, they list their
// own command as "toggling." We want to avoid having to scroll the help
// synopsis, so keep it under 22 lines (25 lines on an ANSI standard terminal,
// minus two for the top/bottom screen border, minus one for mandatory
// window top padding).
static const wchar_t *helps[] = {
	L"(q)uit                        ctrl+'L': redraw the screen",
	L"'e': view environment details 'H': toggle this help display",
	L"'v': view selection details   'D': view recent diagnostics",
	L"'E': view active mountpoints / installpoints",
	L"'-': collapse adapter         '+': expand adapter",
	L"'R': rescan selection         'S': reset selection",
	L"'⏎Enter': browse adapter      '⌫BkSpc': leave adapter browser",
	L"'k'/'↑': navigate up          'j'/'↓': navigate down",
	NULL
};

static const wchar_t *helps_block[] = {
	L"'h'/'←': navigate left        'l'/'→': navigate right",
	L"'m': make partition table     'r': remove partition table",
	L"'W': wipe master boot record  'B': bad blocks check",
	L"'n': new partition            'd': delete partition",
	L"'s': set partition attributes 'M': make new filesystem",
	L"'F': fsck filesystem          'w': wipe filesystem",
	L"'U': set UUID                 'L': set label/name",
	L"'b': bind to aggregate        'f': free from aggregate",
	L"'o': mount filesystem         'O': unmount filesystem",
	NULL
};

static const wchar_t *helps_target[] = {
	L"'i': set target               'I': unset target",
	L"'t': mount target             'T': unmount target",
	NULL
};

static size_t
max_helpstr_len(const wchar_t **h){
	size_t max = 0;

	while(*h){
		if(wcslen(*h) > max){
			max = wcslen(*h);
		}
		++h;
	}
	return max;
}

static int
helpstrs(WINDOW *hw,int row){
	const wchar_t *hs;
	int z,rows;

	rows = getmaxy(hw);
	assert(wattrset(hw,SUBDISPLAY_ATTR) == OK);
	for(z = 0 ; (hs = helps[z]) && z < rows ; ++z){
		assert(mvwaddwstr(hw,row + z,START_COL,hs) != ERR);
	}
	row += z;
	if(!current_adapter || !current_adapter->selected){
		assert(wattrset(hw,SUBDISPLAY_INVAL_ATTR) == OK);
	}else{
		assert(wattrset(hw,SUBDISPLAY_ATTR) == OK);
	}
	for(z = 0 ; (hs = helps_block[z]) && z < rows ; ++z){
		assert(mvwaddwstr(hw,row + z,START_COL,hs) != ERR);
	}
	row += z;
	if(!target_mode_p()){
		assert(wattrset(hw,SUBDISPLAY_INVAL_ATTR) == OK);
	}else{
		assert(wattrset(hw,SUBDISPLAY_ATTR) == OK);
	}
	for(z = 0 ; (hs = helps_target[z]) && z < rows ; ++z){
		assert(mvwaddwstr(hw,row + z,START_COL,hs) != ERR);
	}
	return OK;
}

static int
update_help_cond(WINDOW *w){
	if(help.p){
		return helpstrs(w,1);
	}
	return 0;
}

static int
mount_target(void){
	// FIXME
	return 0;
}

static int
umount_target(void){
	// FIXME
	return 0;
}

static const int DIAGROWS = 8;

// Used after shutting down on error, which will clean the screen. This takes
// the last few diagnostics and prints them to stderr.
static int
dump_diags(void){
	logent l[10];
	int y,r;

	y = sizeof(l) / sizeof(*l);
	if((y = get_logs(y,l)) < 0){
		return -1;
	}
	for(r = 0 ; r < y ; ++r){
		char tbuf[27],*c;

		if(l[r].msg == NULL){
			break;
		}
		assert(ctime_r(&l[r].when,tbuf));
		tbuf[strlen(tbuf) - 1] = ' '; // kill newline
		while( (c = strchr(l[r].msg,'\n')) || (c = strchr(l[r].msg,'\t')) ){
			*c = ' ';
		}
		fprintf(stderr,"%s %s\n",tbuf,l[r].msg);
		free(l[r].msg);
	}
	return 0;
}

static int
update_diags(struct panel_state *ps){
	WINDOW *w = panel_window(ps->p);
	logent l[DIAGROWS];
	int y,x,r;

	getmaxyx(w,y,x);
	y = ps->ysize;
	assert(x > 26 + START_COL * 2); // see ctime_r(3)
	if((y = get_logs(y,l)) < 0){
		return -1;
	}
	assert(wattrset(w,SUBDISPLAY_ATTR) == OK);
	for(r = 0 ; r < y ; ++r){
		char *c,tbuf[x - START_COL * 2 + 1];
		struct tm tm;
		size_t tb;
		int p;

		if(l[r].msg == NULL){
			break;
		}
		if(localtime_r(&l[r].when,&tm) == NULL){
			break;
		}
		if(strftime(tbuf,sizeof(tbuf),"%F %T",&tm) == 0){
			break;;
		}
		tb = sizeof(tbuf) / sizeof(*tbuf) - strlen(tbuf);
		p = snprintf(tbuf + strlen(tbuf),tb," %s",l[r].msg);
		if(p < 0 || (unsigned)p >= tb){
			tbuf[sizeof(tbuf) / sizeof(*tbuf) - 1] = '\0';
		}
		if( (c = strchr(tbuf,'\n')) ){
			*c = '\0';
		}
		assert(mvwprintw(w,y - r,START_COL,"%s",tbuf) != ERR);
		free(l[r].msg);
	}
	return 0;
}

static int
display_diags(WINDOW *mainw,struct panel_state *ps){
	memset(ps,0,sizeof(*ps));
	if(new_display_panel(mainw,ps,DIAGROWS,0,L"press 'D' to dismiss diagnostics")){
		goto err;
	}
	if(update_diags(ps)){
		goto err;
	}
	return OK;

err:
	if(ps->p){
		WINDOW *psw = panel_window(ps->p);

		hide_panel(ps->p);
		del_panel(ps->p);
		delwin(psw);
	}
	memset(ps,0,sizeof(*ps));
	return ERR;
}

static const int DETAILROWS = 7; // FIXME make it dynamic based on selections

static int
display_details(WINDOW *mainw,struct panel_state *ps){
	memset(ps,0,sizeof(*ps));
	if(new_display_panel(mainw,ps,DETAILROWS,78,L"press 'v' to dismiss details")){
		goto err;
	}
	if(current_adapter){
		if(update_details(panel_window(ps->p))){
			goto err;
		}
	}
	return 0;

err:
	if(ps->p){
		WINDOW *psw = panel_window(ps->p);

		hide_panel(ps->p);
		del_panel(ps->p);
		delwin(psw);
	}
	memset(ps,0,sizeof(*ps));
	return ERR;
}

static int
display_help(WINDOW *mainw,struct panel_state *ps){
	static const int helprows = sizeof(helps) / sizeof(*helps) - 1 +
		sizeof(helps_block) / sizeof(*helps_block) - 1 +
		sizeof(helps_target) / sizeof(*helps_target) - 1; // NULL != row
	unsigned helpcols;

	helpcols = max_helpstr_len(helps);
	if(max_helpstr_len(helps_target) > helpcols){
		helpcols = max_helpstr_len(helps_target);
	}
	if(max_helpstr_len(helps_block) > helpcols){
		helpcols = max_helpstr_len(helps_block);
	}
	helpcols += 4; // spacing + borders
	memset(ps,0,sizeof(*ps));
	if(new_display_panel(mainw,ps,helprows,helpcols,L"press 'H' to dismiss help")){
		goto err;
	}
	if(helpstrs(panel_window(ps->p),1)){
		goto err;
	}
	return OK;

err:
	if(ps->p){
		WINDOW *psw = panel_window(ps->p);

		hide_panel(ps->p);
		del_panel(ps->p);
		delwin(psw);
	}
	memset(ps,0,sizeof(*ps));
	return ERR;
}

#define ENVROWS 10
#define COLORSPERROW 32

static int
env_details(WINDOW *hw,int rows){
	const int col = START_COL;
	const int row = 1;
	int z,srows,scols;

	assert(wattrset(hw,SUBDISPLAY_ATTR) == OK);
	getmaxyx(stdscr,srows,scols);
	if((z = rows) >= ENVROWS){
		z = ENVROWS - 1;
	}
	switch(z){ // Intentional fallthroughs all the way to 0
	case (ENVROWS - 1):{
		while(z > 1){
			int c0,c1;

			c0 = (z - 2) * COLORSPERROW;
			c1 = c0 + (COLORSPERROW - 1);
			assert(mvwprintw(hw,row + z,col,"0x%02x--0x%02x: ",c0,c1) == OK);
			while(c0 <= c1){
			        if(c0 < COLORS){
			                assert(wattrset(hw,COLOR_PAIR(c0)) == OK);
			                assert(wprintw(hw,"X") == OK);
			        }else{
			                assert(wattrset(hw,SUBDISPLAY_ATTR) == OK);
			                assert(wprintw(hw," ") == OK);
			        }
			        ++c0;
			}
			--z;
			assert(wattrset(hw,SUBDISPLAY_ATTR) == OK);
		}
	}case 1:{
		assert(mvwprintw(hw,row + z,col,"Colors (pairs): %u (%u) Geom: %dx%d",
			        COLORS,COLOR_PAIRS,srows,scols) != ERR);
		--z;
	}case 0:{
		const char *lang = getenv("LANG");
		const char *term = getenv("TERM");

		lang = lang ? lang : "Undefined";
		assert(mvwprintw(hw,row + z,col,"LANG: %-21s TERM: %s  ESCDELAY: %d",lang,term,ESCDELAY) != ERR);
		--z;
		break;
	}default:{
		return ERR;
	}
	}
	return OK;
}

static void
print_mount(WINDOW *w,int *row,int both,const device *d){
	char buf[PREFIXSTRLEN + 1];

	mvwprintw(w,*row,START_COL,"%-*.*s %-5.5s %-36.36s " PREFIXFMT " %-6.6s",
			FSLABELSIZ,FSLABELSIZ,d->label ? d->label : "n/a",
			d->mnttype,
			d->uuid ? d->uuid : "n/a",
			qprefix(d->mntsize,1,buf,sizeof(buf),0),
			d->name);
	++*row;
	if(!both){
		return;
	}
	wattroff(w,A_BOLD);
	mvwprintw(w,*row,START_COL," %s %s",d->mnt,d->mntops);
	wattron(w,A_BOLD);
	++*row;
}

static int
map_details(WINDOW *hw){
	const controller *c;
	int y,rows;

	rows = getmaxy(hw);
	wattrset(hw,A_BOLD|COLOR_PAIR(SUBDISPLAY_COLOR));
	mvwprintw(hw,START_COL,START_COL,"%-*.*s %-5.5s %-36.36s " PREFIXFMT " %s",
			FSLABELSIZ,FSLABELSIZ,"Label",
			"Type","UUID","Bytes","Device");
	if((y = START_COL + 1) >= rows){
		return -1;
	}
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			if(d->mnt){
				print_mount(hw,&y,y + 1 < rows,d);
				if(y >= rows){
					return 0;
				}
			}else if(d->target){
				/*if(print_target(p,p->target) < 0){
					return -1;
				}*/
				if(++y >= rows){
					return 0;
				}
			}
			for(p = d->parts ; p ; p = p->next){
				if(p->mnt){
					print_mount(hw,&y,y + 1 < rows,p);
					if(y >= rows){
						return 0;
					}
				}else if(p->target){
					/*if(print_target(p,p->target) < 0){
						return -1;
					}*/
					if(++y >= rows){
						return 0;
					}
				}
			}
		}
	}
	return 0;
}

static int
display_enviroment(WINDOW *mainw,struct panel_state *ps){
	memset(ps,0,sizeof(*ps));
	if(new_display_panel(mainw,ps,ENVROWS,78,L"press 'e' to dismiss display")){
		goto err;
	}
	if(env_details(panel_window(ps->p),ps->ysize)){
		goto err;
	}
	return OK;

err:
	if(ps->p){
		WINDOW *psw = panel_window(ps->p);

		hide_panel(ps->p);
		del_panel(ps->p);
		delwin(psw);
	}
	memset(ps,0,sizeof(*ps));
	return ERR;
}

static int
display_maps(WINDOW *mainw,struct panel_state *ps){
	// FIXME compute based off number of maps + targets
	unsigned rows = getmaxy(mainw) - 15;

	memset(ps,0,sizeof(*ps));
	if(new_display_panel(mainw,ps,rows,0,L"press 'E' to dismiss display")){
		goto err;
	}
	if(map_details(panel_window(ps->p))){
		goto err;
	}
	return OK;

err:
	if(ps->p){
		WINDOW *psw = panel_window(ps->p);

		hide_panel(ps->p);
		del_panel(ps->p);
		delwin(psw);
	}
	memset(ps,0,sizeof(*ps));
	return ERR;
}

static void
hide_panel_locked(struct panel_state *ps){
	if(ps){
		WINDOW *psw;

		psw = panel_window(ps->p);
		hide_panel(ps->p);
		assert(del_panel(ps->p) == OK);
		ps->p = NULL;
		assert(delwin(psw) == OK);
		ps->ysize = -1;
	}
}

static void
toggle_panel(WINDOW *w,struct panel_state *ps,int (*psfxn)(WINDOW *,struct panel_state *)){
	if(ps->p){
		hide_panel_locked(ps);
		active = NULL;
	}else{
		hide_panel_locked(active);
		active = ((psfxn(w,ps) == OK) ? ps : NULL);
	}
}


static unsigned
node_lines(int e,const blockobj *l){
	unsigned lns;

	if(e == EXPANSION_NONE){
		return 0;
	}
	lns = 1;
	if(l->d->size){
		lns += 2;
	}
	return lns;
}

// Recompute lns values for all nodes, and return the number of lines of
// output available before and after the current selection. If there is no
// current selection, the return value ought not be ascribed meaning. O(N) on
// the number of drives, not just those visible -- unacceptable! FIXME
static void
recompute_lines(adapterstate *is,int *before,int *after){
	blockobj *l;
	int newsel;

	*after = -1;
	*before = -1;
	newsel = 1;
	for(l = is->bobjs ; l ; l = l->next){
		unsigned lns;

		lns = node_lines(is->expansion,l);
		if(l == is->rb->selected){
			*before = newsel;
			*after = lns ? lns - 1 : 0;
		}else if(*after >= 0){
			*after += lns;
		}else{
			newsel += lns;
		}
	}
}

// When we expand or collapse, we want the current selection to contain above
// it approximately the same proportion of the entire adapter. That is, if
// we're at the top, we ought remain so; if we're at the bottom, we ought
// remain so; if we fill the entire screen before and after the operation, we
// oughtn't move more than a few rows at the most.
//
// oldsel: old line of the selection, within the window
// oldrows: old number of rows in the iface
// newrows: new number of rows in the iface
// oldlines: number of lines selection used to occupy
static void
recompute_selection(adapterstate *is,int oldsel,int oldrows,int newrows){
	int newsel,bef,aft;

	// Calculate the maximum new line -- we can't leave space at the top or
	// bottom, so we can't be after the true number of lines of output that
	// precede us, or before the true number that follow us.
	recompute_lines(is,&bef,&aft);
	if(bef < 0 || aft < 0){
		assert(!is->rb->selected);
		return;
	}
	// Account for lost/restored lines within the selection. Negative means
	// we shrank, positive means we grew, 0 stayed the same.
	// Calculate the new target line for the selection
	newsel = oldsel * newrows / oldrows;
	if(oldsel * newrows % oldrows >= oldrows / 2){
		++newsel;
	}
	// If we have a full screen's worth after us, we can go anywhere
	if(newsel > bef){
		newsel = bef;
	}
	/*wstatus_locked(stdscr,"newsel: %d bef: %d aft: %d oldsel: %d maxy: %d",
			newsel,bef,aft,oldsel,getmaxy(is->rb->win));
	update_panels();
	doupdate();*/
	if(newsel + aft <= getmaxy(is->rb->win) - 3){
		newsel = getmaxy(is->rb->win) - aft - 3;
	}
	if(newsel + (int)node_lines(is->expansion,is->rb->selected) >= getmaxy(is->rb->win) - 2){
		newsel = getmaxy(is->rb->win) - 1 - node_lines(is->expansion,is->rb->selected);
	}
	/*wstatus_locked(stdscr,"newsel: %d bef: %d aft: %d oldsel: %d maxy: %d",
			newsel,bef,aft,oldsel,getmaxy(is->rb->win));
	update_panels();
	doupdate();*/
	if(newsel){
		is->rb->selline = newsel;
	}
	assert(is->rb->selline >= 1);
	assert(is->rb->selline < getmaxy(is->rb->win) - 1 || !is->expansion);
}

static int
expand_adapter_locked(void){
	adapterstate *is;
	int old,oldrows;

	if(!current_adapter){
		return 0;
	}
	is = current_adapter->as;
	if(is->expansion == EXPANSION_FULL){
		return 0;
	}
	++is->expansion;
	old = current_adapter->selline;
	oldrows = getmaxy(current_adapter->win);
	assert(resize_adapter(current_adapter) == OK);
	recompute_selection(is,old,oldrows,getmaxy(current_adapter->win));
	redraw_adapter(current_adapter);
	return 0;
}

static int
collapse_adapter_locked(void){
	adapterstate *is;
	int old,oldrows;

	if(!current_adapter){
		return 0;
	}
	is = current_adapter->as;
	if(is->expansion == EXPANSION_NONE){
		return 0;
	}
	--is->expansion;
	old = current_adapter->selline;
	oldrows = getmaxy(current_adapter->win);
	assert(resize_adapter(current_adapter) == OK);
	recompute_selection(is,old,oldrows,getmaxy(current_adapter->win));
	redraw_adapter(current_adapter);
	return 0;
}

static int
select_adapter(void){
	reelbox *rb;

	if((rb = current_adapter) == NULL){
		return -1;
	}
	if(rb->selected){
		locked_diag("Already browsing [%s]",rb->as->c->ident);
		return 0;
	}
	if(rb->as->bobjs == NULL){
		return -1;
	}
	assert(rb->selline == -1);
	return select_adapter_dev(rb,rb->as->bobjs,2);
}

/* Action interface -- always require a selection */
static adapterstate *
get_selected_adapter(void){
	if(!current_adapter){
		return NULL;
	}
	return current_adapter->as;
}

static blockobj *
get_selected_blockobj(void){
	reelbox *rb;

	if((rb = current_adapter) == NULL){
		return NULL;
	}
	return rb->selected;
}

static void
enslave_disk(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("A block device must be selected");
		return;
	}
	// FIXME enslave it
}

static void
liberate_disk(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("A block device must be selected");
		return;
	}
	// FIXME liberate it
}

static void
remove_ptable(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Partition table removal requires selection of a block device");
		return;
	}
	if(b->d->layout != LAYOUT_NONE){
		locked_diag("Not a partitionable block device");
		return;
	}
	wipe_ptable(b->d,NULL);
}

static struct form_option *
pttype_table(int *count){
	struct form_option *fo = NULL,*tmp;
	pttable_type *types;
	int z;

	if((types = get_ptable_types(count)) == NULL){
		return NULL;
	}
	for(z = 0 ; z < *count ; ++z){
		char *key,*desc;

		if((key = strdup(types[z].name)) == NULL){
			goto err;
		}
		if((desc = strdup(types[z].desc)) == NULL){
			free(key);
			goto err;
		}
		if((tmp = realloc(fo,sizeof(*fo) * (*count + 1))) == NULL){
			free(key);
			free(desc);
			goto err;
		}
		fo = tmp;
		fo[z].option = key;
		fo[z].desc = desc;
	}
	return fo;

err:
	while(z--){
		free(fo[z].option);
		free(fo[z].desc);
	}
	free(fo);
	*count = 0;
	return NULL;
}

static void
make_ptable(void){
	struct form_option *ops_ptype;
	int opcount;
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Partition table creation requires selection of a block device");
		return;
	}
	if(b->d->layout != LAYOUT_NONE){
		// FIXME
		locked_diag("Partition table creation requires a physical device");
		return;
	}
	if(b->d->blkdev.pttable){
		locked_diag("Partition table already exists on %s",b->d->name);
		return;
	}
	if(b->zone == NULL){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if((ops_ptype = pttype_table(&opcount)) == NULL){
		return;
	}
	raise_form("select a table type",pttype_callback,ops_ptype,opcount);
}

static void
new_partition(void){
	struct form_option *ops_ptype;
	int opcount;
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Partition creation requires selection of a block device");
		return;
	}
	if(b->zone == NULL){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(b->d->layout != LAYOUT_NONE){
		// FIXME for blockdev.pttable check below. remove!
		locked_diag("Partition tables can only be made on block devices");
		return;
	}
	if(b->d->blkdev.pttable == NULL){
		locked_diag("Partition creation requires a partition table");
		return;
	}
	if(b->zone->p == NULL){
		if((ops_ptype = ptype_table(&opcount)) == NULL){
			return;
		}
		raise_form("select a partition type",ptype_callback,ops_ptype,opcount);
		return;
	}else if(b->zone->p->layout == LAYOUT_NONE){
		locked_diag("A partition table needs be created on %s",b->d->name);
		return;
	}else if(b->zone->p->layout == LAYOUT_PARTITION){
		locked_diag("Partition %s exists; remove it first",b->zone->p->name);
		return;
	}
	// FIXME do stuff for mdadm, dm, zpool?
}

static struct form_option *
fs_table(int *count){
	struct form_option *fo = NULL,*tmp;
	pttable_type *types;
	int z;

	if((types = get_fs_types(count)) == NULL){
		return NULL;
	}
	for(z = 0 ; z < *count ; ++z){
		char *key,*desc;

		if((key = strdup(types[z].name)) == NULL){
			goto err;
		}
		if((desc = strdup(types[z].desc)) == NULL){
			free(key);
			goto err;
		}
		if((tmp = realloc(fo,sizeof(*fo) * (*count + 1))) == NULL){
			free(key);
			free(desc);
			goto err;
		}
		fo = tmp;
		fo[z].option = key;
		fo[z].desc = desc;
	}
	return fo;

err:
	while(z--){
		free(fo[z].option);
		free(fo[z].desc);
	}
	free(fo);
	*count = 0;
	return NULL;
}

static void
new_filesystem(void){
	struct form_option *ops_fs;
	int opcount;
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Filesystem creation requires a selected block device");
		return;
	}
	if(b->zone == NULL){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(b->zone->p && b->zone->p->layout != LAYOUT_PARTITION){
		locked_diag("Filesystems cannot be created in empty space");
		return;
	}else{
		if((ops_fs = fs_table(&opcount)) == NULL){
			return;
		}
		raise_form("select a filesystem type",fs_callback,ops_fs,opcount);
		return;
	}
}

static void
kill_filesystem(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Filesystem wipe requires a selected block device");
		return;
	}
	if(b->zone == NULL){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(b->zone->p && b->zone->p->layout != LAYOUT_PARTITION){
		locked_diag("Filesystems cannot be wiped from empty space");
		return;
	}
	if(b->zone->p){
		wipe_filesystem(b->zone->p);
	}else{
		wipe_filesystem(b->d);
	}
}

static void
set_partition_attrs(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Partition modification requires selection of a partition");
		return;
	}
	if(b->d->layout != LAYOUT_PARTITION){
		locked_diag("Selected object is not a partition");
		return;
	}
	// FIXME pop up a form allowing attr set
	// FIXME set that fucker
}

static void
delete_partition(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Partition deletion requires selection of a partition");
		return;
	}
	if(b->zone == NULL){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(b->zone->p == NULL){
		locked_diag("Space is already unpartitioned");
		return;
	}else if(b->zone->p->layout == LAYOUT_NONE){
		locked_diag("No partition table exists on %s",b->d->name);
		return;
	}else if(b->zone->p->layout == LAYOUT_PARTITION){
		wipe_partition(b->zone->p);
		return;
	}
	// FIXME do stuff for mdadm, dm, zpool?
}

static void
rescan_selection(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		adapterstate *as;

		if((as = get_selected_adapter()) == NULL){
			locked_diag("Need a selected adapter or block device");
			return;
		}
		locked_diag("Rescanning adapter %s",as->c->name);
		rescan_controller(as->c);
		return;
	}
	locked_diag("Rescanning block device %s",b->d->name);
	rescan_blockdev(b->d);
}

static void
reset_selection(void){
	adapterstate *as;

	if((as = get_selected_adapter()) == NULL){
		locked_diag("Need a selected adapter");
		return;
	}
	locked_diag("Resetting adapter %s",as->c->name);
	reset_controller(as->c);
}

static void
wipe_mbr(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("MBR wipe requires selection of a block device");
		return;
	}
	wipe_dosmbr(b->d);
}

static void
badblock_check(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Block check requires selection of a block device");
		return;
	}
	badblock_scan(b->d,0); // FIXME allow destructive badblock check
}

static void
mount_filesystem(void){
	// FIXME get current partition, empty space, or blockobj
	// FIXME make that fucker
}

static void
umount_filesystem(void){
	// FIXME get current partition, empty space, or blockobj
	// FIXME make that fucker
}

// We received input while a modal freeform string input form was active.
// Divert it from the typical UI, and handle it according to the form.
static void
handle_actform_string_input(int ch){
	struct form_state *fs = actform;
	void (*cb)(const char *);

	switch(ch){
	case 12: // CTRL+L FIXME
		pthread_mutex_lock(&bfl);
		wrefresh(curscr);
		screen_update();
		pthread_mutex_unlock(&bfl);
		break;
	case '\r': case '\n': case KEY_ENTER:{
		char *str;

		pthread_mutex_lock(&bfl);
		cb = actform->fxn;
		assert(str = strdup(actform->inp.buffer));
		free_form(actform);
		actform = NULL;
		cb(str);
		free(str);
		pthread_mutex_unlock(&bfl);
		break;
	}case KEY_ESC:{
		pthread_mutex_lock(&bfl);
		cb = actform->fxn;
		free_form(actform);
		actform = NULL;
		cb(NULL);
		pthread_mutex_unlock(&bfl);
		break;
	}case KEY_BACKSPACE:{
		pthread_mutex_lock(&bfl);
		if(strlen(fs->inp.buffer)){
			fs->inp.buffer[strlen(fs->inp.buffer) - 1] = '\0';
			form_string_options(fs);
			screen_update();
		}
		pthread_mutex_unlock(&bfl);
		break;
	}default:{
		char *tmp;

		if(ch >= 256 || !isgraph(ch)){
			diag("please %s (⎋esc returns)",actform->boxstr);
		}
		pthread_mutex_lock(&bfl);
		if((tmp = realloc(fs->inp.buffer,strlen(fs->inp.buffer) + 2)) == NULL){
			locked_diag("Couldn't allocate input buffer (%s?)",strerror(errno));
			pthread_mutex_unlock(&bfl);
			return;
		}
		fs->inp.buffer = tmp;
		fs->inp.buffer[strlen(fs->inp.buffer) + 1] = '\0';
		fs->inp.buffer[strlen(fs->inp.buffer)] = (unsigned char)ch;
		form_string_options(fs);
		screen_update();
		pthread_mutex_unlock(&bfl);
	} }
}

// We received input while a modal form was active. Divert it from the typical
// UI, and handle it according to the form.
static void
handle_actform_input(int ch){
	struct form_state *fs = actform;
	void (*cb)(const char *);

	if(fs->formtype == FORM_STRING_INPUT){
		handle_actform_string_input(ch);
		return;
	}
	switch(ch){
		case 12: // CTRL+L FIXME
			pthread_mutex_lock(&bfl);
			wrefresh(curscr);
			screen_update();
			pthread_mutex_unlock(&bfl);
			break;
		case '\r': case '\n': case KEY_ENTER:{
			char *optstr;

			pthread_mutex_lock(&bfl);
			assert(optstr = strdup(actform->ops[actform->idx].option));
			cb = actform->fxn;
			free_form(actform);
			actform = NULL;
			cb(optstr);
			free(optstr);
			pthread_mutex_unlock(&bfl);
			break;
		}case KEY_ESC:
			pthread_mutex_lock(&bfl);
			cb = actform->fxn;
			free_form(actform);
			actform = NULL;
			cb(NULL);
			pthread_mutex_unlock(&bfl);
			break;
		case KEY_UP: case 'k':{
			pthread_mutex_lock(&bfl);
			if(fs->idx-- == 0){
				fs->idx = fs->ysize - 1;
			}
			form_options(fs);
			screen_update();
			pthread_mutex_unlock(&bfl);
			break;
		}
		case KEY_DOWN: case 'j':{
			pthread_mutex_lock(&bfl);
			if(fs->idx++ >= fs->ysize - 1){
				fs->idx = 0;
			}
			form_options(fs);
			screen_update();
			pthread_mutex_unlock(&bfl);
			break;
		}
		default:{
			diag("please %s (⎋esc returns)",actform->boxstr);
			break;
		}
	}
}

static void
setup_target(void){
	if(target_mode_p()){
		locked_diag("Already have a target at %s",growlight_target);
		return;
	}
	// FIXME 
}

static void
unset_target(void){
	if(!target_mode_p()){
		locked_diag("Not in target mode");
		return;
	}
	if(set_target(NULL)){
		return;
	}
	locked_diag("Successfully left target mode");
}

static void
handle_ncurses_input(WINDOW *w){
	int ch;

	while((ch = getch()) != ERR){
		if(actform){
			handle_actform_input(ch);
			continue;
		}
		switch(ch){
			case 'H':{
				pthread_mutex_lock(&bfl);
				toggle_panel(w,&help,display_help);
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'D':{
				pthread_mutex_lock(&bfl);
				toggle_panel(w,&diags,display_diags);
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'v':{
				pthread_mutex_lock(&bfl);
				toggle_panel(w,&details,display_details);
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'e':{
				pthread_mutex_lock(&bfl);
				toggle_panel(w,&environment,display_enviroment);
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'E':{
				pthread_mutex_lock(&bfl);
				toggle_panel(w,&maps,display_maps);
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case KEY_BACKSPACE: case KEY_ESC:
				pthread_mutex_lock(&bfl);
				deselect_adapter_locked();
				update_details_cond(panel_window(details.p));
				update_help_cond(panel_window(help.p));
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			case '\r': case '\n': case KEY_ENTER:
				pthread_mutex_lock(&bfl);
				select_adapter();
				update_details_cond(panel_window(details.p));
				update_help_cond(panel_window(help.p));
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			case 12: // CTRL+L FIXME
				pthread_mutex_lock(&bfl);
				wrefresh(curscr);
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			case '+':
				pthread_mutex_lock(&bfl);
				expand_adapter_locked();
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			case '-':{
				pthread_mutex_lock(&bfl);
				collapse_adapter_locked();
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case KEY_RIGHT: case 'l':{
				pthread_mutex_lock(&bfl);
				if(selection_active()){
					use_next_zone(current_adapter->selected);
				}
				redraw_adapter(current_adapter);
				update_details_cond(panel_window(details.p));
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case KEY_LEFT: case 'h':{
				pthread_mutex_lock(&bfl);
				if(selection_active()){
					use_prev_zone(current_adapter->selected);
				}
				redraw_adapter(current_adapter);
				update_details_cond(panel_window(details.p));
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case KEY_UP: case 'k':{
				pthread_mutex_lock(&bfl);
				if(!selection_active()){
					use_prev_controller(w,&details);
				}else{
					use_prev_device();
				}
				update_details_cond(panel_window(details.p));
				update_help_cond(panel_window(help.p));
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case KEY_DOWN: case 'j':{
				pthread_mutex_lock(&bfl);
				if(!selection_active()){
					use_next_controller(w,&details);
				}else{
					use_next_device();
				}
				update_details_cond(panel_window(details.p));
				update_help_cond(panel_window(help.p));
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'm':{
				pthread_mutex_lock(&bfl);
				make_ptable();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'r':{
				pthread_mutex_lock(&bfl);
				remove_ptable();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'W':{
				pthread_mutex_lock(&bfl);
				wipe_mbr();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'B':{
				pthread_mutex_lock(&bfl);
				badblock_check();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'R':{
				pthread_mutex_lock(&bfl);
				rescan_selection();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'S':{
				pthread_mutex_lock(&bfl);
				reset_selection();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'n':{
				pthread_mutex_lock(&bfl);
				new_partition();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'd':{
				pthread_mutex_lock(&bfl);
				delete_partition();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 's':{
				pthread_mutex_lock(&bfl);
				set_partition_attrs();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'M':{
				pthread_mutex_lock(&bfl);
				new_filesystem();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'w':{
				pthread_mutex_lock(&bfl);
				kill_filesystem();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'o':{
				pthread_mutex_lock(&bfl);
				mount_filesystem();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'O':{
				pthread_mutex_lock(&bfl);
				umount_filesystem();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 't':{
				pthread_mutex_lock(&bfl);
				mount_target();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'T':{
				pthread_mutex_lock(&bfl);
				umount_target();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'b':{
				pthread_mutex_lock(&bfl);
				enslave_disk();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'f':{
				pthread_mutex_lock(&bfl);
				liberate_disk();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'i':{
				pthread_mutex_lock(&bfl);
				setup_target();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'I':{
				pthread_mutex_lock(&bfl);
				unset_target();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case 'q': case 'Q':
				diag("User-initiated shutdown");
				return;
			default:{
				const char *hstr = !help.p ? " ('H' for help)" : "";
				// diag() locks/unlocks, and calls screen_update()
				if(isprint(ch)){
					diag("unknown command '%c'%s",ch,hstr);
				}else{
					diag("unknown scancode %d%s",ch,hstr);
				}
				break;
			}
		}
	}
	diag("Error reading from console, aborting");
}

static adapterstate *
create_adapter_state(controller *a){
	adapterstate *as;

	if( (as = malloc(sizeof(*as))) ){
		memset(as,0,sizeof(*as));
		as->c = a;
		as->expansion = EXPANSION_MAX;
		// next, prev, rb are managed by caller
	}
	return as;
}

static void
free_adapter_state(adapterstate *as){
	if(as){
		blockobj *b;

		while( (b = as->bobjs) ){
			as->bobjs = b->next;
			free(b);
		}
		free(as);
	}
}

static void *
adapter_callback(controller *a, void *state){
	adapterstate *as;
	reelbox *rb;

	pthread_mutex_lock(&bfl);
	if((as = state) == NULL){
		if( (state = as = create_adapter_state(a)) ){
			int newrb,rows,cols;

			getmaxyx(stdscr,rows,cols);
			if( (newrb = bottom_space_p(rows)) ){
				newrb = rows - newrb;
				if((rb = create_reelbox(as,rows,newrb,cols)) == NULL){
					free_adapter_state(as);
					pthread_mutex_unlock(&bfl);
					return NULL;
				}
				if(last_reelbox){
					// set up the adapter list entries
					as->next = last_reelbox->as->next;
					as->next->prev = as;
					as->prev = last_reelbox->as;
					last_reelbox->as->next = as;
					// and also the rb list entries
					if( (rb->next = last_reelbox->next) ){
						rb->next->prev = rb;
					}
					rb->prev = last_reelbox;
					last_reelbox->next = rb;
				}else{
					as->prev = as->next = as;
					rb->next = rb->prev = NULL;
					top_reelbox = rb;
					current_adapter = rb;
				}
				last_reelbox = rb;
			}else{ // insert it after the last visible one, no rb
				as->next = top_reelbox->as;
				top_reelbox->as->prev->next = as;
				as->prev = top_reelbox->as->prev;
				top_reelbox->as->prev = as;
				as->rb = NULL;
				rb = NULL;
			}
			++count_adapters;
			draw_main_window(stdscr);
		}else{
			rb = NULL;
		}
	}else{
		rb = as->rb;
	}
	if(rb){
		//resize_adapter(rb);
		redraw_adapter(rb);
	}
	screen_update();
	pthread_mutex_unlock(&bfl);
	return as;
}

static void
free_zchain(zobj **z){
	zobj *zt,*zstart;

	if((zstart = *z) == NULL){
		return;
	}
	do{
		zt = *z;
		*z = (*z)->next;
		free(zt);
	}while( (*z != zstart) );
	*z = NULL;
}

static void
update_blockobj(blockobj *b,device *d){
	unsigned fs,mounts,zones,zonesel;
	uintmax_t sector;
	zobj *z,*lastz;
	device *p;

	fs = mounts = 0;
	if(b->zone){
		zonesel = b->zone->zoneno;
	}else{
		zonesel = 0;
	}
	z = NULL;
	if(d->mnttype){
		++fs;
		if(d->mnt){
			++mounts;
		}
		if((z = create_zobj(z,0,0,last_usable_sector(d),d)) == NULL){
			goto err;
		}
		sector = last_usable_sector(d) + 1;
		zones = 1;
	}else{
		sector = 0;
		zones = 0;
	}
	if(d->target){
		++mounts;
	}
	if(d->swapprio != SWAP_INVALID){
		++fs;
	}
	for(p = d->parts ; p ; p = p->next){
		if(sector != p->partdev.fsector){
			if((z = create_zobj(z,zones,sector,p->partdev.fsector - 1,NULL)) == NULL){
				goto err;
			}
			++zones;
		}
		if((z = create_zobj(z,zones,p->partdev.fsector,p->partdev.lsector,p)) == NULL){
			goto err;
		}
		++zones;
		if(p->mnttype){
			++fs;
			if(p->mnt){
				++mounts;
			}
		}
		if(p->target){
			++mounts;
		}
		if(p->swapprio != SWAP_INVALID){
			++fs;
		}
		sector = p->partdev.lsector + 1;
	}
	if(d->logsec && d->size){
		if(sector != last_usable_sector(d) + 1){
			if((z = create_zobj(z,zones,sector,last_usable_sector(d),NULL)) == NULL){
				goto err;
			}
			++zones;
		}
	}
	free_zchain(&b->zchain);
	b->zone = NULL;
	if(zonesel >= zones){
		zonesel = zones ? zones - 1 : 0;
	}
	zonesel = zones - zonesel;
	if( (lastz = z) ){
		while(z->prev){
			if(zonesel-- == 1){
				b->zone = z;
			}
			z = z->prev;
		}
		if(zonesel-- == 1){
			b->zone = z;
		}
		z->prev = lastz;
		lastz->next = z;
	}
	b->zchain = z;
	b->zones = zones;
	return;

err:
	while( (lastz = z) ){
		z = z->prev;
		free(lastz);
	}
	assert(0); // FIXME
}

static blockobj *
create_blockobj(device *d){
	blockobj *b;

	if( (b = malloc(sizeof(*b))) ){
		memset(b,0,sizeof(*b));
		b->d = d;
		update_blockobj(b,d);
	}
	return b;
}

static void *
block_callback(device *d,void *v){
	adapterstate *as;
	blockobj *b;

	pthread_mutex_lock(&bfl);
	as = d->c->uistate;
	if((b = v) == NULL){
		if( (b = create_blockobj(d)) ){
			if(as->devs == 0){
				b->prev = b->next = NULL;
				as->bobjs = b;
			}else{
				b->next = as->bobjs;
				b->prev = NULL;
				as->bobjs->prev = b;
				as->bobjs = b;
			}
			if(d->size){
				++as->devdisps;
			}
			++as->devs;
		}
	}else{
		update_blockobj(b,d);
	}
	if(as->rb){
		int old,oldrows;

		old = as->rb->selline;
		oldrows = getmaxy(as->rb->win);
		resize_adapter(as->rb);
		recompute_selection(as,old,oldrows,getmaxy(as->rb->win));
		redraw_adapter(as->rb);
		screen_update();
	}
	pthread_mutex_unlock(&bfl);
	return b;
}

static void
block_free(void *cv,void *bv){
	adapterstate *as = cv;
	blockobj *bo = bv;
	reelbox *rb;

	pthread_mutex_lock(&bfl);
	if( (rb = as->rb) ){
		if(bo == rb->selected){
			if(bo->prev){
				select_adapter_dev(rb,bo->prev,-1);
			}else if(bo->next){
				select_adapter_dev(rb,bo->next,1);
			}else{
				select_adapter_dev(rb,NULL,0);
			}
		}
	}
	if(bo->prev){
		bo->prev->next = bo->next;
	}
	if(bo->next){
		bo->next->prev = bo->prev;
	}
	if(as->bobjs == bo){
		as->bobjs = bo->next;
	}
	if(bo->d->size){
		--as->devdisps;
	}
	--as->devs;
	free(bo);
	if(as->rb){
		int old,oldrows;

		old = as->rb->selline;
		oldrows = getmaxy(rb->win);
		resize_adapter(as->rb);
		recompute_selection(as,old,oldrows,getmaxy(rb->win));
		redraw_adapter(as->rb);
		screen_update();
	}
	pthread_mutex_unlock(&bfl);
}

static void
adapter_free(void *cv){
	adapterstate *as = cv;
	reelbox *rb = rb;

	pthread_mutex_lock(&bfl);
	as->prev->next = as->next;
	as->next->prev = as->prev;
	if( (rb = as->rb) ){
		int delta = getmaxy(rb->win) + 1,scrrows,scrcols;

		//fprintf(stderr,"Removing iface at %d\n",rb->scrline);
		assert(werase(rb->win) == OK);
		assert(hide_panel(rb->panel) == OK);
		getmaxyx(stdscr,scrrows,scrcols);
		if(rb->next){
			rb->next->prev = rb->prev;
		}else{
			last_reelbox = rb->prev;
		}
		if(rb->prev){
			rb->prev->next = rb->next;
		}else{
			top_reelbox = rb->next;
		}
		as->next->prev = as->prev;
		as->prev->next = as->next;
		if(rb == current_adapter){
			// FIXME need do all the stuff we do in _next_/_prev_
			if((current_adapter = rb->next) == NULL){
				current_adapter = rb->prev;
			}
			pull_adapters_up(rb,scrrows,scrcols,delta);
			// give the details window to new current_iface
			if(details.p){
				if(current_adapter){
					update_details(panel_window(details.p));
				}else{
					hide_panel_locked(&details);
					active = NULL;
				}
			}
		}else if(rb->scrline > current_adapter->scrline){
			pull_adapters_up(rb,scrrows,scrcols,delta);
		}else{ // pull them down; removed is above current_adapter
			int ts;

			pull_adapters_down(rb,scrrows,scrcols,delta);
			if( (ts = top_space_p(scrrows)) ){
				pull_adapters_up(NULL,scrrows,scrcols,ts);
			}
		}
		screen_update();
		free_reelbox(rb);
	}else{
		as->next->prev = as->prev;
		as->prev->next = as->next;
	}
	free_adapter_state(as); // clears subentries
	--count_adapters;
	draw_main_window(stdscr); // Update the device count
	pthread_mutex_unlock(&bfl);
}

static void
vdiag(const char *fmt,va_list v){
	char *nl;

	pthread_mutex_lock(&bfl);
	vsnprintf(statusmsg,sizeof(statusmsg),fmt,v);
	if( (nl = strchr(statusmsg,'\n')) ){
		*nl = '\0';
	}
	draw_main_window(stdscr);
	if(diags.p){
		update_diags(&diags);
	}
	screen_update();
	pthread_mutex_unlock(&bfl);
}

int main(int argc,char * const *argv){
	const glightui ui = {
		.vdiag = vdiag,
		.adapter_event = adapter_callback,
		.block_event = block_callback,
		.adapter_free = adapter_free,
		.block_free = block_free,
	};
	WINDOW *w;

	if(setlocale(LC_ALL,"") == NULL){
		fprintf(stderr,"Couldn't find locale\n");
		return EXIT_FAILURE;
	}
	if((w = ncurses_setup()) == NULL){
		return EXIT_FAILURE;
	}
	if(growlight_init(argc,argv,&ui)){
		ncurses_cleanup(&w);
		dump_diags();
		return EXIT_FAILURE;
	}
	handle_ncurses_input(w);
	if(growlight_stop()){
		ncurses_cleanup(&w);
		dump_diags();
		return EXIT_FAILURE;
	}
	if(ncurses_cleanup(&w)){
		dump_diags();
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
