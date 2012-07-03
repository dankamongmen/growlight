#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <pthread.h>

#include "config.h"
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

static int selection_active;
static pthread_mutex_t bfl = PTHREAD_MUTEX_INITIALIZER;

struct panel_state {
	PANEL *p;
	int ysize;		      // number of lines of *text* (not win)
};

#define PANEL_STATE_INITIALIZER { .p = NULL, .ysize = -1, }
#define SUBDISPLAY_ATTR (COLOR_PAIR(SUBDISPLAY_COLOR) | A_BOLD)

static struct panel_state *active;
static struct panel_state help = PANEL_STATE_INITIALIZER;
static struct panel_state details = PANEL_STATE_INITIALIZER;

struct adapterstate;

struct partobj;

typedef struct blockobj {
	struct blockobj *next,*prev;
	const device *d;
	unsigned lns;			// number of lines obj would take up
	struct partobj *pobjs;
} blockobj;

typedef struct reelbox {
	WINDOW *win;
	PANEL *panel;
	struct reelbox *next,*prev;
	struct adapterstate *as;
	int scrline,selline;
	blockobj *selected;
} reelbox;

typedef struct adapterstate {
	const controller *c;
	unsigned mounts,fs,parts,devs;
	enum {
		EXPANSION_NONE,
		EXPANSION_DEVS,
		EXPANSION_PARTS,
		EXPANSION_FS,
		EXPANSION_MOUNTS,
	} expansion;
	struct adapterstate *next,*prev;
	blockobj *bobjs;
	reelbox *rb;
} adapterstate;

#define EXPANSION_MAX EXPANSION_MOUNTS

static char statusmsg[73];
static unsigned count_adapters;
// dequeue + single selection
static reelbox *current_adapter,*top_reelbox,*last_reelbox;

#define START_COL 1		// Room to leave for borders
#define PAD_COLS(cols) ((cols) - START_COL * 2)

static inline void
screen_update(void){
	update_panels();
	assert(doupdate() == OK);
}

static inline int
adapter_up_p(const adapterstate *as __attribute__ ((unused))){
	return 1; // FIXME
}

