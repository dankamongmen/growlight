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

static int selection_active; // FIXME we ought be able to eliminate this
static pthread_mutex_t bfl = PTHREAD_MUTEX_INITIALIZER;

struct panel_state {
	PANEL *p;
	int ysize;		      // number of lines of *text* (not win)
};

#define PANEL_STATE_INITIALIZER { .p = NULL, .ysize = -1, }
#define SUBDISPLAY_ATTR (COLOR_PAIR(SUBDISPLAY_COLOR) | A_BOLD)

static struct panel_state *active;
static struct panel_state help = PANEL_STATE_INITIALIZER;
static struct panel_state diags = PANEL_STATE_INITIALIZER;
static struct panel_state details = PANEL_STATE_INITIALIZER;
static struct panel_state environment = PANEL_STATE_INITIALIZER;

struct adapterstate;

struct partobj;

typedef struct blockobj {
	struct blockobj *next,*prev;
	const device *d;
	unsigned lns;			// number of lines obj would take up
	unsigned parts;			// number of parts last we checked
	unsigned fs;			// number of filesystems...
	unsigned mounts;		// number of mounts...
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
	if(active){
		assert(top_panel(active->p) != ERR);
	}
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

static int
device_lines(int expa,const blockobj *bo){
	int l = 0;

	switch(expa){ // Intentional fallthroughs
		case EXPANSION_MOUNTS:
			l += bo->mounts;
		case EXPANSION_FS:
			l += bo->fs;
		case EXPANSION_PARTS:
			l += bo->parts;
		case EXPANSION_DEVS:
			++l;
	}
	return l;
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
	if(rows > 2){
		assert(mvwvline(w,1,cols - 1,ACS_VLINE,rows - 1) != ERR);
		assert(mvwvline(w,1,0,ACS_VLINE,rows - 1) != ERR);
	}
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
	bcolor = adapter_up_p(as) ? UBORDER_COLOR : DBORDER_COLOR;
	hcolor = adapter_up_p(as) ? UHEADING_COLOR : DHEADING_COLOR;
	attrs = current ? A_REVERSE : A_BOLD;
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
		if(current){
			assert(wattron(w,A_BOLD) == OK);
		}
		assert(mvwprintw(w,0,1,"[") != ERR);
		assert(wcolor_set(w,hcolor,NULL) == OK);
		if(current){
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
		if(current){
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

static void
print_fs(int expansion,const device *d,WINDOW *w,unsigned *line,unsigned rows,
						unsigned endp){
	char buf[PREFIXSTRLEN + 1];

	if(expansion < EXPANSION_FS){
		return;
	}
	assert(wcolor_set(w,COLOR_GREEN,NULL) == OK);
	if(*line >= rows - !endp){
		return;
	}
	if(d->mnttype){
		assert(mvwprintw(w,*line,START_COL * 3,"%-*.*s %-5.5s %-36.36s " PREFIXFMT,
				FSLABELSIZ,FSLABELSIZ,
				d->label ? d->label : "n/a",
				d->mnttype,
				d->uuid ? d->uuid : "n/a",
				qprefix(d->mntsize,1,buf,sizeof(buf),0)) != ERR);
		if(++*line >= rows - !endp){
			return;
		}
	}
	if(d->swapprio != SWAP_INVALID){
		assert(mvwprintw(w,*line,START_COL * 3,"%-*.*s %-5.5s %-36.36s " PREFIXFMT,
				FSLABELSIZ,FSLABELSIZ,
				d->label ? d->label : "n/a",
				d->mnttype,
				d->uuid ? d->uuid : "n/a",
				qprefix(d->mntsize,1,buf,sizeof(buf),0)) != ERR);
		if(++*line >= rows - !endp){
			return;
		}
	}
	if(expansion < EXPANSION_MOUNTS){
		return;
	}
	if(d->mnt){
		assert(mvwprintw(w,*line,START_COL * 4,"%s %s",
					d->mnt,d->mntops) != ERR);
		if(++*line >= rows - !endp){
			return;
		}
	}
	if(d->target){
		assert(mvwprintw(w,*line,START_COL * 4,"%s %s",
				d->target->path,d->target->ops) != ERR);
		if(++*line >= rows - !endp){
			return;
		}
	}
}

static void
print_dev(const reelbox *rb,const adapterstate *as,const blockobj *bo,
			unsigned line,unsigned rows,unsigned endp){
	const int selected = bo == rb->selected;
	char buf[PREFIXSTRLEN + 1];

	if(line >= rows - !endp){
		return;
	}
	switch(bo->d->layout){
		case LAYOUT_NONE:
	if(bo->d->blkdev.realdev){
		if(bo->d->blkdev.rotate){
			if(selected){
				assert(wattrset(rb->win,A_REVERSE|COLOR_WHITE) == OK);
			}else{
				assert(wattrset(rb->win,COLOR_WHITE) == OK);
			}
		}else{
			if(selected){
				assert(wattrset(rb->win,A_REVERSE|COLOR_CYAN) == OK);
			}else{
				assert(wattrset(rb->win,COLOR_CYAN) == OK);
			}
		}
	}else{
		if(selected){
			assert(wattrset(rb->win,A_REVERSE|COLOR_WHITE) == OK);
		}else{
			assert(wattrset(rb->win,COLOR_WHITE) == OK);
		}
	}
	assert(mvwprintw(rb->win,line,START_COL,"%-10.10s %-16.16s %4.4s " PREFIXFMT " %4uB %-6.6s%-16.16s %-4.4s",
				bo->d->name,
				bo->d->model ? bo->d->model : "n/a",
				bo->d->revision ? bo->d->revision : "n/a",
				qprefix(bo->d->logsec * bo->d->size,1,buf,sizeof(buf),0),
				bo->d->physsec,
				bo->d->blkdev.pttable ? bo->d->blkdev.pttable : "none",
				bo->d->wwn ? bo->d->wwn : "n/a",
				bo->d->blkdev.realdev ? transport_str(bo->d->blkdev.transport) : "n/a"
				) != ERR);
		break;
		case LAYOUT_MDADM:
	if(selected){
		assert(wattrset(rb->win,A_REVERSE|COLOR_MAGENTA) == OK);
	}else{
		assert(wattrset(rb->win,COLOR_MAGENTA) == OK);
	}
	assert(mvwprintw(rb->win,line,START_COL,"%-10.10s %-16.16s %4.4s " PREFIXFMT " %4uB %-6.6s%-16.16s %-4.4s",
				bo->d->name,
				bo->d->model ? bo->d->model : "n/a",
				bo->d->revision ? bo->d->revision : "n/a",
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
	if(selected){
		assert(wattrset(rb->win,A_REVERSE|COLOR_MAGENTA) == OK);
	}else{
		assert(wattrset(rb->win,COLOR_MAGENTA) == OK);
	}
	assert(mvwprintw(rb->win,line,START_COL,"%-10.10s %-16.16s %4ju " PREFIXFMT " %4uB %-6.6s%-16.16s %-4.4s",
				bo->d->name,
				bo->d->model ? bo->d->model : "n/a",
				(uintmax_t)bo->d->zpool.zpoolver,
				qprefix(bo->d->size,1,buf,sizeof(buf),0),
				bo->d->physsec,
				"spa",
				bo->d->wwn ? bo->d->wwn : "n/a",
				transport_str(bo->d->zpool.transport)
				) != ERR);
		break;
	}
	++line;
	if(as->expansion >= EXPANSION_PARTS){
		const device *p;

		print_fs(as->expansion,bo->d,rb->win,&line,rows,endp);
		for(p = bo->d->parts ; p ; p = p->next){
			if(line >= rows - !endp){
				return;
			}
			assert(wcolor_set(rb->win,COLOR_BLUE,NULL) == OK);
			assert(mvwprintw(rb->win,line,START_COL * 2,
						"%-10.10s %-36.36s " PREFIXFMT " %-5.5s %-13.13ls",
						p->name,
						p->partdev.uuid ? p->partdev.uuid : "",
						qprefix(p->logsec * p->size,1,buf,sizeof(buf),0),
						partrole_str(p->partdev.partrole,p->partdev.flags),
						p->partdev.pname ? p->partdev.pname : L"n/a"
						) != ERR);
			++line;
			print_fs(as->expansion,p,rb->win,&line,rows,endp);
		}
	}
}

static void
print_adapter_devs(const adapterstate *as,int rows,unsigned topp,unsigned endp){
	// If the interface is down, we don't lead with the summary line
	const blockobj *cur;
	const reelbox *rb;
	long line;
	int cols;

	if((rb = as->rb) == NULL){
		return;
	}
	if(as->expansion < EXPANSION_DEVS){
		return;
	}
	cols = getmaxx(rb->win);
	// First, print the selected device (if there is one)
	cur = rb->selected;
	line = rb->selline;
	while(cur && line + (long)device_lines(as->expansion,cur) >= !!topp){
		print_dev(rb,as,cur,line,rows,endp);
		// here we traverse, then account...
		if( (cur = cur->prev) ){
			line -= device_lines(as->expansion,cur);
		}
	}
	line = rb->selected ? (rb->selline +
		(long)device_lines(as->expansion,rb->selected)) : -(long)topp + 1;
	cur = (rb->selected ? rb->selected->next : as->bobjs);
	while(cur && line < rows){
		print_dev(rb,as,cur,line,rows,endp);
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
	print_adapter_devs(as,rows,topp,endp);
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
	if(select_adapter_node(rb,NULL,0)){
		return -1;
	}
	return redraw_adapter(rb);
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

// FIXME why accept 'rows' just to blow it away?
static int
adapter_details(WINDOW *hw,const controller *c,int rows){
	int cols;

	getmaxyx(hw,rows,cols);
	if(cols < START_COL * 2){
		return 0;
	}
	if(rows == 0){
		return 0;
	}
	assert(wattrset(hw,SUBDISPLAY_ATTR) != ERR);
	assert(mvwprintw(hw,1,START_COL,"%-*.*s",cols - 2,cols - 2,c->name) != ERR);
	if(rows == 1){
		return 0;
	}
	if(c->bus == BUS_VIRTUAL){
		assert(mvwprintw(hw,2,START_COL,"%-*.*s",cols - 2,cols - 2,"No details available") != ERR);
		return 0;
	}
	assert(mvwprintw(hw,2,START_COL,"Firmware: %s BIOS: %s",
				c->fwver ? c->fwver : "Unknown",
				c->biosver ? c->biosver : "Unknown") != ERR);
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
		delta = (getmaxy(rb->win) - 2 - device_lines(rb->as->expansion,rb->selected->next))
				- rb->selline;
	}
	select_adapter_dev(rb,rb->selected->next,delta);
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
// Thus we refer to other window commands as "viewing", while 'h' here is
// described as "toggling". When other windows come up, they list their
// own command as "toggling." We want to avoid having to scroll the help
// synopsis, so keep it under 22 lines (25 lines on an ANSI standard terminal,
// minus two for the top/bottom screen border, minus one for mandatory
// window top padding).
static const wchar_t *helps[] = {
	L"'q': quit			ctrl+'L': redraw the screen",
	L"'e': view environment details 'h': toggle this help display",
	L"'v': view selection details   'l': view recent diagnostics",
	L"'k'/'↑': previous selection   'j'/'↓': next selection",
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
		assert(mvwaddwstr(hw,row + z,START_COL,hs) != ERR);
	}
	return OK;
}

static const int DIAGROWS = 8;

static int
update_diags(struct panel_state *ps){
	WINDOW *w = panel_window(ps->p);
	logent l[DIAGROWS];
	int y,x,r;

	getmaxyx(w,y,x);
	y = ps->ysize;
	assert(x > 26 + START_COL * 2); // see ctime_r(3)
	if(get_logs(y,l)){
		return -1;
	}
	assert(wattrset(w,SUBDISPLAY_ATTR) == OK);
	for(r = 0 ; r < y ; ++r){
		char *c,tbuf[x - START_COL * 2 + 1];
		size_t tb;
		int p;

		if(l[r].msg == NULL){
			break;
		}
		assert(ctime_r(&l[r].when,tbuf));
		tbuf[strlen(tbuf) - 1] = ' '; // kill newline
		tb = sizeof(tbuf) / sizeof(*tbuf) - strlen(tbuf);
		p = snprintf(tbuf + strlen(tbuf) - 1,tb," %s",l[r].msg);
		if(p < 0 || (unsigned)p >= tb){
			tbuf[tb - 1] = '\0';
		}
		if( (c = strchr(tbuf,'\n')) ){
			*c = '\0';
		}
		assert(mvwprintw(w,y - r,START_COL,"%s",tbuf) != ERR);
		free(l[r - 1].msg);
	}
	return 0;
}

static int
display_diags(WINDOW *mainw,struct panel_state *ps){
	int x,y;

	getmaxyx(mainw,y,x);
	assert(y);
	memset(ps,0,sizeof(*ps));
	if(new_display_panel(mainw,ps,DIAGROWS,x - START_COL * 4,L"press 'l' to dismiss diagnostics")){
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

static const int DETAILROWS = 9;

static int
display_details(WINDOW *mainw,struct panel_state *ps){
	memset(ps,0,sizeof(*ps));
	if(new_display_panel(mainw,ps,DETAILROWS,0,L"press 'v' to dismiss details")){
		goto err;
	}
	if(current_adapter){
		if(adapter_details(panel_window(ps->p),current_adapter->as->c,ps->ysize)){
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
		assert(mvwprintw(hw,row + z,col,"LANG: %-21s TERM: %s",lang,term) != ERR);
		--z;
		break;
	}default:{
		return ERR;
	}
	}
	return OK;
}

static int
display_enviroment(WINDOW *mainw,struct panel_state *ps){
	memset(ps,0,sizeof(*ps));
	if(new_display_panel(mainw,ps,ENVROWS,0,L"press 'e' to dismiss display")){
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
	if(e > EXPANSION_DEVS){
		lns += l->parts;
		if(e > EXPANSION_PARTS){
			lns += l->fs;
			if(e > EXPANSION_FS){
				lns += l->mounts;
			}
		}
	}
	return lns;
}

// Recompute ->lns values for all nodes, and return the number of lines of
// output available before and after the current selection. If there is no
// current selection, the return value ought not be ascribed meaning. O(N) on
// the number of drives, not just those visible -- unacceptable! FIXME
static void
recompute_lines(adapterstate *is,int *before,int *after){
	blockobj *l;
	int newsel;

	*after = -1;
	*before = -1;
	newsel = !!adapter_up_p(is);
	for(l = is->bobjs ; l ; l = l->next){
		l->lns = node_lines(is->expansion,l);
		if(l == is->rb->selected){
			*before = newsel;
			*after = l->lns ? l->lns - 1 : 0;
		}else if(*after >= 0){
			*after += l->lns;
		}else{
			newsel += l->lns;
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
	if(newsel + aft <= getmaxy(is->rb->win) - 2 - !!adapter_up_p(is)){
		newsel = getmaxy(is->rb->win) - aft - 2 - !!adapter_up_p(is);
	}
	if(newsel + (int)node_lines(is->expansion,is->rb->selected) >= getmaxy(is->rb->win) - 2){
		newsel = getmaxy(is->rb->win) - 2 - node_lines(is->expansion,is->rb->selected);
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
	if(is->expansion == EXPANSION_MAX){
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
	if(is->expansion == 0){
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
		return 0;
	}
	if(rb->as->bobjs == NULL){
		return -1;
	}
	assert(rb->selline == -1);
	return select_adapter_dev(rb,rb->as->bobjs,2);
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
			case 'l':{
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
			case '\r': case '\n': case KEY_ENTER:
				pthread_mutex_lock(&bfl);
				if(select_adapter() == 0){
					selection_active = 1;
				}
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			case 12: // CTRL+L FIXME
				pthread_mutex_lock(&bfl);
				wrefresh(curscr);
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			case '+': case KEY_RIGHT:
				pthread_mutex_lock(&bfl);
				expand_adapter_locked();
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			case '-': case KEY_LEFT:{
				pthread_mutex_lock(&bfl);
				collapse_adapter_locked();
				screen_update();
				pthread_mutex_unlock(&bfl);
				break;
			}
			case KEY_UP: case 'k':{
				pthread_mutex_lock(&bfl);
				if(!selection_active){
					use_prev_controller(w,&details);
					if(active){
						assert(top_panel(active->p) != ERR);
					}
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
					if(active){
						assert(top_panel(active->p) != ERR);
					}
				}else{
					use_next_device();
				}
				screen_update();
				pthread_mutex_unlock(&bfl);
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
		blockobj *b;

		while( (b = as->bobjs) ){
			as->bobjs = b->next;
			free(b);
		}
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
update_blockobj(adapterstate *as,blockobj *b,const device *d){
	unsigned fs,mounts,parts;
	const device *p;

	fs = mounts = parts = 0;
	if(d->mnttype){
		++fs;
		if(d->mnt){
			++mounts;
		}
	}
	if(d->target){
		++mounts;
	}
	if(d->swapprio != SWAP_INVALID){
		++fs;
	}
	for(p = d->parts ; p ; p = p->next){
		++parts;
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
	}
	as->mounts += (mounts - b->mounts);
	as->parts += (parts - b->parts);
	as->fs += (fs - b->fs);
	b->fs = fs;
	b->mounts = mounts;
	b->parts = parts;
	b->lns = node_lines(as->expansion,b);
}

static blockobj *
create_blockobj(adapterstate *as,const device *d){
	blockobj *b;

	if( (b = malloc(sizeof(*b))) ){
		memset(b,0,sizeof(*b));
		b->d = d;
		update_blockobj(as,b,d);
	}
	return b;
}

static void *
block_callback(const device *d,void *v){
	adapterstate *as;
	blockobj *b;

	pthread_mutex_lock(&bfl);
	as = d->c->uistate;
	if((b = v) == NULL){
		if( (b = create_blockobj(as,d)) ){
			if(as->devs == 0){
				b->prev = b->next = NULL;
				as->bobjs = b;
			}else{
				b->next = as->bobjs;
				b->prev = NULL;
				as->bobjs->prev = b;
				as->bobjs = b;
			}
			++as->devs;
		}
	}else{
		update_blockobj(as,b,d);
	}
	if(as->rb){
		resize_adapter(as->rb);
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
	as->mounts -= bo->mounts;
	as->parts -= bo->parts;
	as->fs -= bo->fs;
	--as->devs;
	free(bo);
	if(as->rb){
		resize_adapter(as->rb);
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
					adapter_details(panel_window(details.p),get_current_adapter(),details.ysize);
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

static void
fatal(const char *fmt,...){
	va_list va;

	assert(endwin() != ERR);
	va_start(va,fmt);
	vfprintf(stderr,fmt,va);
	va_end(va);
}

int main(int argc,char * const *argv){
	const glightui ui = {
		.vdiag = vdiag,
		.adapter_event = adapter_callback,
		.block_event = block_callback,
		.adapter_free = adapter_free,
		.block_free = block_free,
		.fatal = fatal,
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
