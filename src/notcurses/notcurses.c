#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <pthread.h>
#include <atasmart.h>
#include <scsi/scsi.h>

#include "fs.h"
#include "mbr.h"
#include "zfs.h"
#include "swap.h"
#include "mdadm.h"
#include "health.h"
#include "ptable.h"
#include "ptypes.h"
#include "growlight.h"
#include "aggregate.h"
#include "notui-aggregate.h"

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
  struct ncplane *p;
};

// inherited jank from ncurses->notcurses conversion, eliminate me FIXME
#define REVERSE (NCSTYLE_REVERSE << 8u)

#define PANEL_STATE_INITIALIZER { .p = NULL, }

static struct panel_state *splash;
static struct panel_state *active;
static struct panel_state maps = PANEL_STATE_INITIALIZER;
static struct panel_state help = PANEL_STATE_INITIALIZER;
static struct panel_state diags = PANEL_STATE_INITIALIZER;
static struct panel_state details = PANEL_STATE_INITIALIZER;

static int helpstrs(struct ncplane* n);
static int map_details(struct ncplane* n);
static int update_details(struct ncplane* n);

static inline void
update_details_cond(struct ncplane *n){
  if(n){
    update_details(n);
  }
}

static inline void
update_help_cond(struct ncplane* p){
  if(p){
    helpstrs(p);
  }
}

static inline void
update_map_cond(struct ncplane* p){
  if(p){
    map_details(p);
  }
}

struct form_option {
  char *option;      // option key (the string passed to cb)
  char *desc;      // longer description
};

struct form_input {
  const char *prompt;    // short prompt. currently aliases boxstr
  char *longprompt;    // longer prompt, not currently used
  char *buffer;      // input buffer, initialized to ""
};

typedef enum {
  FORM_SELECT,      // form_option[]
  FORM_STRING_INPUT,    // form_input
  FORM_MULTISELECT,    // form_options[]
  FORM_CHECKBOXEN,    // form_options[]
  FORM_SPLASH_PROMPT,    // form_input
} form_enum;

// Regarding scrolling selection windows: the movement model is the same as
// the main scrollwindow: moving up at the topmost line keeps you at the top
// of the widget, and rotates the selections. scrolloff equals the number of
// lines options have been rotated down, and takes values between 0 and
// opcount - 1. All option selection windows scroll, even if they fit all their
// options, to maintain continuity of UI.
struct form_state {
  struct ncplane* p;
  void (*fxn)(const char*);  // callback once form is done
  void (*mcb)(const char* ,char** ,int,int); // multiform callback
  int longop;      // length of prompt or longest op
  char* boxstr;      // string for box label
  form_enum formtype;    // type of form
  struct panel_state *extext;  // explication text, above the form
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
      int idx;    // selection index
      int scrolloff;    // scroll offset
      struct form_option *ops;// form_option array for *this instance*
      int opcount;    // total number of ops
      int selectno;    // number of selections, total
      int selections;    // number of active selections
      char** selarray;  // array of selections by name
    };
    struct form_input inp;  // form_input state for this instance
  };
};

static struct form_state *actform;

// Our color pairs
enum {
  HEADER_COLOR = 1,
  STATUS_COLOR,
  UHEADING_COLOR,
  UNHEADING_COLOR,
  UBORDER_COLOR,      // Adapters
  SELBORDER_COLOR,    // Current adapter
  DBORDER_COLOR,      // Block bar borders
  PBORDER_COLOR,
  PHEADING_COLOR,

  //10
  SUBDISPLAY_COLOR,
  OPTICAL_COLOR,
  ROTATE_COLOR,
  VIRTUAL_COLOR,
  SSD_COLOR,
  FS_COLOR,
  EMPTY_COLOR,      // Empty sectors
  METADATA_COLOR,      // Partition table metadata
  MDADM_COLOR,
  ZPOOL_COLOR,

  // 20
  PART_COLOR0,      // A defined but unused partition
  PART_COLOR1,
  PART_COLOR2,
  PART_COLOR3,
  FORMBORDER_COLOR,
  FORMTEXT_COLOR,
  INPUT_COLOR,      // Form input color
  SELECTED_COLOR,      // Selected options in multiform
  MOUNT_COLOR0,      // Mounted, untargeted filesystems
  MOUNT_COLOR1,

  //30
  MOUNT_COLOR2,
  MOUNT_COLOR3,
  TARGET_COLOR0,      // Targeted filesystems
  TARGET_COLOR1,
  TARGET_COLOR2,
  TARGET_COLOR3,
  FUCKED_COLOR,      // Things that warrant attention
  SPLASHBORDER_COLOR,
  SPLASHTEXT_COLOR,

  ORANGE_COLOR,
  GREEN_COLOR,
  BLACK_COLOR,

  FIRST_FREE_COLOR
};

static void
compat_set_fg(struct ncplane* nc, int pair){
  switch(pair){
    case 0:
      ncplane_set_fg_rgb8(nc, 128, 192, 128);
      break;
    case HEADER_COLOR:
      ncplane_set_fg_rgb8(nc, 95, 0, 175);
      break;
    case STATUS_COLOR:
      ncplane_set_fg_rgb8(nc, 0, 95, 175);
      break;
    case UHEADING_COLOR:
      ncplane_set_fg_rgb8(nc, 218, 218, 218);
      break;
    case UNHEADING_COLOR:
      ncplane_set_fg_rgb8(nc, 135, 175, 255);
      break;
    case UBORDER_COLOR:
      ncplane_set_fg_rgb8(nc, 0, 215, 175);
      break;
    case SELBORDER_COLOR:
      ncplane_set_fg_rgb8(nc, 0, 255, 215);
      break;
    case DBORDER_COLOR:
      ncplane_set_fg_rgb8(nc, 135, 175, 255); break;
    case PBORDER_COLOR:
      ncplane_set_fg_rgb8(nc, 215, 255, 0); break;
    case PHEADING_COLOR:
      ncplane_set_fg_rgb8(nc, 197, 15, 31); break;
    case SELECTED_COLOR:
      ncplane_set_fg_rgb8(nc, 135, 95, 255); break;
    case VIRTUAL_COLOR:
      ncplane_set_fg_rgb8(nc, 0xd7, 0xd7, 0xaf); break;
    case SSD_COLOR:
      ncplane_set_fg_rgb8(nc, 0xd7, 0xd7, 0xd7); break;
    case FS_COLOR:
      ncplane_set_fg_rgb8(nc, 0x5f, 0xff, 0x5f); break;
    case EMPTY_COLOR: // Empty sectors
      ncplane_set_fg_rgb(nc, 0xffd700); break;
    case METADATA_COLOR: // Partition table metadata
      ncplane_set_fg_rgb8(nc, 249, 241, 165); break;
    case MDADM_COLOR:
      ncplane_set_fg_rgb8(nc, 0xaf, 0xaf, 0xff); break;
    case ORANGE_COLOR:
      ncplane_set_fg_rgb8(nc, 0xd7, 0x5f, 0x00); break;
    case ZPOOL_COLOR:
      ncplane_set_fg_rgb8(nc, 128, 192, 226); break;
    case SUBDISPLAY_COLOR:
      ncplane_set_fg_rgb8(nc, 255, 255, 255); break;
    case OPTICAL_COLOR:
      ncplane_set_fg_rgb8(nc, 175, 215, 0); break;
    case ROTATE_COLOR:
      ncplane_set_fg_rgb8(nc, 175, 175, 135); break;
    case PART_COLOR0:
      ncplane_set_fg_rgb8(nc, 0x00, 0xd7, 0xaf); break;
    case PART_COLOR1:
      ncplane_set_fg_rgb8(nc, 0x00, 0xff, 0xd7); break;
    case PART_COLOR2:
      ncplane_set_fg_rgb8(nc, 0x00, 0xff, 0xff); break;
    case PART_COLOR3:
      ncplane_set_fg_rgb8(nc, 0x00, 0xaf, 0x87); break;
    case FORMBORDER_COLOR:
      ncplane_set_fg_rgb8(nc, 0xaf, 0xaf, 0x87); break;
    case FORMTEXT_COLOR:
      ncplane_set_fg_rgb8(nc, 0x00, 0xd7, 0xaf); break;
    case INPUT_COLOR:
      ncplane_set_fg_rgb8(nc, 0x00, 0xd7, 0x5f); break;
    case MOUNT_COLOR0:
      ncplane_set_fg_rgb8(nc, 0xd0, 0xd0, 0xd0); break;
    case MOUNT_COLOR1:
      ncplane_set_fg_rgb8(nc, 0xbc, 0xbc, 0xbc); break;
    case MOUNT_COLOR2:
      ncplane_set_fg_rgb8(nc, 0xa8, 0xa8, 0xa8); break;
    case MOUNT_COLOR3:
      ncplane_set_fg_rgb8(nc, 0x94, 0x94, 0x94); break;
    case TARGET_COLOR0:      // Targeted filesystems
      ncplane_set_fg_rgb8(nc, 0xaf, 0xff, 0x87); break;
    case TARGET_COLOR1:
      ncplane_set_fg_rgb8(nc, 0x5f, 0xd7, 0xf5); break;
    case TARGET_COLOR2:
      ncplane_set_fg_rgb8(nc, 0x87, 0xd7, 0x87); break;
    case TARGET_COLOR3:
      ncplane_set_fg_rgb8(nc, 0x87, 0xff, 0xaf); break;
    case FUCKED_COLOR:      // Things that warrant attention
      ncplane_set_fg_rgb8(nc, 95, 0, 0); break;
    case SPLASHBORDER_COLOR:
      ncplane_set_fg_rgb8(nc, 95, 95, 215); break;
    case SPLASHTEXT_COLOR:
      ncplane_set_fg_rgb8(nc, 95, 95, 255); break;
    case BLACK_COLOR:
      ncplane_set_fg_rgb8(nc, 254, 0, 255); break;
    case GREEN_COLOR:
      ncplane_set_fg_rgb8(nc, 95, 255, 215); break;
    default:
      ncplane_set_fg_rgb8(nc, 255, 255, 255);
      break;
  }
}

enum {
  SUBDISPLAY_ATTR,
  SUBDISPLAY_INVAL_ATTR,
};

static void
compat_set_fg_all(struct ncplane* nc, int attr){
  switch(attr){
    case SUBDISPLAY_ATTR:
      ncplane_set_styles(nc, NCSTYLE_BOLD);
      compat_set_fg(nc, SUBDISPLAY_COLOR);
      break;
    case SUBDISPLAY_INVAL_ATTR:
      ncplane_set_styles(nc, 0);
      compat_set_fg(nc, SUBDISPLAY_COLOR);
      break;
    default:
      assert(false);
  }
}

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

#define REP_METADATA L'm' //L'\u1d50'//L'M', L'ᵐ'
#define REP_EMPTY L'e' //L'\u2091' //L'E', L'ₑ'

static inline wchar_t
subscript(int in){
  assert(in >= 0 && in < 10);
  return L'\u2080' + in;
}

static struct notcurses* NC;
static struct ncreel* PR;
static struct ncmenu* mainmenu;

static inline void
screen_update(void){
  // must do the ncreel first, as it can create new ones at the top
  if(PR){
    ncreel_redraw(PR);
  }
  if(active){
    ncplane_move_top(active->p);
  }
  if(actform){
    if(actform->extext){
      ncplane_move_top(actform->extext->p);
    }
    ncplane_move_top(actform->p);
  }
  if(splash){
    ncplane_move_top(splash->p);
  }
	ncplane_move_top(ncmenu_plane(mainmenu));
  notcurses_render(NC);
}

static int update_diags(struct panel_state *);

// See below (blockobj) for description of zones.
typedef struct zobj {
  int zoneno;      // in-order, starting at 0
  uintmax_t fsector, lsector;  // first and last logical sector, inclusive
  device* p;      // partition/block device, NULL for empty space
  wchar_t rep;      // character used for representation
          //  if ->p is NULL
  int following;      // Number of zones following us on disk
  struct zobj* prev;
  struct zobj* next;
} zobj;

typedef struct blockobj {
  struct blockobj* next;
  struct blockobj* prev;
  device* d;
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
  zobj* zchain;
  zobj* zone;
} blockobj;

// An adapter, which might be invisible (in which case the nctablet is NULL). If a
// nctablet exists for this adapter, its userptr is the adapterstate.
typedef struct adapterstate {
  controller* c;
  unsigned devs;
  enum {
    EXPANSION_NONE,
    EXPANSION_FULL,
  } expansion;
  int selline;
  blockobj* selected;
  struct adapterstate* next;
  struct adapterstate* prev;
  blockobj* bobjs;
  struct nctablet* rb;   // FIXME name is an anachronism
} adapterstate;

#define EXPANSION_MAX EXPANSION_FULL

static char statusmsg[BUFSIZ];
static unsigned count_adapters;

#define START_COL 1    // Room to leave for borders
#define PAD_COLS(cols) ((cols))

static adapterstate *
get_current_adapter(void){
  struct nctablet* t = ncreel_focused(PR);
  adapterstate* as = nctablet_userptr(t);
  return as;
}

static blockobj*
get_selected_blockobj(void){
  adapterstate *as;

  if((as = get_current_adapter()) == NULL){
    return NULL;
  }
  return as->selected;
}

static int
selection_active(void){
  if(get_selected_blockobj()){
    return 1;
  }
  return 0;
}

static inline int
blockobj_unloadedp(const blockobj* bo){
  return bo && ((bo->d->layout == LAYOUT_NONE && bo->d->blkdev.unloaded)
                || bo->d->size == 0);
}

static inline int
selected_unloadedp(void){
  return blockobj_unloadedp(get_selected_blockobj());
}

