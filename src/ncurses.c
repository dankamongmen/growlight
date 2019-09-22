#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <pthread.h>
#include <atasmart.h>

#include "fs.h"
#include "mbr.h"
#include "zfs.h"
#include "swap.h"
#include "mdadm.h"
#include "config.h"
#include "health.h"
#include "ptable.h"
#include "ptypes.h"
#include "ncurses.h"
#include "growlight.h"
#include "aggregate.h"
#include "ui-aggregate.h"

#define KEY_ESC 27

static void shutdown_cycle(void) __attribute__ ((noreturn));

void locked_diag(const char *fmt,...) __attribute__ ((format (printf,1,2)));

// For the ANSI standard terminal, we can fit only 4 lines of explicative text
// onto the screen, so make each glyph count. By default, 76 characters can be
// placed on each row.
static const char SEARCH_TEXT[] =
"Your search term will be matched against device names, UUIDs, partition "
"names, filesystem names (volume labels), manufacturers, model numbers, and "
"serial numbers.";

static const char LOOP_TEXT[] =
"The specified file will be treated as a block device once associated with "
"the selected loop device.";

static const char PARTFLAG_TEXT[] =
"Select a collection of flags to set on the partition.";

static const char MOUNTOPS_TEXT[] =
"Select a collection of options to apply to this mount.";

static const char TARGET_TEXT[] =
"Enter a mount point relative to the target's root.";

static const char TARG_TEXT[] =
"Enter a mount point relative to the target's root. It must already exist, and "
"no other filesystem should yet be mounted there. The filesystem will be made "
"available for use now, and included in the target /etc/fstab for use later.";

static const char MOUNT_TEXT[] =
"Enter a mount point. It must already exist, and no other filesystem should "
"yet be mounted there.";

static const char PTTYPE_TEXT[] =
"Select a partition table type. GPT is recommended unless you must use tools "
"and/or hardware which don't understand it. Please note that MSDOS partition "
"tables do not support partitions in excess of 2TB.";

static const char PSPEC_TEXT[] =
"Specify the new partition size as either a percentage of the containing "
"space, a number of bytes, or a starting and ending sector (inclusive), "
"separated by a colon (':').";

static const char FSNAME_TEXT[] =
"Named filesystems can be more easily used in bootloaders and other tools, and "
"provide references independent of dynamic device topology. ";

static const char PNAME_TEXT[] =
"Named partitions can be more easily used in bootloaders and other tools, and "
"provide references independent of dynamic device topology. A GPT partition's "
"name consists of up to 32 UTF-16LE codepoints.";

static const char PARTTYPE_TEXT[] =     // characters here will be bumped ---v
"Creating a given filesystem is generally neither enabled nor prohibited due "
"to partition type, but mismatched types might confuse tools, hardware, and "
"users. UEFI through version 1.1 boots from a GPT's ESP partition. UEFI+MBR "
"and BIOS boot from a primary (as opposed to logical) MBR partition.";

static const char FSTYPE_TEXT[] =
"UEFI through version 1.1 requires FAT16 for the EFI System Partition. As of "
"version 3.5, ext4 is the default Linux filesystem, but Windows and OS X do "
"not natively support it. I recommend use of EXT4 or FAT16 for root and ZFS "
"(in a redundant configuration) for other filesystems.";

static pthread_mutex_t bfl = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

struct panel_state {
	PANEL *p;
};

#define PANEL_STATE_INITIALIZER { .p = NULL, }
#define SUBDISPLAY_ATTR (COLOR_PAIR(SUBDISPLAY_COLOR) | A_BOLD)
#define SUBDISPLAY_INVAL_ATTR (COLOR_PAIR(SUBDISPLAY_COLOR))

static struct panel_state *splash;
static struct panel_state *active;
static struct panel_state maps = PANEL_STATE_INITIALIZER;
static struct panel_state help = PANEL_STATE_INITIALIZER;
static struct panel_state diags = PANEL_STATE_INITIALIZER;
static struct panel_state details = PANEL_STATE_INITIALIZER;
static struct panel_state environment = PANEL_STATE_INITIALIZER;

static int helpstrs(WINDOW *);
static int map_details(WINDOW *);
static int update_details(WINDOW *);

static inline void
update_details_cond(PANEL *p){
	if(p){
		update_details(panel_window(p));
	}
}

static inline void
update_help_cond(PANEL *p){
	if(p){
		helpstrs(panel_window(p));
	}
}

static inline void
update_map_cond(PANEL *p){
	if(p){
		map_details(panel_window(p));
	}
}

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
	FORM_MULTISELECT,		// form_options[]
	FORM_CHECKBOXEN,		// form_options[]
	FORM_SPLASH_PROMPT,		// form_input
} form_enum;

// Regarding scrolling selection windows: the movement model is the same as
// the main scrollwindow: moving up at the topmost line keeps you at the top
// of the widget, and rotates the selections. scrolloff equals the number of
// lines options have been rotated down, and takes values between 0 and
// opcount - 1. All option selection windows scroll, even if they fit all their
// options, to maintain continuity of UI.
struct form_state {
	PANEL *p;
	void (*fxn)(const char *);	// callback once form is done
	void (*mcb)(const char *,char **,int,int); // multiform callback
	int longop;			// length of prompt or longest op
	char *boxstr;			// string for box label
	form_enum formtype;		// type of form
	struct panel_state *extext;	// explication text, above the form
	union {
		struct {
			// There's padding on the interface, and a border -- 4
			// lines total. idx maps to true array indices, and has
			// nothing to do with what's currently displayed (save
			// that it's guaranteed to be onscreen). scrollidx also
			// maps to true array indices, and identifies the first
			// option listed. Thus:
			//
			//   idx - scrollidx % opcount == current display line
			//   current display line + scrollidx % opcount == idx
			//   idx == scrollidx -> display at top
			//   idx == scrollidx + ysize - 4 -> display at bottom
			//
			int idx;		// selection index
			int scrolloff;		// scroll offset
			struct form_option *ops;// form_option array for *this instance*
			int opcount;		// total number of ops
			int selectno;		// number of selections, total
			int selections;		// number of active selections
			char **selarray;	// array of selections by name
		};
		struct form_input inp;	// form_input state for this instance
	};
};

static struct form_state *actform;

// Our color pairs
enum {
	HEADER_COLOR = 1,
	STATUS_COLOR,
	UHEADING_COLOR,
	UNHEADING_COLOR,
	UBORDER_COLOR,			// Adapters
	SELBORDER_COLOR,		// Current adapter
	DBORDER_COLOR,			// Block bar borders
	PBORDER_COLOR,
	PHEADING_COLOR,
	SUBDISPLAY_COLOR,
	OPTICAL_COLOR,
	ROTATE_COLOR,
	VIRTUAL_COLOR,
	SSD_COLOR,
	FS_COLOR,
	EMPTY_COLOR,			// Empty sectors
	METADATA_COLOR,			// Partition table metadata
	MDADM_COLOR,
	ZPOOL_COLOR,
	PART_COLOR0,			// A defined but unused partition
	PART_COLOR1,
	PART_COLOR2,
	PART_COLOR3,
	FORMBORDER_COLOR,
	FORMTEXT_COLOR,
	INPUT_COLOR,			// Form input color
	SELECTED_COLOR,			// Selected options in multiform
	MOUNT_COLOR0,			// Mounted, untargeted filesystems
	MOUNT_COLOR1,
	MOUNT_COLOR2,
	MOUNT_COLOR3,
	TARGET_COLOR0,			// Targeted filesystems
	TARGET_COLOR1,
	TARGET_COLOR2,
	TARGET_COLOR3,
	FUCKED_COLOR,			// Things that warrant attention
	SPLASHBORDER_COLOR,
	SPLASHTEXT_COLOR,

	ORANGE_COLOR,
	GREEN_COLOR,
	BLACK_COLOR,

	FIRST_FREE_COLOR
};

static inline int
next_targco(int targco){
	if(++targco > TARGET_COLOR3){
		targco = TARGET_COLOR0;
	}
	return targco;
}

static inline int
next_mountco(int mountco){
	if(++mountco > MOUNT_COLOR3){
		mountco = MOUNT_COLOR0;
	}
	return mountco;
}

static inline int
next_partco(int partco){
	if(++partco > PART_COLOR3){
		partco = PART_COLOR0;
	}
	return partco;
}

#define COLOR_LIGHTRED 9
#define COLOR_LIGHTGREEN 10
#define COLOR_LIGHTYELLOW 11
#define COLOR_LIGHTBLUE 12
#define COLOR_LIGHTMAGENTA 13 // (pink)
#define COLOR_LIGHTCYAN 14
#define COLOR_LIGHTWHITE 15
#define COLOR_HIDDEN 16
#define COLOR_SKYBLUE 0x20 // 32 (xterm color cube)
#define COLOR_PURPLE 0x39 // incredibly vibrant 0x68
#define COLOR_CRAP 0x6f
#define COLOR_LIGHTPURPLE 0x3f
#define COLOR_CYAN0 0x7b
#define COLOR_CYAN1 0x23
#define COLOR_CYAN2 0x2c
#define COLOR_CYAN3 0x33
#define COLOR_MAGENTA0 0x44
#define COLOR_MAGENTA1 0x46
#define COLOR_MAGENTA2 0x48
#define COLOR_MAGENTA3 0x4a
#define COLOR_MAIZE 0xdc
#define COLOR_WHITE0 0xfc
#define COLOR_WHITE1 0xfa
#define COLOR_WHITE2 0xf8
#define COLOR_WHITE3 0xf6

#define REP_METADATA L'm' //L'\u1d50'//L'M', L'ᵐ'
#define REP_EMPTY L'e' //L'\u2091' //L'E', L'ₑ'

static inline wchar_t
subscript(int in){
	assert(in >= 0 && in < 10);
	return L'\u2080' + in;
}

static inline void
screen_update(void){
	if(active){
		assert(top_panel(active->p) != ERR);
	}
	if(actform){
		if(actform->extext){
			assert(top_panel(actform->extext->p) != ERR);
		}
		assert(top_panel(actform->p) != ERR);
	}
	if(splash){
		assert(top_panel(splash->p) != ERR);
	}
	update_panels();
	assert(doupdate() == OK);
}

static int
setup_colors(void){
	int z;

	if(init_pair(HEADER_COLOR,COLOR_LIGHTPURPLE,-1) == ERR){
		assert(init_pair(HEADER_COLOR,COLOR_BLUE,-1) == OK);
	}
	if(init_pair(STATUS_COLOR,COLOR_SKYBLUE,-1) == ERR){
		assert(init_pair(STATUS_COLOR,COLOR_YELLOW,-1) != ERR);
	}
	if(init_pair(UHEADING_COLOR,COLOR_WHITE,-1) == ERR){
		assert(init_pair(UHEADING_COLOR,COLOR_BLUE,-1) != ERR);
		assert(init_pair(SELBORDER_COLOR,COLOR_CYAN,-1) != ERR);
	}else{
		assert(init_pair(SELBORDER_COLOR,COLOR_BLUE,-1) != ERR);
	}
	if(init_pair(UNHEADING_COLOR,COLOR_SKYBLUE,-1) == ERR){
		assert(init_pair(UNHEADING_COLOR,COLOR_BLUE,-1) != ERR);
	}
	assert(init_pair(UBORDER_COLOR,COLOR_CYAN,-1) != ERR);
	assert(init_pair(PBORDER_COLOR,COLOR_YELLOW,COLOR_BLACK) == OK);
	if(init_pair(DBORDER_COLOR,COLOR_CRAP,-1) == ERR){
		assert(init_pair(DBORDER_COLOR,COLOR_RED,-1) != ERR);
	}
	assert(init_pair(PHEADING_COLOR,COLOR_RED,COLOR_BLACK) == OK);
	assert(init_pair(SUBDISPLAY_COLOR,COLOR_WHITE,COLOR_BLACK) == OK);
	assert(init_pair(OPTICAL_COLOR,COLOR_YELLOW,-1) == OK);
	if(init_pair(ROTATE_COLOR,COLOR_LIGHTWHITE,-1) == ERR){
		assert(init_pair(ROTATE_COLOR,COLOR_WHITE,-1) != ERR);
	}
	assert(init_pair(VIRTUAL_COLOR,COLOR_WHITE,-1) == OK);
	if(init_pair(SSD_COLOR,COLOR_LIGHTWHITE,-1) == ERR){
		assert(init_pair(SSD_COLOR,COLOR_WHITE,-1) != ERR);
	}
	assert(init_pair(FS_COLOR,COLOR_GREEN,-1) == OK);
	if(init_pair(EMPTY_COLOR,COLOR_MAIZE,-1) == ERR){
		assert(init_pair(EMPTY_COLOR,COLOR_GREEN,-1) == OK);
	}
	if(init_pair(METADATA_COLOR,COLOR_LIGHTYELLOW,-1) == ERR){
		assert(init_pair(METADATA_COLOR,COLOR_YELLOW,-1) == OK);
	}
	if(init_pair(MDADM_COLOR,COLOR_LIGHTBLUE,-1) == ERR){
		assert(init_pair(MDADM_COLOR,COLOR_BLUE,-1) != ERR);
	}
	assert(init_pair(ZPOOL_COLOR,COLOR_BLUE,-1) == OK);
	if(init_pair(PART_COLOR0,COLOR_CYAN0,-1) == ERR){
		assert(init_pair(PART_COLOR0,COLOR_CYAN,-1) == OK);
	}
	if(init_pair(PART_COLOR1,COLOR_CYAN1,-1) == ERR){
		assert(init_pair(PART_COLOR1,COLOR_CYAN,-1) == OK);
	}
	if(init_pair(PART_COLOR2,COLOR_CYAN2,-1) == ERR){
		assert(init_pair(PART_COLOR2,COLOR_CYAN,-1) == OK);
	}
	if(init_pair(PART_COLOR3,COLOR_CYAN3,-1) == ERR){
		assert(init_pair(PART_COLOR3,COLOR_CYAN,-1) == OK);
	}
	assert(init_pair(FORMBORDER_COLOR,COLOR_MAGENTA,COLOR_BLACK) == OK);
	if(init_pair(FORMTEXT_COLOR,COLOR_LIGHTCYAN,COLOR_BLACK) == ERR){
		assert(init_pair(FORMTEXT_COLOR,COLOR_CYAN,COLOR_BLACK) != ERR);
	}
	if(init_pair(INPUT_COLOR,COLOR_LIGHTGREEN,COLOR_BLACK) == ERR){
		assert(init_pair(INPUT_COLOR,COLOR_GREEN,COLOR_BLACK) != ERR);
	}
	if(init_pair(SELECTED_COLOR,COLOR_LIGHTCYAN,-1) == ERR){
		assert(init_pair(SELECTED_COLOR,COLOR_CYAN,-1) != ERR);
	}
	if(init_pair(MOUNT_COLOR0,COLOR_WHITE0,-1) == ERR){
		assert(init_pair(MOUNT_COLOR0,COLOR_WHITE,-1) == OK);
	}
	if(init_pair(MOUNT_COLOR1,COLOR_WHITE1,-1) == ERR){
		assert(init_pair(MOUNT_COLOR1,COLOR_WHITE,-1) == OK);
	}
	if(init_pair(MOUNT_COLOR2,COLOR_WHITE2,-1) == ERR){
		assert(init_pair(MOUNT_COLOR2,COLOR_WHITE,-1) == OK);
	}
	if(init_pair(MOUNT_COLOR3,COLOR_WHITE3,-1) == ERR){
		assert(init_pair(MOUNT_COLOR3,COLOR_WHITE,-1) == OK);
	}
	if(init_pair(TARGET_COLOR0,COLOR_MAGENTA0,-1) == ERR){
		assert(init_pair(TARGET_COLOR0,COLOR_MAGENTA,-1) == OK);
	}
	if(init_pair(TARGET_COLOR1,COLOR_MAGENTA1,-1) == ERR){
		assert(init_pair(TARGET_COLOR1,COLOR_MAGENTA,-1) == OK);
	}
	if(init_pair(TARGET_COLOR2,COLOR_MAGENTA2,-1) == ERR){
		assert(init_pair(TARGET_COLOR2,COLOR_MAGENTA,-1) == OK);
	}
	if(init_pair(TARGET_COLOR3,COLOR_MAGENTA3,-1) == ERR){
		assert(init_pair(TARGET_COLOR3,COLOR_MAGENTA,-1) == OK);
	}
	if(init_pair(FUCKED_COLOR,COLOR_LIGHTRED,-1) == ERR){
		assert(init_pair(FUCKED_COLOR,COLOR_RED,-1) != ERR);
	}
	if(init_pair(SPLASHBORDER_COLOR,COLOR_PURPLE,COLOR_BLACK) == ERR){
		assert(init_pair(SPLASHBORDER_COLOR,COLOR_GREEN,COLOR_BLACK) != ERR);
	}
	if(init_pair(SPLASHTEXT_COLOR,COLOR_LIGHTCYAN,COLOR_BLACK) == ERR){
		assert(init_pair(SPLASHTEXT_COLOR,COLOR_CYAN,COLOR_BLACK) != ERR);
	}
	assert(init_pair(ORANGE_COLOR,COLOR_YELLOW,-1) == OK);
	assert(init_pair(GREEN_COLOR,COLOR_GREEN,-1) == OK);
	assert(init_pair(BLACK_COLOR,COLOR_BLACK,COLOR_BLACK) == OK);
	for(z = FIRST_FREE_COLOR ; z < COLORS ; ++z){
		init_pair(z,z,-1);
	}
	wrefresh(curscr);
	screen_update();
	return 0;
}

static void
form_colors(void){
	// Don't reset the status color or header color, nor (obviously) the
	// form nor splash colors.
	locked_diag("%s",""); // Don't leave a highlit status up from long ago
	init_pair(UBORDER_COLOR,-1,-1);
	init_pair(SELBORDER_COLOR,-1,-1);
	init_pair(UHEADING_COLOR,-1,-1);
	init_pair(UNHEADING_COLOR,-1,-1);
	init_pair(PBORDER_COLOR,-1,-1);
	init_pair(DBORDER_COLOR,-1,-1);
	init_pair(PHEADING_COLOR,-1,-1);
	init_pair(SUBDISPLAY_COLOR,-1,-1);
	init_pair(OPTICAL_COLOR,-1,-1);
	init_pair(ROTATE_COLOR,-1,-1);
	init_pair(VIRTUAL_COLOR,-1,-1);
	init_pair(SSD_COLOR,-1,-1);
	init_pair(FS_COLOR,-1,-1);
	init_pair(EMPTY_COLOR,-1,-1);
	init_pair(METADATA_COLOR,-1,-1);
	init_pair(MDADM_COLOR,-1,-1);
	init_pair(ZPOOL_COLOR,-1,-1);
	init_pair(PART_COLOR0,-1,-1);
	init_pair(PART_COLOR1,-1,-1);
	init_pair(PART_COLOR2,-1,-1);
	init_pair(PART_COLOR3,-1,-1);
	init_pair(MOUNT_COLOR0,-1,-1);
	init_pair(MOUNT_COLOR1,-1,-1);
	init_pair(MOUNT_COLOR2,-1,-1);
	init_pair(MOUNT_COLOR3,-1,-1);
	init_pair(TARGET_COLOR0,-1,-1);
	init_pair(TARGET_COLOR1,-1,-1);
	init_pair(TARGET_COLOR2,-1,-1);
	init_pair(TARGET_COLOR3,-1,-1);
	init_pair(FUCKED_COLOR,-1,-1);
	init_pair(ORANGE_COLOR,-1,-1);
	init_pair(GREEN_COLOR,-1,-1);
	wrefresh(curscr);
	screen_update();
}

static int update_diags(struct panel_state *);

struct adapterstate;

struct partobj;

// See below (blockobj) for description of zones.
typedef struct zobj {
	int zoneno;			// in-order, starting at 0
	uintmax_t fsector,lsector;	// first and last logical sector, inclusive
	device *p;			// partition/block device, NULL for empty space
	wchar_t rep;			// character used for representation
					//  if ->p is NULL
	int following;			// Number of zones following us on disk
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
	// zchain: list of zone objects
	// zone: currently selected zone, NULL iff |zchain| == 0
	zobj *zchain,*zone;
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
	controller *c;
	unsigned devs;
	enum {
		EXPANSION_NONE,
		EXPANSION_FULL,
	} expansion;
	struct adapterstate *next,*prev;
	blockobj *bobjs;
	reelbox *rb;
} adapterstate;

#define EXPANSION_MAX EXPANSION_FULL

static char statusmsg[BUFSIZ];

static unsigned count_adapters;
// dequeue + single selection
static reelbox *current_adapter,*top_reelbox,*last_reelbox;

#define START_COL 1		// Room to leave for borders
#define PAD_COLS(cols) ((cols))

static blockobj *
get_selected_blockobj(void){
	reelbox *rb;

	if((rb = current_adapter) == NULL){
		return NULL;
	}
	return rb->selected;
}

static inline int
blockobj_unloadedp(const blockobj *bo){
	return bo && ((bo->d->layout == LAYOUT_NONE && bo->d->blkdev.unloaded)
		|| bo->d->size == 0);
}

static inline int
selected_unloadedp(void){
	return blockobj_unloadedp(get_selected_blockobj());
}

static inline int
blockobj_unpartitionedp(const blockobj *bo){
	return bo && !bo->zone &&
		(bo->d->mnttype ||
		 (bo->d->layout == LAYOUT_NONE && !bo->d->blkdev.pttable)
		 || (bo->d->layout == LAYOUT_MDADM && !bo->d->mddev.pttable)
		 || (bo->d->layout == LAYOUT_DM && !bo->d->dmdev.pttable)
		 || bo->d->layout == LAYOUT_ZPOOL);
}

static inline int
selected_unpartitionedp(void){
	return blockobj_unpartitionedp(get_selected_blockobj());
}

static inline int
blockobj_emptyp(const blockobj *bo){
	return bo && bo->zone && !bo->zone->p;
}

static inline int
selected_emptyp(void){
	return blockobj_emptyp(get_selected_blockobj());
}