// This is the number of l we'd have in an optimal world; we might have
// fewer available to us on this screen at this time.
static int
lines_for_adapter(const struct adapterstate *as){
	int l = 2;

	switch(as->expansion){ // Intentional fallthrus
		case EXPANSION_MOUNTS:
			l += as->mounts;
		case EXPANSION_FS:
			l += as->fs;
		case EXPANSION_PARTS:
			l += as->parts;
		case EXPANSION_DEVS:
			l += as->devs;
		case EXPANSION_NONE:
			return l;
	}
	assert(0);
	return -1;
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

// Our color pairs
enum {
	BORDER_COLOR = 1,		// Main window
	HEADER_COLOR,
	FOOTER_COLOR,
	DHEADING_COLOR,
	UHEADING_COLOR,
	DBORDER_COLOR,
	UBORDER_COLOR,
	PBORDER_COLOR,
	PHEADING_COLOR,
	SUBDISPLAY_COLOR,
};

int bevel_notop(WINDOW *w){
	static const cchar_t bchr[] = {
		{ .attr = 0, .chars = L"╰", },
		{ .attr = 0, .chars = L"╯", },
	};
	int rows,cols;

	getmaxyx(w,rows,cols);
	assert(rows && cols);
	if(rows > 1){
		assert(mvwvline(w,0,0,ACS_VLINE,rows - 1) != ERR);
		assert(mvwvline(w,0,cols - 1,ACS_VLINE,rows - 1) != ERR);
	}
	assert(mvwadd_wch(w,rows - 1,0,&bchr[0]) != ERR);
	assert(mvwhline(w,rows - 1,1,ACS_HLINE,cols - 2) != ERR);
	assert(mvwins_wch(w,rows - 1,cols - 1,&bchr[1]) != ERR);
	return OK;
}

int bevel_noborder(WINDOW *w){
	int rows,cols;

	getmaxyx(w,rows,cols);
	assert(rows && cols);
	if(rows > 1){
		assert(mvwvline(w,1,0,ACS_VLINE,rows) != ERR);
		assert(mvwvline(w,1,cols - 1,ACS_VLINE,rows) != ERR);
	}
	return OK;
}

int bevel_nobottom(WINDOW *w){
	static const cchar_t bchr[] = {
		{ .attr = 0, .chars = L"╭", },
		{ .attr = 0, .chars = L"╮", },
	};
	int rows,cols;

	getmaxyx(w,rows,cols);
	assert(rows && cols);
	assert(mvwadd_wch(w,0,0,&bchr[0]) != ERR);
	assert(mvwins_wch(w,0,cols - 1,&bchr[1]) != ERR);
	assert(mvwhline(w,0,1,ACS_HLINE,cols - 2) != ERR);
	if(rows > 1){
		assert(mvwvline(w,1,0,ACS_VLINE,rows - 1) != ERR);
		assert(mvwvline(w,1,cols - 1,ACS_VLINE,rows - 1) != ERR);
	}
	return OK;
}

static int
bevel(WINDOW *w){
	static const cchar_t bchr[] = {
		{ .attr = 0, .chars = L"╭", },
		{ .attr = 0, .chars = L"╮", },
		{ .attr = 0, .chars = L"╰", },
		{ .attr = 0, .chars = L"╯", },
	};
	int rows,cols;

	getmaxyx(w,rows,cols);
	assert(rows && cols);
	// called as one expects: 'mvwadd_wch(w,rows - 1,cols - 1,&bchr[3]);'
	// we get ERR returned. this is known behavior: fuck ncurses. instead,
	// we use mvwins_wch, which doesn't update the cursor position.
	// see http://lists.gnu.org/archive/html/bug-ncurses/2007-09/msg00001.ht
	assert(mvwadd_wch(w,0,0,&bchr[0]) != ERR);
	assert(whline(w,ACS_HLINE,cols - 2) != ERR);
	assert(mvwins_wch(w,0,cols - 1,&bchr[1]) != ERR);
	assert(mvwvline(w,1,cols - 1,ACS_VLINE,rows - 1) != ERR);
	assert(mvwvline(w,1,0,ACS_VLINE,rows - 1) != ERR);
	assert(mvwadd_wch(w,rows - 1,0,&bchr[2]) != ERR);
	assert(whline(w,ACS_HLINE,cols - 2) != ERR);
	assert(mvwins_wch(w,rows - 1,cols - 1,&bchr[3]) != ERR);
	return OK;
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
draw_main_window(WINDOW *w){
	int rows,cols,scol;

	getmaxyx(w,rows,cols);
	assert(wattrset(w,A_DIM | COLOR_PAIR(BORDER_COLOR)) != ERR);
	if(bevel(w) != OK){
		goto err;
	}
	// 5 for 0-offset, '[', ']', and 2 spaces on right side.
	// 5 for '|', space before and after, and %2d-formatted integer
	scol = cols - 5 - __builtin_strlen(PACKAGE) - 1 - __builtin_strlen(VERSION)
		- 5 - __builtin_strlen("adapters" - (count_adapters != 1));
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

static int
setup_colors(void){
	assert(init_pair(BORDER_COLOR,COLOR_GREEN,-1) == OK);
	assert(init_pair(HEADER_COLOR,COLOR_BLUE,-1) == OK);
	assert(init_pair(FOOTER_COLOR,COLOR_YELLOW,-1) == OK);
	assert(init_pair(DHEADING_COLOR,COLOR_WHITE,-1) == OK);
	assert(init_pair(UHEADING_COLOR,COLOR_BLUE,-1) == OK);
	assert(init_pair(DBORDER_COLOR,COLOR_WHITE,-1) == OK);
	assert(init_pair(UBORDER_COLOR,COLOR_CYAN,-1) == OK);
	assert(init_pair(PBORDER_COLOR,COLOR_YELLOW,-1) == OK);
	assert(init_pair(PHEADING_COLOR,COLOR_RED,-1) == OK);
	assert(init_pair(SUBDISPLAY_COLOR,COLOR_WHITE,-1) == OK);
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
		//assert( (ret->win = newwin(l,PAD_COLS(cols),scrline,START_COL)) );
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
adapter_box(const adapterstate *as,WINDOW *w,int active,unsigned abovetop,
						unsigned belowend){
	int bcolor,hcolor,rows,cols;
	int attrs;

	getmaxyx(w,rows,cols);
	bcolor = adapter_up_p(as) ? UBORDER_COLOR : DBORDER_COLOR;
	hcolor = adapter_up_p(as) ? UHEADING_COLOR : DHEADING_COLOR;
	attrs = active ? A_REVERSE : A_BOLD;
	assert(wattrset(w,attrs | COLOR_PAIR(bcolor)) == OK);
	if(abovetop == 0){
		if(belowend == 0){
			assert(bevel(w) == OK);
		}else{
			assert(bevel_nobottom(w) == OK);
		}
	}else{
		if(belowend == 0){
			assert(bevel_notop(w) == OK);
		}else{
			assert(bevel_noborder(w) == OK);
		}
	}
	assert(wattroff(w,A_REVERSE) == OK);

	if(abovetop == 0){
		if(active){
			assert(wattron(w,A_BOLD) == OK);
		}
		assert(mvwprintw(w,0,1,"[") != ERR);
		assert(wcolor_set(w,hcolor,NULL) == OK);
		if(active){
			assert(wattron(w,A_BOLD) == OK);
		}else{
			assert(wattroff(w,A_BOLD) == OK);
		}
		assert(waddstr(w,as->c->ident) != ERR);
		/*
		assert(wprintw(w," (%s",is->typestr) != ERR);
		if(strlen(i->drv.driver)){
			assert(waddch(w,' ') != ERR);
			assert(waddstr(w,i->drv.driver) != ERR);
			if(strlen(i->drv.version)){
				assert(wprintw(w," %s",i->drv.version) != ERR);
			}
			if(strlen(i->drv.fw_version)){
				assert(wprintw(w," fw %s",i->drv.fw_version) != ERR);
			}
		}
		assert(waddch(w,')') != ERR);
		*/
		assert(wcolor_set(w,bcolor,NULL) != ERR);
		if(active){
			assert(wattron(w,A_BOLD) == OK);
		}
		assert(wprintw(w,"]") != ERR);
		assert(wmove(w,0,cols - 4) != ERR);
		assert(wattron(w,A_BOLD) == OK);
		assert(waddwstr(w,as->expansion == EXPANSION_MAX ? L"[-]" :
					as->expansion == 0 ? L"[+]" : L"[±]") != ERR);
		assert(wattron(w,attrs) != ERR);
		assert(wattroff(w,A_REVERSE) != ERR);
	}
	if(belowend == 0){
		if(as->c->bus == BUS_PCIe){
			assert(mvwprintw(w,rows - 1,2,"[") != ERR);
			assert(wcolor_set(w,hcolor,NULL) != ERR);
			if(active){
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
			if(active){
				assert(wattron(w,A_BOLD) == OK);
			}
			assert(wprintw(w,"]") != ERR);
		}
	}
}

static void
print_adapter_devs(const adapterstate *as,unsigned rows,unsigned topp,unsigned endp){
	char buf[PREFIXSTRLEN + 1];
	const blockobj *bo;
	const reelbox *rb;
	unsigned line;

	if((rb = as->rb) == NULL){
		return;
	}
	if(as->expansion < EXPANSION_DEVS){
		return;
	}
	bo = rb->selected;
	line = rb->selline;
	while(bo && line + bo->lns >= !!topp){
		if(line >= rows - !endp){
			break;
		}
		assert(mvwprintw(rb->win,line,START_COL * 2,"%s",bo->d->name) != ERR);
		if( (bo = bo->prev) ){
			line -= bo->lns;
		}
	}
	line = rb->selected ? rb->selline + rb->selected->lns : -topp + 1;
	bo = rb->selected ? rb->selected->next : as->bobjs;
	while(bo && line < rows){
		if(line >= rows - !endp){
			break;
		}
		switch(bo->d->layout){
			case LAYOUT_NONE:
		assert(mvwprintw(rb->win,line,START_COL * 2,"%-10.10s %-16.16s %4.4s " PREFIXFMT " %4uB %-6.6s%-16.16s %-4.4s",
					bo->d->name,bo->d->model,
					bo->d->revision,
					qprefix(bo->d->logsec * bo->d->size,1,buf,sizeof(buf),0),
					bo->d->physsec,
					bo->d->blkdev.pttable ? bo->d->blkdev.pttable : "none",
					bo->d->wwn ? bo->d->wwn : "n/a",
					bo->d->blkdev.realdev ? transport_str(bo->d->blkdev.transport) : "n/a"
					) != ERR);
			break;
			case LAYOUT_MDADM:
		assert(mvwprintw(rb->win,line,START_COL * 2,"%-10.10s %-16.16s %4.4s " PREFIXFMT " %4uB %-6.6s%-16.16s %-4.4s",
					bo->d->name,bo->d->model,
					bo->d->revision,
					qprefix(bo->d->logsec * bo->d->size,1,buf,sizeof(buf),0),
					bo->d->physsec,
					"n/a",
					bo->d->wwn ? bo->d->wwn : "n/a",
					transport_str(bo->d->mddev.transport)
					) != ERR);
			break;
			case LAYOUT_PARTITION:
			break;
			case LAYOUT_ZPOOL:
			break;
		}
		line += bo->lns;
		bo = bo->next;
	}
}

static int
redraw_adapter(const reelbox *rb){
	const int active = (rb == current_adapter);
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
	adapter_box(as,rb->win,active,topp,endp);
	print_adapter_devs(as,rows,topp,endp);
	return OK;
}

// Positive delta moves down, negative delta moves up, except for l2 == NULL
// where we always move to -1 (and delta is ignored).
static int
select_adapter_node(reelbox *rb,struct blockobj *bo,int delta){
	assert(bo != rb->selected);
	if((rb->selected = bo) == NULL){
		rb->selline = -1;
	}else{
		rb->selline += delta;
	}
	return redraw_adapter(rb);
}

static int
deselect_adapter_locked(void){
	reelbox *rb;

	if((rb = current_adapter) == NULL){
		return 0;
	}
	if(rb->selected == NULL){
		return 0;
	}
	return select_adapter_node(rb,NULL,0);
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

	assert(delta > 0);
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

static int
adapter_details(WINDOW *hw,const controller *c,int rows){
	assert(hw && c && rows); // FIXME
	return 0;
}

static void pull_adapters_down(reelbox *,int,int,int);


// Pass a NULL puller to move all adapters up
static void
pull_adapters_up(reelbox *puller,int rows,int cols,int delta){
	reelbox *rb;

	assert(delta > 0);
	rb = puller ? puller->next : top_reelbox;
	while(rb){
		rb->scrline -= delta;
		move_adapter_generic(rb,rows,cols,-delta);
		if(panel_hidden(rb->panel)){
			//fprintf(stderr,"PULLED THE TOP OFF\n");
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
	int rows,cols,subrows,subcols;
	adapterstate *is;

	assert(rb && rb->as);
	i = rb->as->c;
	assert(i);
	if(panel_hidden(rb->panel)){ // resize upon becoming visible
		return OK;
	}
	is = rb->as;
	getmaxyx(stdscr,rows,cols);
	const int nlines = adapter_lines_bounded(is,rows);
	getmaxyx(rb->win,subrows,subcols);
	assert(subcols); // FIXME
	if(nlines < subrows){ // Shrink the adapter
		assert(werase(rb->win) == OK);
		// Without screen_update(), the werase() doesn't take effect,
		// even if wclear() is used.
		screen_update();
		assert(wresize(rb->win,nlines,PAD_COLS(cols)) != ERR);
		assert(replace_panel(rb->panel,rb->win) != ERR);
		if(rb->scrline < current_adapter->scrline){
			rb->scrline += subrows - nlines;
			assert(move_panel(rb->panel,rb->scrline,1) != ERR);
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
			assert(move_panel(rb->panel,rb->scrline,1) != ERR);
		}
		assert(wresize(rb->win,nlines,PAD_COLS(cols)) != ERR);
		assert(replace_panel(rb->panel,rb->win) != ERR);
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

	assert(delta > 0);
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
			//fprintf(stderr,"PULLED THE BOTTOM OFF\n");
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
				adapter_details(panel_window(ps->p),is->c,ps->ysize);
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
		adapter_details(panel_window(ps->p),rb->as->c,ps->ysize);
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
				adapter_details(panel_window(ps->p),as->c,ps->ysize);
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
		adapter_details(panel_window(ps->p),rb->as->c,ps->ysize);
	}
}

static void
use_prev_device(void){
	reelbox *rb;

	if((rb = current_adapter) == NULL){
		return;
	}
	// FIXME
}

static void
use_next_device(void){
	reelbox *rb;

	if((rb = current_adapter) == NULL){
		return;
	}
	// FIXME
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
	screen_update();
	pthread_mutex_unlock(&bfl);
}

static void
diag(const char *fmt,...){
	va_list va;

	va_start(va,fmt);
	vdiag(fmt,va);
	va_end(va);
}


// Create a panel at the bottom of the window, referred to as the "subdisplay".
// Only one can currently be active at a time. Window decoration and placement
// is managed here; only the rows needed for display ought be provided.
static int
new_display_panel(WINDOW *w,struct panel_state *ps,int rows,int cols,const wchar_t *hstr){
	const wchar_t crightstr[] = L"http://dank.qemfd.net/dankwiki/index.php/Growlight";
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
	// Keep it one line up from the last display line, so that you get
	// adapter summaries (unless you've got a bottom-partial).
	assert( (psw = newwin(rows + 2,cols,y - (rows + 4),x - cols)) );
	if(psw == NULL){
		return ERR;
	}
	assert((ps->p = new_panel(psw)));
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
// Thus we refer to other window commands as "viewing", while 'h' here is
// described as "toggling". When other windows come up, they list their
// own command as "toggling." We want to avoid having to scroll the help
// synopsis, so keep it under 22 lines (25 lines on an ANSI standard terminal,
// minus two for the top/bottom screen border, minus one for mandatory
// window top padding).
static const wchar_t *helps[] = {
	L"'q': quit			ctrl+'L': redraw the screen",
	L"'⇆Tab' move between displays  'P': toggle subdisplay pinning",
	L"'e': view environment details 'h': toggle this help display",
	L"'v': view adapter details     'l': view recent diagnostics",
	L"'⏎Enter': browse adapter      '⌫BkSpc': leave adaper browser",
	L"'k'/'↑': previous selection   'j'/'↓': next selection",
	L"'⇞PgUp': previous page        '⇟PgDwn': next page",
	L"'↖Home': first selection      '↘End': last selection",
	L"'-'/'←': collapse selection   '+'/'→': expand selection",
	NULL
};

static size_t
max_helpstr_len(const wchar_t **helps){
	size_t max = 0;

	while(*helps){
		if(wcslen(*helps) > max){
			max = wcslen(*helps);
		}
		++helps;
	}
	return max;
}

static int
helpstrs(WINDOW *hw,int row,int rows){
	const wchar_t *hs;
	int z;

	assert(wattrset(hw,SUBDISPLAY_ATTR) == OK);
	for(z = 0 ; (hs = helps[z]) && z < rows ; ++z){
		assert(mvwaddwstr(hw,row + z,1,hs) != ERR);
	}
	return OK;
}

static int
display_help(WINDOW *mainw,struct panel_state *ps){
	static const int helprows = sizeof(helps) / sizeof(*helps) - 1; // NULL != row
	const int helpcols = max_helpstr_len(helps) + 4; // spacing + borders

	memset(ps,0,sizeof(*ps));
	if(new_display_panel(mainw,ps,helprows,helpcols,L"press 'h' to dismiss help")){
		goto err;
	}
	if(helpstrs(panel_window(ps->p),1,ps->ysize)){
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

static void
handle_ncurses_input(WINDOW *w){
	int ch;

	while((ch = getch()) != 'q' && ch != 'Q'){
		switch(ch){
			case 'h':{
				pthread_mutex_lock(&bfl);
				toggle_panel(w,&help,display_help);
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case KEY_UP: case 'k':{
				pthread_mutex_lock(&bfl);
				if(!selection_active){
					use_prev_controller(w,&details);
				}else{
					use_prev_device();
				}
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case KEY_DOWN: case 'j':{
				pthread_mutex_lock(&bfl);
				if(!selection_active){
					use_next_controller(w,&details);
				}else{
					use_next_device();
				}
				pthread_mutex_unlock(&bfl);
				screen_update();
				break;
			}
			default:{
				const char *hstr = !help.p ? " ('h' for help)" : "";
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
}

static adapterstate *
create_adapter_state(const controller *a){
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
		free(as);
	}
}

static void *
adapter_callback(const controller *a, void *state){
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
				// Want the subdisplay left above this new adapter,
				// should they intersect.
				assert(bottom_panel(rb->panel) == OK);
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

static blockobj *
create_blockobj(const adapterstate *as,const device *d){
	blockobj *b;

	if( (b = malloc(sizeof(*b))) ){
		memset(b,0,sizeof(*b));
		b->lns = as->expansion > EXPANSION_NONE;
		b->d = d;
	}
	return b;
}

static void *
block_callback(const controller *c,const device *d,void *v){
	adapterstate *as = c->uistate;
	blockobj *b;

	if((b = v) == NULL){
		if( (b = create_blockobj(as,d)) ){
			if(as->devs == 0){
				b->prev = b->next = as->bobjs = b;
			}else{
				b->next = as->bobjs;
				b->prev = as->bobjs->prev;
				as->bobjs->prev = b;
				b->prev->next = b;
			}
			++as->devs;
		}
		if(as->rb){
			resize_adapter(as->rb);
			redraw_adapter(as->rb);
		}
	}
	if(as->rb == NULL){
		return b;
	}
	if(as->rb != current_adapter){
		return b;
	}
	assert(top_panel(as->rb->panel) != ERR);
	screen_update();
	return b;
}

int main(int argc,char * const *argv){
	const glightui ui = {
		.vdiag = vdiag,
		.adapter_event = adapter_callback,
		.block_event = block_callback,
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
		return EXIT_FAILURE;
	}
	handle_ncurses_input(w);
	if(growlight_stop()){
		ncurses_cleanup(&w);
		return EXIT_FAILURE;
	}
	if(ncurses_cleanup(&w)){
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