static inline int
blockobj_unpartitionedp(const blockobj* bo){
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
blockobj_emptyp(const blockobj* bo){
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
    locked_diag("Filesystem signature already exists on %s", d->name);
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
bevel(struct ncplane* nc, int rows, int cols, bool drawtop, bool drawbot){
  if(rows <= 0 || cols <= 0){
    return -1;
  }
  unsigned ctlword = 0 ;
  if(drawtop == false){
    ctlword |= NCBOXMASK_TOP;
    ctlword |= (2u << NCBOXCORNER_SHIFT);
    ncplane_putwstr_yx(nc, 0, 0, L"│");
    ncplane_putwstr_yx(nc, 0, cols - 1, L"│");
  }
  if(drawbot == false){
    ctlword |= NCBOXMASK_BOTTOM;
    ctlword |= (2u << NCBOXCORNER_SHIFT);
    ncplane_putwstr_yx(nc, rows - 1, 0, L"│");
    ncplane_putwstr_yx(nc, rows - 1, cols - 1, L"│");
  }
  ncplane_cursor_move_yx(nc, 0, 0);
  return ncplane_rounded_box_sized(nc, 0, ncplane_channels(nc),
                                   rows, cols, ctlword);
}

static int
bevel_all(struct ncplane* nc){
  int rows, cols;
  ncplane_dim_yx(nc, &rows, &cols);
  return bevel(nc, rows, cols, true, true);
}

static int
cwattrset(struct ncplane* n, int style){
  ncplane_set_styles(n, style >> 8);
  compat_set_fg(n, style & 0xff);
  return 0;
}

static int
cwattroff(struct ncplane* n, int style){
  ncplane_off_styles(n, style >> 8);
  compat_set_fg(n, style & 0xff);
  return 0;
}

static int
cwattron(struct ncplane* n, int style){
  ncplane_on_styles(n, style >> 8u);
  compat_set_fg(n, style & 0xffu);
  return 0;
}

static int
cmvwhline(struct ncplane* nc, int y, int x, const char* ch, int n){
  if(ncplane_cursor_move_yx(nc, y, x)){
    return -1;
  }
  cell c = CELL_TRIVIAL_INITIALIZER;
  if(cell_load(nc, &c, ch) < 0){
    return -1;
  }
  c.channels = ncplane_channels(nc);
  if(ncplane_hline(nc, &c, n) != n){
    cell_release(nc, &c);
    return -1;
  }
  cell_release(nc, &c);
  return 0;
}

static int __attribute__ ((format (printf, 4, 5)))
cmvwprintw(struct ncplane* n, int y, int x, const char* fmt, ...){
  if(ncplane_cursor_move_yx(n, y, x)){
    return -1;
  }
  va_list va;
  va_start(va, fmt);
  int ret = ncplane_vprintf(n, fmt, va);
  va_end(va);
  return ret;
}

static int __attribute__ ((format (printf, 2, 3)))
cwprintw(struct ncplane* n, const char* fmt, ...){
  va_list va;
  va_start(va, fmt);
  int ret = ncplane_vprintf(n, fmt, va);
  va_end(va);
  return ret;
}

static int
cwbkgd(struct ncplane* nc){
  return ncplane_set_base(nc, " ", 0, 0);
}

static void
draw_main_window(struct ncplane* n){
  int rows, cols, x, y;
  char buf[BUFSIZ];

  ncplane_set_bg_rgb8(n, 0xa0, 0xa0, 0xa0);
  ncplane_set_fg_rgb8(n, 95, 0, 175);
  snprintf(buf, sizeof(buf), "%s %s (%d)", PACKAGE, VERSION, count_adapters - 1);
  ncplane_dim_yx(n, &rows, &cols);
  cwattrset(n, HEADER_COLOR);
  ncplane_putstr_yx(n, rows - 1, 0, buf);
  ncplane_cursor_yx(n, &y, &x);
  assert(x >= 0);
  cols -= x + 1;
  cwattron(n, NCSTYLE_BOLD | STATUS_COLOR);
  cwprintw(n, " %-*.*s", cols, cols, statusmsg);
}

static void
locked_vdiag(const char *fmt, va_list v){
  size_t off;

  vsnprintf(statusmsg, sizeof(statusmsg) / sizeof(*statusmsg) - 1, fmt, v);
  statusmsg[sizeof(statusmsg) / sizeof(*statusmsg) - 1] = '\0';
  for(off = 0 ; off < sizeof(statusmsg) / sizeof(*statusmsg) - 1 ; ++off){
    if(!isgraph(statusmsg[off])){
      if(!statusmsg[off]){
        break;
      }
      statusmsg[off] = ' ';
    }
  }
  draw_main_window(notcurses_stdplane(NC));
  if(diags.p){
    update_diags(&diags);
  }
  screen_update();
}

void locked_diag(const char *fmt, ...){
  va_list v;

  va_start(v, fmt);
  locked_vdiag(fmt, v);
  va_end(v);
}

static inline int
device_lines(int expa, const blockobj* bo){
  int l = 0;

  if(expa != EXPANSION_NONE){
    assert(bo);
    assert(bo->d);
    if(bo->d->size){
      l += 2;
    }
    ++l;
  }
  return l;
}

static zobj *
create_zobj(zobj *prev, unsigned zno, uintmax_t fsector, uintmax_t lsector,
            device *p, wchar_t rep){
  zobj *z;

  assert(lsector >= fsector);
  if(lsector < fsector){
    return NULL;
  }
  assert(fsector > 0 || zno == 0);
  assert(fsector == 0 || zno > 0);
  if((fsector == 0) != (zno == 0)){
    return NULL;
  }
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

// Create a panel at the bottom of the window, referred to as the "subdisplay".
// Only one can currently be active at a time. Window decoration and placement
// is managed here; only the rows needed for display ought be provided.
static int
new_display_panel(struct ncplane* nc, struct panel_state* ps,
                  int rows, int cols, const wchar_t* hstr,
                  const wchar_t* bstr, int borderpair){
  const int crightlen = bstr ? wcslen(bstr) : 0;
  int ybelow, yabove;
  int x, y;

  // Desired space above and below, which will be impugned upon as needed
  ybelow = 3;
  yabove = 5;
  ncplane_dim_yx(nc, &y, &x);
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
  if((ps->p = ncplane_new(notcurses_stdplane(NC), rows + 2, cols, yabove, 0, NULL, NULL)) == NULL){
    locked_diag("Couldn't create subpanel, uh-oh");
    return -1;
  }
  cwattron(ps->p, NCSTYLE_BOLD);
  compat_set_fg(ps->p, borderpair);
  bevel_all(ps->p);
  cwattroff(ps->p, NCSTYLE_BOLD);
  compat_set_fg(ps->p, PHEADING_COLOR);
  if(hstr){
    ncplane_putwstr_yx(ps->p, 0, START_COL * 2, hstr);
  }
  if(bstr){
    ncplane_putwstr_yx(ps->p, rows + 1, cols - (crightlen + START_COL * 2), bstr);
  }
  return 0;
}

static void
hide_panel_locked(struct panel_state* ps){
  if(ps){
    ncplane_destroy(ps->p);
    ps->p = NULL;
  }
}

// Print the contents of the block device in a horizontal bar of arbitrary size
static void
print_blockbar(struct ncplane* n, const blockobj* bo, int y, int sx, int ex, int selected){
  char pre[BPREFIXSTRLEN + 1];
  const char *selstr = NULL;
  wchar_t wbuf[ex - sx + 2];
  const int wchars = sizeof(wbuf) / sizeof(*wbuf);
  int targco, mountco, partco;
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
    int co = mnttype_aggregablep(d->mnttype) ? PART_COLOR0 : FS_COLOR;

    cwattrset(n, NCSTYLE_BOLD|co);
    qprefix(zs, 1, pre, 1);
    if(!d->mnt.count || swprintf(wbuf, wchars, L" %s%s%ls%s%ls%s%s%sat %s ",
      d->label ? "" : "nameless ",
      d->mnttype,
      d->label ? L" “" : L"",
      d->label ? d->label : "",
      d->label ? L"” " : L" ",
      zs ? "(" : "", zs ? pre : "", zs ? ") " : "",
      d->mnt.list[0]) >= wchars){
      if(swprintf(wbuf, wchars, L" %s%s%ls%s%ls%s%s%s ",
        d->label ? "" : "nameless ",
        d->mnttype,
        d->label ? L" “" : L"",
        d->label ? d->label : "",
        d->label ? L"” " : L" ",
        zs ? "(" : "", zs ? pre : "", zs ? ")" : ""
        ) >= wchars){
        if(swprintf(wbuf, wchars, L" %s%s%s%s ",
          d->mnttype,
          zs ? "(" : "", zs ? pre : "", zs ? ")" : ""
          ) >= wchars){
          swprintf(wbuf, wchars, L"%s", d->mnttype);
        }
      }
    }
    cmvwhline(n, y, sx, "∾", ex - sx + 1);
    ncplane_putwstr_yx(n, y, sx, L" ");
    ncplane_putwstr_yx(n, y, sx + (ex - sx + 1 - wcslen(wbuf)) / 2, wbuf);
    ncplane_putwstr_yx(n, y, ex - 1, L" ");
    selstr = d->name;
  }else if(d->layout == LAYOUT_NONE && d->blkdev.unloaded){
    cwattrset(n, NCSTYLE_BOLD|OPTICAL_COLOR);
    selstr = "No media detected in drive";
    cmvwprintw(n, y, sx, "%-*.*s", ex - sx, ex - sx, selstr);
  }else if((d->layout == LAYOUT_NONE && d->blkdev.pttable == NULL) ||
    (d->layout == LAYOUT_MDADM && d->mddev.pttable == NULL) ||
    (d->layout == LAYOUT_DM && d->dmdev.pttable == NULL)){
    cwattrset(n, NCSTYLE_BOLD|EMPTY_COLOR);
    selstr = d->layout == LAYOUT_NONE ? "unpartitioned space" :
        "unpartitionable space";
    snprintf(buf, sizeof(buf), " %s %s ", qprefix(d->size, 1, pre, 1), selstr) < (int)sizeof(buf);
    cmvwhline(n, y, sx, "∾", ex - sx + 1);
    ncplane_putwstr_yx(n, y, sx, L" ");
    ncplane_putstr_yx(n, y, sx + (ex - sx + 1 - strlen(buf)) / 2, buf);
    ncplane_putwstr_yx(n, y, ex - 1, L" ");
  }
  if((z = bo->zchain) == NULL){
    if(selected){
      cwattron(n, REVERSE);
      cmvwprintw(n, y - 1, sx + 1, "⇗⇨⇨⇨%.*s", (int)(ex - (sx + 5)), selstr);
      cwattroff(n, REVERSE);
    }
    return;
  }
  partco = PART_COLOR0;
  targco = TARGET_COLOR0;
  mountco = MOUNT_COLOR0;
  do{
    unsigned ch, och;
    wchar_t rep[2] = {L'\0', L'\0'};

    wbuf[0] = L'\0';
    zs = (z->lsector - z->fsector + 1) * bo->d->logsec;
    qprefix(zs, 1, pre, 1);
    if(z->p == NULL){ // unused space among partitions, or metadata
      int co = (z->rep == REP_METADATA ? METADATA_COLOR :
          EMPTY_COLOR);
      const char *repstr = z->rep == REP_METADATA ?
        "partition table metadata" : "empty space";

      if(selected && z == bo->zone){
        selstr = repstr;
        cwattrset(n, NCSTYLE_BOLD|NCSTYLE_UNDERLINE|co);
      }else{
        cwattrset(n, co);
      }
      if(swprintf(wbuf, wchars - 2, L"%s %s", pre, repstr) >= wchars - 2){
        if(swprintf(wbuf, wchars - 2, L"%s", repstr) >= wchars - 2){
          wbuf[0] = L'\0';
        }
      }
      rep[0] = z->rep;
    }else{ // dedicated partition
      if(selected && z == bo->zone){ // partition and device are selected
        if(targeted_p(z->p)){
          cwattrset(n, NCSTYLE_BOLD|NCSTYLE_UNDERLINE|targco);
          targco = next_targco(targco);
        }else if(z->p->mnt.count){
          cwattrset(n, NCSTYLE_BOLD|NCSTYLE_UNDERLINE|mountco);
          mountco = next_mountco(mountco);
        }else if(z->p->mnttype && !mnttype_aggregablep(z->p->mnttype)){
          cwattrset(n, NCSTYLE_BOLD|NCSTYLE_UNDERLINE|FS_COLOR);
        }else{
          cwattrset(n, NCSTYLE_BOLD|NCSTYLE_UNDERLINE|partco);
          partco = next_partco(partco);
        }
        // FIXME need to store pname as multibyte char *
        // selstr = z->p->partdev.pname;
        // selstr = selstr ? selstr : z->p->name;
        if( (selstr = z->p->name) ){
          if(swprintf(wbuf, wchars - 2, L"%s %s", pre, selstr) >= wchars - 2){
            if(swprintf(wbuf, wchars - 2, L"%s", selstr) >= wchars - 2){
              wbuf[0] = L'\0';
            }
          }
        }
      }else{ // partition is not selected
        if(targeted_p(z->p)){
          cwattrset(n, targco);
          targco = next_targco(targco);
        }else if(z->p->mnt.count){
          cwattrset(n, mountco);
          mountco = next_mountco(mountco);
        }else if(z->p->mnttype && !mnttype_aggregablep(z->p->mnttype)){
          cwattrset(n, FS_COLOR);
        }else{
          cwattrset(n, partco);
          partco = next_partco(partco);
        }
        if(swprintf(wbuf, wchars - 2, L"%s %s", pre, z->p->name) >= wchars - 2){
          if(swprintf(wbuf, wchars - 2, L"%s", z->p->name) >= wchars - 2){
            wbuf[0] = L'\0';
          }
        }
      }
      if(z->p->partdev.alignment < d->physsec){ // misaligned!
        cwattrset(n, NCSTYLE_BOLD|FUCKED_COLOR);
      }
      if(z->p->mnttype){
        if((!z->p->mnt.count || swprintf(wbuf, wchars - 2, L"%s at %s (%s)", z->p->mnttype, z->p->mnt.list[0], pre) >= wchars - 2)){
          if(!z->p->label || swprintf(wbuf, wchars - 2, L"%s “%s” (%s)", z->p->mnttype, z->p->label, pre) >= wchars - 2){
            if(swprintf(wbuf, wchars - 2, L"%s (%s)", z->p->mnttype, pre) >= wchars - 2){
              wbuf[0] = L'\0';
            }
          }
        }
      }
      rep[0] = z->p->partdev.pnumber % 16;
      if(rep[0] >= 10){
        rep[0] = L'a' + (rep[0] - 10); // FIXME lame
      }else{
        rep[0] = L'0' + rep[0];  // FIXME lame
      }
    }
    ch = (((z->lsector - z->fsector) * 1000) / ((d->size * 1000 / d->logsec) / (ex - sx)));
    if(ch == 0){
      ch = 1;
    }
    och = off;
    while(ch--){
      ncplane_putwstr_yx(n, y, off, rep);
      if(++off > ex - z->following){
        off = ex - z->following;
        break;
      }
    }
    cwattron(n, REVERSE);
    if(selstr){
      if(och < ex / 2u){
        cmvwprintw(n, y - 1, och, "⇗⇨⇨⇨%.*s", (int)(ex - (off + strlen(selstr) + 4)), selstr);
      }else{
        cmvwprintw(n, y - 1, off - 4 - strlen(selstr), "%s⇦⇦⇦⇖", selstr);
      }
    }
    cwattroff(n, REVERSE);
    // Truncate it at whitespace until it's small enough to fit
    while(wcslen(wbuf) && wcslen(wbuf) + 2 > (off - och + 1)){
      wchar_t *wtrunc = wcsrchr(wbuf, L' ');

      if(wtrunc){
        *wtrunc = L'\0';
      }else{
        wbuf[0] = L'\0';
      }
    }
    if(wcslen(wbuf)){
      size_t start = och + ((off - och + 1) - wcslen(wbuf)) / 2;

      cwattron(n, NCSTYLE_BOLD);
      ncplane_putwstr_yx(n, y, start, wbuf);
      ncplane_putwstr_yx(n, y, start - 1, L" ");
      ncplane_putwstr_yx(n, y, start + wcslen(wbuf), L" ");
    }
    selstr = NULL;
  }while((z = z->next) != bo->zchain);
}

// returns number of lines printed
// drawfromtop: if true, we are cut off at the top (printing up), otherwise we'd be
// cut off at the bottom (printing down).
static int
print_dev(struct ncplane* n, const adapterstate* as, const blockobj* bo,
          int line, int rows, unsigned cols, bool drawfromtop){
  char buf[BPREFIXSTRLEN + 1];
  int co, rx, attr;
  char rolestr[12]; // taken from %-11.11s below

//fprintf(stderr, " HERE FOR %s: %s line %d rows %d cols %d lout %d\n", as->c->name, bo->d->name, line, rows, cols, bo->d->layout);
  ncplane_set_bg_default(n);
  if(line >= rows){
    return 0;
  }
  strcpy(rolestr, "");
  bool selected = false;
  if(as == get_current_adapter()){
    selected = line >= 1 && line == as->selline;
  }
  rx = cols - 79;
  switch(bo->d->layout){
case LAYOUT_NONE:
    if(bo->d->blkdev.realdev){
      if(bo->d->blkdev.rotation == SSD_ROTATION){
        cwattrset(n, SSD_COLOR);
        strncpy(rolestr, "solidstate", sizeof(rolestr));
      }else if(bo->d->blkdev.rotation > 0){
        int32_t speed = bo->d->blkdev.rotation;
        cwattrset(n, ROTATE_COLOR);
        if(speed > 0){
          if(speed > 99999){
            speed = 99999;
          }
          snprintf(rolestr, sizeof(rolestr), "%d rpm", speed);
        }else{
          strncpy(rolestr, "ferromag", sizeof(rolestr));
        }
      }else if(bo->d->kerneltype == TYPE_ROM){
        cwattrset(n, OPTICAL_COLOR);
        strncpy(rolestr, "optical", sizeof(rolestr));
      }else if(bo->d->kerneltype == TYPE_TAPE){
        cwattrset(n, OPTICAL_COLOR);
        strncpy(rolestr, "tape", sizeof(rolestr));
      }
      // FIXME do we want a default here?
    }else{
      cwattrset(n, VIRTUAL_COLOR);
      strncpy(rolestr, "virtual", sizeof(rolestr));
    }
    if(!bo->d->size || line + 2 < rows){
      if(bo->d->size){
        line += 2;
      }else if(selected){
        cwattron(n, REVERSE);
      }
      qprefix(bo->d->size, 1, buf, 0);
      cmvwprintw(n, line, 1, "%11.11s  %-16.16s %4.4s %*s %4uB %-6.6s%-16.16s %4.4s %-*.*s",
        bo->d->name,
        bo->d->model ? bo->d->model : "n/a",
        bo->d->revision ? bo->d->revision : "n/a",
        PREFIXFMT(buf),
        bo->d->physsec,
        bo->d->blkdev.pttable ? bo->d->blkdev.pttable : "none",
        bo->d->blkdev.wwn ? bo->d->blkdev.wwn : "n/a",
        bo->d->blkdev.realdev ? transport_str(bo->d->blkdev.transport) : "n/a",
        rx, rx, "");
      if(bo->d->size){
        line -= 2;
      }
    }
    break;
case LAYOUT_MDADM:
    if(bo->d->mddev.level){
      strncpy(rolestr, bo->d->mddev.level, sizeof(rolestr) - 1);
      rolestr[sizeof(rolestr) - 1] = '\0';
    }
    if(bo->d->mddev.degraded){
      co = FUCKED_COLOR;
    }else{
      co = MDADM_COLOR;
    }
    cwattrset(n, co);
    if(line >= drawfromtop){
      if(!bo->d->size || line + 2 <= rows/* - !drawfromtop*/){
        if(bo->d->size){
          line += 2;
        }else if(selected){
          cwattron(n, REVERSE);
        }
        qprefix(bo->d->size, 1, buf, 0);
        cmvwprintw(n, line, 1, "%11.11s  %-16.16s %4.4s %*s %4uB %-6.6s%-16.16s %4.4s %-*.*s",
          bo->d->name,
          bo->d->model ? bo->d->model : "n/a",
          bo->d->revision ? bo->d->revision : "n/a",
          PREFIXFMT(buf),
          bo->d->physsec,
          bo->d->mddev.pttable ? bo->d->mddev.pttable : "none",
          bo->d->mddev.mdname ? bo->d->mddev.mdname : "n/a",
          transport_str(bo->d->mddev.transport),
          rx, rx, "");
        if(bo->d->size){
          line -= 2;
        }
      }
    }
    cwattrset(n, MDADM_COLOR);
    break;
case LAYOUT_DM:
    strncpy(rolestr, "dm", sizeof(rolestr));
    cwattrset(n, MDADM_COLOR);
    if(line >= drawfromtop){
      if(!bo->d->size || line + 2 < rows/* - !drawfromtop*/){
        if(bo->d->size){
          line += 2;
        }else if(selected){
          cwattron(n, REVERSE);
        }
        qprefix(bo->d->size, 1, buf, 0);
        cmvwprintw(n, line, 1, "%11.11s  %-16.16s %4.4s %*s %4uB %-6.6s%-16.16s %4.4s %-*.*s",
          bo->d->name,
          bo->d->model ? bo->d->model : "n/a",
          bo->d->revision ? bo->d->revision : "n/a",
          PREFIXFMT(buf),
          bo->d->physsec,
          bo->d->dmdev.pttable ? bo->d->dmdev.pttable : "none",
          bo->d->dmdev.dmname ? bo->d->dmdev.dmname : "n/a",
          transport_str(bo->d->dmdev.transport),
          rx, rx, "");
        if(bo->d->size){
          line -= 2;
        }
      }
    }
    break;
case LAYOUT_PARTITION:
    break;
case LAYOUT_ZPOOL:
    strncpy(rolestr, "zpool", sizeof(rolestr));
    cwattrset(n, ZPOOL_COLOR);
    if(line >= drawfromtop){
      if(!bo->d->size || line + 2 < rows/* - !drawfromtop*/){
        if(bo->d->size){
          line += 2;
        }else if(selected){
          cwattron(n, REVERSE);
        }
        qprefix(bo->d->size, 1, buf, 0);
        cmvwprintw(n, line, 1, "%11.11s  %-16.16s %4ju %*s %4uB %-6.6s%-16.16s %4.4s %-*.*s",
          bo->d->name,
          bo->d->model ? bo->d->model : "n/a",
          (uintmax_t)bo->d->zpool.zpoolver,
          PREFIXFMT(buf),
          bo->d->physsec,
          "spa",
          "n/a",  // FIXME
          transport_str(bo->d->zpool.transport),
          rx, rx, "");
        if(bo->d->size){
          line -= 2;
        }
      }
    }
    break;
  }
  if(bo->d->size == 0){
    return 1;
  }
  cwattroff(n, REVERSE);
  cwattrset(n, NCSTYLE_BOLD|SUBDISPLAY_COLOR);

  // Box-diagram (3-line) mode. Print the name on the first line.
  if(line >= drawfromtop){
    cmvwprintw(n, line, START_COL, "%11.11s", bo->d->name);
  }

  // Print summary below device name, in the same color, but prefix it
  // with the single-character SMART status when applicable.
  if(line + 1 < rows/* - !drawfromtop*/ && line + 1 >= drawfromtop){
    wchar_t rep = L' ';
    if(bo->d->blkdev.smart >= 0){
      if(bo->d->blkdev.smart == SK_SMART_OVERALL_GOOD){
        cwattrset(n, NCSTYLE_BOLD|GREEN_COLOR);
        rep = L'✔';
      }else if(bo->d->blkdev.smart != SK_SMART_OVERALL_BAD_STATUS
          && bo->d->blkdev.smart != SK_SMART_OVERALL_BAD_SECTOR_MANY){
        cwattrset(n, NCSTYLE_BOLD|ORANGE_COLOR);
        rep = L'☠';
      }else{
        cwattrset(n, NCSTYLE_BOLD|FUCKED_COLOR);
        rep = L'✗';
      }
    }
    cmvwprintw(n, line + 1, START_COL, "%lc", rep);
    cwattrset(n, NCSTYLE_BOLD|SUBDISPLAY_COLOR);
    if(strlen(rolestr)){
      cwprintw(n, "%10.10s", rolestr);
    }else{
      cwprintw(n, "          ");
    }
  }
  // ...and finally the temperature/vfailure status, and utilization...
  if((line + 2 <= rows/* - !drawfromtop*/) && (line + 2 >= drawfromtop)){
    int sumline = line + 2;
    if(bo->d->layout == LAYOUT_NONE){
      if(bo->d->blkdev.celsius && bo->d->blkdev.celsius < 100u){
        if(bo->d->blkdev.celsius >= 60u){
          cwattrset(n, NCSTYLE_BOLD|FUCKED_COLOR);
        }else if(bo->d->blkdev.celsius >= 40u){
          cwattrset(n, NCSTYLE_BOLD|ORANGE_COLOR);
        }else{
          cwattrset(n, GREEN_COLOR);
        }
        // FIXME would be nice to use ℃ , but it looks weird
        cmvwprintw(n, sumline, START_COL, "%2.ju° ", bo->d->blkdev.celsius);
      }else{
        cmvwprintw(n, sumline, START_COL, "    ");
      }
    }else if(bo->d->layout == LAYOUT_MDADM){
      if(bo->d->mddev.degraded){
        cwattrset(n, NCSTYLE_BOLD|FUCKED_COLOR);
        cmvwprintw(n, sumline, START_COL,
            "%1lux☠ ", bo->d->mddev.degraded);
      }else{
        cwattrset(n, GREEN_COLOR);
        cmvwprintw(n, sumline, START_COL, "up  ");
      }
    }else if(bo->d->layout == LAYOUT_DM){
      // FIXME add more detail...type of dm etc
      cwattrset(n, GREEN_COLOR);
      cmvwprintw(n, sumline, START_COL, "up  ");
    }else if(bo->d->layout == LAYOUT_ZPOOL){
      if(bo->d->zpool.state != POOL_STATE_ACTIVE){
        cwattrset(n, NCSTYLE_BOLD|FUCKED_COLOR);
        cmvwprintw(n, sumline, START_COL, "☠☠☠ ");
      }else{
        cwattrset(n, GREEN_COLOR);
        cmvwprintw(n, sumline, START_COL, "up  ");
      }
    }
    uintmax_t io;
    io = bo->d->statdelta.sectors_read;
    io += bo->d->statdelta.sectors_written;
    io *= bo->d->logsec;
    // FIXME normalize according to timeq
    cwattrset(n, SELECTED_COLOR);
    // FIXME 'i' shows up only when there are fewer than 3 sigfigs
    // to the left of the decimal point...very annoying
    if(io){
      char qbuf[BPREFIXSTRLEN + 1];
      bprefix(io, 1, qbuf, 1);
      cmvwprintw(n, sumline, -1, "%7.7s", qbuf); // might chop off 'i'
    }else{
      cmvwprintw(n, sumline, -1, " no i/o");
    }
  }

  cwattrset(n, NCSTYLE_BOLD | DBORDER_COLOR);
  if(selected){
    cwattron(n, REVERSE);
  }
  if(line >= drawfromtop){
    ncplane_putwstr_yx(n, line, START_COL + 10 + 1, L"╭");
    cmvwhline(n, line, START_COL + 2 + 10, "─", cols - START_COL * 2 - 2 - 10);
    ncplane_putwstr_yx(n, line, cols - START_COL * 2, L"╮");
  }
  if(++line >= rows){
    return 1;
  }

  if(line >= drawfromtop){
    ncplane_putwstr_yx(n, line, START_COL + 10 + 1, L"│");
    print_blockbar(n, bo, line, START_COL + 10 + 2,
          cols - START_COL - 1, selected);
  }
  attr = NCSTYLE_BOLD | DBORDER_COLOR;
  cwattrset(n, attr);
  if(selected){
    cwattron(n, REVERSE);
  }
  if(line >= drawfromtop){
    ncplane_putwstr_yx(n, line, cols - START_COL * 2, L"│");
  }
  if(++line >= rows){
    return 2;
  }
  if(line >= drawfromtop){
    int c = cols - 80;
    ncplane_putwstr_yx(n, line, START_COL + 10 + 1, L"╰");
    ncplane_putwstr_yx(n, line, START_COL + 12, L"┤");
    ncplane_putwstr_yx(n, line, cols - 3 - c, L"├");
    if(c > 0){
      cmvwhline(n, line, cols - 3 - c + 1, "─", c);
    }
    ncplane_putwstr_yx(n, line, cols - START_COL * 2, L"╯");
  }
  ++line;
  return 3;
}

static void
adapter_box(const adapterstate* as, struct ncplane* nc, bool drawtop,
            bool drawbot, int rows){
//fprintf(stderr, "above: %d below: %d rows: %d\n", abovetop, belowend, rows);
  int current = as == get_current_adapter();
  int bcolor, hcolor, cols;
  int attrs;

  ncplane_dim_yx(nc, NULL, &cols);
  --cols;
  if(current){
    hcolor = UHEADING_COLOR; // plus NCSTYLE_BOLD
    bcolor = SELBORDER_COLOR;
    attrs = NCSTYLE_BOLD;
  }else{
    hcolor = UNHEADING_COLOR;;
    bcolor = UBORDER_COLOR;
    attrs = 0;
  }
  cwattrset(nc, attrs | bcolor);
//fprintf(stderr, "ABOVETOP: %d BELOWEND: %d name: %s\n", abovetop, belowend, as->c->name);
  bevel(nc, rows, cols, drawtop, drawbot);
  ncplane_set_bg_default(nc);
  if(drawtop){
    if(current){
      cwattron(nc, NCSTYLE_BOLD);
    }else{
      cwattroff(nc, NCSTYLE_BOLD);
    }
    cmvwprintw(nc, 0, 7, "%ls", L"[");
    compat_set_fg(nc, hcolor);
    ncplane_putstr(nc, as->c->ident);
    if(as->c->numa_node >= 0){
      cwprintw(nc, " [%d]", as->c->numa_node);
    }
    if(as->c->bandwidth){
      char buf[PREFIXSTRLEN + 1], dbuf[PREFIXSTRLEN + 1];

      if(as->c->demand){
        cwprintw(nc, " (%sbps to chip, %sbps (%ju%%) demanded)",
          qprefix(as->c->bandwidth, 1, buf, 1),
          qprefix(as->c->demand, 1, dbuf, 1),
          as->c->demand * 100 / as->c->bandwidth);
      }else{
        cwprintw(nc, " (%sbps to chip)",
          qprefix(as->c->bandwidth, 1, buf, 1));
      }
    }else if(as->c->bus != BUS_VIRTUAL && as->c->demand){
      char dbuf[PREFIXSTRLEN + 1];

      cwprintw(nc, " (%sbps demanded)", qprefix(as->c->demand, 1, dbuf, 1));
    }
    cwattron(nc, bcolor);
    cwprintw(nc, "]");
    cwattron(nc, NCSTYLE_BOLD);
    ncplane_cursor_move_yx(nc, 0, cols - 5);
    ncplane_putwstr(nc, as->expansion != EXPANSION_MAX ? L"[+]" : L"[-]");
    cwattron(nc, attrs);
  }
  if(drawbot){
    if(as->c->bus == BUS_PCIe){
      compat_set_fg(nc, bcolor);
      if(current){
        cwattron(nc, NCSTYLE_BOLD);
      }else{
        cwattroff(nc, NCSTYLE_BOLD);
      }
      cmvwprintw(nc, rows - 1, 6, "[");
      compat_set_fg(nc, hcolor);
      if(as->c->pcie.lanes_neg == 0){
        cwprintw(nc, "Onboard %04x:%02x.%02x.%x",
          as->c->pcie.domain, as->c->pcie.bus,
          as->c->pcie.dev, as->c->pcie.func);
      }else{
        cwprintw(nc, "PCI Express %04x:%02x.%02x.%x (x%u, gen %s)",
            as->c->pcie.domain, as->c->pcie.bus,
            as->c->pcie.dev, as->c->pcie.func,
            as->c->pcie.lanes_neg, pcie_gen(as->c->pcie.gen));
      }
      compat_set_fg(nc, bcolor);
      cwprintw(nc, "]");
    }
  }
}

// returns the number of lines printed (including borders)
// drawfromtop: direction in which we draw. if true, from the top.
static int
print_adapter_devs(struct ncplane* n, const adapterstate *as, bool drawfromtop){
  bool drawtop = true, drawbottom = true;
  // If the interface is down, we don't lead with the summary line
  const blockobj *cur;
  int printed = 0;
  long line;
  int p;

//fprintf(stderr, "%s [%d/%d] sel: %p selline: %d\n", as->c->name, rows, cols, as->selected, as->selline);
  if(as->expansion == EXPANSION_NONE){
    return 0;
  }
  // First, print the selected device (if there is one), and those above
  int rows, cols;
  ncplane_dim_yx(n, &rows, &cols);
//fprintf(stderr, "START WITH %p at %ld of %d\n", cur, line, rows);
  --rows;
  --cols;
  cur = as->selected;
  line = as->selline;
  while(cur && line >= drawfromtop){
    p = print_dev(n, as, cur, line, rows, cols, drawfromtop);
    printed += p;
    if( (cur = cur->prev) ){
      line -= device_lines(as->expansion, cur);
    }
//fprintf(stderr, "SELECTED MOVES TO %p %d (%ld/%d)\n", cur, drawfromtop, line, rows);
  }
  if(as->selected){
    line = as->selline + (long)device_lines(as->expansion, as->selected);
    cur = as->selected->next;
  }else{
    cur = as->bobjs;
    // if nothing was selected, we might have to clip at the top. check to see
    // if we're moving up. if so, run through all devices to get a total length,
    // and then move forward until we find the first visible one. begin
    // printing, in this case, at line 0. you've been clipped!
    line = drawfromtop;
    int totallines = 0;
    const blockobj* iter = cur;
    line = rows - 1;
    while(iter){
      totallines += device_lines(as->expansion, iter);
      line -= device_lines(as->expansion, iter);
      iter = iter->next;
    }
//fprintf(stderr, "total lines: %d line: %ld rows: %d\n", totallines, line, rows);
    if(line > 0){ // they'll all fit, huzzah
      line = 1;
    }else{
      drawtop = false;
      while(cur && (line + device_lines(as->expansion, cur) < 0)){
        line += device_lines(as->expansion, cur);
        cur = cur->next;
      }
    }
  }
//fprintf(stderr, "SELECTED CUR FORWARD %p %d (line %ld/%d rows)\n", cur, drawfromtop, line, rows);
  while(cur && line < rows - !drawfromtop){
    p = print_dev(n, as, cur, line, rows, cols, drawfromtop);
//fprintf(stderr, "ITERATING %d %p (%ld < %d) %d\n", p, cur, line, rows, drawfromtop);
    printed += p;
    if(line < 0){
      printed += line < -p ? -p : line;
    }
    line += p;
    cur = cur->next;
  }
//fprintf(stderr, "BASEPRINTED %d/%d through %ld for %s\n", printed, rows, line, as->c->name);
//fprintf(stderr, "PRINTED %d/%d through %ld for %s\n", printed, rows, line, as->c->name);
  if(line > rows - !drawfromtop){
    drawbottom = false;
    line = rows - !drawfromtop;
  }
  printed += drawtop + drawbottom; // top+bottom borders
  adapter_box(as, n, drawtop, drawbottom, printed);
  //assert(printed <= rows);
  return printed;
}

static int
redraw_adapter(struct nctablet* t, bool drawfromtop){
  struct ncplane* n = nctablet_plane(t);
  const adapterstate *as = nctablet_userptr(t);
  //ncplane_erase(n);
  int lines = print_adapter_devs(n, as, drawfromtop);
  if(lines < 0){
    return -1;
  }
  return lines;
}

// -------------------------------------------------------------------------
// -- splash API. splashes are displayed during long operations, especially
//    those requiring an external program.
// -------------------------------------------------------------------------
struct panel_state* show_splash(const wchar_t* msg){
  struct panel_state* ps;

  assert(!splash);
  if((ps = malloc(sizeof(*ps))) == NULL){
    return NULL;
  }
  memset(ps, 0, sizeof(*ps));
  // FIXME gross, clean all of this up
  if(new_display_panel(notcurses_stdplane(NC), ps, 3, wcslen(msg) + 4, NULL,
                       NULL, SPLASHBORDER_COLOR)){
    free(ps);
    return NULL;
  }
  int cols;
  ncplane_dim_yx(ps->p, NULL, &cols);
  cwattrset(ps->p, NCSTYLE_BOLD|SPLASHTEXT_COLOR);
  cmvwhline(ps->p, 1, 1, " ", cols - 2);
  cmvwhline(ps->p, 2, 1, " ", cols - 2);
  ncplane_putwstr_yx(ps->p, 2, 2, msg);
  cmvwhline(ps->p, 3, 1, " ", cols - 2);
  ncplane_move_yx(ps->p, 3, 3);
  screen_update();
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
static struct form_state*
create_form(const char* str, void (*fxn)(const char*), form_enum ftype, int scrolloff){
  struct form_state* fs;

  if( (fs = malloc(sizeof(*fs))) ){
    memset(fs, 0, sizeof(*fs));
    if((fs->boxstr = strdup(str)) == NULL){
      locked_diag("Couldn't create input dialog (%s?)", strerror(errno));
      free(fs);
      return NULL;
    }
    fs->scrolloff = scrolloff;
    fs->selectno = 1;
    fs->formtype = ftype;
    fs->fxn = fxn;
  }else{
    locked_diag("Couldn't create input dialog (%s?)", strerror(errno));
  }
  return fs;
}

static void
destroy_form_locked(struct form_state* fs){
  if(fs){
    int z;

    assert(fs == actform);
    ncplane_destroy(fs->p);
    fs->p = NULL;
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
    screen_update();
  }
}

static void
multiform_options(struct form_state *fs){
  const struct form_option *opstrs = fs->ops;
  struct ncplane* fsw = fs->p;
  int z, cols, selidx, maxz;

  assert(fs->formtype == FORM_MULTISELECT);
  ncplane_dim_yx(fsw, &maxz, &cols);
  cwattrset(fsw, FORMBORDER_COLOR);
  cwattron(fsw, NCSTYLE_BOLD);
  ncplane_putwstr_yx(fsw, 1, 1, L"╭");
  ncplane_putwstr_yx(fsw, 1, fs->longop + 4, L"╮");
  maxz -= 3;
  for(z = 1 ; z < maxz ; ++z){
    int op = ((z - 1) + fs->scrolloff) % fs->opcount;

    assert(op >= 0);
    assert(op < fs->opcount);
    cwattroff(fsw, NCSTYLE_BOLD);
    compat_set_fg(fsw, FORMBORDER_COLOR);
    if(fs->selectno >= z){
      cmvwprintw(fsw, z + 1, START_COL * 2, "%d", z);
    }else if(fs->selections >= z){
      cmvwprintw(fsw, z + 2, START_COL * 2, "%d", z);
    }
    if(z < fs->opcount + 1){
      cwattron(fsw, NCSTYLE_BOLD);
      compat_set_fg(fsw, FORMTEXT_COLOR);
      cmvwprintw(fsw, z + 1, START_COL * 2 + fs->longop + 4, "%-*.*s ",
                 fs->longop, fs->longop, opstrs[op].option);
      if(op == fs->idx){
        cwattron(fsw, REVERSE);
      }
      compat_set_fg(fsw, INPUT_COLOR);
      for(selidx = 0 ; selidx < fs->selections ; ++selidx){
        if(strcmp(opstrs[op].option, fs->selarray[selidx]) == 0){
          compat_set_fg(fsw, SELECTED_COLOR);
          break;
        }
      }
      cwprintw(fsw, "%-*.*s", cols - fs->longop * 2 - 9,
        cols - fs->longop * 2 - 9, opstrs[op].desc);
      cwattroff(fsw, REVERSE);
    }
  }
  cwattrset(fsw, FORMBORDER_COLOR);
  cwattron(fsw, NCSTYLE_BOLD);
  for(z = 0 ; z < fs->selections ; ++z){
    ncplane_putstr_yx(fsw, z >= fs->selectno ? 3 + z : 2 + z, 4, fs->selarray[z]);
  }
  ncplane_putwstr_yx(fsw, fs->selectno + 2, 1, L"╰");
  ncplane_putwstr_yx(fsw, fs->selectno + 2, fs->longop + 4, L"╯");
}

static void
check_options(struct form_state *fs){
  const struct form_option *opstrs = fs->ops;
  int z, cols, selidx, maxz;

  assert(fs->formtype == FORM_CHECKBOXEN);
  ncplane_dim_yx(fs->p, &maxz, &cols);
  maxz -= 3;
  cwattrset(fs->p, FORMBORDER_COLOR);
  cwattron(fs->p, NCSTYLE_BOLD);
  for(z = 1 ; z < maxz ; ++z){
    int op = ((z - 1) + fs->scrolloff) % fs->opcount;

    assert(op >= 0);
    assert(op < fs->opcount);
    cwattroff(fs->p, NCSTYLE_BOLD);
    compat_set_fg(fs->p, FORMBORDER_COLOR);
    if(z < fs->opcount + 1){
      wchar_t ballot = L'☐';

      cwattron(fs->p, NCSTYLE_BOLD);
      compat_set_fg(fs->p, FORMTEXT_COLOR);
      for(selidx = 0 ; selidx < fs->selections ; ++selidx){
        if(strcmp(opstrs[op].option, fs->selarray[selidx]) == 0){
          ballot = L'☒';
          break;
        }
      }
      if(op == fs->idx){
        cwattron(fs->p, REVERSE);
      }else{
        compat_set_fg(fs->p, INPUT_COLOR);
      }
      cmvwprintw(fs->p, z + 1, START_COL * 2, "%lc %-*.*s ",
        ballot, fs->longop, fs->longop, opstrs[op].option);
      cwprintw(fs->p, "%-*.*s", cols - fs->longop - 7,
        cols - fs->longop - 7, opstrs[op].desc);
      cwattroff(fs->p, REVERSE);
    }
  }
  cwattrset(fs->p, FORMBORDER_COLOR);
}

static void
form_options(struct form_state *fs){
  const struct form_option *opstrs = fs->ops;
  int z, cols, rows;

  if(fs->formtype != FORM_SELECT){
    return;
  }
  ncplane_dim_yx(fs->p, &rows, &cols);
  cwattron(fs->p, NCSTYLE_BOLD);
  for(z = 1 ; z < rows - 3 ; ++z){
    int op = ((z - 1) + fs->scrolloff) % fs->opcount;

    assert(op >= 0);
    assert(op < fs->opcount);
    compat_set_fg(fs->p, FORMTEXT_COLOR);
    cmvwprintw(fs->p, z + 1, START_COL * 2, "%-*.*s ",
      fs->longop, fs->longop, opstrs[op].option);
    if(op == fs->idx){
      cwattron(fs->p, REVERSE);
    }
    compat_set_fg(fs->p, INPUT_COLOR);
    cwprintw(fs->p, "%-*.*s", cols - fs->longop - 1 - START_COL * 4,
      cols - fs->longop - 1 - START_COL * 4, opstrs[op].desc);
    cwattroff(fs->p, REVERSE);
  }
}

#define FORM_Y_OFFSET 5

static struct panel_state *
raise_form_explication(struct ncplane* n, const char* text, int linesz){
  int cols, x, y, brk, tot;
  int linepre[linesz - 1];
  int linelen[linesz - 1];
  struct panel_state *ps;

  // There're two columns of padding surrounding the subwindow
  ncplane_dim_yx(n, NULL, &cols);
  --cols;
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
  ps = malloc(sizeof(*ps));
  assert(ps);
  int ncols;
  ncplane_dim_yx(n, NULL, &ncols);
  ps->p = ncplane_new(notcurses_stdplane(NC), y + 3, cols, linesz - (y + 2), ncols - cols, NULL, NULL);
  assert(ps->p);
  cwbkgd(ps->p);
  cwattrset(ps->p, FORMBORDER_COLOR);
  bevel_all(ps->p);
  compat_set_fg(ps->p, FORMTEXT_COLOR);
  do{
    ncplane_printf_yx(ps->p, y + 1, 1, "%.*s", linelen[y], text + linepre[y]);
  }while(y--);
  screen_update();
  return ps;
}

void raise_multiform(const char *str, void (*fxn)(const char *, char **, int, int),
    struct form_option *opstrs, int ops, int defidx,
    int selectno, char **selarray, int selections, const char *text,
    int scrollidx){
  size_t longop, longdesc;
  struct form_state *fs;
  int cols, rows;
  int x, y;

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
  ncplane_dim_yx(notcurses_stdplane(NC), &y, &x);
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
  if((fs = create_form(str, NULL, FORM_MULTISELECT, scrollidx)) == NULL){
    return;
  }
  fs->mcb = fxn;
  if((fs->p = ncplane_new(notcurses_stdplane(NC), rows, cols, FORM_Y_OFFSET, x - cols, NULL, NULL)) == NULL){
    locked_diag("Couldn't create form window, uh-oh");
    free_form(fs);
    return;
  }
  cwbkgd(fs->p);
  // FIXME adapt for scrolling (default might be off-window at beginning)
  if((fs->idx = defidx) < 0){
    fs->idx = defidx = 0;
  }
  fs->opcount = ops;
  fs->selarray = selarray;
  fs->selections = selections;
  cwattroff(fs->p, NCSTYLE_BOLD);
  compat_set_fg(fs->p, FORMBORDER_COLOR);
  bevel_all(fs->p);
  cwattron(fs->p, NCSTYLE_BOLD);
  cmvwprintw(fs->p, 0, cols - strlen(fs->boxstr) - 4, "%s", fs->boxstr);
  ncplane_putwstr_yx(fs->p, rows - 1, cols - wcslen(ESCSTR) - 1, ESCSTR);
#undef ESCSTR
  cwattroff(fs->p, NCSTYLE_BOLD);
  fs->longop = longop;
  fs->ops = opstrs;
  fs->selectno = selectno;
  multiform_options(fs);
  fs->extext = raise_form_explication(notcurses_stdplane(NC), text, FORM_Y_OFFSET);
  actform = fs;
  ncplane_move_top(fs->p);
  screen_update();
}

// A collection of checkboxes
static void
raise_checkform(const char* str, void (*fxn)(const char*, char**, int, int),
                struct form_option* opstrs, int ops, int defidx,
                char** selarray, int selections, const char* text,
                int scrollidx){
  size_t longop, longdesc;
  struct form_state* fs;
  int cols, rows;
  int x, y;

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
  notcurses_term_dim_yx(NC, &y, &x);
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
  if((fs = create_form(str, NULL, FORM_CHECKBOXEN, scrollidx)) == NULL){
    return;
  }
  fs->mcb = fxn;
  fs->p = ncplane_new(notcurses_stdplane(NC), rows, cols, FORM_Y_OFFSET, x - cols, NULL, NULL);
  if(fs->p == NULL){
    locked_diag("Couldn't create form panel, uh-oh");
    free_form(fs);
    return;
  }
  struct ncplane* fsw = fs->p;
  cwbkgd(fsw);
  // FIXME adapt for scrolling (default might be off-window at beginning)
  if((fs->idx = defidx) < 0){
    fs->idx = defidx = 0;
  }
  fs->opcount = ops;
  fs->selarray = selarray;
  fs->selections = selections;
  cwattroff(fsw, NCSTYLE_BOLD);
  compat_set_fg(fsw, FORMBORDER_COLOR);
  bevel_all(fsw);
  cwattron(fsw, NCSTYLE_BOLD);
  cmvwprintw(fsw, 0, cols - strlen(fs->boxstr) - 2, "%s", fs->boxstr);
  int nrows;
  ncplane_dim_yx(fsw, &nrows, NULL);
  ncplane_putwstr_yx(fsw, nrows - 1, cols - wcslen(ESCSTR) - 1, ESCSTR);
#undef ESCSTR
  cwattroff(fsw, NCSTYLE_BOLD);
  fs->longop = longop;
  fs->ops = opstrs;
  check_options(fs);
  fs->extext = raise_form_explication(notcurses_stdplane(NC), text, FORM_Y_OFFSET);
  actform = fs;
  screen_update();
}

// -------------------------------------------------------------------------
// - select type form, for single choice from among a set
// -------------------------------------------------------------------------
void raise_form(const char* str, void (*fxn)(const char*),
                struct form_option* opstrs, int ops, int defidx,
                const char* text){
  size_t longop, longdesc;
  struct form_state *fs;
  int cols, rows;
  int x, y;

  if(opstrs == NULL || !ops){
    locked_diag("Passed empty %u-option string table", ops);
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
  notcurses_term_dim_yx(NC, &y, &x);
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
  if((fs = create_form(str, fxn, FORM_SELECT, 0)) == NULL){
    return;
  }
  fs->p = ncplane_new(notcurses_stdplane(NC), rows, cols + START_COL * 4, FORM_Y_OFFSET, x - cols - 4, NULL, NULL);
  if(fs->p == NULL){
    locked_diag("Couldn't create form panel, uh-oh");
    free_form(fs);
    return;
  }
  cwbkgd(fs->p);
  // FIXME adapt for scrolling (default might be off-window at beginning)
  if((fs->idx = defidx) < 0){
    fs->idx = defidx = 0;
  }
  fs->opcount = ops;
  cwattroff(fs->p, NCSTYLE_BOLD);
  compat_set_fg(fs->p, FORMBORDER_COLOR);
  bevel_all(fs->p);
  cwattron(fs->p, NCSTYLE_BOLD);
  cmvwprintw(fs->p, 0, cols - strlen(fs->boxstr), "%s", fs->boxstr);
  ncplane_putwstr_yx(fs->p, rows - 1, cols - wcslen(L"⎋esc returns"), L"⎋esc returns");
  cwattroff(fs->p, NCSTYLE_BOLD);
  fs->longop = longop;
  fs->ops = opstrs;
  form_options(fs);
  actform = fs;
  fs->extext = raise_form_explication(notcurses_stdplane(NC), text, FORM_Y_OFFSET);
  screen_update();
}

// -------------------------------------------------------------------------
// - string type form, for generic input
// -------------------------------------------------------------------------
static void
form_string_options(struct form_state* fs){
  struct ncplane* n = fs->p;
  int cols;

  if(fs->formtype != FORM_STRING_INPUT){
    return;
  }
  ncplane_dim_yx(n, NULL, &cols);
  ncplane_set_styles(n, NCSTYLE_BOLD);
  compat_set_fg(n, FORMTEXT_COLOR);
  cmvwhline(n, 1, 1, " ", cols - 2);
  cmvwprintw(n, 1, START_COL, "%-*.*s: ", fs->longop, fs->longop, fs->inp.prompt);
  compat_set_fg(n, INPUT_COLOR);
  cwprintw(n, "%.*s", cols - fs->longop - 2 - 2, fs->inp.buffer);
  ncplane_off_styles(n, NCSTYLE_BOLD);
}

void raise_str_form(const char* str, void (*fxn)(const char*),
                    const char* def, const char* text){
  struct form_state* fs;
  int cols;
  int x, y;

  assert(str && fxn);
  if(actform){
    locked_diag("An input dialog is already active");
    return;
  }
  if((fs = create_form(str, fxn, FORM_STRING_INPUT, 0)) == NULL){
    return;
  }
  fs->longop = strlen(str);
  cols = fs->longop + 40 + 1; // FIXME? 40 for input currently
  notcurses_term_dim_yx(NC, &y, &x);
  assert(x >= cols + 3);
  assert(y >= 3);
  fs->p = ncplane_new(notcurses_stdplane(NC), 3, cols + START_COL * 2, FORM_Y_OFFSET, x - cols - 3, NULL, NULL);
  if(fs->p == NULL){
    locked_diag("Couldn't create form panel, uh-oh");
    free_form(fs);
    return;
  }
  cwattroff(fs->p, NCSTYLE_BOLD);
  compat_set_fg(fs->p, FORMBORDER_COLOR);
  bevel_all(fs->p);
  cwattron(fs->p, NCSTYLE_BOLD);
  cmvwprintw(fs->p, 0, cols - strlen(fs->boxstr), "%s", fs->boxstr);
  ncplane_putwstr_yx(fs->p, 2, cols - wcslen(L"⎋esc returns"), L"⎋esc returns");
  fs->inp.prompt = fs->boxstr;
  def = def ? def : "";
  fs->inp.buffer = strdup(def);
  form_string_options(fs);
  actform = fs;
  fs->extext = raise_form_explication(notcurses_stdplane(NC), text, FORM_Y_OFFSET);
  screen_update();
}

static const struct form_option common_fsops[] = {
  {
    .option = "ro",
    .desc = "Read-only",
  //},{ // this is the default
  //  .option = "rw",
  //  .desc = "Read-write",
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
ops_table(int *count, const char *match, int *defidx, char ***selarray, int *selections,
    const struct form_option *flags, unsigned fcount){
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
    if(match && strcmp(match, fo[z].option) == 0){
      int zz;

      *defidx = z;
      for(zz = 0 ; selections && zz < *selections ; ++zz){
        if(strcmp(key, (*selarray)[zz]) == 0){
          free((*selarray)[zz]);
          (*selarray)[zz] = NULL;
          if(zz < *selections - 1){
            memmove(&(*selarray)[zz], &(*selarray)[zz + 1], sizeof(**selarray) * (*selections - 1 - zz));
          }
          --*selections;
          zz = -1;
          break;
        }
      }
      if(zz >= *selections){
        typeof(*selarray) tmp;

        if((tmp = realloc(*selarray, sizeof(*selarray) * (*selections + 1))) == NULL){
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

static char forming_targ[PATH_MAX + 1];

static void
mountop_callback(const char *op, char **selarray, int selections, int scrollidx){
  struct form_option *ops_agg;
  int opcount, defidx;
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
    locked_diag("Media is not loaded on %s", b->d->name);
    return;
  }
  if(strcmp(op, "") == 0){
    unsigned mntos = 0;

    while(selections--){
      mntos |= flag_for_mountop(selarray[selections]);
    }
    if(blockobj_unpartitionedp(b)){
      mmount(b->d, forming_targ, mntos, NULL);
    }else if(blockobj_emptyp(b)){
      locked_diag("%s is not a partition, aborting.\n", b->zone->p->name);
    }else{
      mmount(b->zone->p, forming_targ, mntos, NULL);
    }
    return;
  }
  ops_agg = NULL;
  opcount = 0;
  defidx = 1;
  if((ops_agg = ops_table(&opcount, op, &defidx, &selarray, &selections,
      common_fsops, sizeof(common_fsops) / sizeof(*common_fsops))) == NULL){
    // FIXME free
    return;
  }
  raise_checkform("set mount options", mountop_callback, ops_agg,
    opcount, defidx, selarray, selections, MOUNTOPS_TEXT, scrollidx);
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
  if((unsigned)snprintf(forming_targ, sizeof(forming_targ), "%s%s", growlight_target, path) >= sizeof(forming_targ)){
    locked_diag("Bad mountpoint: %s", path);
    return;
  }
  if((b = get_selected_blockobj()) == NULL){
    locked_diag("Must select a filesystem to mount");
    return;
  }
  if(selected_unloadedp()){
    locked_diag("Media is not loaded on %s", b->d->name);
    return;
  }
  if(blockobj_emptyp(b)){
    locked_diag("%s is not a partition, aborting.\n", b->zone->p->name);
    return;
  }else{
    int opcount = 0, defidx = 0;
    ops_agg = NULL;
    char **selarray = NULL;
    int selections = 0;

    if((ops_agg = ops_table(&opcount, NULL, &defidx, &selarray, &selections,
        common_fsops, sizeof(common_fsops) / sizeof(*common_fsops))) == NULL){
      // FIXME free
      return;
    }
    raise_checkform("set mount options", mountop_callback, ops_agg,
      opcount, defidx, selarray, selections, MOUNTOPS_TEXT, scrollidx);
    return;
  }
  locked_diag("I'm confused. Aborting.\n");
}

// -------------------------------------------------------------------------
// - filesystem type form, for new filesystem creation
// -------------------------------------------------------------------------
static struct form_option *
fs_table(int *count, const char *match, int *defidx){
  struct form_option* fo = NULL;
  struct form_option* tmp;
  pttable_type* types;
  int z;

  *defidx = -1;
  if((types = get_fs_types(count)) == NULL){
    return NULL;
  }
  for(z = 0 ; z < *count ; ++z){
    char *key, *desc;

    if((key = strdup(types[z].name)) == NULL){
      goto err;
    }
    if(match){
      if(strcmp(key, match) == 0){
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
    if((tmp = realloc(fo, sizeof(*fo) * (*count + 1))) == NULL){
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
  if(actform){
    ncplane_move_top(actform->p);
    if(actform->extext){
      ncplane_move_top(actform->extext->p);
    }
  }
  screen_update();
}

static int
fs_do_internal(device *d, const char *fst, const char *name){
  struct panel_state *ps;
  int r;

  if(!mkfs_safe_p(d)){
    return -1;
  }
  ps = show_splash(L"Creating filesystem...");
  r = make_filesystem(d, fst, name);
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
  if(!(b = get_selected_blockobj())){
    locked_diag("Lost selection while targeting");
    destroy_fs_forms();
    return;
  }
  if(selected_unloadedp()){
    locked_diag("%s is unloaded, aborting.\n", b->d->name);
  }else if(selected_unpartitionedp()){
    r = fs_do_internal(b->d, pending_fstype, name);
  }else if(selected_partitionp()){
    r = fs_do_internal(b->zone->p, pending_fstype, name);
  }else if(selected_emptyp()){
    locked_diag("Cannot make filesystems in empty space");
  }
  if(r == 0){
    locked_diag("Successfully created %s filesystem", pending_fstype);
  }
  destroy_fs_forms();
}

static void
fs_named_callback(const char *name){
  if(name == NULL){
    struct form_option *ops_fs;
    int opcount, defidx;

    if((ops_fs = fs_table(&opcount, pending_fstype, &defidx)) == NULL){
      destroy_fs_forms();
      return;
    }
    raise_form("select a filesystem type", fs_callback, ops_fs,
        opcount, defidx, FSTYPE_TEXT);
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
  raise_str_form("enter filesystem name", fs_named_callback, NULL, FSNAME_TEXT);
}

// -------------------------------------------------------------------------
// -- end filesystem type form
// -------------------------------------------------------------------------

// A NULL return is only an error if *count is set to -1. If *count is set to
// 0, it simply means this partition table type doesn't have type tags.
static struct form_option *
ptype_table(const device *d, int *count, int match, int *defidx){
  struct form_option *fo = NULL, *tmp;
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
    char *key, *desc;

    if(!ptype_supported(pttable, pt)){
      continue;
    }
    if((key = malloc(KEYSIZE)) == NULL){
      goto err;
    }
    if((desc = strdup(pt->name)) == NULL){
      free(key);
      goto err;
    }
    if(snprintf(key, KEYSIZE, "%04x", pt->code) >= (int)KEYSIZE){
      locked_diag("Couldn't convert key 0x%x", pt->code);
      free(key);
      free(desc);
      goto err;
    }
    if((tmp = realloc(fo, sizeof(*fo) * (*count + 1))) == NULL){
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
static uintmax_t pending_fsect, pending_lsect; // set by partition spec callback
static char *pending_spec;    // set by spec callback; heap-allocated

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
    locked_diag("Media is not loaded on %s", b->d->name);
    return NULL;
  }
  if(selected_unpartitionedp()){
    locked_diag("Partition creation requires a partition table");
    return NULL;
  }
  if(b->zone->rep == REP_METADATA){
    locked_diag("Remove partition table on %s to reclaim metadata", b->d->name);
    return NULL;
  }
  if(b->zone->p){
    locked_diag("Partition %s exists; remove it first", b->zone->p->name);
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
    raise_str_form("enter partition spec", psectors_callback,
        pending_spec, PSPEC_TEXT);
    return;
  }
  if((b = partition_base_p()) == NULL){
    return;
  }
  n = name;
  memset(&ps, 0, sizeof(ps));
  if((wcs = mbsrtowcs(NULL, &n, 0, &ps)) == (size_t)-1){
    locked_diag("Couldn't interpret multibyte '%s'", name);
    cleanup_new_partition();
    return;
  }
  if((wstr = malloc(sizeof(*wstr) * (wcs + 1))) == NULL){
    locked_diag("Couldn't allocate wide string");
    cleanup_new_partition();
    return;
  }
  n = name;
  memset(&ps, 0, sizeof(ps));
  if(mbsrtowcs(wstr, &n, wcs + 1, &ps) != wcs){
    locked_diag("Error converting multibyte '%s'", name);
    cleanup_new_partition();
    return;
  }
  sps = show_splash(L"Creating partition...");
  // Cannot reference b further until we've had a callback
  r = add_partition(b->d, wstr, pending_fsect, pending_lsect, pending_ptype);
  if(sps){
    kill_splash(sps);
  }
  free(wstr);
  cleanup_new_partition();
  if(!r){
    if(strcmp(name, "")){
      locked_diag("Created new partition %s on %s\n", name, b->d->name);
    }else{
      locked_diag("Created new unnamed partition on %s\n", b->d->name);
    }
  }
}

static int
lex_part_spec(const char *psects, zobj *z, size_t sectsize,
      uintmax_t *fsect, uintmax_t *lsect){
  unsigned long long ull;
  const char *col, *pct;
  char *el;

  // If we have a percent, it must be the last character, and there may
  // not be a colon present, and the value must be between 1 and 100,
  // inclusive, and the value must be an integer.
  if( (pct = strchr(psects, '%')) ){
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
    if((ul = strtoul(psects, &el, 10)) > 100){
      return -1;
    }
    *fsect = z->fsector;
    *lsect = ((z->lsector - z->fsector) * ul) / 100 + *fsect;
    return 0;
  }else if( (col = strchr(psects, ':')) ){
    unsigned long long ull2;

    if(*psects == '-'){ // reject negative numbers
      locked_diag("Not a number: %s", psects);
      return -1;
    }
    if((ull = strtoull(psects, &el, 0)) == ULLONG_MAX){
      locked_diag("Not a number: %s", psects);
      return -1;
    }
    if(el != col){
      locked_diag("Invalid delimiter: %s", psects);
      return -1;
    }
    ++el;
    ++col;
    if(*col == '-'){ // reject negative numbers
      locked_diag("Not a number: %s", psects);
      return -1;
    }
    if((ull2 = strtoull(col, &el, 0)) == ULLONG_MAX){
      locked_diag("Not a number: %s", col);
      return -1;
    }
    if(*el){
      locked_diag("Not a number: %s", col);
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
    locked_diag("Not a number: %s", psects);
    return -1;
  }
  if((ull = strtoull(psects, &el, 0)) == ULLONG_MAX && errno == ERANGE){
    locked_diag("Not a number: %s", psects);
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
    locked_diag("%llu is not a multiple of %zu", ull, sectsize);
    return -1;
  }
  ull /= sectsize;
  if(ull > (z->lsector - z->fsector + 1)){
    locked_diag("There are only %ju sectors available\n", z->lsector - z->fsector);
    return -1;
  }
  *fsect = z->fsector;
  *lsect = z->fsector + ull - 1;
  return 0;
}

static void
psectors_callback(const char *psects){
  uintmax_t fsect, lsect;
  struct panel_state *ps;
  blockobj *b;
  int r;

  if((b = partition_base_p()) == NULL){
    return;
  }
  if(psects == NULL){ // go back to partition type
    struct form_option *ops_ptype;
    int opcount, defidx;

    if((ops_ptype = ptype_table(b->d, &opcount, pending_ptype, &defidx)) == NULL){
      if(opcount == 0){
        raise_str_form("enter partition spec", psectors_callback,
            pending_spec ? pending_spec : "100%", PSPEC_TEXT);
        return;
      }
      cleanup_new_partition();
      return;
    }
    raise_form("select a partition type", ptype_callback, ops_ptype,
        opcount, defidx, PARTTYPE_TEXT);
    return;
  }
  pending_spec = strdup(psects);
  if(lex_part_spec(psects, b->zone, b->d->logsec, &fsect, &lsect)){
    locked_diag("Not a valid partition spec: \"%s\"\n", psects);
    raise_str_form("enter partition spec", psectors_callback,
        psects, PSPEC_TEXT);
    return;
  }
  if(partitions_named_p(b->d)){
    pending_spec = strdup(psects);
    pending_fsect = fsect;
    pending_lsect = lsect;
    raise_str_form("enter partition name", ptype_name_callback,
        NULL, PNAME_TEXT);
    return;
  }
  ps = show_splash(L"Creating partition...");
  r = add_partition(b->d, NULL, fsect, lsect, pending_ptype);
  if(ps){
    kill_splash(ps);
  }
  cleanup_new_partition();
  if(!r){
    locked_diag("Created new partition on %s\n", b->d->name);
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
  if(((pt = strtoul(pty, &pend, 16)) == ULONG_MAX && errno == ERANGE) || *pend){
    locked_diag("Bad partition type selection: %s", pty);
    cleanup_new_partition();
    return;
  }
  pending_ptype = pt;
  raise_str_form("enter partition spec", psectors_callback,
      pending_spec ? pending_spec : "100%", PSPEC_TEXT);
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
  if((ops_ptype = ptype_table(b->d, &opcount, -1, &defidx)) == NULL){
    if(opcount == 0){
      raise_str_form("enter partition spec", psectors_callback,
          pending_spec ? pending_spec : "100%", PSPEC_TEXT);
      return;
    }
    return;
  }
  raise_form("select a partition type", ptype_callback, ops_ptype, opcount,
      defidx, PARTTYPE_TEXT);
}

// -------------------------------------------------------------------------
// -- end partition type form
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// - partition nctabletype form, for new partition table creation
// -------------------------------------------------------------------------
static inline int
partition_table_makeablep(const blockobj *b){
  if(!b){
    locked_diag("Lost selection while choosing table type");
    return 0;
  }
  if(blockobj_unloadedp(b)){
    locked_diag("Media is not loaded on %s", b->d->name);
    return 0;
  }
  if(!selected_unpartitionedp()){
    locked_diag("Partition table exists on %s", b->d->name);
    return 0;
  }
  if(b->d->layout != LAYOUT_NONE){
    locked_diag("Will not create partition table on %s", b->d->name);
    return 0;
  }
  if(b->d->mnttype){
    locked_diag("Filesystem signature exists on %s", b->d->name);
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
  b = get_selected_blockobj();
  if(!b){
    locked_diag("Lost selection while choosing table type");
    return;
  }
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

static const controller *
get_current_controller(void){
  adapterstate* as = get_current_adapter();
  if(as){
    return as->c;
  }
  return NULL;
}

static int
select_adapter_dev(adapterstate* as, blockobj* bo, int delta){
  assert(bo != as->selected);
  if((as->selected = bo) == NULL){
    as->selline = -1;
  }else{
    as->selline += delta;
  }
  return 0;
}

static void
deselect_adapter_locked(void){
  adapterstate* as;

  if((as = get_current_adapter()) == NULL){
    return;
  }
  if(as->selected == NULL){
    return;
  }
  select_adapter_dev(as, NULL, 0);
}

static void
detail_fs(struct ncplane* hw, const device* d, int row){
  if(d->mnttype){
    char buf[BPREFIXSTRLEN + 1];

    ncplane_off_styles(hw, NCSTYLE_BOLD);
    const char *size = d->mntsize ? bprefix(d->mntsize, 1, buf, 1) : "";
    cmvwprintw(hw, row, START_COL, "%*s%c ",
        BPREFIXFMT(size), d->mntsize ? 'B' : ' ');
    ncplane_on_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "%s%s", d->label ? "" : "unlabeled ", d->mnttype);
    if(d->label){
      ncplane_off_styles(hw, NCSTYLE_BOLD);
      cwprintw(hw, " %lc%s%lc", L'“', d->label, L'”');
      ncplane_on_styles(hw, NCSTYLE_BOLD);
    }
    cwprintw(hw, "%s", d->mnt.count ? " at " : "");
    ncplane_off_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "%s", d->mnt.count ? d->mnt.list[0] : "");
    ncplane_on_styles(hw, NCSTYLE_BOLD);
  }
}

// One must not call diag() from any function called by update_details(), or
// else you will get one of a deadlock or a stack overflow due to corecursion.
static int
update_details(struct ncplane* hw){
  const controller* c = get_current_controller();
  char buf[BPREFIXSTRLEN + 1];
  int cols, rows, curcol, n;
  const char* pttype;
  const blockobj* b;
  const device* d;

  if(c == NULL){
    return 0; // FIXME hide thyself!
  }
  ncplane_dim_yx(hw, &rows, &cols);
  ncplane_cursor_yx(hw, NULL, &curcol);
  if(cols < START_COL * 2){
    return 0;
  }
  if(rows == 0){
    return 0;
  }
  for(n = 1 ; n < rows - 1 ; ++n){
    cmvwhline(hw, n, START_COL, " ", cols - 2);
  }
  compat_set_fg_all(hw, SUBDISPLAY_ATTR);
  cmvwprintw(hw, 1, START_COL, "%-*.*s", cols - 2, cols - 2, c->name);
  if(rows == 1){
    return 0;
  }
  if(c->bus == BUS_VIRTUAL){
    cmvwprintw(hw, 2, START_COL, "%-*.*s", cols - 2, cols - 2, "No details available");
  }else{
    cmvwprintw(hw, 2, START_COL, "Firmware: ");
    ncplane_off_styles(hw, NCSTYLE_BOLD);
    ncplane_putstr(hw, c->fwver ? c->fwver : "Unknown");
    ncplane_on_styles(hw, NCSTYLE_BOLD);
    ncplane_putstr(hw, " BIOS: ");
    ncplane_off_styles(hw, NCSTYLE_BOLD);
    ncplane_putstr(hw, c->biosver ? c->biosver : "Unknown");
    ncplane_on_styles(hw, NCSTYLE_BOLD);
    ncplane_putstr(hw, " Load: ");
    qprefix(c->demand, 1, buf, 1);
    ncplane_off_styles(hw, NCSTYLE_BOLD);
    ncplane_putstr(hw, buf);
    ncplane_putstr(hw, "bps");
    ncplane_on_styles(hw, NCSTYLE_BOLD);
  }
  if((b = get_selected_blockobj()) == NULL){
    return 0;
  }
  d = b->d;
  assert(d);
  if(d->layout == LAYOUT_NONE){
    const char *sn = d->blkdev.serial;

    cmvwprintw(hw, 3, START_COL, "%s: ", d->name);
    ncplane_off_styles(hw, NCSTYLE_BOLD);
    ncplane_putstr(hw, d->model ? d->model : "n/a");
    ncplane_putstr(hw, d->revision ? d->revision : "");
    ncplane_on_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, " (%sB) S/N: ", bprefix(d->size, 1, buf, 1));
    ncplane_off_styles(hw, NCSTYLE_BOLD);
    ncplane_putstr(hw, sn ? sn : "n/a");
    ncplane_on_styles(hw, NCSTYLE_BOLD);
    if(cols - curcol > 13){
      cwprintw(hw, " WC%lc WRV%lc RO%lc",
          d->blkdev.wcache ? L'+' : L'-',
          d->blkdev.rwverify == RWVERIFY_SUPPORTED_ON ? L'+' :
          d->blkdev.rwverify == RWVERIFY_SUPPORTED_OFF ? L'-' : L'x',
          d->roflag ? L'+' : L'-');
    }
    assert(d->physsec <= 4096);
    cmvwprintw(hw, 4, START_COL, "Sectors: ");
    ncplane_off_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "%ju ", d->size / (d->logsec ? d->logsec : 1));
    ncplane_on_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "(");
    ncplane_off_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "%uB ", d->logsec);
    ncplane_on_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "logical / ");
    ncplane_off_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "%uB ", d->physsec);
    ncplane_on_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "physical) %s",
    transport_str(d->blkdev.transport));
    if(transport_bw(d->blkdev.transport)){
      uintmax_t transbw = transport_bw(d->blkdev.transport);
      cwprintw(hw, " (");
      ncplane_off_styles(hw, NCSTYLE_BOLD);
      // FIXME throws -Wformat-truncation on gcc9
      cwprintw(hw, "%sbps", qprefix(transbw, 1, buf, 1));
      ncplane_on_styles(hw, NCSTYLE_BOLD);
      cwprintw(hw, ")");
    }
  }else{
    cmvwprintw(hw, 3, START_COL, "%s: %s %s (%s) RO%lc", d->name,
          d->model ? d->model : "n/a",
          d->revision ? d->revision : "n/a",
          bprefix(d->size, 1, buf, 1),
          d->roflag ? L'+' : L'-');
    if(d->layout == LAYOUT_MDADM){
      cwprintw(hw, " Stride: ");
      ncplane_off_styles(hw, NCSTYLE_BOLD);
      if(d->mddev.stride == 0){
        ncplane_putstr(hw, "n/a");
      }else{
        cwprintw(hw, "%sB", bprefix(d->mddev.stride, 1, buf, 1));
      }
      ncplane_on_styles(hw, NCSTYLE_BOLD);
      cwprintw(hw, " SWidth: ");
      ncplane_off_styles(hw, NCSTYLE_BOLD);
      if(d->mddev.swidth == 0){
        ncplane_putstr(hw, "n/a");
      }else{
        cwprintw(hw, "%u", d->mddev.swidth);
      }
      ncplane_on_styles(hw, NCSTYLE_BOLD);
    }
    assert(d->physsec <= 4096);
    cmvwprintw(hw, 4, START_COL, "Sectors: ");
    ncplane_off_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "%ju ", d->size / (d->logsec ? d->logsec : 1));
    ncplane_on_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "(");
    ncplane_off_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "%uB ", d->logsec);
    ncplane_on_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "logical / ");
    ncplane_off_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "%uB ", d->physsec);
    ncplane_on_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "physical)");
  }
  cmvwprintw(hw, 5, START_COL, "Partitioning: ");
  ncplane_off_styles(hw, NCSTYLE_BOLD);
  pttype = (d->layout == LAYOUT_NONE ? d->blkdev.pttable ? d->blkdev.pttable : "none" :
      d->layout == LAYOUT_MDADM ? d->mddev.pttable ? d->mddev.pttable : "none" :
      d->layout == LAYOUT_DM ? d->dmdev.pttable ? d->dmdev.pttable : "none" :
      "n/a");
  cwprintw(hw, "%s", pttype);
  ncplane_on_styles(hw, NCSTYLE_BOLD);
  ncplane_putstr(hw, " I/O scheduler: ");
  ncplane_off_styles(hw, NCSTYLE_BOLD);
  ncplane_putstr(hw, d->sched ? d->sched : "custom");
  ncplane_on_styles(hw, NCSTYLE_BOLD);
  if(blockobj_unloadedp(b)){
    cmvwprintw(hw, 6, START_COL, "Media is not loaded");
    return 0;
  }
  if(blockobj_unpartitionedp(b)){
    char ubuf[BPREFIXSTRLEN + 1];

    bprefix(d->size, 1, ubuf, 1);
    ncplane_off_styles(hw, NCSTYLE_BOLD);
    cmvwprintw(hw, 6, START_COL, "%*sB ", BPREFIXFMT(ubuf));
    ncplane_on_styles(hw, NCSTYLE_BOLD);
    cwprintw(hw, "%s", "unpartitioned media");
    detail_fs(hw, b->d, 7);
    return 0;
  }
  if(b->zone){
    char align[BPREFIXSTRLEN + 1];
    char zbuf[BPREFIXSTRLEN + 1];

    if(b->zone->p){
      assert(b->zone->p->layout == LAYOUT_PARTITION);
      bprefix(b->zone->p->partdev.alignment, 1, align, 1);
      // FIXME limit length!
      bprefix(d->logsec * (b->zone->lsector - b->zone->fsector + 1),1, zbuf, 1);
      ncplane_off_styles(hw, NCSTYLE_BOLD);
      cmvwprintw(hw, 6, START_COL, "%*sB ", BPREFIXFMT(zbuf));
      ncplane_on_styles(hw, NCSTYLE_BOLD);
      cwprintw(hw, "P%lc%lc ", subscript((b->zone->p->partdev.pnumber % 100 / 10)),
          subscript((b->zone->p->partdev.pnumber % 10)));
      ncplane_off_styles(hw, NCSTYLE_BOLD);
      cwprintw(hw, "%ju", b->zone->fsector);
      ncplane_on_styles(hw, NCSTYLE_BOLD);
      cwprintw(hw, "→");
      ncplane_off_styles(hw, NCSTYLE_BOLD);
      cwprintw(hw, "%ju ", b->zone->lsector);
      ncplane_on_styles(hw, NCSTYLE_BOLD);
      ncplane_putstr(hw, b->zone->p->name);
      ncplane_off_styles(hw, NCSTYLE_BOLD);
      if(b->zone->p->partdev.pname){
        cwprintw(hw, " “%ls” ", b->zone->p->partdev.pname);
      }else{
        cwprintw(hw, " (%s) ", "unnamed");
      }
      ncplane_on_styles(hw, NCSTYLE_BOLD);
                        if(curcol <= cols - 2 - 4){
        cwprintw(hw, "%04x", get_code_specific(pttype, b->zone->p->partdev.ptype));
      }
      if(curcol <= cols - 2 - 11){
        cwprintw(hw, " %sB align", align);
      }
      detail_fs(hw, b->zone->p, 7);
    }else{
      // FIXME print alignment for unpartitioned space as well,
      // but not until we implement zones in core (bug 252)
      // or we'll need recreate alignment() etc here
      ncplane_off_styles(hw, NCSTYLE_BOLD);
      bprefix(d->logsec * (b->zone->lsector - b->zone->fsector + 1), 1, zbuf, 1);
      cmvwprintw(hw, 6, START_COL, "%*sB ", BPREFIXFMT(zbuf));
      ncplane_on_styles(hw, NCSTYLE_BOLD);
      ncplane_off_styles(hw, NCSTYLE_BOLD);
      cwprintw(hw, "%ju", b->zone->fsector);
      ncplane_on_styles(hw, NCSTYLE_BOLD);
      cwprintw(hw, "→");
      ncplane_off_styles(hw, NCSTYLE_BOLD);
      cwprintw(hw, "%ju ", b->zone->lsector);
      ncplane_on_styles(hw, NCSTYLE_BOLD);
      cwprintw(hw, "%s ", b->zone->rep == REP_METADATA ?
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
  L"                              'H': toggle this help display",
  L"'v': view selection details   'D': view recent diagnostics",
  L"'E': view mounts / targets    'z': modify aggregate",
  L"'A': create aggregate         'Z': destroy aggregate",
  L"'-': collapse adapter         '+': expand adapter",
  L"'k'/↑: navigate up            'j'/↓: navigate down",
  L"PageUp: previous adapter      PageDown: next adapter",
  L"'/': search                   'p': configure loop device",
  NULL
};

static const wchar_t *helps_block[] = {
  L"'h'/←: navigate left          'l'/→: navigate right",
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
helpstrs(struct ncplane* n){
  const wchar_t *hs;
  int z, rows, cols;
  int row = 1;

  ncplane_dim_yx(n, &rows, &cols);
  compat_set_fg_all(n, SUBDISPLAY_ATTR);
  for(z = 0 ; (hs = helps[z]) && z < rows ; ++z){
    cmvwhline(n, row + z, START_COL, " ", cols - 2);
    ncplane_putwstr_yx(n, row + z, START_COL, hs);
  }
  row += z;
  if(!get_selected_blockobj()){
    compat_set_fg_all(n, SUBDISPLAY_INVAL_ATTR);
  }else{
    compat_set_fg_all(n, SUBDISPLAY_ATTR);
  }
  for(z = 0 ; (hs = helps_block[z]) && z < rows ; ++z){
    cmvwhline(n, row + z, START_COL, " ", cols - 2);
    ncplane_putwstr_yx(n, row + z, START_COL, hs);
  }
  row += z;
  if(!target_mode_p()){
    compat_set_fg_all(n, SUBDISPLAY_INVAL_ATTR);
  }else{
    compat_set_fg_all(n, SUBDISPLAY_ATTR);
  }
  for(z = 0 ; (hs = helps_target[z]) && z < rows ; ++z){
    cmvwhline(n, row + z, START_COL, " ", cols - 2);
    ncplane_putwstr_yx(n, row + z, START_COL, hs);
  }
  return 0;
}

static inline void
lock_notcurses(void){
  lock_growlight();
  pthread_mutex_lock(&bfl);
}

static inline void
unlock_notcurses(void){
  update_details_cond(details.p);
  update_help_cond(help.p);
  update_map_cond(maps.p);
  screen_update();
  pthread_mutex_unlock(&bfl);
  unlock_growlight();
}

// Used in growlight callbacks, since the growlight lock will already be held
// in any such case.
static inline void
lock_notcurses_growlight(void){
  pthread_mutex_lock(&bfl);
}

static inline void
unlock_notcurses_growlight(void){
  update_details_cond(details.p);
  update_help_cond(help.p);
  update_map_cond(maps.p);
  screen_update();
  pthread_mutex_unlock(&bfl);
}

static void
use_prev_zone(blockobj* b){
  if(b->zone){
    b->zone = b->zone->prev;
  }
}

static void
use_next_zone(blockobj* b){
  if(b->zone){
    b->zone = b->zone->next;
  }
}

static void
use_prev_device(void){
  adapterstate* as;

  if((as = get_current_adapter()) == NULL){
    return;
  }
  if(as->selected == NULL || as->selected->prev == NULL){
    if(as->prev){
      locked_diag("Press PageUp to go to the previous adapter");
    }
    return;
  }
  int delta = -device_lines(as->expansion, as->selected->prev);
  select_adapter_dev(as, as->selected->prev, delta);
}

static void
use_next_device(void){
  adapterstate* as;

  if((as = get_current_adapter()) == NULL){
    return;
  }
  if(as->selected == NULL || as->selected->next == NULL){
    if(as->next){
      locked_diag("Press PageDown to go to the next adapter");
    }
    return;
  }
  int delta = device_lines(as->expansion, as->selected);
  select_adapter_dev(as, as->selected->next, delta);
}

static const int DIAGROWS = 14;

// Used after shutting down on error, which will clean the screen. This takes
// the last few diagnostics and prints them, unmutilated, to stderr.
static int
dump_diags(void){
  logent l[10];
  int y, r;

  y = sizeof(l) / sizeof(*l);
  if((y = get_logs(y, l)) < 0){
    return -1;
  }
  for(r = 0 ; r < y ; ++r){
    char tbuf[27];

    if(l[r].msg == NULL){
      break;
    }
    ctime_r(&l[r].when, tbuf);
    fprintf(stderr, "%s %s", tbuf, l[r].msg);
    free(l[r].msg);
  }
  return 0;
}

static int
update_diags(struct panel_state *ps){
  logent l[DIAGROWS];
  int y, x, r;

  ncplane_dim_yx(ps->p, &y, &x);
  y -= 2;
  assert(x > 26 + START_COL * 2); // see ctime_r(3)
  if((y = get_logs(y, l)) < 0){
    return -1;
  }
  compat_set_fg_all(ps->p, SUBDISPLAY_ATTR);
  for(r = 0 ; r < y ; ++r){
    char *c, tbuf[x];
    struct tm tm;
    size_t tb;
    int p;

    if(l[r].msg == NULL){
      break;
    }
    if(localtime_r(&l[r].when, &tm) == NULL){
      break;
    }
    if(strftime(tbuf, sizeof(tbuf), "%F %T", &tm) == 0){
      break;;
    }
    tb = sizeof(tbuf) / sizeof(*tbuf) - strlen(tbuf);
    p = snprintf(tbuf + strlen(tbuf), tb, " %-*.*s",
        (int)tb - 2, (int)tb - 2, l[r].msg);
    if(p < 0 || (unsigned)p >= tb){
      tbuf[sizeof(tbuf) / sizeof(*tbuf) - 1] = '\0';
    }
    if( (c = strchr(tbuf, '\n')) ){
      *c = '\0';
    }
    c = tbuf;
    while((c = strchr(tbuf, '\b')) || (c = strchr(tbuf, '\t'))){
      *c = ' ';
    }
    cmvwprintw(ps->p, y - r, START_COL, "%-*.*s", x - 2, x - 2, tbuf);
    free(l[r].msg);
  }
  return 0;
}

static int
display_diags(struct ncplane* mainw, struct panel_state* ps){
  memset(ps, 0, sizeof(*ps));
  if(new_display_panel(mainw, ps, DIAGROWS, 0,
                       L"press 'D' to dismiss diagnostics", NULL,
                       PBORDER_COLOR)){
    goto err;
  }
  if(update_diags(ps)){
    goto err;
  }
  return 0;

err:
  if(ps->p){
    ncplane_destroy(ps->p);
  }
  memset(ps, 0, sizeof(*ps));
  return -1;
}

static const int DETAILROWS = 7; // FIXME make it dynamic based on selections

static int
display_details(struct ncplane* mainw, struct panel_state* ps){
  memset(ps, 0, sizeof(*ps));
  if(new_display_panel(mainw, ps, DETAILROWS, 78,
                       L"press 'v' to dismiss details",
                       NULL, PBORDER_COLOR)){
    goto err;
  }
  if(get_current_adapter()){
    if(update_details(ps->p)){
      goto err;
    }
  }
  return 0;

err:
  if(ps->p){
    ncplane_destroy(ps->p);
  }
  memset(ps, 0, sizeof(*ps));
  return -1;
}

static int
display_help(struct ncplane* nc, struct panel_state* ps){
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
  memset(ps, 0, sizeof(*ps));
  if(new_display_panel(nc, ps, helprows, helpcols,
                       L"press 'H' to dismiss help",
                       L"https://nick-black.com/dankwiki/index.php/Growlight",
                       PBORDER_COLOR)){
    goto err;
  }
  if(helpstrs(ps->p)){
    goto err;
  }
  return 0;

err:
  if(ps->p){
    ncplane_destroy(ps->p);
  }
  memset(ps, 0, sizeof(*ps));
  return -1;
}

static void
detail_mounts(struct ncplane* w, int* row, int maxy, const device* d){
  char buf[PREFIXSTRLEN + 1], b[256];
  int cols, r;
  unsigned z;

  ncplane_dim_yx(w, NULL, &cols);
  assert(d->mnt.count == d->mntops.count);
  for(z = 0 ; z < d->mnt.count ; ++z){
    if(*row == maxy){
      return;
    }
    if(growlight_target && !strncmp(d->mnt.list[z], growlight_target, strlen(growlight_target))){
      continue;
    }
    cmvwhline(w, *row, START_COL, " ", cols - 2);
    qprefix(d->mntsize, 1, buf, 0);
    cmvwprintw(w, *row, START_COL, "%-*.*s %-5.5s %-36.36s %*s %-*.*s",
        FSLABELSIZ, FSLABELSIZ, d->label ? d->label : "n/a",
        d->mnttype,
        d->uuid ? d->uuid : "n/a",
        PREFIXFMT(buf),
        cols - (FSLABELSIZ + 47 + PREFIXCOLUMNS),
        cols - (FSLABELSIZ + 47 + PREFIXCOLUMNS),
        d->name);
    if(++*row == maxy){
      return;
    }
    cwattroff(w, NCSTYLE_BOLD);
    if((r = snprintf(b, sizeof(b), " %s %s", d->mnt.list[z], d->mntops.list[z])) >= (int)sizeof(b)){
      b[sizeof(b) - 1] = '\0';
    }
    cmvwhline(w, *row, START_COL, " ", cols - 2);
    cmvwprintw(w, *row, START_COL, "%-*.*s", cols - 2, cols - 2, b);
    cwattron(w, NCSTYLE_BOLD);
    ++*row;
  }
}

static void
detail_targets(struct ncplane* w, int* row, int both, const device* d){
  char buf[PREFIXSTRLEN + 1], b[256]; // FIXME uhhhh
  int cols, r;
  unsigned z;

  ncplane_dim_yx(w, NULL, &cols);
  if(growlight_target == NULL){
    return;
  }
  for(z = 0 ; z < d->mnt.count ; ++z){
    if(strncmp(d->mnt.list[z], growlight_target, strlen(growlight_target))){
      continue;
    }
    cmvwhline(w, *row, START_COL, " ", cols - 2);
    qprefix(d->mntsize, 1, buf, 0);
    cmvwprintw(w, *row, START_COL, "%-*.*s %-5.5s %-36.36s %*s %-*.*s",
        FSLABELSIZ, FSLABELSIZ, d->label ? d->label : "n/a",
        d->mnttype,
        d->uuid ? d->uuid : "n/a",
        PREFIXFMT(buf),
        cols - (FSLABELSIZ + 47 + PREFIXCOLUMNS),
        cols - (FSLABELSIZ + 47 + PREFIXCOLUMNS),
        d->name);
    ++*row;
    if(!both){
      return;
    }
    cwattroff(w, NCSTYLE_BOLD);
    if((r = snprintf(b, sizeof(b), " %s %s", d->mnt.list[z], d->mntops.list[z])) >= (int)sizeof(b)){
      b[sizeof(b) - 1] = '\0';
    }
    cmvwhline(w, *row, START_COL, " ", cols - 2);
    cmvwprintw(w, *row, START_COL, "%-*.*s", cols - 2, cols - 2, b);
    cwattron(w, NCSTYLE_BOLD);
    ++*row;
    break; // FIXME no space currently
  }
}

static int
map_details(struct ncplane *hw){
  int y, rows, cols, curcol;
  const controller *c;
  char *fstab;

  ncplane_dim_yx(hw, &rows, &cols);
  --rows;
  y = 1;
  if(growlight_target){
    int blockout;

    cwattrset(hw, NCSTYLE_BOLD|PHEADING_COLOR);
    cmvwprintw(hw, y, 1, "Operating in target mode (%s)", growlight_target);
    ncplane_cursor_yx(hw, NULL, &curcol);
    if( (blockout = cols - curcol - 1) ){
      cwprintw(hw, "%*.*s", blockout, blockout, "");
    }
    ++y;
  }
  cwattrset(hw, NCSTYLE_BOLD|FORMTEXT_COLOR);
  // First we list the target fstab, and then the targets
  // FIXME this is probably multibyte input and needs be handled as such
  if( (fstab = dump_targets()) ){
    unsigned pos, linestart;

    pos = 0;
    linestart = 0;
    while(fstab[pos]){
      if(fstab[pos] == '\n'){
        fstab[pos] = '\0';
        if(pos != linestart){
          cmvwprintw(hw, y, 1, "%-*.*s", cols - 2, cols - 2, fstab + linestart);
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
      cmvwprintw(hw, y, 1, "%-*.*s", cols - 2, cols - 2, fstab + linestart);
      if(++y >= rows){
        return 0;
      }
    }
    free(fstab);
  }
  cwattrset(hw, NCSTYLE_BOLD|SUBDISPLAY_COLOR);
  cmvwhline(hw, y, 1, " ", cols - 2);
  cmvwprintw(hw, y, 1, "%-*.*s %-5.5s %-36.36s %*s %s",
      FSLABELSIZ, FSLABELSIZ, "Label",
      "Type", "UUID", PREFIXFMT("Bytes"), "Device");
  if(++y >= rows){
    return 0;
  }
  cwattrset(hw, NCSTYLE_BOLD|FORMTEXT_COLOR);
  for(c = get_controllers() ; c ; c = c->next){
    const device *d;

    for(d = c->blockdevs ; d ; d = d->next){
      const device *p;

      detail_targets(hw, &y, y + 1 < rows, d);
      if(y >= rows){
        return 0;
      }
      for(p = d->parts ; p ; p = p->next){
        detail_targets(hw, &y, y + 1 < rows, p);
        if(y >= rows){
          return 0;
        }
      }
    }
  }
  // Now list the existing maps, a superset of the targets
  cwattrset(hw, NCSTYLE_BOLD|SUBDISPLAY_COLOR);
  for(c = get_controllers() ; c ; c = c->next){
    const device *d;

    for(d = c->blockdevs ; d ; d = d->next){
      const device *p;

      detail_mounts(hw, &y, rows - y, d);
      if(y >= rows){
        return 0;
      }
      for(p = d->parts ; p ; p = p->next){
        detail_mounts(hw, &y, rows - y, p);
        if(y >= rows){
          return 0;
        }
      }
    }
  }

  while(y < rows){
    cmvwhline(hw, y++, 1, " ", cols - 2);
  }
  return 0;
}

static int
display_maps(struct ncplane* mainw, struct panel_state* ps){
  // FIXME compute based off number of maps + targets
  int rows;
  ncplane_dim_yx(mainw, &rows, NULL);
  rows -= 15;

  memset(ps, 0, sizeof(*ps));
  if(new_display_panel(mainw, ps, rows, 0, L"press 'E' to dismiss display",
                       NULL, PBORDER_COLOR)){
    goto err;
  }
  if(map_details(ps->p)){
    goto err;
  }
  return 0;

err:
  if(ps->p){
    ncplane_destroy(ps->p);
  }
  memset(ps, 0, sizeof(*ps));
  return -1;
}

static void
toggle_panel(struct ncplane* nc, struct panel_state *ps,
             int (*psfxn)(struct ncplane*, struct panel_state*)){
  if(ps->p){
    hide_panel_locked(ps);
    active = NULL;
  }else{
    hide_panel_locked(active);
    active = ((psfxn(nc, ps) == 0) ? ps : NULL);
  }
}

/*
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
  if(newsel + aft <= getmaxy(is->rb->win) - 3){
    newsel = getmaxy(is->rb->win) - aft - 3;
  }
  if(newsel + (int)node_lines(is->expansion,is->rb->selected) >= getmaxy(is->rb->win) - 2){
    newsel = getmaxy(is->rb->win) - 1 - node_lines(is->expansion,is->rb->selected);
  }
  if(newsel){
    is->rb->selline = newsel;
  }
}
*/

static int
expand_adapter_locked(void){
  adapterstate *is;

  is = get_current_adapter();
  if(is == NULL){
    return 0;
  }
  if(is->expansion == EXPANSION_FULL){
    return 0;
  }
  ++is->expansion;
  // int old, oldrows;
  // old = current_adapter->selline;
  // oldrows = getmaxy(current_adapter->win);
  // recompute_selection(is, old, oldrows, getmaxy(current_adapter->win));
  return 0;
}

static int
collapse_adapter_locked(void){
  adapterstate *is;

  is = get_current_adapter();
  if(is == NULL){
    return 0;
  }
  if(is->expansion == EXPANSION_NONE){
    return 0;
  }
  --is->expansion;
  // int old, oldrows;
  // old = current_adapter->selline;
  // oldrows = getmaxy(current_adapter->win);
  // recompute_selection(is, old, oldrows, getmaxy(current_adapter->win));
  return 0;
}

static int
select_adapter(void){
  adapterstate* as;
  if((as = get_current_adapter()) == NULL){
    return -1;
  }
  if(as->selected){
    // locked_diag("Already browsing [%s]", as->c->ident);
    return 0;
  }
  if(as->bobjs == NULL){
    return -1;
  }
  assert(as->selline == -1);
  return select_adapter_dev(as, as->bobjs, 2);
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
  if(strcasecmp(op, "do it") == 0){
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
      locked_diag("%s is not partitionable", b->d->name);
      return;
    }
    if(b->d->blkdev.pttable == NULL){
      locked_diag("%s has no partition table", b->d->name);
      return;
    }
    wipe_ptable(b->d, NULL);
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
    locked_diag("%s is not partitionable", b->d->name);
    return;
  }
  if(blockobj_unpartitionedp(b)){
    locked_diag("%s has no partition table", b->d->name);
    return;
  }
  confirm_operation("remove the partition table", remove_ptable_confirm);
}

static struct form_option *
pttype_table(int *count){
  struct form_option *fo = NULL, *tmp;
  pttable_type *types;
  int z;

  if((types = get_ptable_types(count)) == NULL){
    return NULL;
  }
  for(z = 0 ; z < *count ; ++z){
    char *key, *desc;

    if((key = strdup(types[z].name)) == NULL){
      goto err;
    }
    if((desc = strdup(types[z].desc)) == NULL){
      free(key);
      goto err;
    }
    if((tmp = realloc(fo, sizeof(*fo) * (*count + 1))) == NULL){
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
  raise_form("select a table type", pttype_callback, ops_ptype, opcount, -1,
      PTTYPE_TEXT);
}

static void
new_filesystem(void){
  blockobj *b = get_selected_blockobj();
  struct form_option *ops_fs;
  int opcount, defidx;

  if(b == NULL){
    locked_diag("A block device must be selected");
    return;
  }
  if(selected_unloadedp()){
    locked_diag("Media is not loaded on %s", b->d->name);
    return;
  }
  if(selected_emptyp()){
    locked_diag("Selected region of %s is empty space", b->d->name);
    return;
  }
  if(!selected_mkfs_safe_p()){
    return;
  }
  if((ops_fs = fs_table(&opcount, NULL, &defidx)) == NULL){
    return;
  }
  raise_form("select a filesystem type", fs_callback, ops_fs, opcount,
      defidx, FSTYPE_TEXT);
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
      locked_diag("Media is not loaded on %s", b->d->name);
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
    locked_diag("Wiped filesystem on %s", d->name);
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
    locked_diag("Media is not loaded on %s", b->d->name);
    return;
  }
  if(selected_emptyp()){
    locked_diag("Filesystems cannot be wiped from empty space");
    return;
  }
  if(b->zone){
    if(b->zone->p && !b->zone->p->mnttype){
      locked_diag("No filesystem signature on %s\n", b->zone->p->name);
    }else if(b->zone->p->mnt.count){
      locked_diag("Filesystem on %s is mounted. Use 'O'/'T' to unmount.\n", b->zone->p->name);
    }else{
      confirm_operation("wipe the filesystem signature", kill_filesystem_confirm);
    }
  }else if(!b->zone){
    if(!b->d->mnttype){
      locked_diag("No filesystem signature on %s\n", b->d->name);
    }else if(b->d->mnt.count){
      locked_diag("Filesystem on %s is mounted. Use 'O'/'T' to unmount.\n", b->d->name);
    }else{
      confirm_operation("wipe the filesystem signature", kill_filesystem_confirm);
    }
  }else{
    confirm_operation("wipe the filesystem signature", kill_filesystem_confirm);
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
  }, {
    .option = "0x0000000000000002", // 2^1, 2
    .desc = "Hide from EFI",
  }, {
    .option = "0x0000000000000004", // 2^2, 4
    .desc = "Legacy BIOS bootable",
  }, { // readonly for EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
    .option = "0x1000000000000000", // 2^60
    .desc = "Read-only",
  }, {
    .option = "0x2000000000000000", // 2^61
    .desc = "Shadow copy",
  }, { // hidden for EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
    .option = "0x4000000000000000", // 2^62
    .desc = "Hidden",
  }, { // no default drive letter for EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
    .option = "0x8000000000000000", // 2^63
    .desc = "Inhibit automounting",
  },
};

static struct form_option *
flag_table(int *count, const char *match, int *defidx, char ***selarray, int *selections,
    const struct form_option *flags, unsigned fcount){
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
    if(match && strcmp(match, fo[z].option) == 0){
      int zz;

      *defidx = z;
      for(zz = 0 ; selections && zz < *selections ; ++zz){
        if(strcmp(key, (*selarray)[zz]) == 0){
          free((*selarray)[zz]);
          (*selarray)[zz] = NULL;
          if(zz < *selections - 1){
            memmove(&(*selarray)[zz], &(*selarray)[zz + 1], sizeof(**selarray) * (*selections - 1 - zz));
          }
          --*selections;
          zz = -1;
          break;
        }
      }
      if(zz >= *selections){
        typeof(*selarray) tmp;

        if((tmp = realloc(*selarray, sizeof(*selarray) * (*selections + 1))) == NULL){
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
do_partflag(char **selarray, int selections){
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
    ul = strtoull(selarray[z], &eptr, 16);
    assert(ul && (ul < ULLONG_MAX || errno != ERANGE) && !*eptr);
    flags |= ul;
  }
  if(partition_set_flags(d, flags)){
    return;
  }
}

static void
partflag_callback(const char *fn, char **selarray, int selections, int scrollp){
  struct form_option *flags_agg;
  int opcount, defidx;

  if(fn == NULL){
    // FIXME free selections
    return;
  }
  if(strcmp(fn, "") == 0){
    do_partflag(selarray, selections);
    // FIXME free
    return;
  }
  if((flags_agg = flag_table(&opcount, fn, &defidx, &selarray, &selections,
      gpt_flags, sizeof(gpt_flags) / sizeof(*gpt_flags))) == NULL){
    // FIXME free
    return;
  }
  raise_checkform("set GPT partition flags", partflag_callback, flags_agg,
    opcount, defidx, selarray, selections, PARTFLAG_TEXT, scrollp);
}

static void
dos_partflag_callback(const char *fn, char **selarray, int selections, int scrollp){
  struct form_option *flags_agg;
  int opcount, defidx;

  if(fn == NULL){
    // FIXME free selections
    return;
  }
  if(strcmp(fn, "") == 0){
    do_partflag(selarray, selections);
    // FIXME free
    return;
  }
  if((flags_agg = flag_table(&opcount, fn, &defidx, &selarray, &selections,
      dos_flags, sizeof(dos_flags) / sizeof(*dos_flags))) == NULL){
    // FIXME free
    return;
  }
  raise_checkform("set DOS partition flags", dos_partflag_callback, flags_agg,
    opcount, defidx, selarray, selections, PARTFLAG_TEXT, scrollp);
}

static int
flag_to_selections(uint64_t flags, char ***selarray, int *selections,
      const struct form_option *ops, unsigned opcount){
  assert(selarray && selections);
  assert(!*selarray && !*selections);
  while(opcount--){
    unsigned long long ul;
    char *eptr;

    ul = strtoull(ops[opcount].option, &eptr, 16);
    assert(ul && (ul < ULLONG_MAX || errno != ERANGE) && !*eptr);
    if(flags & ul){
      typeof(*selarray) tmp;

      if((tmp = realloc(*selarray, sizeof(*selarray) * (*selections + 1))) == NULL){
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
  int opcount, defidx;
  blockobj *b;

  if((b = get_selected_blockobj()) == NULL){
    locked_diag("Partition modification requires selection of a partition");
    return;
  }
  if(selected_unloadedp()){
    locked_diag("Media is not loaded on %s", b->d->name);
    return;
  }
  if(!selected_partitionp()){
    locked_diag("Selected object is not a partition");
    return;
  }
  // FIXME need to initialize widget based off current flags
  if(strcmp("gpt", b->d->blkdev.pttable) == 0){
    if(flag_to_selections(b->zone->p->partdev.flags, &selarray, &selections,
          gpt_flags, sizeof(gpt_flags) / sizeof(*gpt_flags))){
      return;
    }
    if((flags_agg = flag_table(&opcount, NULL, &defidx, &selarray, &selections,
        gpt_flags, sizeof(gpt_flags) / sizeof(*gpt_flags))) == NULL){
      // FIXME free selarray
      return;
    }
    raise_checkform("set GPT partition flags", partflag_callback, flags_agg,
        opcount, defidx, selarray, selections, PARTFLAG_TEXT, 0);
  }else if(strcmp("dos", b->d->blkdev.pttable) == 0 ||
    strcmp("msdos", b->d->blkdev.pttable) == 0){

    if(flag_to_selections(b->zone->p->partdev.flags, &selarray, &selections,
          dos_flags, sizeof(dos_flags) / sizeof(*dos_flags))){
      return;
    }
    if((flags_agg = flag_table(&opcount, NULL, &defidx, &selarray, &selections,
        dos_flags, sizeof(dos_flags) / sizeof(*dos_flags))) == NULL){
      return;
    }
    raise_checkform("set DOS partition flags", dos_partflag_callback, flags_agg,
        opcount, defidx, selarray, selections, PARTFLAG_TEXT, 0);
  }else{
    assert(0);
  }
}

static inline int
fsck_suitable_p(const device *d){
  if(d->mnt.count){
    locked_diag("Will not fsck mounted filesystem on %s", d->name);
    return 0;
  }
  if(d->mnttype == NULL){
    locked_diag("No filesystem found on %s", d->name);
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
    locked_diag("Media is not loaded on %s", b->d->name);
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
      locked_diag("Validated filesystem on %s", d->name);
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
      locked_diag("Media is not loaded on %s", b->d->name);
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
    locked_diag("Media is not loaded on %s", b->d->name);
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
    locked_diag("%s has a valid filesystem signature. Wipe it with 'w'.", b->zone->p->name);
    return;
  }
  confirm_operation("delete the partition", delete_partition_confirm);
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
    locked_diag("Media is unloaded on %s\n", b->d->name);
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
    locked_diag("Media is unloaded on %s\n", b->d->name);
    return;
  }
  confirm_operation("wipe the mbr", wipe_mbr_confirm);
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
  badblock_scan(b->d, 0); // FIXME allow destructive badblock check
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
    locked_diag("Media is not loaded on %s", b->d->name);
    return;
  }
  if(path == NULL){
    locked_diag("User cancelled mount operation");
    return;
  }
  if(selected_unpartitionedp()){
    mmount(b->d, path, 0, NULL);
  }else{
    assert(selected_partitionp());
    mmount(b->zone->p, path, 0, NULL);
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
    locked_diag("Media is not loaded on %s", b->d->name);
    return;
  }
  if(!selected_unpartitionedp()){
    if(selected_emptyp()){
      locked_diag("Cannot mount unused space");
      return;
    }
    if(b->zone->p->mnttype == NULL){
      locked_diag("No filesystem detected on %s", b->zone->p->name);
      return;
    }
    if(fstype_swap_p(b->zone->p->mnttype)){
      return;
    }
  }else{
    if(b->d->mnttype == NULL){
      locked_diag("No filesystem detected on %s", b->d->name);
      return;
    }
    if(fstype_swap_p(b->d->mnttype)){
      return;
    }
  }
  raise_str_form("enter mountpount", mountpoint_callback, "/", MOUNT_TEXT);
}

static void
numount_target(void){
  blockobj *b;

  if((b = get_selected_blockobj()) == NULL){
    locked_diag("Must select a filesystem to mount");
    return;
  }
  if(selected_unloadedp()){
    locked_diag("Media is not loaded on %s", b->d->name);
    return;
  }
  if(!selected_unpartitionedp()){
    if(selected_emptyp()){
      locked_diag("Cannot unmount unused space");
      return;
    }
    if(!targeted_p(b->zone->p)){
      locked_diag("Block device %s is not a target", b->zone->p->name);
      return;
    }
    if(unmount(b->zone->p, NULL)){
      return;
    }
    return;
  }else{
    if(!targeted_p(b->d)){
      locked_diag("Block device %s is not a target", b->d->name);
      return;
    }
    if(unmount(b->d, NULL)){
      return;
    }
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
    locked_diag("Media is not loaded on %s", b->d->name);
    return -1;
  }
  if(selected_unpartitionedp()){
    locked_diag("BIOS cannot boot from unpartitioned %s", b->d->name);
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
    locked_diag("Block device %s is not a target", d->name);
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
    locked_diag("Media is not loaded on %s", b->d->name);
    return -1;
  }
  if(selected_unpartitionedp()){
    locked_diag("UEFI cannot boot from unpartitioned %s", b->d->name);
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
    locked_diag("Block device %s is not a target", d->name);
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
    locked_diag("Media is not loaded on %s", b->d->name);
    return;
  }
  if(!selected_unpartitionedp()){
    if(selected_emptyp()){
      locked_diag("Cannot mount unused space");
      return;
    }
    if(targeted_p(b->zone->p)){
      locked_diag("%s is already a target", b->zone->p->name);
      return;
    }
    if(b->zone->p->mnttype == NULL){
      locked_diag("No filesystem detected on %s", b->zone->p->name);
      return;
    }
  }else{
    if(targeted_p(b->d)){
      locked_diag("%s is already a target", b->d->name);
      return;
    }
    if(b->d->mnttype == NULL){
      locked_diag("No filesystem detected on %s", b->d->name);
      return;
    }
  }
  raise_str_form("enter target mountpount", targpoint_callback, "/", TARG_TEXT);
}

static void
umount_filesystem(void){
  blockobj *b;

  if((b = get_selected_blockobj()) == NULL){
    locked_diag("Must select a filesystem to unmount");
    return;
  }
  if(selected_unloadedp()){
    locked_diag("Media is not loaded on %s", b->d->name);
    return;
  }
  if(!selected_unpartitionedp()){
    if(selected_emptyp()){
      locked_diag("Cannot unmount unused space");
      return;
    }
    if(fstype_swap_p(b->zone->p->mnttype)){
      swapoffdev(b->zone->p);
    }else{
      unmount(b->zone->p, NULL);
    }
  }else{
    if(fstype_swap_p(b->d->mnttype)){
      swapoffdev(b->d);
    }else{
      unmount(b->d, NULL);
    }
  }
}

static void
remove_last_bufchar(char *buf){
  char *killem = buf;
  mbstate_t mb;
  size_t m;

  memset(&mb, 0, sizeof(mb));
  while( (m = mbrlen(buf, strlen(buf), &mb)) ){
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
    lock_notcurses();
    fs->inp.buffer[0] = '\0';
    form_string_options(fs);
    unlock_notcurses();
    break;
  case '\r': case '\n': case NCKEY_ENTER:{
    char *str;

    lock_notcurses();
    str = strdup(actform->inp.buffer);
    assert(NULL != str);
    free_form(actform);
    actform = NULL;
    notcurses_cursor_disable(NC);
    cb(str);
    free(str);
    unlock_notcurses();
    break;
  }case NCKEY_ESC:{
    lock_notcurses();
    free_form(actform);
    actform = NULL;
    notcurses_cursor_disable(NC);
    cb(NULL);
    unlock_notcurses();
    break;
  }case NCKEY_BACKSPACE:{
    lock_notcurses();
    remove_last_bufchar(fs->inp.buffer);
    form_string_options(fs);
    unlock_notcurses();
    break;
  }default:{
    char *tmp;

    if(ch >= 256 || !isgraph(ch)){
      diag("please %s, or cancel", actform->boxstr);
    }
    lock_notcurses();
    if((tmp = realloc(fs->inp.buffer, strlen(fs->inp.buffer) + 2)) == NULL){
      locked_diag("Couldn't allocate input buffer (%s?)", strerror(errno));
      unlock_notcurses();
      return;
    }
    fs->inp.buffer = tmp;
    fs->inp.buffer[strlen(fs->inp.buffer) + 1] = '\0';
    fs->inp.buffer[strlen(fs->inp.buffer)] = (unsigned char)ch;
    form_string_options(fs);
    unlock_notcurses();
  } }
}

// We received input while a modal subwindow was active. Divert it from the
// typical UI, and handle it according to the subwindow. If we are not
// interested in the input, return it for further use. Otherwise, return 0 to
// indicate intercept.
static char32_t
handle_subwindow_input(char32_t ch){
  switch(ch){
    default:
      // locked_diag("FIXME handle subwindow input");
      break;
  }
  return ch;
}

static void
handle_actform_splash_input(void){
  lock_notcurses();
  free_form(actform);
  actform = NULL;
  notcurses_cursor_disable(NC);
  unlock_notcurses();
}

// We received input while a modal form was active. Divert it from the typical
// UI, and handle it according to the form. Returning non-zero quits the
// program, and should pretty much only indicate that 'q' was pressed.
static char32_t
handle_actform_input(wchar_t ch){
  struct form_state *fs = actform;
  void (*mcb)(const char *, char **, int, int);
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
    case ' ': case '\r': case '\n': case NCKEY_ENTER:{
      int op, selections, scrolloff;
      char **selarray;
      char *optstr;

      lock_notcurses();
        op = fs->idx;
        optstr = strdup(fs->ops[op].option);
        assert(NULL != optstr);
        selarray = fs->selarray;
        selections = fs->selections;
        scrolloff = fs->scrolloff;
        fs->selarray = NULL;
        free_form(actform);
        actform = NULL;
        screen_update();
        if(mcb){
          mcb(optstr, selarray, selections, scrolloff);
        }else{
          cb(optstr);
        }
        free(optstr);
      unlock_notcurses();
      break;
    }case NCKEY_ESC:{
      lock_notcurses();
      if(fs->formtype == FORM_MULTISELECT || fs->formtype == FORM_CHECKBOXEN){
        int scrolloff = fs->scrolloff;
        free_form(actform);
        actform = NULL;
        mcb(NULL, NULL, 0, scrolloff);
      }else{
        free_form(actform);
        actform = NULL;
        cb(NULL);
      }
      unlock_notcurses();
      break;
    }case NCKEY_UP: case 'k':{
      lock_notcurses();
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
      unlock_notcurses();
      break;
    }case NCKEY_DOWN: case 'j':{
      lock_notcurses();
      int maxz;
      ncplane_dim_yx(fs->p, &maxz, NULL);
      maxz = maxz - 5 >= fs->opcount - 1 ? fs->opcount - 1 : maxz - 5;
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
      unlock_notcurses();
      break;
    }case 'q':{
      return 'q';
    }case 'C':{
      int selections, scrolloff;
      char **selarray;

      lock_notcurses();
      if(fs->formtype == FORM_MULTISELECT || fs->formtype == FORM_CHECKBOXEN){
        selarray = fs->selarray;
        selections = fs->selections;
        scrolloff = fs->scrolloff;
        fs->selarray = NULL;
        free_form(actform);
        actform = NULL;
        mcb("", selarray, selections, scrolloff);
        unlock_notcurses();
        break;
      }
      unlock_notcurses();
    } // intentional fallthrough
    default:{
      diag("please %s, or cancel", actform->boxstr);
      break;
    }
  }
  return 0;
}

static void
destroy_aggregate_confirm(const char *op){
  const blockobj *b;

  if(!op || !approvedp(op)){
    locked_diag("aggregate destruction was cancelled");
    return;
  }
  if((b = get_selected_blockobj()) == NULL){
    locked_diag("Aggregate destruction requires selection of an aggregate");
    return;
  }
  if(b->d->layout == LAYOUT_NONE || b->d->layout == LAYOUT_PARTITION){
    locked_diag("%s is not an aggregate", b->d->name);
  }else if(b->d->layout == LAYOUT_ZPOOL){
    destroy_zpool(b->d);
  }else if(b->d->layout == LAYOUT_MDADM){
    destroy_mdadm(b->d);
  }else if(b->d->layout == LAYOUT_DM){
    locked_diag("Not yet implemented FIXME"); // FIXME
  }else{
    locked_diag("Unknown layout type %d on %s", b->d->layout, b->d->name);
  }
}

static void
untargeted_exit_confirm(const char* op){
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
    locked_diag("%s is not an aggregate", b->d->name);
    return;
  }
  confirm_operation("destroy the aggregate", destroy_aggregate_confirm);
}

static void
modify_aggregate(void){
  blockobj *b;

  if((b = get_selected_blockobj()) == NULL){
    locked_diag("Aggregate modification requires selection of an aggregate");
    return;
  }
  if(b->d->layout == LAYOUT_NONE || b->d->layout == LAYOUT_PARTITION){
    locked_diag("%s is not an aggregate", b->d->name);
    return;
  }
  locked_diag("Aggregate modification not yet implemented FIXME");
}

static void
search_callback(const char* term){
  if(term == NULL){
    locked_diag("Search was cancelled");
    return;
  }
  locked_diag("Search not yet implemented FIXME");
}

static void
start_search(void){
  raise_str_form("start typing an identifier", search_callback, NULL,
      SEARCH_TEXT);
}

static void
loop_callback(const char* term){
  if(term == NULL){
    locked_diag("Loop device setup cancelled");
    return;
  }
  locked_diag("Looping not yet implemented FIXME");
}

static void
configure_loop_dev(void){
  raise_str_form("Select a file to loop", loop_callback, NULL, LOOP_TEXT);
}

static void
set_label(void){
  device *d;

  if((d = selected_filesystem()) == NULL){
    locked_diag("No filesystem is selected");
    return;
  }
  if(!fstype_named_p(d->mnttype)){
    locked_diag("%s does not support labels", d->mnttype);
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
    locked_diag("%s does not support UUIDs", d->mnttype);
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
    locked_diag("Already have a target at %s", growlight_target);
    return;
  }
  if(set_target(token)){
    return;
  }
  locked_diag("Now targeting %s", token);
}

static void
setup_target(void){
  if(target_mode_p()){
    locked_diag("Already have a target at %s", growlight_target);
    return;
  }
  raise_str_form("enter target path", do_setup_target, "/target", TARGET_TEXT);
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
handle_input(struct ncplane* w){
  char32_t ch;
  ncinput ni;
  int r;

  // FIXME can we not just throw lock_ and unlock_ around the entire stanza?
  while((ch = notcurses_getc_blocking(NC, &ni)) != (char32_t)-1){
    if(ch == 'L' && ni.ctrl){
      lock_notcurses();
      notcurses_refresh(NC, NULL, NULL);
      locked_diag("refreshed screen");
      unlock_notcurses();
      continue;
    }
    if(ch == NCKEY_RESIZE){
      lock_notcurses();
      struct ncplane *ncp = ncreel_plane(PR);
      int dimy, dimx;
      notcurses_refresh(NC, &dimy, &dimx);
      ncplane_resize_simple(ncp, dimy - 2, dimx);
      locked_diag("resized to %dx%d", dimx, dimy);
      unlock_notcurses();
      continue;
    }
		bool menuinput;
    lock_notcurses();
		menuinput = ncmenu_offer_input(mainmenu, &ni);
    unlock_notcurses(); // FIXME don't always want a redraw here
		if(menuinput){
			continue;
		}
		if(ch == NCKEY_ENTER || ch == NCKEY_SPACE){
			if(ncmenu_selected(mainmenu, &ni) == NULL){
				continue;
			} // otherwise, continue through with selected item
			ncmenu_rollup(mainmenu);
			ch = ni.id;
		}
		if(ch <= NCKEY_RELEASE && ch >= NCKEY_BUTTON1){
			if(ncmenu_mouse_selected(mainmenu, &ni, &ni) == NULL){
				continue;
			} // otherwise, continue through with selected item
			ncmenu_rollup(mainmenu);
			ch = ni.id;
		}
    if(actform){
      if((ch = handle_actform_input(ch)) == (char32_t)-1){
        break;
      }
      if(ch == 0){
        continue;
      }
    }
    if(active){
      if((ch = handle_subwindow_input(ch)) == (char32_t)-1){
        return;
      }
      if(ch == 0){ // intercepted
        continue;
      }
    }
    switch(ch){
      case 'H':{
        lock_notcurses();
        toggle_panel(w, &help, display_help);
        unlock_notcurses();
        break;
      }
      case 'D':{
        lock_notcurses();
        toggle_panel(w, &diags, display_diags);
        unlock_notcurses();
        break;
      }
      case 'v':{
        lock_notcurses();
        toggle_panel(w, &details, display_details);
        unlock_notcurses();
        break;
      }
      case 'E':{
        lock_notcurses();
        toggle_panel(w, &maps, display_maps);
        unlock_notcurses();
        break;
      }
      case '+':
        lock_notcurses();
        expand_adapter_locked();
        unlock_notcurses();
        break;
      case '-':{
        lock_notcurses();
        collapse_adapter_locked();
        unlock_notcurses();
        break;
      }
      case NCKEY_RIGHT: case 'l':{
        lock_notcurses();
        if(selection_active()){
          use_next_zone(get_selected_blockobj());
        }
        unlock_notcurses();
        break;
      }
      case NCKEY_LEFT: case 'h':{
        lock_notcurses();
        if(selection_active()){
          use_prev_zone(get_selected_blockobj());
        }
        unlock_notcurses();
        break;
      }
      case NCKEY_UP: case 'k':{
        lock_notcurses();
        use_prev_device();
        unlock_notcurses();
        break;
      }
      case NCKEY_DOWN: case 'j':{
        lock_notcurses();
        use_next_device();
        unlock_notcurses();
        break;
      }
			case NCKEY_PGUP: case NCKEY_BUTTON4: {
        int sel;
        lock_notcurses();
        sel = selection_active();
        ncreel_prev(PR);
        if(sel){
          select_adapter();
        }
        unlock_notcurses();
        break;
      }
			case NCKEY_PGDOWN: case NCKEY_BUTTON5: {
        int sel;
        lock_notcurses();
// fprintf(stderr, "-------------- BEGIN PgDown ---------------\n");
        sel = selection_active();
        deselect_adapter_locked();
        ncreel_next(PR);
        if(sel){
          select_adapter();
        }
// fprintf(stderr, "--------------- END PgDown ----------------\n");
        unlock_notcurses();
        break;
      }
      case 'm':{
        lock_notcurses();
        make_ptable();
        unlock_notcurses();
        break;
      }
      case 'r':{
        lock_notcurses();
        remove_ptable();
        unlock_notcurses();
        break;
      }
      case 'W':{
        lock_notcurses();
        wipe_mbr();
        unlock_notcurses();
        break;
      }
      case 'B':{
        lock_notcurses();
        badblock_check();
        unlock_notcurses();
        break;
      }
      case 'n':{
        lock_notcurses();
        new_partition();
        unlock_notcurses();
        break;
      }
      case 'd':{
        lock_notcurses();
        delete_partition();
        unlock_notcurses();
        break;
      }
      case 'F':{
        lock_notcurses();
        fsck_partition();
        unlock_notcurses();
        break;
      }
      case 'U':{
        lock_notcurses();
        set_uuid();
        unlock_notcurses();
        break;
      }
      case 'L':{
        lock_notcurses();
        set_label();
        unlock_notcurses();
        break;
      }
      case 's':{
        lock_notcurses();
        set_partition_attrs();
        unlock_notcurses();
        break;
      }
      case 'M':{
        lock_notcurses();
        new_filesystem();
        unlock_notcurses();
        break;
      }
      case 'w':{
        lock_notcurses();
        kill_filesystem();
        unlock_notcurses();
        break;
      }
      case 'o':{
        lock_notcurses();
        mount_filesystem();
        unlock_notcurses();
        break;
      }
      case 'O':{
        lock_notcurses();
        umount_filesystem();
        unlock_notcurses();
        break;
      }
      case 't':{
        lock_notcurses();
        nmount_target();
        unlock_notcurses();
        break;
      }
      case 'T':{
        lock_notcurses();
        numount_target();
        unlock_notcurses();
        break;
      }
      case 'b':{
        lock_notcurses();
        enslave_disk();
        unlock_notcurses();
        break;
      }
      case 'f':{
        lock_notcurses();
        liberate_disk();
        unlock_notcurses();
        break;
      }
      case 'i':{
        lock_notcurses();
        setup_target();
        unlock_notcurses();
        break;
      }
      case '/':{
        lock_notcurses();
        start_search();
        unlock_notcurses();
        break;
      }
      case 'p':{
        lock_notcurses();
        configure_loop_dev();
        unlock_notcurses();
        break;
      }
      case 'I':{
        lock_notcurses();
        unset_target();
        unlock_notcurses();
        break;
      }
      case 'A':
        lock_notcurses();
        if(actform){
          locked_diag("An input dialog is already active");
        }else{
          raise_aggregate_form();
        }
        unlock_notcurses();
        break;
      case 'z':
        lock_notcurses();
        modify_aggregate();
        unlock_notcurses();
        break;
      case 'Z':
        lock_notcurses();
        destroy_aggregate();
        unlock_notcurses();
        break;
// Finalization commands
      case '*':
        lock_notcurses();
        if((r = uefiboot()) == 0){
          locked_diag("Successfully finalized target /etc/fstab");
        }
        unlock_notcurses();
        if(r == 0){
          return;
        }
        break;
      case '#':
        lock_notcurses();
        if((r = biosboot()) == 0){
          locked_diag("Successfully finalized target /etc/fstab");
        }
        unlock_notcurses();
        if(r == 0){
          return;
        }
        break;
      case '@':
        lock_notcurses();
        if((r = finalize_target()) == 0){
          locked_diag("Successfully finalized target /etc/fstab");
        }
        unlock_notcurses();
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
        confirm_operation("exit without finalizing a target", untargeted_exit_confirm);
        break;
      default:{
        const char *hstr = !help.p ? " ('H' for help)" : "";
        // diag() locks/unlocks, and calls screen_update()
        if(isprint(ch)){
          diag("unknown command '%c'%s", ch, hstr);
        }else{
          diag("unknown scancode 0x%x%s", ch, hstr);
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
    memset(as, 0, sizeof(*as));
    as->c = a;
    as->expansion = EXPANSION_MAX;
    as->selected = NULL;
    as->selline = -1;
    as->rb = NULL;
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

  lock_notcurses_growlight();
  if((as = state) == NULL){
    if(a->blockdevs){
      if( (state = as = create_adapter_state(a)) ){
        int rows, cols;

//fprintf(stderr, "NEW ADAPTER STATE ASSIGNED %s %p\n", as->c->name, as);
        notcurses_term_dim_yx(NC, &rows, &cols);
        if((as->rb = ncreel_add(PR, NULL, NULL, redraw_adapter, as)) == NULL){
          free_adapter_state(as);
          unlock_notcurses_growlight();
          return NULL;
        }
        ++count_adapters;
      }
    }else{
      as = NULL;
    }
  }
  unlock_notcurses_growlight();
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
//   d->layout == LAYOUT_NONE:
//     d->blkdev.unloaded: device unloaded or inaccessible
//     d->blkdev.pttable == NULL: no partitioning table
//  d->layout == * ??? FIXME
// b->zone->p == NULL: empty space
// b->zone->p: partition
static void
update_blockobj(blockobj* b, device* d){
  zobj *z, *lastz, *firstchoice;
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
        sector = last_usable_sector(d);
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
create_blockobj(device* d){
  blockobj* b;

  if( (b = malloc(sizeof(*b))) ){
    memset(b, 0, sizeof(*b));
    b->d = d;
    update_blockobj(b, d);
  }
  return b;
}

static void *
block_callback(device* d, void* v){
  adapterstate* as;
  blockobj* b;

  if(d->layout == LAYOUT_PARTITION){
    return NULL; // FIXME ought be an assert; this shouldn't happen
  }
  lock_notcurses_growlight();
//fprintf(stderr, "---------begin block event on %s\n", d->name);
  if((as = d->c->uistate) == NULL){
//fprintf(stderr, "MAKE THAT INVISIBLE block event on %s\n!", d->name);
    if((as = d->c->uistate = adapter_callback(d->c, NULL)) == NULL){
      return NULL;
    }
  }
  if((b = v) == NULL){
    if( (b = create_blockobj(d)) ){
//fprintf(stderr, "ASSIGNED %p to %p on %p\n", d, b->d, as);
      if(as->devs == 0){
        b->prev = b->next = NULL;
        as->bobjs = b;
      }else{ // append at end, necessary due to how adapters are drawn
        blockobj* prev = as->bobjs;
        while(prev->next){
          prev = prev->next;
        }
        b->next = NULL;
        b->prev = prev;
        prev->next = b;
      }
      ++as->devs;
    }
  }else{
    update_blockobj(b, d);
  }
  if(as->rb){
    if(get_current_adapter() == as){
      if(b->prev == NULL && b->next == NULL){
        select_adapter();
      }
    }
  }
//fprintf(stderr, "---------end block event on %s\n", d->name);
  unlock_notcurses_growlight();
  return b;
}

static void
block_free(void *cv, void *bv){
  adapterstate *as = cv;
  blockobj *bo = bv;

  lock_notcurses_growlight();
  if(bo == as->selected){
    if(bo->prev){
      select_adapter_dev(as, bo->prev, -device_lines(as->expansion, bo));
    }else if(bo->next){
      select_adapter_dev(as, bo->next, device_lines(as->expansion, bo));
    }else{
      select_adapter_dev(as, NULL, 0);
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
  unlock_notcurses_growlight();
}

static void
adapter_free(void *cv){
  adapterstate *as = cv;
  lock_notcurses_growlight();
  as->prev->next = as->next;
  as->next->prev = as->prev;
  if(as->rb){
    ncreel_del(PR, as->rb);
  }
  as->next->prev = as->prev;
  as->prev->next = as->next;
  free_adapter_state(as); // clears subentries
  --count_adapters;
  draw_main_window(notcurses_stdplane(NC)); // Update the device count
  unlock_notcurses_growlight();
}

static void
vdiag(const char *fmt, va_list v){
  lock_notcurses_growlight();
  locked_vdiag(fmt, v);
  unlock_notcurses_growlight();
}

// FIXME destroy ncreel
static void
shutdown_cycle(void){
  struct panel_state *ps;

  diag("User-initiated shutdown\n");
  ps = show_splash(L"Shutting down...");
  /*if(growlight_stop()){
    kill_splash(ps);
    notcurses_stop(NC);
    dump_diags();
    exit(EXIT_FAILURE);
  } FIXME */
  kill_splash(ps);
  if(notcurses_stop(NC)){
    dump_diags();
    exit(EXIT_FAILURE);
  }
  exit(EXIT_SUCCESS);
}

static void raise_info_form(const char *str, const char *text){
  struct form_state *fs;
  int lineguess;
  int cols;
  int x, y;

  assert(str && text);
  if(actform){
    locked_diag("An input dialog is already active");
    return;
  }
  if((fs = create_form(str, NULL, FORM_SPLASH_PROMPT, 0)) == NULL){
    return;
  }
  fs->longop = strlen(str);
  cols = fs->longop; // FIXME? 40 for input currently
  struct ncplane* stdn = notcurses_stdplane(NC);
  ncplane_dim_yx(stdn, &y, &x);
  assert(x >= cols + 3);
  assert(y >= 3);
  // It could be more than this due to line breaking, so add a fudge
  // factor of 2...FIXME
  lineguess = 2; // yuck
  lineguess += strlen(text) / (x - 2) + 1;
  fs->p = ncplane_new(notcurses_stdplane(NC), 3, cols + START_COL * 2, FORM_Y_OFFSET + lineguess, x - cols - 3, NULL, NULL);
  if(fs->p == NULL){
    locked_diag("Couldn't create plane, uh-oh");
    free_form(fs);
    return;
  }
  cwattroff(fs->p, NCSTYLE_BOLD);
  compat_set_fg(fs->p, FORMBORDER_COLOR);
  bevel_all(fs->p);
  cwattron(fs->p, NCSTYLE_BOLD);
  cmvwprintw(fs->p, 1, START_COL, "%-*.*s", cols, cols, str);
  form_string_options(fs);
  actform = fs;
  fs->extext = raise_form_explication(notcurses_stdplane(NC), text, 20);
  screen_update();
}

static void
boxinfo(const char *text, ...){
  va_list v;
  char *buf;
  int max;

  max = BUFSIZ;
  if((buf = malloc(max)) == NULL){
    lock_notcurses_growlight();
    locked_diag("Couldn't display boxinfo");
    unlock_notcurses_growlight();
    return;
  }
  va_start(v, text);
  if(vsnprintf(buf, max, text, v) >= max){
    buf[max - 1] = '\0';
  }
  lock_notcurses_growlight();
  raise_info_form("Press any key to continue...", buf);
  unlock_notcurses_growlight();
  va_end(v);
}

static struct ncmenu*
create_menu(struct ncplane* n){
	struct ncmenu_item block_items[] = {
		{ .desc = "Make partition table", .shortcut = { .id = 'm', }, },
	};
	struct ncmenu_item part_items[] = {
		{ .desc = "Make filesystem", .shortcut = { .id = 'M', }, },
	};
	struct ncmenu_item glight_items[] = {
		{ .desc = "Details window", .shortcut = { .id = 'v', }, },
		{ .desc = "Help window", .shortcut = { .id = 'H', }, },
		{ .desc = "Diagnostics", .shortcut = { .id = 'D', }, },
		{ .desc = "Quit", .shortcut = { .id = 'q', }, },
	};
	struct ncmenu_section sections[] = {
		{ .name = "Growlight", .items = glight_items,
			.itemcount = sizeof(glight_items) / sizeof(*glight_items),
			.shortcut = { .id = 'g', .alt = true, }, },
		{ .name = "Blockdev", .items = block_items,
			.itemcount = sizeof(block_items) / sizeof(*block_items),
			.shortcut = { .id = 'b', .alt = true, }, },
		{ .name = "Partition", .items = part_items,
			.itemcount = sizeof(part_items) / sizeof(*part_items),
			.shortcut = { .id = 'p', .alt = true, }, },
	};
	struct ncmenu_options mopts = {
		.sections = sections,
		.sectioncount = sizeof(sections) / sizeof(*sections),
	  .headerchannels = CHANNELS_RGB_INITIALIZER(0xff, 0xff, 0xff, 0x6b, 0x38, 0x6b),
		.sectionchannels = CHANNELS_RGB_INITIALIZER(0xd6, 0x70, 0xd6, 0x00, 0x00, 0x00),
	};
	struct ncmenu* nmenu = ncmenu_create(n, &mopts);
	return nmenu;
}

// ensure the version of notcurses we loaded is viable
static bool
notcurses_version_check(void){
  int major, minor, patch, tweak;
  notcurses_version_components(&major, &minor, &patch, &tweak);
  if(major < 1 || (major == 1 && minor < 7) || (major == 1 && minor == 7 && patch < 6)){
    fprintf(stderr, "Needed notcurses 1.7.6+, got %d.%d.%d.%d\n",
            major, minor, patch, tweak);
    return false;
  }
  return true;
}

int main(int argc, char * const *argv){
  const glightui ui = {
    .vdiag = vdiag,
    .boxinfo = boxinfo,
    .adapter_event = adapter_callback,
    .block_event = block_callback,
    .adapter_free = adapter_free,
    .block_free = block_free,
  };
  struct panel_state *ps;
  int showhelp = 1;

  if(setlocale(LC_ALL, "") == NULL){
    fprintf(stderr,"Warning: couldn't load locale\n");
    return EXIT_FAILURE;
  }
  sigset_t sigmask;
  // ensure SIGWINCH is delivered only to a thread doing input
  sigemptyset(&sigmask);
  sigaddset(&sigmask, SIGWINCH);
  pthread_sigmask(SIG_SETMASK, &sigmask, NULL);
  if(!notcurses_version_check()){
    return EXIT_FAILURE;
  }
  notcurses_options opts = { };
  opts.flags = NCOPTION_INHIBIT_SETLOCALE;
	// opts.loglevel = NCLOGLEVEL_TRACE;
  if((NC = notcurses_init(&opts, stdout)) == NULL){
    return EXIT_FAILURE;
  }
	notcurses_mouse_enable(NC);
  int ydim, xdim;
  notcurses_stddim_yx(NC, &ydim, &xdim);
  struct ncplane* n = ncplane_new(notcurses_stdplane(NC), ydim - 2, xdim, 1, 0, NULL, NULL);
  if(n == NULL){
    notcurses_stop(NC);
    return EXIT_FAILURE;
  }
	if((mainmenu = create_menu(notcurses_stdplane(NC))) == NULL){
    notcurses_stop(NC);
    return EXIT_FAILURE;
	}
  ps = show_splash(L"Initializing...");
  ncreel_options popts;
  memset(&popts, 0, sizeof(popts));
  popts.bordermask = NCBOXMASK_TOP | NCBOXMASK_BOTTOM
                     | NCBOXMASK_LEFT | NCBOXMASK_RIGHT;
  popts.tabletmask = NCBOXMASK_TOP | NCBOXMASK_BOTTOM
                     | NCBOXMASK_LEFT | NCBOXMASK_RIGHT;
  PR = ncreel_create(n, &popts);
  if(PR == NULL){
    kill_splash(ps);
    notcurses_stop(NC);
    dump_diags();
    return EXIT_FAILURE;
  }
  locked_diag("by nick black <nickblack@linux.com>");
  if(growlight_init(argc, argv, &ui, &showhelp)){
    ncreel_destroy(PR);
    PR = NULL;
    kill_splash(ps);
    notcurses_stop(NC);
    dump_diags();
    return EXIT_FAILURE;
  }
  lock_growlight();
  kill_splash(ps);
  if(showhelp){
    toggle_panel(n, &help, display_help);
    screen_update();
  }
  unlock_growlight();
  handle_input(n);
  shutdown_cycle(); // calls exit() on all paths
}