static inline int
blockobj_partitionp(const blockobj *bo){
	return bo && bo->zone && bo->zone->p;
}

static inline int
selected_partitionp(void){
	return blockobj_partitionp(get_selected_blockobj());
}

static inline device *
selected_filesystem(void){
	blockobj *bo;

	if((bo = get_selected_blockobj()) == NULL){
		return NULL;
	}
	if(blockobj_unpartitionedp(bo)){
		if(bo->d->mnttype){
			return bo->d;
		}
	}else{
		if(blockobj_partitionp(bo)){
			if(bo->zone->p->mnttype){
				return bo->zone->p;
			}
		}
	}
	return NULL;
}

static inline int
mkfs_safe_p(const device *d){
	if(d->mnttype){
		locked_diag("Filesystem signature already exists on %s",d->name);
		return 0;
	}
	return 1;
}

static inline int
selected_mkfs_safe_p(void){
	const blockobj *bo = get_selected_blockobj();

	if(bo && bo->d){
		if(bo->zone && bo->zone->p){
			return mkfs_safe_p(bo->zone->p);
		}else if(bo->zone){
			locked_diag("Can't make filesystem in empty space");
			return 0;
		}
		return mkfs_safe_p(bo->d);
	}
	return 0;
}

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
		{ .attr = 0, .chars = L"╰", },
		{ .attr = 0, .chars = L"╯", },
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
		{ .attr = 0, .chars = L"╭", },
		{ .attr = 0, .chars = L"╮", },
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
		{ .attr = 0, .chars = L"╭", },
		{ .attr = 0, .chars = L"╮", },
		{ .attr = 0, .chars = L"╰", },
		{ .attr = 0, .chars = L"╯", },
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

static void
draw_main_window(WINDOW *w){
	int rows,cols,scol,x,y;
	char buf[BUFSIZ];

	scol = snprintf(buf,sizeof(buf),"%s %s (%d)",PACKAGE,VERSION,count_adapters - 1);
	assert(scol > 0 && (unsigned)scol < sizeof(buf));
	getmaxyx(w,rows,cols);
	assert(wattrset(w,COLOR_PAIR(HEADER_COLOR)) != ERR);
	mvwaddstr(w,rows - 1,0,buf);
	getyx(w,y,x);
	assert(y >= 0);
	cols -= x + 2;
	assert(wattron(w,A_BOLD | COLOR_PAIR(STATUS_COLOR)) != ERR);
	assert(wprintw(w," %-*.*s",cols,cols,statusmsg) != ERR);
}

static void
locked_vdiag(const char *fmt,va_list v){
	size_t off;

	vsnprintf(statusmsg,sizeof(statusmsg) / sizeof(*statusmsg) - 1,fmt,v);
	statusmsg[sizeof(statusmsg) / sizeof(*statusmsg) - 1] = '\0';
	for(off = 0 ; off < sizeof(statusmsg) / sizeof(*statusmsg) - 1 ; ++off){
		if(!isgraph(statusmsg[off])){
			if(!statusmsg[off]){
				break;
			}
			statusmsg[off] = ' ';
		}
	}
	draw_main_window(stdscr);
	if(diags.p){
		update_diags(&diags);
	}
	screen_update();
}

void locked_diag(const char *fmt,...){
	va_list v;

	va_start(v,fmt);
	locked_vdiag(fmt,v);
	va_end(v);
}

static inline int
device_lines(int expa,const blockobj *bo){
	int l = 0;

	if(expa != EXPANSION_NONE){
		if(bo->d->size){
			l += 2;
		}
		++l;
	}
	return l;
}

// This is the number of lines we'd have in an optimal world; we might have
// fewer available to us on this screen at this time.
static int
lines_for_adapter(const struct adapterstate *as){
	int l = 2;

	const blockobj *bo;

	for(bo = as->bobjs ; bo ; bo = bo->next){
		l += device_lines(as->expansion,bo);
	}
	return l;
}

static zobj *
create_zobj(zobj *prev,unsigned zno,uintmax_t fsector,uintmax_t lsector,
			device *p,wchar_t rep){
	zobj *z;

	assert(lsector >= fsector);
	assert(fsector > 0 || zno == 0);
	assert(fsector == 0 || zno > 0);
	if( (z = malloc(sizeof(*z))) ){
		z->zoneno = zno;
		z->fsector = fsector;
		z->lsector = lsector;
		z->rep = rep;
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

	if(l > rows - 1){ // bottom summary line
		l = rows - 1;
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
	}else if(rb->scrline < 0){
		return 0;
	}else if(rb->scrline == 0 && adapter_lines_bounded(as,rows) != getmaxy(rb->win)){
		return 0;
	}
	return 1;
}

// Returns the amount of space available at the bottom.
static inline int
bottom_space_p(int rows){
	if(!last_reelbox){
		return rows;
	}
	if(getmaxy(last_reelbox->win) + getbegy(last_reelbox->win) >= rows - 2){
		return 0;
	}
	return (rows - 1) - (getmaxy(last_reelbox->win) + getbegy(last_reelbox->win));
}

// Create a panel at the bottom of the window, referred to as the "subdisplay".
// Only one can currently be active at a time. Window decoration and placement
// is managed here; only the rows needed for display ought be provided.
static int
new_display_panel(WINDOW *w,struct panel_state *ps,int rows,int cols,
			const wchar_t *hstr,const wchar_t *bstr,int borderpair){
	const int crightlen = bstr ? wcslen(bstr) : 0;
	int ybelow,yabove;
	WINDOW *psw;
	int x,y;

	// Desired space above and below, which will be impugned upon as needed
	ybelow = 3;
	yabove = 5;
	getmaxyx(w,y,x);
	if(cols == 0){
		cols = x - START_COL * 2; // indent 2 on the left, 0 on the right
	}else{
		assert(x >= cols + START_COL * 2);
	}
	if(rows + ybelow + yabove >= y){
		if(rows + ybelow >= y){
			yabove = 0;
		}else{
			yabove = y - (rows + ybelow);
		}
	}else{
		yabove += y - (rows + ybelow + yabove);
	}
	if((psw = newwin(rows + 2,cols,yabove,0)) == NULL){
		locked_diag("Can't display subwindow, uh-oh");
		return ERR;
	}
	if((ps->p = new_panel(psw)) == NULL){
		locked_diag("Couldn't create subpanel, uh-oh");
		delwin(psw);
		return ERR;
	}
	assert(top_panel(ps->p) != ERR);
	// memory leaks follow if we're compiled with NDEBUG! FIXME
	assert(wattron(psw,A_BOLD) != ERR);
	assert(wcolor_set(psw,borderpair,NULL) == OK);
	assert(bevel(psw) == OK);
	assert(wattroff(psw,A_BOLD) != ERR);
	assert(wcolor_set(psw,PHEADING_COLOR,NULL) == OK);
	if(hstr){
		assert(mvwaddwstr(psw,0,START_COL * 2,hstr) != ERR);
	}
	if(bstr){
		assert(mvwaddwstr(psw,rows + 1,cols - (crightlen + START_COL * 2),bstr) != ERR);
	}
	return OK;
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
	}
}

// Print the contents of the block device in a horizontal bar of arbitrary size
static void
print_blockbar(WINDOW *w,const blockobj *bo,int y,int sx,int ex,int selected){
	static const cchar_t bchr[] = {
		{ .attr = 0, .chars = L"∾", },
		{ .attr = 0, .chars = L" ", },
		{ .attr = 0, .chars = L" ", },
	};
	char pre[PREFIXSTRLEN + 1];
	const char *selstr = NULL;
	wchar_t wbuf[ex - sx + 2];
	int targco,mountco,partco;
	const device *d = bo->d;
	char buf[ex - sx + 2];
	const zobj *z;
	int off = sx;
	uintmax_t zs;

	zs = d->mntsize ? d->mntsize :
		(last_usable_sector(d) - first_usable_sector(d) + 1) * bo->d->logsec;
	// Sometimes, a partitioned block device will be given a mnttype by
	// libblkid (usually due to a filesystem signature in unpartitioned
	// space at the beginning). In that case, don't try to print based off
	// the bogon block device mnttype.
	if(d->mnttype && (d->layout != LAYOUT_NONE || !d->blkdev.pttable)){
		int co = mnttype_aggregablep(d->mnttype) ?
			COLOR_PAIR(PART_COLOR0) : COLOR_PAIR(FS_COLOR);

		assert(wattrset(w,A_BOLD|co) == OK);
		qprefix(zs,1,pre,sizeof(pre),1);
		if(!d->mnt.count || swprintf(wbuf,sizeof(wbuf),L" %s%s%ls%s%ls%s%s%sat %s ",
			d->label ? "" : "nameless ",
			d->mnttype,
			d->label ? L" “" : L"",
			d->label ? d->label : "",
			d->label ? L"” " : L" ",
			zs ? "(" : "", zs ? pre : "", zs ? ") " : "",
			d->mnt.list[0]) >= (int)(sizeof(wbuf))){
			if(swprintf(wbuf,sizeof(wbuf),L" %s%s%ls%s%ls%s%s%s ",
				d->label ? "" : "nameless ",
				d->mnttype,
				d->label ? L" “" : L"",
				d->label ? d->label : "",
				d->label ? L"” " : L" ",
				zs ? "(" : "", zs ? pre : "", zs ? ")" : ""
				) >= (int)(sizeof(wbuf))){
				if((unsigned)swprintf(wbuf,sizeof(wbuf),L" %s%s%s%s ",
					d->mnttype,
					zs ? "(" : "", zs ? pre : "", zs ? ")" : ""
					) >= sizeof(wbuf)){
					assert((unsigned)swprintf(wbuf,sizeof(wbuf),L"%s",d->mnttype) < sizeof(wbuf));
				}
			}
		}
		mvwhline_set(w,y,sx,&bchr[0],ex - sx + 1);
		mvwadd_wch(w,y,sx,&bchr[1]);
		mvwaddwstr(w,y,sx + (ex - sx + 1 - wcslen(wbuf)) / 2,wbuf);
		mvwadd_wch(w,y,ex - 1,&bchr[2]);
		selstr = d->name;
	}else if(d->layout == LAYOUT_NONE && d->blkdev.unloaded){
		assert(wattrset(w,A_BOLD|COLOR_PAIR(OPTICAL_COLOR)) == OK);
		selstr = "No media detected in drive";
		mvwprintw(w,y,sx,"%-*.*s",ex - sx,ex - sx,selstr);
	}else if((d->layout == LAYOUT_NONE && d->blkdev.pttable == NULL) ||
		(d->layout == LAYOUT_MDADM && d->mddev.pttable == NULL) ||
		(d->layout == LAYOUT_DM && d->dmdev.pttable == NULL)){
		assert(wattrset(w,A_BOLD|COLOR_PAIR(EMPTY_COLOR)) == OK);
		selstr = d->layout == LAYOUT_NONE ? "unpartitioned space" :
				"unpartitionable space";
		assert(snprintf(buf,sizeof(buf)," %s %s ",
				qprefix(d->size,1,pre,sizeof(pre),1),
				selstr) < (int)sizeof(buf));
		mvwhline_set(w,y,sx,&bchr[0],ex - sx + 1);
		mvwadd_wch(w,y,sx,&bchr[1]);
		mvwaddstr(w,y,sx + (ex - sx + 1 - strlen(buf)) / 2,buf);
		mvwadd_wch(w,y,ex - 1,&bchr[2]);
	}
	if((z = bo->zchain) == NULL){
		if(selected){
			wattron(w,A_REVERSE);
			mvwprintw(w,y - 1,sx + 1,"⇗⇨⇨⇨%.*s",(int)(ex - (sx + 5)),selstr);
			wattroff(w,A_REVERSE);
		}
		return;
	}
	partco = PART_COLOR0;
	targco = TARGET_COLOR0;
	mountco = MOUNT_COLOR0;
	do{
		unsigned ch,och;
		wchar_t rep;

		wbuf[0] = L'\0';
		zs = (z->lsector - z->fsector + 1) * bo->d->logsec;
		qprefix(zs,1,pre,sizeof(pre),1);
		if(z->p == NULL){ // unused space among partitions, or metadata
			int co = (z->rep == REP_METADATA ? COLOR_PAIR(METADATA_COLOR) :
					COLOR_PAIR(EMPTY_COLOR));
			const char *repstr = z->rep == REP_METADATA ?
				"partition table metadata" : "empty space";

			if(selected && z == bo->zone){
				selstr = repstr;
				assert(wattrset(w,A_BOLD|A_UNDERLINE|co) == OK);
			}else{
				assert(wattrset(w,co) == OK);
			}
			if((unsigned)swprintf(wbuf,sizeof(wbuf) - 2,L"%s %s",
					pre,repstr) >= sizeof(wbuf) - 2){
				if((unsigned)swprintf(wbuf,sizeof(wbuf) - 2,L"%s",repstr) >= sizeof(wbuf) - 2){
					wbuf[0] = L'\0';
				}
			}
			rep = z->rep;
		}else{ // dedicated partition
			if(selected && z == bo->zone){ // partition and device are selected
				if(targeted_p(z->p)){
					assert(wattrset(w,A_BOLD|A_UNDERLINE|COLOR_PAIR(targco)) == OK);
					targco = next_targco(targco);
				}else if(z->p->mnt.count){
					assert(wattrset(w,A_BOLD|A_UNDERLINE|COLOR_PAIR(mountco)) == OK);
					mountco = next_mountco(mountco);
				}else if(z->p->mnttype && !mnttype_aggregablep(z->p->mnttype)){
					assert(wattrset(w,A_BOLD|A_UNDERLINE|COLOR_PAIR(FS_COLOR)) == OK);
				}else{
					assert(wattrset(w,A_BOLD|A_UNDERLINE|COLOR_PAIR(partco)) == OK);
					partco = next_partco(partco);
				}
				// FIXME need to store pname as multibyte char *
				// selstr = z->p->partdev.pname;
				// selstr = selstr ? selstr : z->p->name;
				if( (selstr = z->p->name) ){
					if((unsigned)swprintf(wbuf,sizeof(wbuf) - 2,L"%s %s",
							pre,selstr) >= sizeof(wbuf) - 2){
						if((unsigned)swprintf(wbuf,sizeof(wbuf) - 2,L"%s",selstr) >= sizeof(wbuf) - 2){
							wbuf[0] = L'\0';
						}
					}
				}
			}else{ // partition is not selected
				if(targeted_p(z->p)){
					assert(wattrset(w,COLOR_PAIR(targco)) == OK);
					targco = next_targco(targco);
				}else if(z->p->mnt.count){
					assert(wattrset(w,COLOR_PAIR(mountco)) == OK);
					mountco = next_mountco(mountco);
				}else if(z->p->mnttype && !mnttype_aggregablep(z->p->mnttype)){
					assert(wattrset(w,COLOR_PAIR(FS_COLOR)) == OK);
				}else{
					assert(wattrset(w,COLOR_PAIR(partco)) == OK);
					partco = next_partco(partco);
				}
				if((unsigned)swprintf(wbuf,sizeof(wbuf) - 2,L"%s %s",
							pre,z->p->name) >= sizeof(wbuf) - 2){
					if((unsigned)swprintf(wbuf,sizeof(wbuf) - 2,L"%s",z->p->name) >= sizeof(wbuf) - 2){
						wbuf[0] = L'\0';
					}
				}
			}
			if(z->p->partdev.alignment < d->physsec){ // misaligned!
				assert(wattrset(w,A_BOLD|COLOR_PAIR(FUCKED_COLOR)) == OK);
			}
			if(z->p->mnttype){
				if((!z->p->mnt.count || (unsigned)swprintf(wbuf,sizeof(wbuf) - 2,L"%s at %s (%s)",z->p->mnttype,z->p->mnt.list[0],pre) >= sizeof(wbuf) - 2)){
					if(!z->p->label || (unsigned)swprintf(wbuf,sizeof(wbuf) - 2,L"%s “%s” (%s)",
						z->p->mnttype,z->p->label,pre) >= sizeof(wbuf) - 2){
						if((unsigned)swprintf(wbuf,sizeof(wbuf) - 2,L"%s (%s)",z->p->mnttype,pre) >= sizeof(wbuf) - 2){
							wbuf[0] = L'\0';
						}
					}
				}
			}
			rep = z->p->partdev.pnumber % 16;
			if(rep >= 10){
				rep = L'a' + (rep - 10); // FIXME lame
			}else{
				rep = L'0' + rep;	// FIXME lame
			}
		}
		ch = (((z->lsector - z->fsector) * 1000) / ((d->size * 1000 / d->logsec) / (ex - sx)));
		if(ch == 0){
			ch = 1;
		}
		och = off;
		while(ch--){
			cchar_t crep[] = { { .attr = 0, .chars[0] = rep, }, };
			mvwadd_wch(w,y,off,crep);
			if(++off > ex - z->following){
				off = ex - z->following;
				break;
			}
		}
		wattron(w,A_REVERSE);
		if(selstr){
			if(och < ex / 2u){
				mvwprintw(w,y - 1,och,"⇗⇨⇨⇨%.*s",(int)(ex - (off + strlen(selstr) + 4)),selstr);
			}else{
				mvwprintw(w,y - 1,off - 4 - strlen(selstr),"%s⇦⇦⇦⇖",selstr);
			}
		}
		wattroff(w,A_REVERSE);
		// Truncate it at whitespace until it's small enough to fit
		while(wcslen(wbuf) && wcslen(wbuf) + 2 > (off - och + 1)){
			wchar_t *wtrunc = wcsrchr(wbuf,L' ');

			if(wtrunc){
				*wtrunc = L'\0';
			}else{
				wbuf[0] = L'\0';
			}
		}
		if(wcslen(wbuf)){
			size_t start = och + ((off - och + 1) - wcslen(wbuf)) / 2;

			wattron(w,A_BOLD);
			mvwaddwstr(w,y,start,wbuf);
			mvwaddch(w,y,start - 1,' ');
			mvwaddch(w,y,start + wcslen(wbuf),' ');
		}
		selstr = NULL;
	}while((z = z->next) != bo->zchain);
}

