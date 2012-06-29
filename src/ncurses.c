#include <assert.h>
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

static pthread_mutex_t bfl = PTHREAD_MUTEX_INITIALIZER;

struct adapterstate;

typedef struct reelbox {
	WINDOW *win;
	PANEL *panel;
	struct reelbox *next,*prev;
	struct adapterstate *as;
	int scrline,selline;
	void *selected;
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
	int l = 2 + adapter_up_p(as);

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
	assert(init_pair(DHEADING_COLOR,COLOR_BLUE,-1) == OK);
	assert(init_pair(UHEADING_COLOR,COLOR_YELLOW,-1) == OK);
	assert(init_pair(DBORDER_COLOR,COLOR_BLUE,-1) == OK);
	assert(init_pair(UBORDER_COLOR,COLOR_YELLOW,-1) == OK);
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

static void
handle_ncurses_input(WINDOW *w){
	int ch;

	while((ch = getch()) != 'q' && ch != 'Q'){
		pthread_mutex_lock(&bfl);
		switch(ch){
			case 'h':{
				// FIXME show help
				wclear(w);
				break;
				 }

		}
		pthread_mutex_unlock(&bfl);
	}
}

static void
diag(const char *fmt,va_list v){
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
			fprintf(stderr,"**************%d***********\n",scrline);
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

static int
redraw_adapter(const reelbox *rb){
	const int active = (rb == current_adapter);
	const adapterstate *as = rb->as;
	int scrrows,scrcols;
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
	assert(werase(rb->win) != ERR);
	adapter_box(as,rb->win,active,topp,endp);
	return OK;
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
					// set up the iface list entries
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
				// Want the subdisplay left above this new iface,
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

int main(int argc,char * const *argv){
	const glightui ui = {
		.vdiag = diag,
		.adapter_event = adapter_callback,
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