static void
print_dev(const reelbox *rb,const blockobj *bo,int line,int rows,
			unsigned cols,int topp,unsigned endp){
	char buf[PREFIXSTRLEN + 1];
	int selected,co,rx,attr;
	char rolestr[12]; // taken from %-11.11s below

	if(line >= rows - !endp){
		return;
	}
	strcpy(rolestr,"");
	selected = line >= 1 && line == rb->selline;
	rx = cols - 79;
	switch(bo->d->layout){
case LAYOUT_NONE:
		if(bo->d->blkdev.realdev){
			if(bo->d->blkdev.removable){
				assert(wattrset(rb->win,COLOR_PAIR(OPTICAL_COLOR)) == OK);
				strncpy(rolestr,"removable",sizeof(rolestr));
			}else if(bo->d->blkdev.rotation >= 0){
				int32_t speed = bo->d->blkdev.rotation;
				assert(wattrset(rb->win,COLOR_PAIR(ROTATE_COLOR)) == OK);
				if(speed > 0){
					if(speed > 99999){
						speed = 99999;
					}
					snprintf(rolestr, sizeof(rolestr),
						 "%d rpm", speed);
				}else{
					strncpy(rolestr,"ferromag",sizeof(rolestr));
				}
			}else{
				assert(wattrset(rb->win,COLOR_PAIR(SSD_COLOR)) == OK);
				strncpy(rolestr,"solidstate",sizeof(rolestr));
			}
		}else{
			assert(wattrset(rb->win,COLOR_PAIR(VIRTUAL_COLOR)) == OK);
			strncpy(rolestr,"virtual",sizeof(rolestr));
		}
		if(line + !!topp >= 1){
			if(!bo->d->size || line + 2 < rows - !endp){
				if(bo->d->size){
					line += 2;
				}else if(selected){
					wattron(rb->win,A_REVERSE);
				}
		mvwprintw(rb->win,line,1,"%11.11s  %-16.16s %4.4s " PREFIXFMT " %4uB %-6.6s%-16.16s %4.4s %-*.*s",
					bo->d->name,
					bo->d->model ? bo->d->model : "n/a",
					bo->d->revision ? bo->d->revision : "n/a",
					qprefix(bo->d->size,1,buf,sizeof(buf),0),
					bo->d->physsec,
					bo->d->blkdev.pttable ? bo->d->blkdev.pttable : "none",
					bo->d->blkdev.wwn ? bo->d->blkdev.wwn : "n/a",
					bo->d->blkdev.realdev ? transport_str(bo->d->blkdev.transport) : "n/a",
					rx,rx,"");
				if(bo->d->size){
					line -= 2;
				}
			}
		}
		break;
case LAYOUT_MDADM:
		if(bo->d->mddev.level){
			strncpy(rolestr, bo->d->mddev.level, sizeof(rolestr) - 1);
			rolestr[sizeof(rolestr) - 1] = '\0';
		}
		if(bo->d->mddev.degraded){
			co = COLOR_PAIR(FUCKED_COLOR);
		}else{
			co = COLOR_PAIR(MDADM_COLOR);
		}
		assert(wattrset(rb->win,co) == OK);
		if(line + !!topp >= 1){
			if(!bo->d->size || line + 2 < rows - !endp){
				if(bo->d->size){
					line += 2;
				}else if(selected){
					wattron(rb->win,A_REVERSE);
				}
		mvwprintw(rb->win,line,1,"%11.11s  %-16.16s %4.4s " PREFIXFMT " %4uB %-6.6s%-16.16s %4.4s %-*.*s",
					bo->d->name,
					bo->d->model ? bo->d->model : "n/a",
					bo->d->revision ? bo->d->revision : "n/a",
					qprefix(bo->d->size,1,buf,sizeof(buf),0),
					bo->d->physsec,
					bo->d->mddev.pttable ? bo->d->mddev.pttable : "none",
					bo->d->mddev.mdname ? bo->d->mddev.mdname : "n/a",
					transport_str(bo->d->mddev.transport),
					rx,rx,"");
				if(bo->d->size){
					line -= 2;
				}
			}
		}
		assert(wattrset(rb->win,COLOR_PAIR(MDADM_COLOR)) == OK);
		break;
case LAYOUT_DM:
		strncpy(rolestr,"dm",sizeof(rolestr));
		assert(wattrset(rb->win,COLOR_PAIR(MDADM_COLOR)) == OK);
		if(line + !!topp >= 1){
			if(!bo->d->size || line + 2 < rows - !endp){
				if(bo->d->size){
					line += 2;
				}else if(selected){
					wattron(rb->win,A_REVERSE);
				}
		mvwprintw(rb->win,line,1,"%11.11s  %-16.16s %4.4s " PREFIXFMT " %4uB %-6.6s%-16.16s %4.4s %-*.*s",
					bo->d->name,
					bo->d->model ? bo->d->model : "n/a",
					bo->d->revision ? bo->d->revision : "n/a",
					qprefix(bo->d->size,1,buf,sizeof(buf),0),
					bo->d->physsec,
					bo->d->dmdev.pttable ? bo->d->dmdev.pttable : "none",
					bo->d->dmdev.dmname ? bo->d->dmdev.dmname : "n/a",
					transport_str(bo->d->dmdev.transport),
					rx,rx,"");
				if(bo->d->size){
					line -= 2;
				}
			}
		}
		break;
case LAYOUT_PARTITION:
		break;
case LAYOUT_ZPOOL:
		strncpy(rolestr,"zpool",sizeof(rolestr));
		assert(wattrset(rb->win,COLOR_PAIR(ZPOOL_COLOR)) == OK);
		if(line + !!topp >= 1){
			if(!bo->d->size || line + 2 < rows - !endp){
				if(bo->d->size){
					line += 2;
				}else if(selected){
					wattron(rb->win,A_REVERSE);
				}
		mvwprintw(rb->win,line,1,"%11.11s  %-16.16s %4ju " PREFIXFMT " %4uB %-6.6s%-16.16s %4.4s %-*.*s",
					bo->d->name,
					bo->d->model ? bo->d->model : "n/a",
					(uintmax_t)bo->d->zpool.zpoolver,
					qprefix(bo->d->size,1,buf,sizeof(buf),0),
					bo->d->physsec,
					"spa",
					"n/a", // FIXME
					transport_str(bo->d->zpool.transport),
					rx,rx,"");
				if(bo->d->size){
					line -= 2;
				}
			}
		}
		break;
	}
	if(bo->d->size == 0){
		return;
	}
	wattroff(rb->win, A_REVERSE);
	wattrset(rb->win, A_BOLD|COLOR_PAIR(SUBDISPLAY_COLOR));

	// Box-diagram (3-line) mode. Print the name on the first line.
	if(line + !!topp >= 1){
		mvwprintw(rb->win, line, START_COL, "%11.11s", bo->d->name);
	}

	// Print summary below device name, in the same color, but prefix it
	// with the single-character SMART status when applicable.
	if(line + 1 < rows - !endp && line + !!topp + 1 >= 1){
		wchar_t rep = L' ';
		if(bo->d->blkdev.smart >= 0){
			if(bo->d->blkdev.smart == SK_SMART_OVERALL_GOOD){
				wattrset(rb->win, A_BOLD|COLOR_PAIR(GREEN_COLOR));
				rep = L'✔';
			}else if(bo->d->blkdev.smart != SK_SMART_OVERALL_BAD_STATUS
					&& bo->d->blkdev.smart != SK_SMART_OVERALL_BAD_SECTOR_MANY){
				wattrset(rb->win, A_BOLD|COLOR_PAIR(ORANGE_COLOR));
				rep = L'☠';
			}else{
				wattrset(rb->win, A_BOLD|COLOR_PAIR(FUCKED_COLOR));
				rep = L'✗';
			}
		}
		mvwprintw(rb->win, line + 1, START_COL, "%lc", rep);
		wattrset(rb->win, A_BOLD|COLOR_PAIR(SUBDISPLAY_COLOR));
		if(strlen(rolestr)){
			wprintw(rb->win, "%10.10s", rolestr);
		}else{
			wprintw(rb->win, "          ");
		}
	}
	// ...and finally the temperature/vfailure status, and utilization...
	if((line + 2 < rows - !endp) && (line + !!topp + 2 >= 1)){
		int sumline = line + 2;
		if(bo->d->layout == LAYOUT_NONE){
			if(bo->d->blkdev.celsius && bo->d->blkdev.celsius < 100u){
				if(bo->d->blkdev.celsius >= 60u){
					wattrset(rb->win, A_BOLD|COLOR_PAIR(FUCKED_COLOR));
				}else if(bo->d->blkdev.celsius >= 40u){
					wattrset(rb->win, A_BOLD|COLOR_PAIR(ORANGE_COLOR));
				}else{
					wattrset(rb->win, COLOR_PAIR(GREEN_COLOR));
				}
				// FIXME would be nice to use ℃ , but it looks weird
				mvwprintw(rb->win, sumline, START_COL, "%2.ju° ", bo->d->blkdev.celsius);
			}else{
				mvwprintw(rb->win, sumline, START_COL, "    ");
			}
		}else if(bo->d->layout == LAYOUT_MDADM){
			if(bo->d->mddev.degraded){
				wattrset(rb->win,A_BOLD|COLOR_PAIR(FUCKED_COLOR));
				mvwprintw(rb->win, sumline, START_COL,
					  "%1lux☠ ", bo->d->mddev.degraded);
			}else{
				wattrset(rb->win,COLOR_PAIR(GREEN_COLOR));
				mvwprintw(rb->win, sumline, START_COL, "up  ");
			}
		}else if(bo->d->layout == LAYOUT_DM){
			// FIXME add more detail...type of dm etc
			wattrset(rb->win,COLOR_PAIR(GREEN_COLOR));
			mvwprintw(rb->win,sumline,START_COL,"up  ");
		}else if(bo->d->layout == LAYOUT_ZPOOL){
			if(bo->d->zpool.state != POOL_STATE_ACTIVE){
				wattrset(rb->win,A_BOLD|COLOR_PAIR(FUCKED_COLOR));
				mvwprintw(rb->win,sumline,START_COL,"☠☠☠ ");
			}else{
				wattrset(rb->win,COLOR_PAIR(GREEN_COLOR));
				mvwprintw(rb->win,sumline,START_COL,"up  ");
			}
		}
		uintmax_t io;
		io = bo->d->statdelta.sectors_read;
		io += bo->d->statdelta.sectors_written;
		io *= bo->d->logsec;
		// FIXME normalize according to timeq
		wattrset(rb->win, COLOR_PAIR(SELECTED_COLOR));
		// FIXME 'i' shows up only when there are fewer than 3 sigfigs
		// to the left of the decimal point...very annoying
		if(io){
			bprefix(io, 1, buf, sizeof(buf), 1);
			wprintw(rb->win, "%7.7s", buf);
		}else{
			wprintw(rb->win, " no i/o");
		}
	}

	assert(wattrset(rb->win,A_BOLD|COLOR_PAIR(DBORDER_COLOR)) == OK);
	if(selected){
		wattron(rb->win,A_REVERSE);
	}
	if(line + !!topp >= 1){
		mvwaddch(rb->win,line,START_COL + 10 + 1,ACS_ULCORNER);
		mvwhline(rb->win,line,START_COL + 2 + 10,ACS_HLINE,cols - START_COL * 2 - 2 - 10);
		mvwaddch(rb->win,line,cols - START_COL * 2,ACS_URCORNER);
	}
	if(++line >= rows - !endp){
		return;
	}

	if(line + !!topp >= 1){
		mvwaddch(rb->win,line,START_COL + 10 + 1,ACS_VLINE);
		print_blockbar(rb->win,bo,line,START_COL + 10 + 2,
					cols - START_COL - 1,selected);
	}
	attr = A_BOLD | COLOR_PAIR(DBORDER_COLOR);
	wattrset(rb->win,attr);
	if(selected){
		wattron(rb->win,A_REVERSE);
	}
	if(line + !!topp >= 1){
		mvwaddch(rb->win,line,cols - START_COL * 2,ACS_VLINE);
	}
	if(++line >= rows - !endp){
		return;
	}
	if(line + !!topp >= 1){
		int c = cols - 80;
		static const cchar_t bchrs[] = {
			{ .attr = 0, .chars = L"┤", },
			{ .attr = 0, .chars = L"├", },
		};

		mvwaddch(rb->win,line,START_COL + 10 + 1,attr | ACS_LLCORNER);
		wadd_wch(rb->win,&bchrs[0]);
		mvwadd_wch(rb->win,line,cols - 3 - c,&bchrs[1]);
		if(c > 0){
			whline(rb->win,ACS_HLINE,c);
			wmove(rb->win,line,cols - 2);
		}
		waddch(rb->win,attr | ACS_LRCORNER);
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

// Abovetop: lines hidden at the top of the screen
// Belowend: lines hidden at the bottom of the screen
static void
adapter_box(const adapterstate *as,WINDOW *w,unsigned abovetop,unsigned belowend){
	int current = as->rb == current_adapter;
	int bcolor,hcolor,rows,cols;
	int attrs;

	getmaxyx(w,rows,cols);
	if(current){
		hcolor = UHEADING_COLOR; // plus A_BOLD
		bcolor = SELBORDER_COLOR;
		attrs = A_BOLD;
	}else{
		hcolor = UNHEADING_COLOR;;
		bcolor = UBORDER_COLOR;
		attrs = A_NORMAL;
	}
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
	if(abovetop == 0){
		if(current){
			assert(wattron(w,A_BOLD) == OK);
		}else{
			assert(wattroff(w,A_BOLD) == OK);
		}
		assert(mvwprintw(w,0,7,"%ls",L"[") != ERR);
		assert(wcolor_set(w,hcolor,NULL) == OK);
		assert(waddstr(w,as->c->ident) != ERR);
		if(as->c->numa_node >= 0){
			assert(wprintw(w," [%d]",as->c->numa_node) != ERR);
		}
		if(as->c->bandwidth){
			char buf[PREFIXSTRLEN + 1],dbuf[PREFIXSTRLEN + 1];

			if(as->c->demand){
				wprintw(w," (%sbps to Southbridge, %sbps (%ju%%) demanded)",
					qprefix(as->c->bandwidth,1,buf,sizeof(buf),1),
					qprefix(as->c->demand,1,dbuf,sizeof(dbuf),1),
					as->c->demand * 100 / as->c->bandwidth);
			}else{
				wprintw(w," (%sbps to Southbridge)",
					qprefix(as->c->bandwidth,1,buf,sizeof(buf),1));
			}
		}else if(as->c->bus != BUS_VIRTUAL && as->c->demand){
			char dbuf[PREFIXSTRLEN + 1];

			wprintw(w," (%sbps demanded)",qprefix(as->c->demand,1,dbuf,sizeof(dbuf),1));
		}
		assert(wcolor_set(w,bcolor,NULL) != ERR);
		assert(wprintw(w,"]") != ERR);
		assert(wattron(w,A_BOLD) == OK);
		assert(wmove(w,0,cols - 5) != ERR);
		waddwstr(w,as->expansion != EXPANSION_MAX ? L"[+]" : L"[-]");
		assert(wattron(w,attrs) != ERR);
	}
	if(belowend == 0){
		if(as->c->bus == BUS_PCIe){
			assert(wcolor_set(w,bcolor,NULL) != ERR);
			if(current){
				assert(wattron(w,A_BOLD) == OK);
			}else{
				assert(wattroff(w,A_BOLD) == OK);
			}
			assert(mvwprintw(w,rows - 1,6,"[") != ERR);
			assert(wcolor_set(w,hcolor,NULL) != ERR);
			if(as->c->pcie.lanes_neg == 0){
				wprintw(w,"Southbridge device %04x:%02x.%02x.%x",
					as->c->pcie.domain,as->c->pcie.bus,
					as->c->pcie.dev,as->c->pcie.func);
			}else{
				wprintw(w,"PCI Express device %04x:%02x.%02x.%x (x%u, gen %s)",
						as->c->pcie.domain,as->c->pcie.bus,
						as->c->pcie.dev,as->c->pcie.func,
						as->c->pcie.lanes_neg,pcie_gen(as->c->pcie.gen));
			}
			assert(wcolor_set(w,bcolor,NULL) != ERR);
			assert(wprintw(w,"]") != ERR);
		}
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
	}else if(getbegy(rb->win) == 0){ // no top
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

// -------------------------------------------------------------------------
// -- splash API. splashes are displayed during long operations, especially
//    those requiring an external program.
// -------------------------------------------------------------------------
struct panel_state *show_splash(const wchar_t *msg){
	struct panel_state *ps;

	assert(!splash);
	if((ps = malloc(sizeof(*ps))) == NULL){
		return NULL;
	}
	memset(ps,0,sizeof(*ps));
	// FIXME gross, clean all of this up
	if(new_display_panel(stdscr,ps,3,wcslen(msg) + 4,NULL,NULL,SPLASHBORDER_COLOR)){
		free(ps);
		return NULL;
	}
	wattrset(panel_window(ps->p),A_BOLD|COLOR_PAIR(SPLASHTEXT_COLOR));
	mvwhline(panel_window(ps->p),1,1,' ',getmaxx(panel_window(ps->p)) - 2);
	mvwhline(panel_window(ps->p),2,1,' ',getmaxx(panel_window(ps->p)) - 2);
	mvwaddwstr(panel_window(ps->p),2,2,msg);
	mvwhline(panel_window(ps->p),3,1,' ',getmaxx(panel_window(ps->p)) - 2);
	form_colors();
	move_panel(ps->p,3,3);
	return splash = ps;
}
// -------------------------------------------------------------------------
// -- end splash API
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// -- begin form API
// -------------------------------------------------------------------------
// Forms are modal. They take over the keyboard UI and sit atop everything
// else. Subwindows sit atop the hardware elements of the UI, but do not seize
// any of the input UI. A form and subwindow can coexist.

// -------------------------------------------------------------------------
// -- form creation
// -------------------------------------------------------------------------
static struct form_state *
create_form(const char *str,void (*fxn)(const char *),form_enum ftype,int scrolloff){
	struct form_state *fs;

	if( (fs = malloc(sizeof(*fs))) ){
		memset(fs,0,sizeof(*fs));
		if((fs->boxstr = strdup(str)) == NULL){
			locked_diag("Couldn't create input dialog (%s?)",strerror(errno));
			free(fs);
			return NULL;
		}
		fs->scrolloff = scrolloff;
		fs->selectno = 1;
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
		hide_panel_locked(fs->extext);
		free(fs->extext);
		fs->extext = NULL;
		switch(fs->formtype){
			case FORM_SELECT:
			case FORM_MULTISELECT:
			case FORM_CHECKBOXEN:
				for(z = 0 ; z < fs->opcount ; ++z){
					free(fs->ops[z].option);
					free(fs->ops[z].desc);
				}
				free(fs->selarray);
				fs->selarray = NULL;
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
			case FORM_SPLASH_PROMPT:
				break;
		}
		actform = NULL;
	}
}

static void
free_form(struct form_state *fs){
	if(fs){
		free(fs->boxstr);
		destroy_form_locked(fs);
		free(fs);
		if(splash == NULL){
			setup_colors();
		}
		if(current_adapter){
			touchwin(current_adapter->win);
		}
		screen_update();
	}
}

static void
multiform_options(struct form_state *fs){
	static const cchar_t bchr[] = {
		{ .attr = 0, .chars = L"╮", },
		{ .attr = 0, .chars = L"╯", },
		{ .attr = 0, .chars = L"╭", },
		{ .attr = 0, .chars = L"╰", },
		{ .attr = 0, .chars = L"│", },
	};
	const struct form_option *opstrs = fs->ops;
	WINDOW *fsw = panel_window(fs->p);
	int z,cols,selidx,maxz;

	assert(fs->formtype == FORM_MULTISELECT);
	cols = getmaxx(fsw);
	wattrset(fsw,COLOR_PAIR(FORMBORDER_COLOR));
	wattron(fsw,A_BOLD);
	mvwadd_wch(fsw,1,1,&bchr[2]);
	mvwadd_wch(fsw,1,fs->longop + 4,&bchr[0]);
	maxz = getmaxy(fsw) - 3;
	for(z = 1 ; z < maxz ; ++z){
		int op = ((z - 1) + fs->scrolloff) % fs->opcount;

		assert(op >= 0);
		assert(op < fs->opcount);
		wattroff(fsw,A_BOLD);
		wcolor_set(fsw,FORMBORDER_COLOR,NULL);
		if(fs->selectno >= z){
			mvwprintw(fsw,z + 1,START_COL * 2,"%d",z);
		}else if(fs->selections >= z){
			mvwprintw(fsw,z + 2,START_COL * 2,"%d",z);
		}
		if(z < fs->opcount + 1){
			wattron(fsw,A_BOLD);
			wcolor_set(fsw,FORMTEXT_COLOR,NULL);
			mvwprintw(fsw,z + 1,START_COL * 2 + fs->longop + 4,"%-*.*s ",
				fs->longop,fs->longop,opstrs[op].option);
			if(op == fs->idx){
				wattron(fsw,A_REVERSE);
			}
			wcolor_set(fsw,INPUT_COLOR,NULL);
			for(selidx = 0 ; selidx < fs->selections ; ++selidx){
				if(strcmp(opstrs[op].option,fs->selarray[selidx]) == 0){
					wcolor_set(fsw,SELECTED_COLOR,NULL);
					break;
				}
			}
			wprintw(fsw,"%-*.*s",cols - fs->longop * 2 - 9,
				cols - fs->longop * 2 - 9,opstrs[op].desc);
			wattroff(fsw,A_REVERSE);
		}
	}
	wattrset(fsw,COLOR_PAIR(FORMBORDER_COLOR));
	wattron(fsw,A_BOLD);
	for(z = 0 ; z < fs->selections ; ++z){
		mvwaddstr(fsw,z >= fs->selectno ? 3 + z : 2 + z,4,fs->selarray[z]);
	}
	mvwadd_wch(fsw,fs->selectno + 2,1,&bchr[3]);
	mvwadd_wch(fsw,fs->selectno + 2,fs->longop + 4,&bchr[1]);
}

static void
check_options(struct form_state *fs){
	const struct form_option *opstrs = fs->ops;
	WINDOW *fsw = panel_window(fs->p);
	int z,cols,selidx,maxz;

	assert(fs->formtype == FORM_CHECKBOXEN);
	cols = getmaxx(fsw);
	wattrset(fsw,COLOR_PAIR(FORMBORDER_COLOR));
	wattron(fsw,A_BOLD);
	maxz = getmaxy(fsw) - 3;
	for(z = 1 ; z < maxz ; ++z){
		int op = ((z - 1) + fs->scrolloff) % fs->opcount;

		assert(op >= 0);
		assert(op < fs->opcount);
		wattroff(fsw,A_BOLD);
		wcolor_set(fsw,FORMBORDER_COLOR,NULL);
		if(z < fs->opcount + 1){
			wchar_t ballot = L'☐';

			wattron(fsw,A_BOLD);
			wcolor_set(fsw,FORMTEXT_COLOR,NULL);
			for(selidx = 0 ; selidx < fs->selections ; ++selidx){
				if(strcmp(opstrs[op].option,fs->selarray[selidx]) == 0){
					ballot = L'☒';
					break;
				}
			}
			if(op == fs->idx){
				wattron(fsw,A_REVERSE);
			}else{
				wcolor_set(fsw,INPUT_COLOR,NULL);
			}
			mvwprintw(fsw,z + 1,START_COL * 2,"%lc %-*.*s ",
				ballot,fs->longop,fs->longop,opstrs[op].option);
			wprintw(fsw,"%-*.*s",cols - fs->longop - 7,
				cols - fs->longop - 7,opstrs[op].desc);
			wattroff(fsw,A_REVERSE);
		}
	}
	wattrset(fsw,COLOR_PAIR(FORMBORDER_COLOR));
}

static void
form_options(struct form_state *fs){
	const struct form_option *opstrs = fs->ops;
	WINDOW *fsw = panel_window(fs->p);
	int z,cols;

	if(fs->formtype != FORM_SELECT){
		return;
	}
	cols = getmaxx(fsw);
	wattron(fsw,A_BOLD);
	for(z = 1 ; z < getmaxy(fsw) - 3 ; ++z){
		int op = ((z - 1) + fs->scrolloff) % fs->opcount;

		assert(op >= 0);
		assert(op < fs->opcount);
		wcolor_set(fsw,FORMTEXT_COLOR,NULL);
		mvwprintw(fsw,z + 1,START_COL * 2,"%-*.*s ",
			fs->longop,fs->longop,opstrs[op].option);
		if(op == fs->idx){
			wattron(fsw,A_REVERSE);
		}
		wcolor_set(fsw,INPUT_COLOR,NULL);
		wprintw(fsw,"%-*.*s",cols - fs->longop - 1 - START_COL * 4,
			cols - fs->longop - 1 - START_COL * 4,opstrs[op].desc);
		wattroff(fsw,A_REVERSE);
	}
}

#define FORM_Y_OFFSET 5
static struct panel_state *
raise_form_explication(const WINDOW *w,const char *text,int linesz){
	int linepre[linesz - 1];
	int linelen[linesz - 1];
	struct panel_state *ps;
	int cols,x,y,brk,tot;
	WINDOW *win;

	// There's two columns of padding surrounding the subwindow
	cols = getmaxx(w) - 1;
	tot = 0;
	for(y = 0 ; (unsigned)y < sizeof(linepre) / sizeof(*linepre) ; ++y){
		while(isspace(text[tot])){
			++tot;
		}
		linepre[y] = tot;
		linelen[y] = 0;
		brk = 0;
		for(x = 1 ; x < cols - 1 ; ++x){
			if(!text[tot]){
				brk = x;
				linelen[y] = brk - 1;
				break;
			}
			if(isspace(text[tot])){
				brk = x;
			}
			++tot;
		}
		// A brk value of 0 would indicate a single token longer than
		// the screen, an unlikely event with which we don't yet deal.
		assert(!text[tot] || brk);
		// Go to the beginning of the current token, if we're in one.
		tot -= (x - brk);
		linelen[y] = brk - 1;
		if(!text[tot]){
			if(y == 0){
				cols = x + 1;
			}
			break;
		}
	}
	// If we've not chewed through all the text, we're not going to fit it
	// into the provided space. We don't yet deal with this situation FIXME
	assert(!text[tot]);
	assert( (ps = malloc(sizeof(*ps))) );
	assert( (win = newwin(y + 3,cols,linesz - (y + 2),getmaxx(w) - cols)) );
	assert( (ps->p = new_panel(win)) );
	wbkgd(win,COLOR_PAIR(BLACK_COLOR));
	wattrset(win,COLOR_PAIR(FORMBORDER_COLOR));
	bevel(win);
	wattrset(win,COLOR_PAIR(FORMTEXT_COLOR));
	do{
		assert(mvwaddnstr(win,y + 1,1,text + linepre[y],linelen[y]) != ERR);
	}while(y--);
	assert(top_panel(ps->p) != ERR);
	screen_update();
	return ps;
}

void raise_multiform(const char *str,void (*fxn)(const char *,char **,int,int),
		struct form_option *opstrs,int ops,int defidx,
		int selectno,char **selarray,int selections,const char *text,
		int scrollidx){
	size_t longop,longdesc;
	struct form_state *fs;
	int cols,rows;
	WINDOW *fsw;
	int x,y;

	assert(ops);
	assert(opstrs);
	if(actform){
		locked_diag("An input dialog is already active");
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
	cols = longdesc + longop * 2 + 9;
#define ESCSTR L"'C' confirms setup, ⎋esc returns"
	if(cols < (int)wcslen(ESCSTR) + 2){
		cols = wcslen(ESCSTR) + 2;
	}
	rows = (ops > selectno ? ops : selectno) + 4;
	getmaxyx(stdscr,y,x);
	if(x < cols){
		locked_diag("Window too thin for form, uh-oh");
		return;
	}
	if(y <= rows + FORM_Y_OFFSET){
		rows = y - FORM_Y_OFFSET - 1;
		if(y < FORM_Y_OFFSET + 4 + 1){ // two boundaries + empties, at least 1 selection
			locked_diag("Window too short for form, uh-oh");
			return;
		}
	}
	if((fs = create_form(str,NULL,FORM_MULTISELECT,scrollidx)) == NULL){
		return;
	}
	fs->mcb = fxn;
	if((fsw = newwin(rows,cols,FORM_Y_OFFSET,x - cols)) == NULL){
		locked_diag("Couldn't create form window, uh-oh");
		free_form(fs);
		return;
	}
	if((fs->p = new_panel(fsw)) == NULL){
		locked_diag("Couldn't create form panel, uh-oh");
		delwin(fsw);
		free_form(fs);
		return;
	}
	wbkgd(fsw,COLOR_PAIR(BLACK_COLOR));
	// FIXME adapt for scrolling (default might be off-window at beginning)
	if((fs->idx = defidx) < 0){
		fs->idx = defidx = 0;
	}
	fs->opcount = ops;
	fs->selarray = selarray;
	fs->selections = selections;
	wattroff(fsw,A_BOLD);
	wcolor_set(fsw,FORMBORDER_COLOR,NULL);
	bevel(fsw);
	wattron(fsw,A_BOLD);
	mvwprintw(fsw,0,cols - strlen(fs->boxstr) - 4,"%s",fs->boxstr);
	mvwaddwstr(fsw,getmaxy(fsw) - 1,cols - wcslen(ESCSTR) - 1,ESCSTR);
#undef ESCSTR
	wattroff(fsw,A_BOLD);
	fs->longop = longop;
	fs->ops = opstrs;
	fs->selectno = selectno;
	multiform_options(fs);
	fs->extext = raise_form_explication(stdscr,text,FORM_Y_OFFSET);
	actform = fs;
	form_colors();
	assert(top_panel(fs->p) != ERR);
	screen_update();
}

// A collection of checkboxes
static void
raise_checkform(const char *str,void (*fxn)(const char *,char **,int,int),
		struct form_option *opstrs,int ops,int defidx,
		char **selarray,int selections,const char *text,
		int scrollidx){
	size_t longop,longdesc;
	struct form_state *fs;
	int cols,rows;
	WINDOW *fsw;
	int x,y;

	assert(ops);
	assert(opstrs);
	if(actform){
		locked_diag("An input dialog is already active");
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
	cols = longdesc + longop + 7;
#define ESCSTR L"'C' commits, ⎋esc returns"
	if(cols < (int)wcslen(ESCSTR) + 2){
		cols = wcslen(ESCSTR) + 2;
	}
	rows = ops + 4;
	getmaxyx(stdscr,y,x);
	if(x < cols){
		locked_diag("Window too thin for form, uh-oh");
		return;
	}
	if(y <= rows + FORM_Y_OFFSET){
		rows = y - FORM_Y_OFFSET - 1;
		if(y < FORM_Y_OFFSET + 4 + 1){ // two boundaries + empties, at least 1 selection
			locked_diag("Window too short for form, uh-oh");
			return;
		}
	}
	if((fs = create_form(str,NULL,FORM_CHECKBOXEN,scrollidx)) == NULL){
		return;
	}
	fs->mcb = fxn;
	if((fsw = newwin(rows,cols,FORM_Y_OFFSET,x - cols)) == NULL){
		locked_diag("Couldn't create form window, uh-oh");
		free_form(fs);
		return;
	}
	if((fs->p = new_panel(fsw)) == NULL){
		locked_diag("Couldn't create form panel, uh-oh");
		delwin(fsw);
		free_form(fs);
		return;
	}
	wbkgd(fsw,COLOR_PAIR(BLACK_COLOR));
	// FIXME adapt for scrolling (default might be off-window at beginning)
	if((fs->idx = defidx) < 0){
		fs->idx = defidx = 0;
	}
	fs->opcount = ops;
	fs->selarray = selarray;
	fs->selections = selections;
	wattroff(fsw,A_BOLD);
	wcolor_set(fsw,FORMBORDER_COLOR,NULL);
	bevel(fsw);
	wattron(fsw,A_BOLD);
	mvwprintw(fsw,0,cols - strlen(fs->boxstr) - 2,"%s",fs->boxstr);
	mvwaddwstr(fsw,getmaxy(fsw) - 1,cols - wcslen(ESCSTR) - 1,ESCSTR);
#undef ESCSTR
	wattroff(fsw,A_BOLD);
	fs->longop = longop;
	fs->ops = opstrs;
	check_options(fs);
	fs->extext = raise_form_explication(stdscr,text,FORM_Y_OFFSET);
	actform = fs;
	form_colors();
	assert(top_panel(fs->p) != ERR);
	screen_update();
}

// -------------------------------------------------------------------------
// - select type form, for single choice from among a set
// -------------------------------------------------------------------------
void raise_form(const char *str,void (*fxn)(const char *),struct form_option *opstrs,
			int ops,int defidx,const char *text){
	size_t longop,longdesc;
	struct form_state *fs;
	int cols,rows;
	WINDOW *fsw;
	int x,y;

	if(opstrs == NULL || !ops){
		locked_diag("Passed empty %u-option string table",ops);
		return;
	}
	if(actform){
		locked_diag("An input dialog is already active");
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
	rows = ops + 4;
	getmaxyx(stdscr,y,x);
	if(x < cols + 4){
		locked_diag("Window too thin for form, uh-oh");
		return;
	}
	if(y <= rows + FORM_Y_OFFSET){
		rows = y - FORM_Y_OFFSET - 1;
		if(y < FORM_Y_OFFSET + 4 + 1){ // two boundaries + empties, at least 1 selection
			locked_diag("Window too short for form, uh-oh");
			return;
		}
	}
	if((fs = create_form(str,fxn,FORM_SELECT,0)) == NULL){
		return;
	}
	if((fsw = newwin(rows,cols + START_COL * 4,FORM_Y_OFFSET,x - cols - 4)) == NULL){
		locked_diag("Couldn't create form window, uh-oh");
		free_form(fs);
		return;
	}
	if((fs->p = new_panel(fsw)) == NULL){
		locked_diag("Couldn't create form panel, uh-oh");
		delwin(fsw);
		free_form(fs);
		return;
	}
	wbkgd(fsw,COLOR_PAIR(BLACK_COLOR));
	// FIXME adapt for scrolling (default might be off-window at beginning)
	if((fs->idx = defidx) < 0){
		fs->idx = defidx = 0;
	}
	fs->opcount = ops;
	wattroff(fsw,A_BOLD);
	wcolor_set(fsw,FORMBORDER_COLOR,NULL);
	bevel(fsw);
	wattron(fsw,A_BOLD);
	mvwprintw(fsw,0,cols - strlen(fs->boxstr),"%s",fs->boxstr);
	mvwaddwstr(fsw,getmaxy(fsw) - 1,cols - wcslen(L"⎋esc returns"),L"⎋esc returns");
	wattroff(fsw,A_BOLD);
	fs->longop = longop;
	fs->ops = opstrs;
	form_options(fs);
	actform = fs;
	fs->extext = raise_form_explication(stdscr,text,FORM_Y_OFFSET);
	form_colors();
	assert(top_panel(fs->p) != ERR);
	screen_update();
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
	mvwhline(fsw,1,1,' ',cols - 2);
	mvwprintw(fsw,1,START_COL,"%-*.*s: ",
		fs->longop,fs->longop,fs->inp.prompt);
	wcolor_set(fsw,INPUT_COLOR,NULL);
	wprintw(fsw,"%.*s",cols - fs->longop - 2 - 2,fs->inp.buffer);
	wattroff(fsw,A_BOLD);
}

void raise_str_form(const char *str,void (*fxn)(const char *),
			const char *def,const char *text){
	struct form_state *fs;
	WINDOW *fsw;
	int cols;
	int x,y;

	assert(str && fxn);
	if(actform){
		locked_diag("An input dialog is already active");
		return;
	}
	if((fs = create_form(str,fxn,FORM_STRING_INPUT,0)) == NULL){
		return;
	}
	fs->longop = strlen(str);
	cols = fs->longop + 40 + 1; // FIXME? 40 for input currently
	getmaxyx(stdscr,y,x);
	assert(x >= cols + 3);
	assert(y >= 3);
	if((fsw = newwin(3,cols + START_COL * 2,FORM_Y_OFFSET,x - cols - 3)) == NULL){
		locked_diag("Couldn't create form window, uh-oh");
		free_form(fs);
		return;
	}
	if((fs->p = new_panel(fsw)) == NULL){
		locked_diag("Couldn't create form panel, uh-oh");
		delwin(fsw);
		free_form(fs);
		return;
	}
	assert(top_panel(fs->p) != ERR);
	wattroff(fsw,A_BOLD);
	wcolor_set(fsw,FORMBORDER_COLOR,NULL);
	bevel(fsw);
	wattron(fsw,A_BOLD);
	mvwprintw(fsw,0,cols - strlen(fs->boxstr),"%s",fs->boxstr);
	mvwaddwstr(fsw,getmaxy(fsw) - 1,cols - wcslen(L"⎋esc returns"),L"⎋esc returns");
	fs->inp.prompt = fs->boxstr;
	def = def ? def : "";
	fs->inp.buffer = strdup(def);
	form_string_options(fs);
	actform = fs;
	fs->extext = raise_form_explication(stdscr,text,FORM_Y_OFFSET);
	curs_set(1);
	form_colors();
	screen_update();
}

static const struct form_option common_fsops[] = {
	{
		.option = "ro",
		.desc = "Read-only",
	//},{ // this is the default
	//	.option = "rw",
	//	.desc = "Read-write",
	},{
		.option = "async",
		.desc = "Only asynchronous I/O",
	},{
		.option = "sync",
		.desc = "Only synchronous I/O",
	},{
		.option = "noatime",
		.desc = "No access time updates",
	},{
		.option = "relatime",
		.desc = "Relative access time updates",
	},{
		.option = "strictatime",
		.desc = "Full access time updates",
	},{
		.option = "nostrictatime",
		.desc = "Use the kernel's default policy",
	},{
		.option = "noauto",
		.desc = "Do not mount when running 'mount -a'",
	},{
		.option = "nofail",
		.desc = "Don't halt the boot on filesystem error",
	},{
		.option = "suid",
		.desc = "Honor set-user-ID and set-group-ID bits",
	},{
		.option = "nosuid",
		.desc = "Ignore set-user-ID and set-group-ID bits",
	},{
		.option = "users",
		.desc = "Allow arbitrary users to mount the filesystem",
	},
};

static struct form_option *
ops_table(int *count,const char *match,int *defidx,char ***selarray,int *selections,
		const struct form_option *flags,unsigned fcount){
	struct form_option *fo = NULL;
	int z = 0;

	*count = 0;
	*defidx = -1;
	if((*count = fcount) == 0){
		goto err;
	}
	if((fo = malloc(sizeof(*fo) * *count)) == NULL){
		goto err;
	}
	while(z < *count){
		const char *key = flags[z].option;

		if((fo[z].desc = strdup(flags[z].desc)) == NULL){
			goto err;
		}
		if((fo[z].option = strdup(key)) == NULL){
			free(fo[z].desc);
			goto err;
		}
		if(match && strcmp(match,fo[z].option) == 0){
			int zz;

			*defidx = z;
			for(zz = 0 ; selections && zz < *selections ; ++zz){
				if(strcmp(key,(*selarray)[zz]) == 0){
					free((*selarray)[zz]);
					(*selarray)[zz] = NULL;
					if(zz < *selections - 1){
						memmove(&(*selarray)[zz],&(*selarray)[zz + 1],sizeof(**selarray) * (*selections - 1 - zz));
					}
					--*selections;
					zz = -1;
					break;
				}
			}
			if(zz >= *selections){
				typeof(*selarray) tmp;

				if((tmp = realloc(*selarray,sizeof(*selarray) * (*selections + 1))) == NULL){
					free(fo[zz].option);
					free(fo[zz].desc);
					goto err;
				}
				*selarray = tmp;
				(*selarray)[*selections] = strdup(match);
				++*selections;
			}
		}
		++z;
	}
	*defidx = (*defidx + 1) % *count;
	return fo;

err:
	while(z--){
		free(fo[z].option);
		free(fo[z].desc);
	}
	free(fo);
	return NULL;
}

/*static char *
join_selarray(char * const *selarray,int selections,int sep){
	char *news = NULL,*tmp;
	size_t s = 0;
	int z;

	for(z = 0 ; z < selections ; ++z){
		if((tmp = realloc(news,s + strlen(selarray[z]) + !!s + 1)) == NULL){
			free(news);
			return NULL;
		}
		if(s){
			tmp[s] = sep;
			++s;
		}
		strcpy(tmp + s,selarray[z]);
		s += strlen(selarray[z]);
		news = tmp;
	}
	return news;
}*/

static char forming_targ[PATH_MAX + 1];

static void
mountop_callback(const char *op,char **selarray,int selections,int scrollidx){
	struct form_option *ops_agg;
	int opcount,defidx;
	blockobj *b;

	if(op == NULL){
		locked_diag("User cancelled target operation");
		return;
	}
	if(!growlight_target){
		locked_diag("No target is set");
		return;
	}
	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Must select a filesystem to mount");
		return;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(strcmp(op,"") == 0){
		unsigned mntos = 0;

		while(selections--){
			mntos |= flag_for_mountop(selarray[selections]);
		}
		if(blockobj_unpartitionedp(b)){
			mmount(b->d,forming_targ,mntos,NULL);
			redraw_adapter(current_adapter);
		}else if(blockobj_emptyp(b)){
			locked_diag("%s is not a partition, aborting.\n",b->zone->p->name);
		}else{
			mmount(b->zone->p,forming_targ,mntos,NULL);
			redraw_adapter(current_adapter);
		}
		return;
	}
	ops_agg = NULL;
	opcount = 0;
	defidx = 1;
	if((ops_agg = ops_table(&opcount,op,&defidx,&selarray,&selections,
			common_fsops,sizeof(common_fsops) / sizeof(*common_fsops))) == NULL){
		// FIXME free
		return;
	}
	raise_checkform("set mount options",mountop_callback,ops_agg,
		opcount,defidx,selarray,selections,MOUNTOPS_TEXT,scrollidx);
}

// -------------------------------------------------------------------------
// - target mountpoint form, for mapping within the target
// -------------------------------------------------------------------------
static void
targpoint_callback(const char *path){
	struct form_option *ops_agg;
	int scrollidx = 0;
	blockobj *b;

	if(path == NULL){
		locked_diag("User cancelled target operation");
		return;
	}
	if(!growlight_target){
		locked_diag("No target is set");
		return;
	}
	if((unsigned)snprintf(forming_targ,sizeof(forming_targ),"%s%s",growlight_target,path) >= sizeof(forming_targ)){
		locked_diag("Bad mountpoint: %s",path);
		return;
	}
	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Must select a filesystem to mount");
		return;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(blockobj_emptyp(b)){
		locked_diag("%s is not a partition, aborting.\n",b->zone->p->name);
		return;
	}else{
		int opcount = 0,defidx = 0;
		ops_agg = NULL;
		char **selarray = NULL;
		int selections = 0;

		if((ops_agg = ops_table(&opcount,NULL,&defidx,&selarray,&selections,
				common_fsops,sizeof(common_fsops) / sizeof(*common_fsops))) == NULL){
			// FIXME free
			return;
		}
		raise_checkform("set mount options",mountop_callback,ops_agg,
			opcount,defidx,selarray,selections,MOUNTOPS_TEXT,scrollidx);
		return;
	}
	locked_diag("I'm confused. Aborting.\n");
}

// -------------------------------------------------------------------------
// - filesystem type form, for new filesystem creation
// -------------------------------------------------------------------------
static struct form_option *
fs_table(int *count,const char *match,int *defidx){
	struct form_option *fo = NULL,*tmp;
	pttable_type *types;
	int z;

	*defidx = -1;
	if((types = get_fs_types(count)) == NULL){
		return NULL;
	}
	for(z = 0 ; z < *count ; ++z){
		char *key,*desc;

		if((key = strdup(types[z].name)) == NULL){
			goto err;
		}
		if(match){
			if(strcmp(key,match) == 0){
				*defidx = z;
			}
		}else{
			if(fstype_default_p(key)){
				*defidx = z;
			}
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

static char *pending_fstype;

static void fs_callback(const char *);

static void
destroy_fs_forms(void){
	free(pending_fstype);
	pending_fstype = NULL;
}

void kill_splash(struct panel_state *ps){
	if(splash == ps){
		splash = NULL;
	}
	hide_panel_locked(ps);
	free(ps);
	if(actform == NULL){
		setup_colors();
	}else{
		/*assert(top_panel(actform->p) != ERR);
		if(actform->extext){
			assert(top_panel(actform->extext->p) != ERR);
		}*/
	}
}

static int
fs_do_internal(device *d,const char *fst,const char *name){
	struct panel_state *ps;
	int r;

	if(!mkfs_safe_p(d)){
		return -1;
	}
	ps = show_splash(L"Creating filesystem...");
	r = make_filesystem(d,fst,name);
	if(ps){
		kill_splash(ps);
	}
	return r;
}

static void
fs_do(const char *name){
	blockobj *b = get_selected_blockobj();
	int r = -1;

	if(b == NULL){
		locked_diag("A block device must be selected");
		return;
	}
	if(!current_adapter || !(b = current_adapter->selected)){
		locked_diag("Lost selection while targeting");
		destroy_fs_forms();
		return;
	}
	if(selected_unloadedp()){
		locked_diag("%s is unloaded, aborting.\n",b->d->name);
	}else if(selected_unpartitionedp()){
		r = fs_do_internal(b->d,pending_fstype,name);
	}else if(selected_partitionp()){
		r = fs_do_internal(b->zone->p,pending_fstype,name);
	}else if(selected_emptyp()){
		locked_diag("Cannot make filesystems in empty space");
	}
	if(r == 0){
		locked_diag("Successfully created %s filesystem",pending_fstype);
	}
	redraw_adapter(current_adapter);
	destroy_fs_forms();
}

static void
fs_named_callback(const char *name){
	if(name == NULL){
		struct form_option *ops_fs;
		int opcount,defidx;

		if((ops_fs = fs_table(&opcount,pending_fstype,&defidx)) == NULL){
			destroy_fs_forms();
			return;
		}
		raise_form("select a filesystem type",fs_callback,ops_fs,
				opcount,defidx,FSTYPE_TEXT);
		return;
	}
	fs_do(name);
}

static void
fs_callback(const char *fs){
	if(fs == NULL){ // user cancelled
		locked_diag("Filesystem creation cancelled by the user");
		return;
	}
	free(pending_fstype);
	pending_fstype = NULL;
	if((pending_fstype = strdup(fs)) == NULL){
		destroy_fs_forms();
		return;
	}
	if(fstype_named_p(fs) == 0){
		fs_do(NULL);
		return;
	}
	// FIXME come up with a good default
	raise_str_form("enter filesystem name",fs_named_callback,NULL,FSNAME_TEXT);
}

// -------------------------------------------------------------------------
// -- end filesystem type form
// -------------------------------------------------------------------------

// A NULL return is only an error if *count is set to -1. If *count is set to
// 0, it simply means this partition table type doesn't have type tags.
static struct form_option *
ptype_table(const device *d,int *count,int match,int *defidx){
	struct form_option *fo = NULL,*tmp;
	const char *pttable;
	const ptype *pt;

	assert(d);
	if(d->layout == LAYOUT_NONE){
		pttable = d->blkdev.pttable;
	}else if(d->layout == LAYOUT_MDADM){
		pttable = d->mddev.pttable;
	}else{
		locked_diag("Can't partition this type of device");
		return NULL;
	}
	*count = 0;
	for(pt = ptypes ; pt->name ; ++pt){
		const size_t KEYSIZE = 5; // 4 hex digit code
		char *key,*desc;

		if(!ptype_supported(pttable,pt)){
			continue;
		}
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
		if(match == -1){
			if(ptype_default_p(pt->code)){
				*defidx = *count;
			}
		}else if(match == pt->code){
			*defidx = *count;
		}
		++*count;
	}
	return fo;

err:
	while(*count--){
		free(fo[*count].option);
		free(fo[*count].desc);
	}
	*count = -1;
	free(fo);
	return NULL;
}

// -------------------------------------------------------------------------
// - partition name form, for new partition creation
// -------------------------------------------------------------------------
static void ptype_callback(const char *);
static void psectors_callback(const char *);

static unsigned long pending_ptype; // set by partition type callback
static uintmax_t pending_fsect,pending_lsect; // set by partition spec callback
static char *pending_spec;		// set by spec callback; heap-allocated

// Call on exit from the new partition form path
static void
cleanup_new_partition(void){
	free(pending_spec);
	pending_spec = NULL;
	pending_fsect = pending_lsect = 0;
	pending_ptype = 0;
}

// Verify that the current selection is a place suitable for partition creation.
// Returns the selected blockobj, or NULL.
static blockobj *
partition_base_p(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Partition creation requires selection of a block device");
		return NULL;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return NULL;
	}
	if(selected_unpartitionedp()){
		locked_diag("Partition creation requires a partition table");
		return NULL;
	}
	if(b->zone->rep == REP_METADATA){
		locked_diag("Remove partition table on %s to reclaim metadata",b->d->name);
		return NULL;
	}
	if(b->zone->p){
		locked_diag("Partition %s exists; remove it first",b->zone->p->name);
		return NULL;
	}
	return b;
}

static void
ptype_name_callback(const char *name){
	struct panel_state *sps;
	const char *n;
	wchar_t *wstr;
	mbstate_t ps;
	blockobj *b;
	size_t wcs;
	int r;

	if(name == NULL){ // go back to partition spec
		raise_str_form("enter partition spec",psectors_callback,
				pending_spec,PSPEC_TEXT);
		return;
	}
	if((b = partition_base_p()) == NULL){
		return;
	}
	n = name;
	memset(&ps,0,sizeof(ps));
	if((wcs = mbsrtowcs(NULL,&n,0,&ps)) == (size_t)-1){
		locked_diag("Couldn't interpret multibyte '%s'",name);
		cleanup_new_partition();
		return;
	}
	if((wstr = malloc(sizeof(*wstr) * (wcs + 1))) == NULL){
		locked_diag("Couldn't allocate wide string");
		cleanup_new_partition();
		return;
	}
	n = name;
	memset(&ps,0,sizeof(ps));
	if(mbsrtowcs(wstr,&n,wcs + 1,&ps) != wcs){
		locked_diag("Error converting multibyte '%s'",name);
		cleanup_new_partition();
		return;
	}
	sps = show_splash(L"Creating partition...");
	// Cannot reference b further until we've had a callback
	r = add_partition(b->d,wstr,pending_fsect,pending_lsect,pending_ptype);
	if(sps){
		kill_splash(sps);
	}
	free(wstr);
	cleanup_new_partition();
	if(!r){
		if(strcmp(name,"")){
			locked_diag("Created new partition %s on %s\n",name,b->d->name);
		}else{
			locked_diag("Created new unnamed partition on %s\n",b->d->name);
		}
	}
}

static int
lex_part_spec(const char *psects,zobj *z,size_t sectsize,
			uintmax_t *fsect,uintmax_t *lsect){
	unsigned long long ull;
	const char *col,*pct;
	char *el;

	// If we have a percent, it must be the last character, and there may
	// not be a colon present, and the value must be between 1 and 100,
	// inclusive, and the value must be an integer.
	if( (pct = strchr(psects,'%')) ){
		unsigned long ul;

		if(pct[1]){
			return -1;
		}
		if(pct - psects > 3){
			return -1;
		}
		if(pct == psects){
			return -1;
		}
		if((ul = strtoul(psects,&el,10)) > 100){
			return -1;
		}
		*fsect = z->fsector;
		*lsect = ((z->lsector - z->fsector) * ul) / 100 + *fsect;
		return 0;
	}else if( (col = strchr(psects,':')) ){
		unsigned long long ull2;

		if(*psects == '-'){ // reject negative numbers
			locked_diag("Not a number: %s",psects);
			return -1;
		}
		if((ull = strtoull(psects,&el,0)) == ULLONG_MAX){
			locked_diag("Not a number: %s",psects);
			return -1;
		}
		if(el != col){
			locked_diag("Invalid delimiter: %s",psects);
			return -1;
		}
		++el;
		++col;
		if(*col == '-'){ // reject negative numbers
			locked_diag("Not a number: %s",psects);
			return -1;
		}
		if((ull2 = strtoull(col,&el,0)) == ULLONG_MAX){
			locked_diag("Not a number: %s",col);
			return -1;
		}
		if(*el){
			locked_diag("Not a number: %s",col);
			return -1;
		}
		*fsect = ull;
		*lsect = ull2;
		return 0;
	}
	while(isspace(*psects)){
		++psects;
	}
	if(*psects == '-'){ // reject negative numbers
		locked_diag("Not a number: %s",psects);
		return -1;
	}
	if((ull = strtoull(psects,&el,0)) == ULLONG_MAX && errno == ERANGE){
		locked_diag("Not a number: %s",psects);
		return -1;
	}
	if(el == psects){
		return -1;
	}
	if(*el){
		if(el[1]){
			return -1;
		}
		switch(*el){
			case 'E': case 'e':
				ull *= 1024llu * 1024 * 1024 * 1024 * 1024 * 1024; break;
			case 'P': case 'p':
				ull *= 1024llu * 1024 * 1024 * 1024 * 1024; break;
			case 'T': case 't':
				ull *= 1024llu * 1024 * 1024 * 1024; break;
			case 'G': case 'g':
				ull *= 1024llu * 1024 * 1024; break;
			case 'M': case 'm':
				ull *= 1024llu * 1024; break;
			case 'K': case 'k':
				ull *= 1024llu; break;
			default:
			return -1;
		}
	}
	if(ull % sectsize){
		locked_diag("%llu is not a multiple of %zu",ull,sectsize);
		return -1;
	}
	ull /= sectsize;
	if(ull > (z->lsector - z->fsector + 1)){
		locked_diag("There are only %ju sectors available\n",z->lsector - z->fsector);
		return -1;
	}
	*fsect = z->fsector;
	*lsect = z->fsector + ull - 1;
	return 0;
}

static void
psectors_callback(const char *psects){
	uintmax_t fsect,lsect;
	struct panel_state *ps;
	blockobj *b;
	int r;

	if((b = partition_base_p()) == NULL){
		return;
	}
	if(psects == NULL){ // go back to partition type
		struct form_option *ops_ptype;
		int opcount,defidx;

		if((ops_ptype = ptype_table(b->d,&opcount,pending_ptype,&defidx)) == NULL){
			if(opcount == 0){
				raise_str_form("enter partition spec",psectors_callback,
						pending_spec ? pending_spec : "100%",PSPEC_TEXT);
				return;
			}
			cleanup_new_partition();
			return;
		}
		raise_form("select a partition type",ptype_callback,ops_ptype,
				opcount,defidx,PARTTYPE_TEXT);
		return;
	}
	pending_spec = strdup(psects);
	if(lex_part_spec(psects,b->zone,b->d->logsec,&fsect,&lsect)){
		locked_diag("Not a valid partition spec: \"%s\"\n",psects);
		raise_str_form("enter partition spec",psectors_callback,
				psects,PSPEC_TEXT);
		return;
	}
	if(partitions_named_p(b->d)){
		pending_spec = strdup(psects);
		pending_fsect = fsect;
		pending_lsect = lsect;
		raise_str_form("enter partition name",ptype_name_callback,
				NULL,PNAME_TEXT);
		return;
	}
	ps = show_splash(L"Creating partition...");
	r = add_partition(b->d,NULL,fsect,lsect,pending_ptype);
	if(ps){
		kill_splash(ps);
	}
	cleanup_new_partition();
	if(!r){
		locked_diag("Created new partition on %s\n",b->d->name);
	}
}

// -------------------------------------------------------------------------
// - partition type form, for new partition creation
// -------------------------------------------------------------------------
static void
ptype_callback(const char *pty){
	unsigned long pt;
	char *pend;

	if(pty == NULL){ // user cancelled
		locked_diag("Partition creation cancelled by the user");
		cleanup_new_partition();
		return;
	}
	if(((pt = strtoul(pty,&pend,16)) == ULONG_MAX && errno == ERANGE) || *pend){
		locked_diag("Bad partition type selection: %s",pty);
		cleanup_new_partition();
		return;
	}
	pending_ptype = pt;
	raise_str_form("enter partition spec",psectors_callback,
			pending_spec ? pending_spec : "100%",PSPEC_TEXT);
}

static void
new_partition(void){
	struct form_option *ops_ptype;
	blockobj *b;
	int opcount;
	int defidx;

	if((b = partition_base_p()) == NULL){
		return;
	}
	if((ops_ptype = ptype_table(b->d,&opcount,-1,&defidx)) == NULL){
		if(opcount == 0){
			raise_str_form("enter partition spec",psectors_callback,
					pending_spec ? pending_spec : "100%",PSPEC_TEXT);
			return;
		}
		return;
	}
	raise_form("select a partition type",ptype_callback,ops_ptype,opcount,
			defidx,PARTTYPE_TEXT);
}

// -------------------------------------------------------------------------
// -- end partition type form
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// - partition tabletype form, for new partition table creation
// -------------------------------------------------------------------------
static inline int
partition_table_makeablep(const blockobj *b){
	if(!b){
		locked_diag("Lost selection while choosing table type");
		return 0;
	}
	if(blockobj_unloadedp(b)){
		locked_diag("Media is not loaded on %s",b->d->name);
		return 0;
	}
	if(!selected_unpartitionedp()){
		locked_diag("Partition table exists on %s",b->d->name);
		return 0;
	}
	if(b->d->layout != LAYOUT_NONE){
		locked_diag("Will not create partition table on %s",b->d->name);
		return 0;
	}
	if(b->d->mnttype){
		locked_diag("Filesystem signature exists on %s",b->d->name);
		return 0;
	}
	return 1;
}

static void
pttype_callback(const char *pttype){
	blockobj *b;

	if(pttype == NULL){ // user cancelled
		locked_diag("Partition table creation cancelled by the user");
		return;
	}
	if(!current_adapter){
		locked_diag("Lost selection while choosing table type");
		return;
	}
	b = current_adapter->selected;
	if(!partition_table_makeablep(b)){
		return;
	}
	if(make_partition_table(b->d,pttype) == 0){
		locked_diag("Made %s table on %s",pttype,b->d->name);
	}
}

// -------------------------------------------------------------------------
// -- end partition table type form
// -------------------------------------------------------------------------
static int
confirm_operation(const char *op,void (*confirmcb)(const char *)){
	struct form_option *ops_confirm;

	if((ops_confirm = malloc(sizeof(*ops_confirm) * 2)) == NULL){
		return -1;
	}
	ops_confirm[0].option = strdup("abort");
	ops_confirm[0].desc = strdup("do not perform the operation");
	ops_confirm[1].option = strdup("do it");
	ops_confirm[1].desc = strdup(op);
	// FIXME check values
	raise_form("confirm operation",confirmcb,ops_confirm,2,0,
			"Please confirm the request. You will not be able to undo this action.");
	return 0;
}
// -------------------------------------------------------------------------
// -- end form API
// -------------------------------------------------------------------------

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
	locked_diag("by nick black <nickblack@linux.com>");
	draw_main_window(w);
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
		assert( (ret->win = newwin(l,PAD_COLS(cols),scrline,0)) );
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
		assert(move_panel(rb->panel,targ,0) != ERR);
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
		if(targ < 0){
			nlines = rr + targ;
			targ = 0;
		}else{
			nlines = adapter_lines_bounded(as,rows - targ + 1);
		}
	}
	if(nlines < 1){
		assert(hide_panel(rb->panel) != ERR);
		return;
	}else if(nlines > rr){
		assert(move_panel(rb->panel,targ,0) == OK);
		assert(wresize(rb->win,nlines,PAD_COLS(cols)) == OK);
	}else if(nlines < rr){
		assert(wresize(rb->win,nlines,PAD_COLS(cols)) == OK);
		assert(move_panel(rb->panel,targ,0) == OK);
	}else{
		assert(move_panel(rb->panel,targ,0) == OK);
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
	if(d->mnttype){
		char buf[BPREFIXSTRLEN + 1];

		wattroff(hw,A_BOLD);
		mvwprintw(hw,row,START_COL,BPREFIXFMT "%c ",
				d->mntsize ? bprefix(d->mntsize,1,buf,sizeof(buf),1) : "",
				d->mntsize ? 'B' : ' ');
		wattron(hw,A_BOLD);
		wprintw(hw,"%s%s",d->label ? "" : "unlabeled ",d->mnttype);
		if(d->label){
			wattroff(hw,A_BOLD);
			wprintw(hw," %lc%s%lc",L'“',d->label,L'”');
			wattron(hw,A_BOLD);
		}
		wprintw(hw,"%s",d->mnt.count ? " at " : "");
		wattroff(hw,A_BOLD);
		wprintw(hw,"%s",d->mnt.count ? d->mnt.list[0] : "");
		wattron(hw,A_BOLD);
	}
}

// One must not call diag() from any function called by update_details(), or
// else you will get one of a deadlock or a stack overflow due to corecursion.
static int
update_details(WINDOW *hw){
	const controller *c = get_current_adapter();
	char buf[BPREFIXSTRLEN + 1];
	const char *pttype;
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
		assert(mvwprintw(hw,2,START_COL,"Firmware: ") != ERR);
		wattroff(hw,A_BOLD);
		waddstr(hw,c->fwver ? c->fwver : "Unknown");
		wattron(hw,A_BOLD);
		waddstr(hw," BIOS: ");
		wattroff(hw,A_BOLD);
		waddstr(hw,c->biosver ? c->biosver : "Unknown");
		wattron(hw,A_BOLD);
		waddstr(hw," Load: ");
		qprefix(c->demand,1,buf,sizeof(buf),1);
		wattroff(hw,A_BOLD);
		waddstr(hw,buf);
		waddstr(hw,"bps");
		wattron(hw,A_BOLD);
	}
	if((b = current_adapter->selected) == NULL){
		return 0;
	}
	d = b->d;
	if(d->layout == LAYOUT_NONE){
		const char *sn = d->blkdev.serial;

		mvwprintw(hw, 3, START_COL, "%s: ", d->name);
		wattroff(hw, A_BOLD);
		waddstr(hw, d->model ? d->model : "n/a");
		waddstr(hw, d->revision ? d->revision : "");
		wattron(hw, A_BOLD);
		wprintw(hw, " (%sB) S/N: ", bprefix(d->size, 1, buf, sizeof(buf), 1));
		wattroff(hw, A_BOLD);
		waddstr(hw, sn ? sn : "n/a");
		wattron(hw, A_BOLD);
		if(getmaxx(hw) - getcurx(hw) > 13){
			wprintw(hw," WC%lc WRV%lc RO%lc",
					d->blkdev.wcache ? L'+' : L'-',
					d->blkdev.rwverify == RWVERIFY_SUPPORTED_ON ? L'+' :
					d->blkdev.rwverify == RWVERIFY_SUPPORTED_OFF ? L'-' : L'x',
					d->roflag ? L'+' : L'-');
		}
		assert(d->physsec <= 4096);
		mvwprintw(hw,4,START_COL,"Sectors: ");
		wattroff(hw,A_BOLD);
		wprintw(hw,"%ju ",d->size / (d->logsec ? d->logsec : 1));
		wattron(hw,A_BOLD);
		wprintw(hw,"(");
		wattroff(hw,A_BOLD);
		wprintw(hw,"%uB ",d->logsec);
		wattron(hw,A_BOLD);
		wprintw(hw,"logical / ");
		wattroff(hw,A_BOLD);
		wprintw(hw,"%uB ",d->physsec);
		wattron(hw,A_BOLD);
		wprintw(hw,"physical) %s",
		transport_str(d->blkdev.transport));
		if(transport_bw(d->blkdev.transport)){
			uintmax_t transbw = transport_bw(d->blkdev.transport);
			wprintw(hw," (");
			wattroff(hw,A_BOLD);
			// FIXME throws -Wformat-truncation on gcc9
			wprintw(hw, "%sbps", qprefix(transbw, 1, buf, sizeof(buf), 1));
			wattron(hw,A_BOLD);
			wprintw(hw,")");
		}
	}else{
		mvwprintw(hw,3,START_COL,"%s: %s %s (%s) RO%lc",d->name,
					d->model ? d->model : "n/a",
					d->revision ? d->revision : "n/a",
					bprefix(d->size,1,buf,sizeof(buf),1),
					d->roflag ? L'+' : L'-');
		if(d->layout == LAYOUT_MDADM){
			wprintw(hw," Stride: ");
			wattroff(hw,A_BOLD);
			if(d->mddev.stride == 0){
				waddstr(hw,"n/a");
			}else{
				wprintw(hw,"%sB",bprefix(d->mddev.stride,1,buf,sizeof(buf),1));
			}
			wattron(hw,A_BOLD);
			wprintw(hw," SWidth: ");
			wattroff(hw,A_BOLD);
			if(d->mddev.swidth == 0){
				waddstr(hw,"n/a");
			}else{
				wprintw(hw,"%u",d->mddev.swidth);
			}
			wattron(hw,A_BOLD);
		}
		assert(d->physsec <= 4096);
		mvwprintw(hw,4,START_COL,"Sectors: ");
		wattroff(hw,A_BOLD);
		wprintw(hw,"%ju ",d->size / (d->logsec ? d->logsec : 1));
		wattron(hw,A_BOLD);
		wprintw(hw,"(");
		wattroff(hw,A_BOLD);
		wprintw(hw,"%uB ",d->logsec);
		wattron(hw,A_BOLD);
		wprintw(hw,"logical / ");
		wattroff(hw,A_BOLD);
		wprintw(hw,"%uB ",d->physsec);
		wattron(hw,A_BOLD);
		wprintw(hw,"physical)");
	}
	mvwprintw(hw,5,START_COL,"Partitioning: ");
	wattroff(hw,A_BOLD);
	pttype = (d->layout == LAYOUT_NONE ? d->blkdev.pttable ? d->blkdev.pttable : "none" :
			d->layout == LAYOUT_MDADM ? d->mddev.pttable ? d->mddev.pttable : "none" :
			d->layout == LAYOUT_DM ? d->dmdev.pttable ? d->dmdev.pttable : "none" :
			"n/a");
	wprintw(hw,"%s",pttype);
	wattron(hw,A_BOLD);
	waddstr(hw," I/O scheduler: ");
	wattroff(hw,A_BOLD);
	waddstr(hw,d->sched ? d->sched : "custom");
	wattron(hw,A_BOLD);
	if(blockobj_unloadedp(b)){
		mvwprintw(hw,6,START_COL,"Media is not loaded");
		return 0;
	}
	if(blockobj_unpartitionedp(b)){
		char ubuf[BPREFIXSTRLEN + 1];

		wattroff(hw,A_BOLD);
		mvwprintw(hw,6,START_COL,BPREFIXFMT "B ",
				bprefix(d->size,1,ubuf,sizeof(ubuf),1));
		wattron(hw,A_BOLD);
		wprintw(hw,"%s","unpartitioned media");
		detail_fs(hw,b->d,7);
		return 0;
	}
	if(b->zone){
		char align[BPREFIXSTRLEN + 1];
		char zbuf[BPREFIXSTRLEN + 1];

		if(b->zone->p){
			assert(b->zone->p->layout == LAYOUT_PARTITION);
			bprefix(b->zone->p->partdev.alignment,1,align,sizeof(align),1);
			// FIXME limit length!
			wattroff(hw,A_BOLD);
			mvwprintw(hw,6,START_COL,BPREFIXFMT "B ",
					bprefix(d->logsec * (b->zone->lsector - b->zone->fsector + 1),1,zbuf,sizeof(zbuf),1));
			wattron(hw,A_BOLD);
			wprintw(hw,"P%lc%lc ",subscript((b->zone->p->partdev.pnumber % 100 / 10)),
					subscript((b->zone->p->partdev.pnumber % 10)));
			wattroff(hw,A_BOLD);
			wprintw(hw,"%ju",b->zone->fsector);
			wattron(hw,A_BOLD);
			wprintw(hw,"→");
			wattroff(hw,A_BOLD);
			wprintw(hw,"%ju ",b->zone->lsector);
			wattron(hw,A_BOLD);
			waddstr(hw,b->zone->p->name);
			wattroff(hw,A_BOLD);
			if(b->zone->p->partdev.pname){
				wprintw(hw," “%ls” ",b->zone->p->partdev.pname);
			}else{
				wprintw(hw," (%s) ","unnamed");
			}
			wattron(hw,A_BOLD);
                        if(getcurx(hw) <= cols - 2 - 4){
				wprintw(hw,"%04x", get_code_specific(pttype, b->zone->p->partdev.ptype));
			}
			if(getcurx(hw) <= cols - 2 - 11){
				wprintw(hw, " %sB align", align);
			}
			detail_fs(hw,b->zone->p,7);
		}else{
			// FIXME print alignment for unpartitioned space as well,
			// but not until we implement zones in core (bug 252)
			// or we'll need recreate alignment() etc here
			wattroff(hw,A_BOLD);
			mvwprintw(hw,6,START_COL,BPREFIXFMT "B ",
					bprefix(d->logsec * (b->zone->lsector - b->zone->fsector + 1),1,zbuf,sizeof(zbuf),1));
			wattron(hw,A_BOLD);
			wattroff(hw,A_BOLD);
			wprintw(hw,"%ju",b->zone->fsector);
			wattron(hw,A_BOLD);
			wprintw(hw,"→");
			wattroff(hw,A_BOLD);
			wprintw(hw,"%ju ",b->zone->lsector);
			wattron(hw,A_BOLD);
			wprintw(hw,"%s ",b->zone->rep == REP_METADATA ?
					"partition table metadata" : "unpartitioned space");
		}
	}
	return 0;
}

// When this text is being displayed, the help window is the active window.
// Thus we refer to other window commands as "viewing", while 'H' here is
// described as "toggling". When other windows come up, they list their
// own command as "toggling." We want to avoid having to scroll the help
// synopsis, so keep it under 22 lines (25 lines on an ANSI standard terminal,
// minus two for the top/bottom screen border, minus one for mandatory
// window top padding).
static const wchar_t *helps[] = {
	L"'q': quit                     ctrl+'L': redraw the screen",
	L"'e': view environment details 'H': toggle this help display",
	L"'v': view selection details   'D': view recent diagnostics",
	L"'E': view mounts / targets    'z': modify aggregate",
	L"'A': create aggregate         'Z': destroy aggregate",
	L"'-': collapse adapter         '+': expand adapter",
	L"'k'/'↑': navigate up          'j'/'↓': navigate down",
	L"'⇞PageUp': previous adapter   ⇟PageDown': next adapter",
	L"'/': search                   'p': configure loop device",
	NULL
};

static const wchar_t *helps_block[] = {
	L"'h'/'←': navigate left        'l'/'→': navigate right",
	L"'m': make partition table     'r': remove partition table",
	L"'W': wipe master boot record  'B': bad blocks check",
	L"'n': new partition            'd': delete partition",
	L"'s': set partition attributes 'M': make filesystem/swap",
	L"'F': fsck filesystem          'w': wipe filesystem",
	L"'U': set filesystem UUID      'L': set filesystem label/name",
	L"'o': mount filesystem/swapon  'O': unmount filesystem/swapoff",
	NULL
};

static const wchar_t *helps_target[] = {
	L"'t': mount target             'T': unmount target",
	L"'i': enter target mode        'I': leave target mode",
	L"'*' finalize UEFI / '#' finalize BIOS / '@' finalize fstab",
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
helpstrs(WINDOW *hw){
	const wchar_t *hs;
	int z,rows,cols;
	int row = 1;

	rows = getmaxy(hw);
	cols = getmaxx(hw);
	assert(wattrset(hw,SUBDISPLAY_ATTR) == OK);
	for(z = 0 ; (hs = helps[z]) && z < rows ; ++z){
		mvwhline(hw,row + z,START_COL,' ',cols - 2);
		assert(mvwaddwstr(hw,row + z,START_COL,hs) != ERR);
	}
	row += z;
	if(!current_adapter || !current_adapter->selected){
		assert(wattrset(hw,SUBDISPLAY_INVAL_ATTR) == OK);
	}else{
		assert(wattrset(hw,SUBDISPLAY_ATTR) == OK);
	}
	for(z = 0 ; (hs = helps_block[z]) && z < rows ; ++z){
		mvwhline(hw,row + z,START_COL,' ',cols - 2);
		assert(mvwaddwstr(hw,row + z,START_COL,hs) != ERR);
	}
	row += z;
	if(!target_mode_p()){
		assert(wattrset(hw,SUBDISPLAY_INVAL_ATTR) == OK);
	}else{
		assert(wattrset(hw,SUBDISPLAY_ATTR) == OK);
	}
	for(z = 0 ; (hs = helps_target[z]) && z < rows ; ++z){
		mvwhline(hw,row + z,START_COL,' ',cols - 2);
		assert(mvwaddwstr(hw,row + z,START_COL,hs) != ERR);
	}
	return OK;
}

static inline void
lock_ncurses(void){
	lock_growlight();
	assert(pthread_mutex_lock(&bfl) == 0);
}

static inline void
unlock_ncurses(void){
	update_details_cond(details.p);
	update_help_cond(help.p);
	update_map_cond(maps.p);
	screen_update();
	assert(pthread_mutex_unlock(&bfl) == 0);
	unlock_growlight();
}	

// Used in growlight callbacks, since the growlight lock will already be held
// in any such case.
static inline void
lock_ncurses_growlight(void){
	assert(pthread_mutex_lock(&bfl) == 0);
}

static inline void
unlock_ncurses_growlight(void){
	update_details_cond(details.p);
	update_help_cond(help.p);
	update_map_cond(maps.p);
	screen_update();
	assert(pthread_mutex_unlock(&bfl) == 0);
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

	if(delta >= 0){
		return;
	}
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
			move_panel(rb->panel,rb->scrline,0);
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
			int delta;

			if(rb->scrline - 1 + subrows > nlines){
				delta = subrows - nlines;
			}else{
				delta = -1;
			}
			rb->scrline += delta;
			push_adapters_above(rb,rows,cols,delta);
			move_panel(rb->panel,rb->scrline,0);
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
			if(subrows > 0){
				assert(wresize(rb->win,subrows,PAD_COLS(cols)) != ERR);
				assert(replace_panel(rb->panel,rb->win) != ERR);
			}else{
				assert(werase(rb->win) == OK);
				assert(hide_panel(rb->panel) == OK);
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
				// need NULL out adapter reference FIXME
			}
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
		int maxl,newl;

		if(top_reelbox->scrline <= 2){
			return;
		}
		i = top_reelbox->as->prev;
		if(i->rb){
			return; // already visible
		}
		newl = adapter_lines_bounded(i,top_reelbox->scrline);
		maxl = top_reelbox->scrline;
		if((rb = create_reelbox(i,maxl,top_reelbox->scrline - 1 - newl,cols)) == NULL){
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
use_next_controller(WINDOW *w){
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
	// Don't redraw the old interface yet; it might have been moved/hidden
	if(current_adapter->next == NULL){
		adapterstate *is = current_adapter->as->next;

		if(is->rb == NULL){ // it's off-screen
			is->rb = create_reelbox(is,rows,(rows - 1) - adapter_lines_bounded(is,rows),cols);
			assert(is->rb);
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
			push_adapters_above(rb,rows,cols,-getmaxy(rb->win) - 1);
			if(last_reelbox){
				rb->scrline = last_reelbox->scrline + getmaxy(last_reelbox->win) + 1;
			}else{
				rb->scrline = 0;
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
			delta = getmaxy(rb->win) - adapter_lines_bounded(is,rows);
			rb->scrline = rows - (adapter_lines_bounded(is,rows) + 1);
			push_adapters_above(rb,rows,cols,delta);
			move_adapter_generic(rb,rows,cols,getbegy(rb->win) - rb->scrline);
			assert(wresize(rb->win,adapter_lines_bounded(rb->as,rows),PAD_COLS(cols)) == OK);
			assert(replace_panel(rb->panel,rb->win) != ERR);
			assert(redraw_adapter(rb) == OK);
		}else{ // ...at the top (rotate)
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
use_prev_controller(WINDOW *w){
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
			as->rb = create_reelbox(as,rows,0,cols);
			assert(as);
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
			rb->scrline = 0;
			rb->next = top_reelbox;
			top_reelbox->prev = rb;
			rb->prev = NULL;
			top_reelbox = rb;
			move_adapter_generic(rb,rows,cols,getbegy(rb->win) - rb->scrline);
		}
	}else{ // partially visible...
		adapterstate *is = current_adapter->as;

		if(rb->scrline < oldrb->scrline){ // ... at the top
			rb->scrline = 0;
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
			rb->scrline = 0;
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
}

static void
use_prev_device(void){
	reelbox *rb;
	int delta;

	if((rb = current_adapter) == NULL){
		return;
	}
	if(rb->selected == NULL || rb->selected->prev == NULL){
		if(rb->prev){
			locked_diag("Press PageUp to go to the previous adapter");
		}
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
		if(rb->next){
			locked_diag("Press PageDown to go to the next adapter");
		}
		return;
	}
	delta = device_lines(rb->as->expansion,rb->selected);
	if(rb->selline + delta + device_lines(rb->as->expansion,rb->selected->next) >= getmaxy(rb->win) - 1){
		delta = (getmaxy(rb->win) - 1 - device_lines(rb->as->expansion,rb->selected->next))
				- rb->selline;
	}
	select_adapter_dev(rb,rb->selected->next,delta);
}

static const int DIAGROWS = 14;

// Used after shutting down on error, which will clean the screen. This takes
// the last few diagnostics and prints them, unmutilated, to stderr.
static int
dump_diags(void){
	logent l[10];
	int y,r;

	y = sizeof(l) / sizeof(*l);
	if((y = get_logs(y,l)) < 0){
		return -1;
	}
	for(r = 0 ; r < y ; ++r){
		char tbuf[27];

		if(l[r].msg == NULL){
			break;
		}
		assert(ctime_r(&l[r].when,tbuf));
		fprintf(stderr,"%s %s",tbuf,l[r].msg);
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
	y = getmaxy(w) - 2;
	assert(x > 26 + START_COL * 2); // see ctime_r(3)
	if((y = get_logs(y,l)) < 0){
		return -1;
	}
	assert(wattrset(w,SUBDISPLAY_ATTR) == OK);
	for(r = 0 ; r < y ; ++r){
		char *c,tbuf[x];
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
		p = snprintf(tbuf + strlen(tbuf),tb," %-*.*s",
				(int)tb - 2,(int)tb - 2,l[r].msg);
		if(p < 0 || (unsigned)p >= tb){
			tbuf[sizeof(tbuf) / sizeof(*tbuf) - 1] = '\0';
		}
		if( (c = strchr(tbuf,'\n')) ){
			*c = '\0';
		}
		c = tbuf;
		while((c = strchr(tbuf,'\b')) || (c = strchr(tbuf,'\t'))){
			*c = ' ';
		}
		assert(mvwprintw(w,y - r,START_COL,"%-*.*s",x - 2,x - 2,tbuf) != ERR);
		free(l[r].msg);
	}
	return 0;
}

static int
display_diags(WINDOW *mainw,struct panel_state *ps){
	memset(ps,0,sizeof(*ps));
	if(new_display_panel(mainw,ps,DIAGROWS,0,L"press 'D' to dismiss diagnostics"
				,NULL,PBORDER_COLOR)){
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
	if(new_display_panel(mainw,ps,DETAILROWS,78,L"press 'v' to dismiss details"
				,NULL,PBORDER_COLOR)){
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
	helpcols += 2; // spacing + borders
	memset(ps,0,sizeof(*ps));
	if(new_display_panel(mainw,ps,helprows,helpcols,L"press 'H' to dismiss help",
			L"https://nick-black.com/dankwiki/index.php/Growlight",
			PBORDER_COLOR)){
		goto err;
	}
	if(helpstrs(panel_window(ps->p))){
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
	int z,srows,scols,cols;

	assert(wattrset(hw,SUBDISPLAY_ATTR) == OK);
	getmaxyx(stdscr,srows,scols);
	if((z = rows) >= ENVROWS){
		z = ENVROWS - 1;
	}
	cols = getmaxx(hw);
	switch(z){ // Intentional fallthroughs all the way to 0
	case (ENVROWS - 1):{
		while(z > 1){
			int c0,c1;

			mvwhline(hw,row + z,1,' ',cols - 2);
			c0 = (z - 2) * COLORSPERROW;
			c1 = c0 + (COLORSPERROW - 1);
			assert(mvwprintw(hw,row + z,col,"0x%02x%lc0x%02x: ",c0,L'–',c1) == OK);
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
	} /* intentional fallthrough */
	case 1:{
		mvwhline(hw,row + z,1,' ',cols - 2);
		assert(mvwprintw(hw,row + z,col,"Colors (pairs): %u (%u) Geom: %dx%d Palette: %s",
				COLORS,COLOR_PAIRS,srows,scols,
				can_change_color() ? "dynamic" : "fixed") != ERR);
		--z;
	} /* intentional fallthrough */
	case 0:{
		const char *lang = getenv("LANG");
		const char *term = getenv("TERM");

		mvwhline(hw,row + z,1,' ',cols - 2);
		lang = lang ? lang : "Undefined";
		assert(mvwprintw(hw,row + z,col,"LANG: %-21s TERM: %s ESCDELAY: %d",lang,term,ESCDELAY) != ERR);
		--z;
		break;
	}default:{
		return ERR;
	}
	}
	return OK;
}

static void
detail_mounts(WINDOW *w,int *row,int maxy,const device *d){
	char buf[PREFIXSTRLEN + 1],b[256];
	int cols = getmaxx(w),r;
	unsigned z;

	assert(d->mnt.count == d->mntops.count);
	for(z = 0 ; z < d->mnt.count ; ++z){
		if(*row == maxy){
			return;
		}
		if(growlight_target && !strncmp(d->mnt.list[z],growlight_target,strlen(growlight_target))){
			continue;
		}
		mvwhline(w,*row,START_COL,' ',cols - 2);
		mvwprintw(w,*row,START_COL,"%-*.*s %-5.5s %-36.36s " PREFIXFMT " %-*.*s",
				FSLABELSIZ,FSLABELSIZ,d->label ? d->label : "n/a",
				d->mnttype,
				d->uuid ? d->uuid : "n/a",
				qprefix(d->mntsize,1,buf,sizeof(buf),0),
				cols - (FSLABELSIZ + 47 + PREFIXSTRLEN),
				cols - (FSLABELSIZ + 47 + PREFIXSTRLEN),
				d->name);
		if(++*row == maxy){
			return;
		}
		wattroff(w,A_BOLD);
		if((r = snprintf(b,sizeof(b)," %s %s",d->mnt.list[z],d->mntops.list[z])) >= (int)sizeof(b)){
			b[sizeof(b) - 1] = '\0';
		}
		mvwhline(w,*row,START_COL,' ',cols - 2);
		mvwprintw(w,*row,START_COL,"%-*.*s",cols - 2,cols - 2,b);
		wattron(w,A_BOLD);
		++*row;
	}
}

static void
detail_targets(WINDOW *w,int *row,int both,const device *d){
	char buf[PREFIXSTRLEN + 1],b[256]; // FIXME uhhhh
	int cols = getmaxx(w),r;
	unsigned z;

	if(growlight_target == NULL){
		return;
	}
	for(z = 0 ; z < d->mnt.count ; ++z){
		if(strncmp(d->mnt.list[z],growlight_target,strlen(growlight_target))){
			continue;
		}
		mvwhline(w,*row,START_COL,' ',cols - 2);
		mvwprintw(w,*row,START_COL,"%-*.*s %-5.5s %-36.36s " PREFIXFMT " %-*.*s",
				FSLABELSIZ,FSLABELSIZ,d->label ? d->label : "n/a",
				d->mnttype,
				d->uuid ? d->uuid : "n/a",
				qprefix(d->mntsize,1,buf,sizeof(buf),0),
				cols - (FSLABELSIZ + 47 + PREFIXSTRLEN),
				cols - (FSLABELSIZ + 47 + PREFIXSTRLEN),
				d->name);
		++*row;
		if(!both){
			return;
		}
		wattroff(w,A_BOLD);
		if((r = snprintf(b,sizeof(b)," %s %s",d->mnt.list[z],d->mntops.list[z])) >= (int)sizeof(b)){
			b[sizeof(b) - 1] = '\0';
		}
		mvwhline(w,*row,START_COL,' ',cols - 2);
		mvwprintw(w,*row,START_COL,"%-*.*s",cols - 2,cols - 2,b);
		wattron(w,A_BOLD);
		++*row;
		break; // FIXME no space currently
	}
}

static int
map_details(WINDOW *hw){
	const controller *c;
	int y,rows,cols;
	char *fstab;

	cols = getmaxx(hw);
	rows = getmaxy(hw) - 1;
	y = 1;
	if(growlight_target){
		int blockout;

		wattrset(hw,A_BOLD|COLOR_PAIR(PHEADING_COLOR));
		mvwprintw(hw,y,1,"Operating in target mode (%s)",growlight_target);
		if( (blockout = cols - getcurx(hw) - 1) ){
			wprintw(hw,"%*.*s",blockout,blockout,"");
		}
		++y;
	}
	wattrset(hw,A_BOLD|COLOR_PAIR(FORMTEXT_COLOR));
	// First we list the target fstab, and then the targets
	// FIXME this is probably multibyte input and needs be handled as such
	if( (fstab = dump_targets()) ){
		unsigned pos,linestart;

		pos = 0;
		linestart = 0;
		while(fstab[pos]){
			if(fstab[pos] == '\n'){
				fstab[pos] = '\0';
				if(pos != linestart){
					mvwprintw(hw,y,1,"%-*.*s",cols - 2,cols - 2,fstab + linestart);
					if(++y >= rows){
						return 0;
					}
				}
				linestart = pos + 1;
			}else if(fstab[pos] == '\t'){
				fstab[pos] = ' ';
			}
			++pos;
		}
		if(pos != linestart){
			mvwprintw(hw,y,1,"%-*.*s",cols - 2,cols - 2,fstab + linestart);
			if(++y >= rows){
				return 0;
			}
		}
		free(fstab);
	}
	wattrset(hw,A_BOLD|COLOR_PAIR(SUBDISPLAY_COLOR));
	mvwhline(hw,y,1,' ',cols - 2);
	mvwprintw(hw,y,1,"%-*.*s %-5.5s %-36.36s " PREFIXFMT " %s",
			FSLABELSIZ,FSLABELSIZ,"Label",
			"Type","UUID","Bytes","Device");
	if(++y >= rows){
		return 0;
	}
	wattrset(hw,A_BOLD|COLOR_PAIR(FORMTEXT_COLOR));
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			detail_targets(hw,&y,y + 1 < rows,d);
			if(y >= rows){
				return 0;
			}
			for(p = d->parts ; p ; p = p->next){
				detail_targets(hw,&y,y + 1 < rows,p);
				if(y >= rows){
					return 0;
				}
			}
		}
	}
	// Now list the existing maps, a superset of the targets
	wattrset(hw,A_BOLD|COLOR_PAIR(SUBDISPLAY_COLOR));
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			detail_mounts(hw,&y,rows - y,d);
			if(y >= rows){
				return 0;
			}
			for(p = d->parts ; p ; p = p->next){
				detail_mounts(hw,&y,rows - y,p);
				if(y >= rows){
					return 0;
				}
			}
		}
	}

	while(y < rows){
		mvwhline(hw,y++,1,' ',cols - 2);
	}
	return 0;
}

static int
display_enviroment(WINDOW *mainw,struct panel_state *ps){
	memset(ps,0,sizeof(*ps));
	if(new_display_panel(mainw,ps,ENVROWS,78,L"press 'e' to dismiss display"
				,NULL,PBORDER_COLOR)){
		goto err;
	}
	if(env_details(panel_window(ps->p),getmaxy(panel_window(ps->p)) - 1)){
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
	if(new_display_panel(mainw,ps,rows,0,L"press 'E' to dismiss display"
				,NULL,PBORDER_COLOR)){
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
// output available above and below the current selection. If there is no
// current selection, the return value ought not be ascribed meaning. O(N) on
// the number of drives, not just those visible -- unacceptable! FIXME
static void
recompute_lines(adapterstate *is,int *before,int *after){
	blockobj *l;
	int newsel;

	*after = -1;
	*before = 0;
	newsel = 0;
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

	if(newrows == oldrows){
		return;
	}
	// Calculate the maximum new line -- we can't leave space at the top or
	// bottom, so we can't be after the true number of lines of output that
	// precede us, or before the true number that follow us.
	recompute_lines(is,&bef,&aft);
	if(bef < 0 || aft < 0){
		return;
	}
	if(bef + aft + 1 <= newrows - 3){ // should never be less than, surely
		is->rb->selline = bef + 2;
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
		// locked_diag("Already browsing [%s]", rb->as->c->ident);
		return 0;
	}
	if(rb->as->bobjs == NULL){
		return -1;
	}
	assert(rb->selline == -1);
	return select_adapter_dev(rb, rb->as->bobjs, 2);
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

static int
approvedp(const char *op){
	if(strcasecmp(op,"do it") == 0){
		return 1;
	}
	return 0;
}

static void
remove_ptable_confirm(const char *op){
	if(op && approvedp(op)){
		blockobj *b;

		if((b = get_selected_blockobj()) == NULL){
			locked_diag("Partition table removal requires selection of a block device");
			return;
		}
		if(b->d->layout != LAYOUT_NONE){
			locked_diag("%s is not partitionable",b->d->name);
			return;
		}
		if(b->d->blkdev.pttable == NULL){
			locked_diag("%s has no partition table",b->d->name);
			return;
		}
		wipe_ptable(b->d,NULL);
		return;
	}
	locked_diag("partition table removal was cancelled");
}

static void
remove_ptable(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Partition table removal requires selection of a block device");
		return;
	}
	if(b->d->layout != LAYOUT_NONE){
		locked_diag("%s is not partitionable",b->d->name);
		return;
	}
	if(blockobj_unpartitionedp(b)){
		locked_diag("%s has no partition table",b->d->name);
		return;
	}
	confirm_operation("remove the partition table",remove_ptable_confirm);
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
	if(!partition_table_makeablep(b)){
		return;
	}
	if((ops_ptype = pttype_table(&opcount)) == NULL){
		return;
	}
	raise_form("select a table type",pttype_callback,ops_ptype,opcount,-1,
			PTTYPE_TEXT);
}

static void
new_filesystem(void){
	blockobj *b = get_selected_blockobj();
	struct form_option *ops_fs;
	int opcount,defidx;

	if(b == NULL){
		locked_diag("A block device must be selected");
		return;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(selected_emptyp()){
		locked_diag("Selected region of %s is empty space",b->d->name);
		return;
	}
	if(!selected_mkfs_safe_p()){
		return;
	}
	if((ops_fs = fs_table(&opcount,NULL,&defidx)) == NULL){
		return;
	}
	raise_form("select a filesystem type",fs_callback,ops_fs,opcount,
			defidx,FSTYPE_TEXT);
	return;
}

static void
kill_filesystem_confirm(const char *op){
	if(op && approvedp(op)){
		blockobj *b;
		device *d;

		if((b = get_selected_blockobj()) == NULL){
			locked_diag("Filesystem wipe requires a selected block device");
			return;
		}
		if(selected_unloadedp()){
			locked_diag("Media is not loaded on %s",b->d->name);
			return;
		}
		if(selected_emptyp()){
			locked_diag("Filesystems cannot be wiped from empty space");
			return;
		}
		if(selected_unpartitionedp()){
			d = b->d;
		}else{
			assert(selected_partitionp());
			d = b->zone->p;
		}
		// FIXME splash screen
		if(wipe_filesystem(d)){
			return;
		}
		redraw_adapter(current_adapter);
		locked_diag("Wiped filesystem on %s",d->name);
		return;
	}
	locked_diag("filesystem wipe was cancelled");
}

static void
kill_filesystem(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Filesystem wipe requires a selected block device");
		return;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(selected_emptyp()){
		locked_diag("Filesystems cannot be wiped from empty space");
		return;
	}
	if(b->zone){
		if(b->zone->p && !b->zone->p->mnttype){
			locked_diag("No filesystem signature on %s\n",b->zone->p->name);
		}else if(b->zone->p->mnt.count){
			locked_diag("Filesystem on %s is mounted. Use 'O'/'T' to unmount.\n",b->zone->p->name);
		}else{
			confirm_operation("wipe the filesystem signature",kill_filesystem_confirm);
		}
	}else if(!b->zone){
		if(!b->d->mnttype){
			locked_diag("No filesystem signature on %s\n",b->d->name);
		}else if(b->d->mnt.count){
			locked_diag("Filesystem on %s is mounted. Use 'O'/'T' to unmount.\n",b->d->name);
		}else{
			confirm_operation("wipe the filesystem signature",kill_filesystem_confirm);
		}
	}else{
		confirm_operation("wipe the filesystem signature",kill_filesystem_confirm);
	}
}

static const struct form_option dos_flags[] = {
	{
		.option = "0x80",
		.desc = "Bootable",
	},
};

static const struct form_option gpt_flags[] = {
	{ // protects OEM partitions from being overwritten by windows
		.option = "0x0000000000000001", // 2^0, 1
		.desc = "System partition",
	},{
		.option = "0x0000000000000002", // 2^1, 2
		.desc = "Hide from EFI",
	},{
		.option = "0x0000000000000004", // 2^2, 4
		.desc = "Legacy BIOS bootable",
	},{ // readonly for EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
		.option = "0x1000000000000000", // 2^60
		.desc = "Read-only",
	},{
		.option = "0x2000000000000000", // 2^61
		.desc = "Shadow copy",
	},{ // hidden for EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
		.option = "0x4000000000000000", // 2^62
		.desc = "Hidden",
	},{ // no default drive letter for EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
		.option = "0x8000000000000000", // 2^63
		.desc = "Inhibit automounting",
	},
};

static struct form_option *
flag_table(int *count,const char *match,int *defidx,char ***selarray,int *selections,
		const struct form_option *flags,unsigned fcount){
	struct form_option *fo = NULL;
	int z = 0;

	*count = 0;
	*defidx = -1;
	if((*count = fcount) == 0){
		goto err;
	}
	if((fo = malloc(sizeof(*fo) * *count)) == NULL){
		goto err;
	}
	while(z < *count){
		const char *key = flags[z].option;

		if((fo[z].desc = strdup(flags[z].desc)) == NULL){
			goto err;
		}
		if((fo[z].option = strdup(key)) == NULL){
			free(fo[z].desc);
			goto err;
		}
		if(match && strcmp(match,fo[z].option) == 0){
			int zz;

			*defidx = z;
			for(zz = 0 ; selections && zz < *selections ; ++zz){
				if(strcmp(key,(*selarray)[zz]) == 0){
					free((*selarray)[zz]);
					(*selarray)[zz] = NULL;
					if(zz < *selections - 1){
						memmove(&(*selarray)[zz],&(*selarray)[zz + 1],sizeof(**selarray) * (*selections - 1 - zz));
					}
					--*selections;
					zz = -1;
					break;
				}
			}
			if(zz >= *selections){
				typeof(*selarray) tmp;

				if((tmp = realloc(*selarray,sizeof(*selarray) * (*selections + 1))) == NULL){
					free(fo[zz].option);
					free(fo[zz].desc);
					goto err;
				}
				*selarray = tmp;
				(*selarray)[*selections] = strdup(match);
				++*selections;
			}
		}
		++z;
	}
	*defidx = (*defidx + 1) % *count;
	return fo;

err:
	while(z--){
		free(fo[z].option);
		free(fo[z].desc);
	}
	free(fo);
	return NULL;
}

static void
do_partflag(char **selarray,int selections){
	unsigned long long flags;
	device *d;
	int z;

	if(!selected_partitionp()){
		locked_diag("Selected object is not a partition");
		return;
	}
	d = get_selected_blockobj()->zone->p;
	flags = 0;
	for(z = 0 ; z < selections ; ++z){
		char *eptr = selarray[z];
		unsigned long long ul;

		errno = 0;
		ul = strtoull(selarray[z],&eptr,16);
		assert(ul && (ul < ULLONG_MAX || errno != ERANGE) && !*eptr);
		flags |= ul;
	}
	if(partition_set_flags(d,flags)){
		return;
	}
}

static void
partflag_callback(const char *fn,char **selarray,int selections,int scrollp){
	struct form_option *flags_agg;
	int opcount,defidx;

	if(fn == NULL){
		// FIXME free selections
		return;
	}
	if(strcmp(fn,"") == 0){
		do_partflag(selarray,selections);
		// FIXME free
		return;
	}
	if((flags_agg = flag_table(&opcount,fn,&defidx,&selarray,&selections,
			gpt_flags,sizeof(gpt_flags) / sizeof(*gpt_flags))) == NULL){
		// FIXME free
		return;
	}
	raise_checkform("set GPT partition flags",partflag_callback,flags_agg,
		opcount,defidx,selarray,selections,PARTFLAG_TEXT,scrollp);
}

static void
dos_partflag_callback(const char *fn,char **selarray,int selections,int scrollp){
	struct form_option *flags_agg;
	int opcount,defidx;

	if(fn == NULL){
		// FIXME free selections
		return;
	}
	if(strcmp(fn,"") == 0){
		do_partflag(selarray,selections);
		// FIXME free
		return;
	}
	if((flags_agg = flag_table(&opcount,fn,&defidx,&selarray,&selections,
			dos_flags,sizeof(dos_flags) / sizeof(*dos_flags))) == NULL){
		// FIXME free
		return;
	}
	raise_checkform("set DOS partition flags",dos_partflag_callback,flags_agg,
		opcount,defidx,selarray,selections,PARTFLAG_TEXT,scrollp);
}

static int
flag_to_selections(uint64_t flags,char ***selarray,int *selections,
			const struct form_option *ops,unsigned opcount){
	assert(selarray && selections);
	assert(!*selarray && !*selections);
	while(opcount--){
		unsigned long long ul;
		char *eptr;

		ul = strtoull(ops[opcount].option,&eptr,16);
		assert(ul && (ul < ULLONG_MAX || errno != ERANGE) && !*eptr);
		if(flags & ul){
			typeof(*selarray) tmp;

			if((tmp = realloc(*selarray,sizeof(*selarray) * (*selections + 1))) == NULL){
				// FIXME backfree
				return -1;
			}
			*selarray = tmp;
			(*selarray)[*selections] = strdup(ops[opcount].option);
			++*selections;
		}
	}
	return 0;
}

static void
set_partition_attrs(void){
	struct form_option *flags_agg;
	char **selarray = NULL;
	int selections = 0;
	int opcount,defidx;
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Partition modification requires selection of a partition");
		return;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(!selected_partitionp()){
		locked_diag("Selected object is not a partition");
		return;
	}
	// FIXME need to initialize widget based off current flags
	if(strcmp("gpt",b->d->blkdev.pttable) == 0){
		if(flag_to_selections(b->zone->p->partdev.flags,&selarray,&selections,
					gpt_flags,sizeof(gpt_flags) / sizeof(*gpt_flags))){
			return;
		}
		if((flags_agg = flag_table(&opcount,NULL,&defidx,&selarray,&selections,
				gpt_flags,sizeof(gpt_flags) / sizeof(*gpt_flags))) == NULL){
			// FIXME free selarray
			return;
		}
		raise_checkform("set GPT partition flags",partflag_callback,flags_agg,
				opcount,defidx,selarray,selections,PARTFLAG_TEXT,0);
	}else if(strcmp("dos",b->d->blkdev.pttable) == 0 ||
		strcmp("msdos",b->d->blkdev.pttable) == 0){

		if(flag_to_selections(b->zone->p->partdev.flags,&selarray,&selections,
					dos_flags,sizeof(dos_flags) / sizeof(*dos_flags))){
			return;
		}
		if((flags_agg = flag_table(&opcount,NULL,&defidx,&selarray,&selections,
				dos_flags,sizeof(dos_flags) / sizeof(*dos_flags))) == NULL){
			return;
		}
		raise_checkform("set DOS partition flags",dos_partflag_callback,flags_agg,
				opcount,defidx,selarray,selections,PARTFLAG_TEXT,0);
	}else{
		assert(0);
	}
}

static inline int
fsck_suitable_p(const device *d){
	if(d->mnt.count){
		locked_diag("Will not fsck mounted filesystem on %s",d->name);
		return 0;
	}
	if(d->mnttype == NULL){
		locked_diag("No filesystem found on %s",d->name);
		return 0;
	}
	return 1;
}

static void
fsck_partition(void){
	blockobj *b;
	device *d;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Partition check requires selection of a partition");
		return;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(selected_emptyp()){
		locked_diag("Selected object is not a partition");
		return;
	}
	if(selected_unpartitionedp()){
		d = b->d;
	}else{
		assert(selected_unpartitionedp());
		d = b->zone->p;
	}
	if(fsck_suitable_p(d)){
		if(check_partition(d) == 0){
			locked_diag("Validated filesystem on %s",d->name);
		}
	}
}

static void
delete_partition_confirm(const char *op){
	if(op && approvedp(op)){
		blockobj *b;

		if((b = get_selected_blockobj()) == NULL){
			locked_diag("Partition deletion requires selection of a partition");
			return;
		}
		if(selected_unloadedp()){
			locked_diag("Media is not loaded on %s",b->d->name);
			return;
		}
		if(selected_unpartitionedp()){
			locked_diag("Space is already unpartitioned");
			return;
		}
		if(selected_emptyp()){
			locked_diag("Cannot remove empty space; partition it instead");
			return;
		}
		if(wipe_partition(b->zone->p)){
			return;
		}
		redraw_adapter(current_adapter);
		return;
	}
	locked_diag("partition deletion was cancelled");
}

static void
delete_partition(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Partition deletion requires selection of a partition");
		return;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(selected_unpartitionedp()){
		locked_diag("Space is already unpartitioned");
		return;
	}
	if(selected_emptyp()){
		locked_diag("Cannot remove empty space; partition it instead");
		return;
	}
	if(b->zone->p->mnttype){
		locked_diag("%s has a valid filesystem signature. Wipe it with 'w'.",b->zone->p->name);
		return;
	}
	confirm_operation("delete the partition",delete_partition_confirm);
}

static void
wipe_mbr_confirm(const char *op){
	blockobj *b;

	if(!op || !approvedp(op)){
		locked_diag("master boot record wipe was cancelled");
		return;
	}
	if((b = get_selected_blockobj()) == NULL){
		locked_diag("MBR wipe requires selection of a block device");
		return;
	}
	if(blockobj_unloadedp(b)){
		locked_diag("Media is unloaded on %s\n",b->d->name);
		return;
	}
	wipe_dosmbr(b->d);
}

static void
wipe_mbr(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("MBR wipe requires selection of a block device");
		return;
	}
	if(blockobj_unloadedp(b)){
		locked_diag("Media is unloaded on %s\n",b->d->name);
		return;
	}
	confirm_operation("wipe the mbr",wipe_mbr_confirm);
}

static void
badblock_do_internal(void){
	struct panel_state *ps;
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Block check requires selection of a block device");
		return;
	}
	ps = show_splash(L"Performing block check...");
	badblock_scan(b->d,0); // FIXME allow destructive badblock check
	if(ps){
		kill_splash(ps);
	}
}

static void
badblock_check(void){
	badblock_do_internal();
}

static void
mountpoint_callback(const char *path){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Must select a filesystem to mount");
		return;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(path == NULL){
		locked_diag("User cancelled mount operation");
		return;
	}
	if(selected_unpartitionedp()){
		mmount(b->d,path,0,NULL);
	}else{
		assert(selected_partitionp());
		mmount(b->zone->p,path,0,NULL);
	}
}

static void
mount_filesystem(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Must select a filesystem to mount");
		return;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(!selected_unpartitionedp()){
		if(selected_emptyp()){
			locked_diag("Cannot mount unused space");
			return;
		}
		if(b->zone->p->mnttype == NULL){
			locked_diag("No filesystem detected on %s",b->zone->p->name);
			return;
		}
		if(fstype_swap_p(b->zone->p->mnttype)){
			if(swapondev(b->zone->p) == 0){
				redraw_adapter(current_adapter);
			}
			return;
		}
	}else{
		if(b->d->mnttype == NULL){
			locked_diag("No filesystem detected on %s",b->d->name);
			return;
		}
		if(fstype_swap_p(b->d->mnttype)){
			if(swapondev(b->d) == 0){
				redraw_adapter(current_adapter);
			}
			return;
		}
	}
	raise_str_form("enter mountpount",mountpoint_callback,"/",MOUNT_TEXT);
}

static void
numount_target(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Must select a filesystem to mount");
		return;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(!selected_unpartitionedp()){
		if(selected_emptyp()){
			locked_diag("Cannot unmount unused space");
			return;
		}
		if(!targeted_p(b->zone->p)){
			locked_diag("Block device %s is not a target",b->zone->p->name);
			return;
		}
		if(unmount(b->zone->p,NULL)){
			return;
		}
		redraw_adapter(current_adapter);
		return;
	}else{
		if(!targeted_p(b->d)){
			locked_diag("Block device %s is not a target",b->d->name);
			return;
		}
		if(unmount(b->d,NULL)){
			return;
		}
		redraw_adapter(current_adapter);
		return;
	}
}

static int
biosboot(void){
	blockobj *b;
	device *d;

	if(!target_mode_p()){
		locked_diag("Not in target mode");
		return -1;
	}
	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Must select a block device to boot");
		return -1;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return -1;
	}
	if(selected_unpartitionedp()){
		locked_diag("BIOS cannot boot from unpartitioned %s",b->d->name);
		return -1;
	}
	if(b->zone->p){
		if(b->zone->p->layout != LAYOUT_PARTITION){
			locked_diag("Cannot boot from unused space");
			return -1;
		}
		d = b->zone->p;
	}else{
		d = b->d;
	}
	if(!targeted_p(d)){
		locked_diag("Block device %s is not a target",d->name);
		return -1;
	}
	// Do not enforce a check against the path for '/'; it can be in /boot
	if(finalize_target()){
		return -1;
	}
	if(prepare_bios_boot(d)){
		return -1;
	}
	locked_diag("Successfully prepared BIOS boot");
	return 0;
}

static int
uefiboot(void){
	blockobj *b;
	device *d;

	if(!target_mode_p()){
		locked_diag("Not in target mode");
		return -1;
	}
	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Must select a block device to boot");
		return -1;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return -1;
	}
	if(selected_unpartitionedp()){
		locked_diag("UEFI cannot boot from unpartitioned %s",b->d->name);
		return -1;
	}
	if(b->zone->p){
		if(b->zone->p->layout != LAYOUT_PARTITION){
			locked_diag("Cannot boot from unused space");
			return -1;
		}
		d = b->zone->p;
	}else{
		d = b->d;
	}
	if(!targeted_p(d)){
		locked_diag("Block device %s is not a target",d->name);
		return -1;
	}
	// Do not enforce a check against the path for '/'; it can be in /boot
	if(finalize_target()){
		return -1;
	}
	if(prepare_uefi_boot(d)){
		return -1;
	}
	locked_diag("Successfully prepared UEFI boot");
	return 0;
}

static void
nmount_target(void){
	blockobj *b;

	if(!target_mode_p()){
		locked_diag("Not in target mode");
		return;
	}
	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Must select a filesystem to mount");
		return;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(!selected_unpartitionedp()){
		if(selected_emptyp()){
			locked_diag("Cannot mount unused space");
			return;
		}
		if(targeted_p(b->zone->p)){
			locked_diag("%s is already a target",b->zone->p->name);
			return;
		}
		if(b->zone->p->mnttype == NULL){
			locked_diag("No filesystem detected on %s",b->zone->p->name);
			return;
		}
	}else{
		if(targeted_p(b->d)){
			locked_diag("%s is already a target",b->d->name);
			return;
		}
		if(b->d->mnttype == NULL){
			locked_diag("No filesystem detected on %s",b->d->name);
			return;
		}
	}
	raise_str_form("enter target mountpount",targpoint_callback,"/",
			TARG_TEXT);
}

static void
umount_filesystem(void){
	blockobj *b;
	int r;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Must select a filesystem to unmount");
		return;
	}
	if(selected_unloadedp()){
		locked_diag("Media is not loaded on %s",b->d->name);
		return;
	}
	if(!selected_unpartitionedp()){
		if(selected_emptyp()){
			locked_diag("Cannot unmount unused space");
			return;
		}
		if(fstype_swap_p(b->zone->p->mnttype)){
			r = swapoffdev(b->zone->p);
		}else{
			r = unmount(b->zone->p,NULL);
		}
	}else{
		if(fstype_swap_p(b->d->mnttype)){
			r = swapoffdev(b->d);
		}else{
			r = unmount(b->d,NULL);
		}
	}
	if(!r){
		redraw_adapter(current_adapter);
	}
}

static void
remove_last_bufchar(char *buf){
	char *killem = buf;
	mbstate_t mb;
	size_t m;

	memset(&mb,0,sizeof(mb));
	while( (m = mbrlen(buf,strlen(buf),&mb)) ){
		if(m == (size_t)-1 || m == (size_t)-2){
			break;
		}
		killem = buf;
		buf += m;
	}
	*killem = '\0';
}

// We received input while a modal freeform string input form was active.
// Divert it from the typical UI, and handle it according to the form.
static void
handle_actform_string_input(int ch){
	struct form_state *fs = actform;
	void (*cb)(const char *);

	cb = actform->fxn;
	switch(ch){
	case 21: // CTRL+u, clear input line FIXME
		lock_ncurses();
		fs->inp.buffer[0] = '\0';
		form_string_options(fs);
		unlock_ncurses();
		break;
	case '\r': case '\n': case KEY_ENTER:{
		char *str;

		lock_ncurses();
		str = strdup(actform->inp.buffer);
		assert(NULL != str);
		free_form(actform);
		actform = NULL;
		curs_set(0);
		cb(str);
		free(str);
		unlock_ncurses();
		break;
	}case KEY_ESC:{
		lock_ncurses();
		free_form(actform);
		actform = NULL;
		curs_set(0);
		cb(NULL);
		unlock_ncurses();
		break;
	}case KEY_BACKSPACE:{
		lock_ncurses();
		remove_last_bufchar(fs->inp.buffer);
		form_string_options(fs);
		unlock_ncurses();
		break;
	}default:{
		char *tmp;

		if(ch >= 256 || !isgraph(ch)){
			diag("please %s, or cancel",actform->boxstr);
		}
		lock_ncurses();
		if((tmp = realloc(fs->inp.buffer,strlen(fs->inp.buffer) + 2)) == NULL){
			locked_diag("Couldn't allocate input buffer (%s?)",strerror(errno));
			unlock_ncurses();
			return;
		}
		fs->inp.buffer = tmp;
		fs->inp.buffer[strlen(fs->inp.buffer) + 1] = '\0';
		fs->inp.buffer[strlen(fs->inp.buffer)] = (unsigned char)ch;
		form_string_options(fs);
		unlock_ncurses();
	} }
}

// We received input while a modal subwindow was active. Divert it from the
// typical UI, and handle it according to the subwindow. If we are not
// interested in the input, return it for further use. Otherwise, return 0 to
// indicate intercept.
static int
handle_subwindow_input(int ch){
	/*switch(ch){
		default:
			locked_diag("FIXME handle subwindow input");
	}*/
	return ch;
}

static void
handle_actform_splash_input(void){
	lock_ncurses();
	free_form(actform);
	actform = NULL;
	curs_set(0);
	unlock_ncurses();
}

// We received input while a modal form was active. Divert it from the typical
// UI, and handle it according to the form. Returning non-zero quits the
// program, and should pretty much only indicate that 'q' was pressed.
static int
handle_actform_input(int ch){
	struct form_state *fs = actform;
	void (*mcb)(const char *,char **,int,int);
	void (*cb)(const char *);

	if(fs->formtype == FORM_STRING_INPUT){
		handle_actform_string_input(ch);
		return 0;
	}else if(fs->formtype == FORM_SPLASH_PROMPT){
		handle_actform_splash_input();
		return 0;
	}else if(fs->formtype == FORM_MULTISELECT || fs->formtype == FORM_CHECKBOXEN){
		mcb = actform->mcb;
		cb = NULL;
	}else{
		mcb = NULL;
		cb = actform->fxn;
	}
	switch(ch){
		case 12: // CTRL+L FIXME
			lock_ncurses();
			wrefresh(curscr);
			unlock_ncurses();
			break;
		case ' ': case '\r': case '\n': case KEY_ENTER:{
			int op,selections,scrolloff;
			char **selarray;
			char *optstr;

			lock_ncurses();
				op = fs->idx;
				optstr = strdup(fs->ops[op].option);
				assert(NULL != optstr);
				selarray = fs->selarray;
				selections = fs->selections;
				scrolloff = fs->scrolloff;
				fs->selarray = NULL;
				free_form(actform);
				actform = NULL;
				setup_colors();
				if(mcb){
					mcb(optstr,selarray,selections,scrolloff);
				}else{
					cb(optstr);
				}
				free(optstr);
			unlock_ncurses();
			break;
		}case KEY_ESC:{
			lock_ncurses();
			if(fs->formtype == FORM_MULTISELECT || fs->formtype == FORM_CHECKBOXEN){
				int scrolloff = fs->scrolloff;
				free_form(actform);
				actform = NULL;
				mcb(NULL,NULL,0,scrolloff);
			}else{
				free_form(actform);
				actform = NULL;
				cb(NULL);
			}
			unlock_ncurses();
			break;
		}case KEY_UP: case 'k':{
			lock_ncurses();
			if(fs->idx == fs->scrolloff){
				if(--fs->scrolloff < 0){
					fs->scrolloff = fs->opcount - 1;
				}
			}
			if(--fs->idx < 0){
				fs->idx = fs->opcount - 1;
			}
			if(fs->formtype == FORM_MULTISELECT){
				multiform_options(fs);
			}else if(fs->formtype == FORM_CHECKBOXEN){
				check_options(fs);
			}else{
				form_options(fs);
			}
			unlock_ncurses();
			break;
		}case KEY_DOWN: case 'j':{
			lock_ncurses();
			int maxz;
			maxz = getmaxy(panel_window(fs->p)) - 5 >= fs->opcount - 1 ? fs->opcount - 1 : getmaxy(panel_window(fs->p)) - 5;
			if(fs->idx == (fs->scrolloff + maxz) % fs->opcount){
				if(++fs->scrolloff >= fs->opcount){
					fs->scrolloff = 0;
				}
			}
			if(++fs->idx >= fs->opcount){
				fs->idx = 0;
			}
			if(fs->formtype == FORM_MULTISELECT){
				multiform_options(fs);
			}else if(fs->formtype == FORM_CHECKBOXEN){
				check_options(fs);
			}else{
				form_options(fs);
			}
			unlock_ncurses();
			break;
		}case 'q':{
			return 'q';
		}case 'C':{
			int selections,scrolloff;
			char **selarray;

			lock_ncurses();
			if(fs->formtype == FORM_MULTISELECT || fs->formtype == FORM_CHECKBOXEN){
				selarray = fs->selarray;
				selections = fs->selections;
				scrolloff = fs->scrolloff;
				fs->selarray = NULL;
				free_form(actform);
				actform = NULL;
				mcb("",selarray,selections,scrolloff);
				unlock_ncurses();
				break;
			}
			unlock_ncurses();
		} /* intentional fallthrough */
		default:{
			diag("please %s, or cancel",actform->boxstr);
			break;
		}
	}
	return 0;
}

static void
destroy_aggregate_confirm(const char *op){
	blockobj *b;

	if(!op || !approvedp(op)){
		locked_diag("aggregate destruction was cancelled");
		return;
	}
	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Aggregate destruction requires selection of an aggregate");
		return;
	}
	if(b->d->layout == LAYOUT_NONE || b->d->layout == LAYOUT_PARTITION){
		locked_diag("%s is not an aggregate",b->d->name);
	}else if(b->d->layout == LAYOUT_ZPOOL){
		destroy_zpool(b->d);
	}else if(b->d->layout == LAYOUT_MDADM){
		destroy_mdadm(b->d);
	}else if(b->d->layout == LAYOUT_DM){
		locked_diag("Not yet implemented FIXME"); // FIXME
	}else{
		locked_diag("Unknown layout type %d on %s",b->d->layout,b->d->name);
	}
}

static void
untargeted_exit_confirm(const char *op){
	if(!op || !approvedp(op)){
		locked_diag("exit cancelled");
		return;
	}
	shutdown_cycle();
}

static void
destroy_aggregate(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Aggregate destruction requires selection of an aggregate");
		return;
	}
	if(b->d->layout == LAYOUT_NONE || b->d->layout == LAYOUT_PARTITION){
		locked_diag("%s is not an aggregate",b->d->name);
		return;
	}
	confirm_operation("destroy the aggregate",destroy_aggregate_confirm);
}

static void
modify_aggregate(void){
	blockobj *b;

	if((b = get_selected_blockobj()) == NULL){
		locked_diag("Aggregate modification requires selection of an aggregate");
		return;
	}
	if(b->d->layout == LAYOUT_NONE || b->d->layout == LAYOUT_PARTITION){
		locked_diag("%s is not an aggregate",b->d->name);
		return;
	}
	locked_diag("Aggregate modification not yet implemented FIXME");
}

static void
search_callback(const char *term){
	if(term == NULL){
		locked_diag("Search was cancelled");
		return;
	}
	locked_diag("Search not yet implemented FIXME");
}

static void
start_search(void){
	raise_str_form("start typing an identifier",search_callback,NULL,
			SEARCH_TEXT);
}

static void
loop_callback(const char *term){
	if(term == NULL){
		locked_diag("Loop device setup cancelled");
		return;
	}
	locked_diag("Looping not yet implemented FIXME");
}

static void
configure_loop_dev(void){
	raise_str_form("Select a file to loop",loop_callback,NULL,
			LOOP_TEXT);
}

static void
set_label(void){
	device *d;

	if((d = selected_filesystem()) == NULL){
		locked_diag("No filesystem is selected");
		return;
	}
	if(!fstype_named_p(d->mnttype)){
		locked_diag("%s does not support labels",d->mnttype);
		return;
	}
	// FIXME
	locked_diag("FIXME not yet implemented");
}

static void
set_uuid(void){
	device *d;

	if((d = selected_filesystem()) == NULL){
		locked_diag("No filesystem is selected");
		return;
	}
	if(!fstype_uuid_p(d->mnttype)){
		locked_diag("%s does not support UUIDs",d->mnttype);
		return;
	}
	// FIXME
	locked_diag("FIXME not yet implemented");
}

static void
do_setup_target(const char *token){
	if(token == NULL){
		locked_diag("Targeting was cancelled");
		return;
	}
	if(token[0] != '/'){
		locked_diag("Target must be an absolute path");
		return;
	}
	if(target_mode_p()){
		locked_diag("Already have a target at %s",growlight_target);
		return;
	}
	if(set_target(token)){
		return;
	}
	locked_diag("Now targeting %s",token);
}

static void
setup_target(void){
	if(target_mode_p()){
		locked_diag("Already have a target at %s",growlight_target);
		return;
	}
	raise_str_form("enter target path",do_setup_target,"/target",TARGET_TEXT);
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
	int ch,r;

	while((ch = getch()) != ERR){
		if(ch == 12){ // CTRL+L FIXME
			lock_ncurses();
			wrefresh(curscr);
			unlock_ncurses();
			continue;
		}
		if(actform){
			if((ch = handle_actform_input(ch)) == ERR){
				break;
			}
			if(ch == 0){
				continue;
			}
		}
		if(active){
			if((ch = handle_subwindow_input(ch)) == ERR){
				return;
			}
			if(ch == 0){ // intercepted
				continue;
			}
		}
		switch(ch){
			case 'H':{
				lock_ncurses();
				toggle_panel(w,&help,display_help);
				unlock_ncurses();
				break;
			}
			case 'D':{
				lock_ncurses();
				toggle_panel(w,&diags,display_diags);
				unlock_ncurses();
				break;
			}
			case 'v':{
				lock_ncurses();
				toggle_panel(w,&details,display_details);
				unlock_ncurses();
				break;
			}
			case 'e':{
				lock_ncurses();
				toggle_panel(w,&environment,display_enviroment);
				unlock_ncurses();
				break;
			}
			case 'E':{
				lock_ncurses();
				toggle_panel(w,&maps,display_maps);
				unlock_ncurses();
				break;
			}
			case '+':
				lock_ncurses();
				expand_adapter_locked();
				unlock_ncurses();
				break;
			case '-':{
				lock_ncurses();
				collapse_adapter_locked();
				unlock_ncurses();
				break;
			}
			case KEY_RIGHT: case 'l':{
				lock_ncurses();
				if(selection_active()){
					use_next_zone(current_adapter->selected);
				}
				redraw_adapter(current_adapter);
				unlock_ncurses();
				break;
			}
			case KEY_LEFT: case 'h':{
				lock_ncurses();
				if(selection_active()){
					use_prev_zone(current_adapter->selected);
				}
				redraw_adapter(current_adapter);
				unlock_ncurses();
				break;
			}
			case KEY_UP: case 'k':{
				lock_ncurses();
				use_prev_device();
				unlock_ncurses();
				break;
			}
			case KEY_DOWN: case 'j':{
				lock_ncurses();
				use_next_device();
				unlock_ncurses();
				break;
			}
			case KEY_PPAGE:{
				int sel;
				lock_ncurses();
				sel = selection_active();
				deselect_adapter_locked();
				use_prev_controller(w);
				if(sel){
					select_adapter();
				}
				unlock_ncurses();
				break;
			}
			case KEY_NPAGE:{
				int sel;
				lock_ncurses();
				sel = selection_active();
				deselect_adapter_locked();
				use_next_controller(w);
				if(sel){
					select_adapter();
				}
				unlock_ncurses();
				break;
			}
			case 'm':{
				lock_ncurses();
				make_ptable();
				unlock_ncurses();
				break;
			}
			case 'r':{
				lock_ncurses();
				remove_ptable();
				unlock_ncurses();
				break;
			}
			case 'W':{
				lock_ncurses();
				wipe_mbr();
				unlock_ncurses();
				break;
			}
			case 'B':{
				lock_ncurses();
				badblock_check();
				unlock_ncurses();
				break;
			}
			case 'n':{
				lock_ncurses();
				new_partition();
				unlock_ncurses();
				break;
			}
			case 'd':{
				lock_ncurses();
				delete_partition();
				unlock_ncurses();
				break;
			}
			case 'F':{
				lock_ncurses();
				fsck_partition();
				unlock_ncurses();
				break;
			}
			case 'U':{
				lock_ncurses();
				set_uuid();
				unlock_ncurses();
				break;
			}
			case 'L':{
				lock_ncurses();
				set_label();
				unlock_ncurses();
				break;
			}
			case 's':{
				lock_ncurses();
				set_partition_attrs();
				unlock_ncurses();
				break;
			}
			case 'M':{
				lock_ncurses();
				new_filesystem();
				unlock_ncurses();
				break;
			}
			case 'w':{
				lock_ncurses();
				kill_filesystem();
				unlock_ncurses();
				break;
			}
			case 'o':{
				lock_ncurses();
				mount_filesystem();
				unlock_ncurses();
				break;
			}
			case 'O':{
				lock_ncurses();
				umount_filesystem();
				unlock_ncurses();
				break;
			}
			case 't':{
				lock_ncurses();
				nmount_target();
				unlock_ncurses();
				break;
			}
			case 'T':{
				lock_ncurses();
				numount_target();
				unlock_ncurses();
				break;
			}
			case 'b':{
				lock_ncurses();
				enslave_disk();
				unlock_ncurses();
				break;
			}
			case 'f':{
				lock_ncurses();
				liberate_disk();
				unlock_ncurses();
				break;
			}
			case 'i':{
				lock_ncurses();
				setup_target();
				unlock_ncurses();
				break;
			}
			case '/':{
				lock_ncurses();
				start_search();
				unlock_ncurses();
				break;
			}
			case 'p':{
				lock_ncurses();
				configure_loop_dev();
				unlock_ncurses();
				break;
			}
			case 'I':{
				lock_ncurses();
				unset_target();
				unlock_ncurses();
				break;
			}
			case 'A':
				lock_ncurses();
				if(actform){
					locked_diag("An input dialog is already active");
				}else{
					raise_aggregate_form();
				}
				unlock_ncurses();
				break;
			case 'z':
				lock_ncurses();
				modify_aggregate();
				unlock_ncurses();
				break;
			case 'Z':
				lock_ncurses();
				destroy_aggregate();
				unlock_ncurses();
				break;
// Finalization commands
			case '*':
				lock_ncurses();
				if((r = uefiboot()) == 0){
					locked_diag("Successfully finalized target /etc/fstab");
				}
				unlock_ncurses();
				if(r == 0){
					return;
				}
				break;
			case '#':
				lock_ncurses();
				if((r = biosboot()) == 0){
					locked_diag("Successfully finalized target /etc/fstab");
				}
				unlock_ncurses();
				if(r == 0){
					return;
				}
				break;
			case '@':
				lock_ncurses();
				if((r = finalize_target()) == 0){
					locked_diag("Successfully finalized target /etc/fstab");
				}
				unlock_ncurses();
				if(r == 0){
					return;
				}
				break;
			case 'q':
				if(!growlight_target){
					return;
				}else if(finalized){
					return;
				}
				confirm_operation("exit without finalizing a target",untargeted_exit_confirm);
				break;
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
adapter_callback(controller *a,void *state){
	adapterstate *as;
	reelbox *rb;

	lock_ncurses_growlight();
	if((as = state) == NULL){
		if(a->blockdevs){
			if( (state = as = create_adapter_state(a)) ){
				int newrb,rows,cols;

				getmaxyx(stdscr,rows,cols);
				if( (newrb = bottom_space_p(rows)) ){
					newrb = rows - newrb;
					if((rb = create_reelbox(as,rows,newrb,cols)) == NULL){
						free_adapter_state(as);
						unlock_ncurses_growlight();
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
			rb = NULL;
		}
	}else{
		rb = as->rb;
	}
	if(rb){
		//resize_adapter(rb);
		redraw_adapter(rb);
	}
	unlock_ncurses_growlight();
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

// b->zone == NULL:
// 	d->layout == LAYOUT_NONE:
// 		d->blkdev.unloaded: device unloaded or inaccessible
// 		d->blkdev.pttable == NULL: no partitioning table
//	d->layout == * ??? FIXME
// b->zone->p == NULL: empty space
// b->zone->p: partition
static void
update_blockobj(blockobj *b,device *d){
	zobj *z,*lastz,*firstchoice;
	uintmax_t sector;
	int zonesel = -1; // -1 for no choice (b->zone == NULL on entry)
	int zones;
	device *p;

	if(blockobj_unloadedp(b)){
		free_zchain(&b->zchain);
		b->zone = NULL;
		return;
	}
	// Remember the zone we had selected, though it might disappear, or
	// change index due to zones appearing prior to it.
	if(b->zone){
		zonesel = b->zone->zoneno;
	}
	z = NULL;
	zones = 0;
	if((d->layout == LAYOUT_NONE && d->blkdev.pttable == NULL) ||
			(d->layout == LAYOUT_MDADM && d->mddev.pttable == NULL) ||
			(d->layout == LAYOUT_DM && d->dmdev.pttable == NULL)){
		sector = d->size / d->logsec + 1;
	}else{
		if( (sector = first_usable_sector(d)) ){
			if((z = create_zobj(z, zones, zones, sector - 1, NULL, REP_METADATA)) == NULL){
				goto err;
			}
			++zones;
		}
	}
	for(p = d->parts ; p ; p = p->next){
		if(sector != p->partdev.fsector){
			if((z = create_zobj(z, zones, sector, p->partdev.fsector - 1, NULL, REP_EMPTY)) == NULL){
				goto err;
			}
			++zones;
		}
		if((z = create_zobj(z, zones, p->partdev.fsector, p->partdev.lsector, p, L'\0')) == NULL){
			goto err;
		}
		++zones;
		sector = p->partdev.lsector + 1;
	}
	if(d->logsec && d->size){
		if(sector < d->size / d->logsec){
			if(sector < last_usable_sector(d) + 1){
				if((z = create_zobj(z, zones, sector, last_usable_sector(d), NULL, REP_EMPTY)) == NULL){
					goto err;
				}
				++zones;
				sector = last_usable_sector(d) + 1;
			}
			if(sector < d->size / d->logsec){
				if((z = create_zobj(z, zones, sector, d->size / d->logsec - 1, NULL, REP_METADATA)) == NULL){
					goto err;
				}
				++zones;
				sector = d->size / d->logsec;
			}
		}
	}
	b->zone = NULL;
	free_zchain(&b->zchain);
	firstchoice = NULL;
	if(zonesel >= zones){
		zonesel = zones ? zones - 1 : 0;
	}
	if( (lastz = z) ){
		z->following = 0;
		// If we hadn't selected one before, select the first
		// empty space (where we can make a partition)
		while(z->prev){
			if((zonesel == z->zoneno) ||
					(zonesel == -1 && !z->p && z->rep == REP_EMPTY)){
				b->zone = z;
			}else if(z->zoneno){
				firstchoice = z;
			}
			z->prev->following = z->following + 1;
			z = z->prev;
		}
		if((zonesel == z->zoneno) || (zonesel == -1 && !z->p && z->rep == REP_EMPTY)){
			b->zone = z;
		}
		z->prev = lastz;
		lastz->next = z;
	}
	if(b->zone == NULL){
		if((b->zone = firstchoice) == NULL){
			b->zone = z;
		}
	}
	b->zchain = z;
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

	if(d->layout == LAYOUT_PARTITION){
		return NULL; // FIXME ought be an assert; this shouldn't happen
	}
	lock_ncurses_growlight();
	if((as = d->c->uistate) == NULL){
		if((as = d->c->uistate = adapter_callback(d->c,NULL)) == NULL){
			return NULL;
		}
	}
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
		if(current_adapter == as->rb){
			if(b->prev == NULL && b->next == NULL){
				select_adapter();
			}
		}
		redraw_adapter(as->rb);
	}
	unlock_ncurses_growlight();
	return b;
}

static void
block_free(void *cv,void *bv){
	adapterstate *as = cv;
	blockobj *bo = bv;
	reelbox *rb;

	lock_ncurses_growlight();
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
	free_zchain(&bo->zchain);
	if(bo->prev){
		bo->prev->next = bo->next;
	}
	if(bo->next){
		bo->next->prev = bo->prev;
	}
	if(as->bobjs == bo){
		as->bobjs = bo->next;
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
	}
	unlock_ncurses_growlight();
}

static void
adapter_free(void *cv){
	adapterstate *as = cv;
	reelbox *rb = rb;

	lock_ncurses_growlight();
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
				if(current_adapter == NULL){
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
		free_reelbox(rb);
	}else{
		as->next->prev = as->prev;
		as->prev->next = as->next;
	}
	free_adapter_state(as); // clears subentries
	--count_adapters;
	draw_main_window(stdscr); // Update the device count
	unlock_ncurses_growlight();
}

static void
vdiag(const char *fmt,va_list v){
	lock_ncurses_growlight();
	locked_vdiag(fmt,v);
	unlock_ncurses_growlight();
}

static void
shutdown_cycle(void){
	struct panel_state *ps;
	WINDOW *w = stdscr;

	diag("User-initiated shutdown\n");
	ps = show_splash(L"Shutting down...");
	if(growlight_stop()){
		kill_splash(ps);
		ncurses_cleanup(&w);
		dump_diags();
		exit(EXIT_FAILURE);
	}
	kill_splash(ps);
	if(ncurses_cleanup(&w)){
		dump_diags();
		exit(EXIT_FAILURE);
	}
	exit(EXIT_SUCCESS);
};

static void raise_info_form(const char *str,const char *text){
	struct form_state *fs;
	int lineguess;
	WINDOW *fsw;
	int cols;
	int x,y;

	assert(str && text);
	if(actform){
		locked_diag("An input dialog is already active");
		return;
	}
	if((fs = create_form(str,NULL,FORM_SPLASH_PROMPT,0)) == NULL){
		return;
	}
	fs->longop = strlen(str);
	cols = fs->longop; // FIXME? 40 for input currently
	getmaxyx(stdscr,y,x);
	assert(x >= cols + 3);
	assert(y >= 3);
	// It could be more than this due to line breaking, so add a fudge
	// factor of 2...FIXME
	lineguess = 2; // yuck
	lineguess += strlen(text) / (x - 2) + 1;
	if((fsw = newwin(3,cols + START_COL * 2,FORM_Y_OFFSET + lineguess,x - cols - 3)) == NULL){
		locked_diag("Couldn't create form window, uh-oh");
		free_form(fs);
		return;
	}
	if((fs->p = new_panel(fsw)) == NULL){
		locked_diag("Couldn't create form panel, uh-oh");
		delwin(fsw);
		free_form(fs);
		return;
	}
	assert(top_panel(fs->p) != ERR);
	wattroff(fsw,A_BOLD);
	wcolor_set(fsw,FORMBORDER_COLOR,NULL);
	bevel(fsw);
	wattron(fsw,A_BOLD);
	mvwprintw(fsw,1,START_COL,"%-*.*s",cols,cols,str);
	form_string_options(fs);
	actform = fs;
	fs->extext = raise_form_explication(stdscr,text,20);
	form_colors();
	screen_update();
}

static void
boxinfo(const char *text,...){
	va_list v;
	char *buf;
	int max;

	max = BUFSIZ;
	if((buf = malloc(max)) == NULL){
		lock_ncurses_growlight();
		locked_diag("Couldn't display boxinfo");
		unlock_ncurses_growlight();
		return;
	}
	va_start(v,text);
	if(vsnprintf(buf,max,text,v) >= max){
		buf[max - 1] = '\0';
	}
	lock_ncurses_growlight();
	raise_info_form("Press any key to continue...",buf);
	unlock_ncurses_growlight();
	va_end(v);
}

int main(int argc,char * const *argv){
	const glightui ui = {
		.vdiag = vdiag,
		.boxinfo = boxinfo,
		.adapter_event = adapter_callback,
		.block_event = block_callback,
		.adapter_free = adapter_free,
		.block_free = block_free,
	};
	WINDOW *w;
	struct panel_state *ps;
	int showhelp = 1;

	if(setlocale(LC_ALL,"") == NULL){
		fprintf(stderr,"Warning: couldn't load locale\n");
		//return EXIT_FAILURE;
	}
	if((w = ncurses_setup()) == NULL){
		return EXIT_FAILURE;
	}
	ps = show_splash(L"Initializing...");
	if(growlight_init(argc,argv,&ui,&showhelp)){
		kill_splash(ps);
		ncurses_cleanup(&w);
		dump_diags();
		return EXIT_FAILURE;
	}
	lock_growlight();
	kill_splash(ps);
	if(showhelp){
		toggle_panel(w,&help,display_help);
		screen_update();
	}
	unlock_growlight();
	handle_ncurses_input(w);
	shutdown_cycle(); // calls exit() on all paths
}
