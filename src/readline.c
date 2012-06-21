#include <term.h>
#include <wchar.h>
#include <errno.h>
#include <unistd.h>
#include <wctype.h>
#include <assert.h>
#include <stdlib.h>
#include <locale.h>
#include <readline/history.h>
#include <readline/readline.h>

#include "fs.h"
#include "mbr.h"
#include "zfs.h"
#include "swap.h"
#include "sysfs.h"
#include "popen.h"
#include "config.h"
#include "target.h"
#include "ptable.h"
#include "health.h"
#include "growlight.h"

#ifdef HAVE_CURSES_H
#include <curses.h>
#else
#ifdef HAVE_NCURSES_H
#include <ncursesw.h>
#else
#ifdef HAVE_NCURSESW_H
#include <ncurses.h>
#else
#ifdef HAVE_NCURSES_CURSES_H
#include <ncurses/curses.h>
#else
#ifdef HAVE_NCURSESW_CURSES_H
#include <ncursesw/curses.h>
#endif
#endif
#endif
#endif
#endif

#define U64STRLEN 20    // Does not include a '\0' (18,446,744,073,709,551,616)
#define U64FMT "%-20ju"
#define U32FMT "%-10ju"
#define PREFIXSTRLEN 7  // Does not include a '\0' (xxx.xxU)
#define PREFIXFMT "%7s"
#define UUIDSTRLEN 36

// Used by quit() to communicate back to the main readline loop
static unsigned lights_off;
static unsigned use_terminfo;

static int
use_terminfo_color(int ansicolor,int boldp){
	if(use_terminfo){
#ifdef HAVE_CURSES
		const char *attrstr = boldp ? "bold" : "sgr0";
		const char *color,*attr;
		char *setaf;

		if((attr = tigetstr(attrstr)) == NULL){
			fprintf(stderr,"Couldn't get terminfo %s\n",attrstr);
			return -1;
		}
		putp(attr);
		if((setaf = tigetstr("setaf")) == NULL){
			fprintf(stderr,"Couldn't get terminfo setaf\n");
			return -1;
		}
		if((color = tparm(setaf,ansicolor)) == NULL){
			fprintf(stderr,"Couldn't get terminfo color %d\n",ansicolor);
			return -1;
		}
		putp(color);
	}
#endif
	return 0;
}

#ifndef HAVE_CURSES
#define use_terminfo_color(color,bold) use_terminfo_color(0,bold)
#endif

static inline int
usage(wchar_t * const *args,const char *arghelp){
	fprintf(stderr,"Usage: %ls %s\n",*args,arghelp);
	return -1;
}

static int
wstrtoull(const wchar_t *wstr,unsigned long long *ull){
	char buf[BUFSIZ],*e;

	if(snprintf(buf,sizeof(buf),"%ls",wstr) >= (int)sizeof(buf)){
		fprintf(stderr,"Bad numeric value: %ls\n",wstr);
		return -1;
	}
	if(buf[0] == '-'){
		fprintf(stderr,"Negative number: %ls\n",wstr);
		return -1;
	}
	errno = 0;
	*ull = strtoull(buf,&e,0);
	if(*ull == ULLONG_MAX && errno == ERANGE){
		fprintf(stderr,"Number too large: %ls\n",wstr);
		return -1;
	}
	if(e == buf){
		fprintf(stderr,"Bad numeric value: %ls\n",wstr);
		return -1;
	}
	if(*e){
		if(e[1]){
			fprintf(stderr,"Invalid number: %ls\n",wstr);
			return -1;
		}
		switch(*e){
			case 'E': case 'e':
				*ull *= 1000000000000000000; break;
			case 'P': case 'p':
				*ull *= 1000000000000000; break;
			case 'T': case 't':
				*ull *= 1000000000000; break;
			case 'G': case 'g':
				*ull *= 1000000000; break;
			case 'M': case 'm':
				*ull *= 1000000; break;
			case 'K': case 'k':
				*ull *= 1000; break;
			default:
			fprintf(stderr,"Invalid number: %ls\n",wstr);
			return -1;
		}
	}
	return 0;
}

static int
make_wfilesystem(device *d,const wchar_t *fs){
	char sfs[NAME_MAX];

	if(snprintf(sfs,sizeof(sfs),"%ls",fs) >= (int)sizeof(fs)){
		fprintf(stderr,"Bad partition table type: %ls\n",fs);
		return -1;
	}
	return make_filesystem(d,sfs);
}

static controller *
lookup_wcontroller(const wchar_t *dev){
	char sdev[NAME_MAX];

	if(snprintf(sdev,sizeof(sdev),"%ls",dev) >= (int)sizeof(dev)){
		fprintf(stderr,"Bad controller name: %ls\n",dev);
		return NULL;
	}
	return lookup_controller(sdev);
}

static device *
lookup_wdevice(const wchar_t *dev){
	char sdev[NAME_MAX];

	if(snprintf(sdev,sizeof(sdev),"%ls",dev) >= (int)sizeof(dev)){
		fprintf(stderr,"Bad device name: %ls\n",dev);
		return NULL;
	}
	return lookup_device(sdev);
}

static int
make_partition_wtable(device *d,const wchar_t *tbl){
	char stbl[NAME_MAX];

	if(snprintf(stbl,sizeof(stbl),"%ls",tbl) >= (int)sizeof(tbl)){
		fprintf(stderr,"Bad partition table type: %ls\n",tbl);
		return -1;
	}
	return make_partition_table(d,stbl);
}

static int
prepare_wmount(device *d,const wchar_t *path,const wchar_t *fs,const wchar_t *ops){
	char spath[PATH_MAX],sfs[NAME_MAX],sops[PATH_MAX];

	if(snprintf(spath,sizeof(spath),"%ls",path) >= (int)sizeof(path)){
		fprintf(stderr,"Bad path: %ls\n",path);
		return -1;
	}
	if(snprintf(sfs,sizeof(sfs),"%ls",fs) >= (int)sizeof(fs)){
		fprintf(stderr,"Bad filesystem type: %ls\n",fs);
		return -1;
	}
	if(snprintf(sops,sizeof(sops),"%ls",ops) >= (int)sizeof(ops)){
		fprintf(stderr,"Bad filesystem options: %ls\n",ops);
		return -1;
	}
	return prepare_mount(d,spath,sfs,sops);
}

// Takes an arbitrarily large number, and prints it into a fixed-size buffer by
// adding the necessary SI suffix. Usually, pass a |PREFIXSTRLEN+1|-sized
// buffer to generate up to PREFIXSTRLEN characters.
//
// val: value to print
// decimal: scaling. '1' if none has taken place.
// buf: buffer in which string will be generated
// bsize: size of buffer. ought be at least PREFIXSTRLEN
// omitdec: inhibit printing of all-0 decimal portions
// mult: base of suffix system (1000 or 1024)
// uprefix: character to print following suffix ('i' for kibibytes basically)
//
// For full safety, pass in a buffer that can hold the decimal representation
// of the largest uintmax_t plus three (one for the unit, one for the decimal
// separator, and one for the NUL byte).
static const char *
genprefix(uintmax_t val,unsigned decimal,char *buf,size_t bsize,
			int omitdec,unsigned mult,int uprefix){
	const char prefixes[] = "KMGTPEY";
	unsigned consumed = 0;
	uintmax_t div;

	div = mult;
	while((val / decimal) >= div && consumed < strlen(prefixes)){
		div *= mult;
		if(UINTMAX_MAX / div < mult){ // watch for overflow
			break;
		}
		++consumed;
	}
	if(div != mult){
		div /= mult;
		val /= decimal;
		if((val % div) / ((div + 99) / 100) || omitdec == 0){
			snprintf(buf,bsize,"%ju.%02ju%c%c",val / div,(val % div) / ((div + 99) / 100),
					prefixes[consumed - 1],uprefix);
		}else{
			snprintf(buf,bsize,"%ju%c%c",val / div,prefixes[consumed - 1],uprefix);
		}
	}else{
		if(val % decimal || omitdec == 0){
			snprintf(buf,bsize,"%ju.%02ju",val / decimal,val % decimal);
		}else{
			snprintf(buf,bsize,"%ju",val / decimal);
		}
	}
	return buf;
}

static inline const char *
qprefix(uintmax_t val,unsigned decimal,char *buf,size_t bsize,int omitdec){
	return genprefix(val,decimal,buf,bsize,omitdec,1000,'\0');
}

static inline const char *
bprefix(uintmax_t val,unsigned decimal,char *buf,size_t bsize,int omitdec){
	return genprefix(val,decimal,buf,bsize,omitdec,1024,'i');
}

#define ZERO_ARG_CHECK(args,arghelp) \
 if(args[1]){ usage(args,arghelp); return -1 ; }

#define ONE_ARG_CHECK(args,arghelp) \
 if(!args[1] || args[2]){ usage(args,arghelp); return -1 ; }

#define TWO_ARG_CHECK(args,arghelp) \
 if(!args[1] || !args[2] || args[3]){ usage(args,arghelp); return -1 ; }

static int help(wchar_t * const *,const char *);
static int print_mdadm(const device *,int,int);

static int
print_target(const mntentry *m){
	int r = 0,rr;

	r += rr = printf("%-*.*s %-5.5s %-36.36s " PREFIXFMT " %-6.6s\n %s %s\n",
			FSLABELSIZ,FSLABELSIZ,m->label ? m->label : "n/a",
			m->fs,
			m->uuid ? m->uuid : "n/a",
			"-1", // FIXME
			m->dev,
			m->path,m->ops);
	if(rr < 0){
		return -1;
	}
	return r;
}

static int
print_mount(const device *d){
	char buf[PREFIXSTRLEN + 1];
	int r = 0,rr;

	r += rr = printf("%-*.*s %-5.5s %-36.36s " PREFIXFMT " %-6.6s\n %s %s\n",
			FSLABELSIZ,FSLABELSIZ,d->label ? d->label : "n/a",
			d->mnttype,
			d->uuid ? d->uuid : "n/a",
			qprefix(d->size * d->logsec,1,buf,sizeof(buf),0),
			d->name,
			d->mnt,d->mntops);
	if(rr < 0){
		return -1;
	}
	return r;
}

static int
print_unmount(const device *d){
	char buf[PREFIXSTRLEN + 1];
	int r = 0,rr;

	r += rr = printf("%-*.*s %-5.5s %-36.36s " PREFIXFMT " %-6.6s\n",
			FSLABELSIZ,FSLABELSIZ,d->label ? d->label : "n/a",
			d->mnttype,
			d->uuid ? d->uuid : "n/a",
			qprefix(d->size * d->logsec,1,buf,sizeof(buf),0),
			d->name);
	if(rr < 0){
		return -1;
	}
	return r;
}

static int
print_swap(const device *p){
	int r = 0,rr;

	assert(p->mnttype);
	r += rr = printf("%-*.*s %-5.5s %-36.36s %-6.6s",
			FSLABELSIZ,FSLABELSIZ,p->label ? p->label : "n/a",
			p->mnttype,
			p->uuid ? p->uuid : "n/a",p->name);
	if(rr < 0){
		return -1;
	}
	if(p->swapprio >= SWAP_MAXPRIO){
		r += rr = printf(" pri=%d\n",p->swapprio);
	}else{
		r += rr = printf("\n");
	}
	if(rr < 0){
		return -1;
	}
	return r;
}

static int
print_fs(const device *p,int descend){
	int r = 0,rr;

	use_terminfo_color(COLOR_GREEN,1);
	if(p->mnttype == NULL){
		return 0;
	}
	if(p->swapprio != SWAP_INVALID){
		if(!descend){
			return 0;
		}
		r += rr = print_swap(p);
	}
	if(p->target){
		r += rr = print_target(p->target);
		if(rr < 0){
			return -1;
		}
	}
	if(p->mnt){
		r += rr = print_mount(p);
	}else{
		r += rr = print_unmount(p);
	}
	if(rr < 0){
		return -1;
	}
	return r;
}

static int
print_partition(const device *p,int descend){
	char buf[PREFIXSTRLEN + 1];
	int r = 0,rr;

	use_terminfo_color(COLOR_BLUE,1);
	r += rr = printf("%-10.10s %-36.36s " PREFIXFMT " %-4.4s %ls\n",
			p->name,
			p->partdev.uuid ? p->partdev.uuid : "n/a",
			qprefix(p->size * p->logsec,1,buf,sizeof(buf),0),
			((p->partdev.partrole == PARTROLE_PRIMARY || p->partdev.partrole == PARTROLE_GPT) && (p->partdev.flags & 0xffu) == 0x80) ? "Boot" :
				p->partdev.partrole == PARTROLE_PRIMARY ? "Pri" :
				p->partdev.partrole == PARTROLE_EXTENDED ? "Ext" :
				p->partdev.partrole == PARTROLE_LOGICAL ? "Log" :
				p->partdev.partrole == PARTROLE_GPT ? "GPT" :
				p->partdev.partrole == PARTROLE_EPS ? "ESP" : "Unk",
				p->partdev.pname ? p->partdev.pname : L"n/a");
	if(rr < 0){
		return -1;
	}
	if(!descend){
		return r;
	}
	r += rr = print_fs(p,0);
	if(rr < 0){
		return -1;
	}
	return r;
}

static const char *
pcie_gen(unsigned gen){
	switch(gen){
		case 1: return "1.0";
		case 2: return "2.0";
		case 3: return "3.0";
		default: return "unknown";
	}
}

static const char *
transport_str(transport_e t){
	return t == SERIAL_ATAIII ? "S3" : t == SERIAL_ATAII ? "S2" :
	 t == SERIAL_ATAI ? "I" : t == SERIAL_ATA8 ? "AST" :
	 t == SERIAL_UNKNOWN ? "Srl" : t == PARALLEL_ATA ? "Par" :
	 t == AGGREGATE_MIXED ? "Mix" : "Ukn";
}

static int
print_drive(const device *d,int descend){
	char buf[PREFIXSTRLEN + 1];
	const device *p;
	int r = 0,rr;

	switch(d->layout){
	case LAYOUT_NONE:{
		if(d->blkdev.realdev){
			if(d->blkdev.rotate){
				use_terminfo_color(COLOR_YELLOW,0);
			}else{
				use_terminfo_color(COLOR_CYAN,1);
			}
		}else{
			use_terminfo_color(COLOR_MAGENTA,1);
		}
		r += rr = printf("%-10.10s %-16.16s %-4.4s " PREFIXFMT " %4uB %c%c%c%c  %-6.6s%-16.16s %-3.3s\n",
			d->name,
			d->model ? d->model : "n/a",
			d->revision ? d->revision : "n/a",
			qprefix(d->logsec * d->size,1,buf,sizeof(buf),0),
			d->physsec,
			d->blkdev.removable ? 'R' : d->blkdev.smart ? 'S' :
				d->blkdev.realdev ? '.' : 'V',
			d->blkdev.rotate ? 'O' : '.',
			d->blkdev.wcache ? 'W' : '.',
			d->blkdev.biosboot ? 'B' : '.',
			d->blkdev.pttable ? d->blkdev.pttable : "none",
			d->wwn ? d->wwn : "n/a",
			d->blkdev.realdev ? transport_str(d->blkdev.transport) : "n/a"
			);
		break;
	}case LAYOUT_MDADM:{
		use_terminfo_color(COLOR_YELLOW,1);
		r += rr = printf("%-10.10s %-16.16s %-4.4s " PREFIXFMT " %4uB %c%c%c%c  %-6.6s%-16.16s %-3.3s\n",
			d->name,
			d->model ? d->model : "n/a",
			d->revision ? d->revision : "n/a",
			qprefix(d->logsec * d->size,1,buf,sizeof(buf),0),
			d->physsec, 'M', '.', '.', '.',
			"n/a",
			d->wwn ? d->wwn : "n/a",
			transport_str(d->mddev.transport)
			);
		break;
	}case LAYOUT_ZPOOL:{
		use_terminfo_color(COLOR_RED,1);
		r += rr = printf("%-10.10s %-16.16s %-4.4s " PREFIXFMT " %4uB %c%c%c%c  %-6.6s%-16.16s %-3.3s\n",
			d->name,
			d->model ? d->model : "n/a",
			d->revision ? d->revision : "n/a",
			qprefix(d->logsec * d->size,1,buf,sizeof(buf),0),
			d->physsec, 'Z', '.', '.', '.',
			"n/a",
			d->wwn ? d->wwn : "n/a",
			transport_str(d->zpool.transport)
			);
		break;
	}case LAYOUT_PARTITION:{
		return -1;
	}default:
		return -1;
	}
	if(rr < 0){
		return -1;
	}
	if(!descend){
		return r;
	}
	r += rr = print_fs(d,descend);
	if(rr < 0){
		return -1;
	}
	for(p = d->parts ; p ; p = p->next){
		r += rr = print_partition(p,descend);
		if(rr < 0){
			return -1;
		}
	}
	return r;
}

static int
print_zpool(const device *d,int descend){
	char buf[PREFIXSTRLEN + 1];
	int r = 0,rr;

	if(d->layout != LAYOUT_ZPOOL){
		return 0;
	}
	r += rr = printf("%-10.10s %-36.36s " PREFIXFMT " %4uB %-6.6s%5lu %-6.6s\n",
			d->name,
			d->uuid ? d->uuid : "n/a",
			qprefix(d->logsec * d->size,1,buf,sizeof(buf),0),
			d->physsec, "n/a",
			d->zpool.disks,d->zpool.level ? d->zpool.level : "n/a"
			);
	if(rr < 0){
		return -1;
	}
	if(!descend){
		return r;
	}
	/*
	for(md = d->mddev.slaves ; md ; md = md->next){
		r += rr = print_dev_mplex(md->component,1,descend);
		if(rr < 0){
			return -1;
		}
		if(strcmp(md->name,md->component->name)){
			const device *p;

			for(p = md->component->parts ; p ; p = p->next){
				if(strcmp(md->name,p->name)){
					continue;
				}
				r += rr = print_partition(p,descend);
				if(rr < 0){
					return -1;
				}
			}
		}

	}
	*/
	return r;
}

static int
print_dev_mplex(const device *d,int prefix,int descend){
	switch(d->layout){
		case LAYOUT_NONE:
			return print_drive(d,descend);
		case LAYOUT_PARTITION:
			return print_partition(d,descend);
		case LAYOUT_MDADM:
			return print_mdadm(d,prefix,descend);
		case LAYOUT_ZPOOL:
			return print_zpool(d,descend);
		default:
			return -1;
	}
}

static int
print_mdadm(const device *d,int prefix,int descend){
	char buf[PREFIXSTRLEN + 1];
	const mdslave *md;
	int r = 0,rr;

	if(d->layout != LAYOUT_MDADM){
		return 0;
	}
	use_terminfo_color(COLOR_YELLOW,1);
	r += rr = printf("%-*.*s%-10.10s %-36.36s " PREFIXFMT " %4uB %-6.6s%5lu %-6.6s\n",
			prefix,prefix,"",
			d->name,
			d->uuid ? d->uuid : "n/a",
			qprefix(d->logsec * d->size,1,buf,sizeof(buf),0),
			d->physsec, "n/a",
			d->mddev.disks,d->mddev.level
			);
	if(rr < 0){
		return -1;
	}
	if(!descend){
		return r;
	}
	for(md = d->mddev.slaves ; md ; md = md->next){
		r += rr = print_dev_mplex(md->component,1,descend);
		if(rr < 0){
			return -1;
		}
		if(strcmp(md->name,md->component->name)){
			const device *p;

			for(p = md->component->parts ; p ; p = p->next){
				if(strcmp(md->name,p->name)){
					continue;
				}
				r += rr = print_partition(p,descend);
				if(rr < 0){
					return -1;
				}
			}
		}

	}
	return r;
}

static int
print_controller(const controller *c,int descend){
	int r = 0,rr;
	device *d;

	use_terminfo_color(COLOR_WHITE,1);
	switch(c->bus){
		case BUS_PCIe:
			if(c->pcie.lanes_neg == 0){
				r += rr = printf("Southbridge device %04hx:%02x.%02x.%x %s\n",
					c->pcie.domain,c->pcie.bus,
					c->pcie.dev,c->pcie.func,
					c->driver);
			}else{
				r += rr = printf("PCI Express device %04hx:%02x.%02x.%x (x%u, gen %s) %s\n",
					c->pcie.domain,c->pcie.bus,
					c->pcie.dev,c->pcie.func,
					c->pcie.lanes_neg,pcie_gen(c->pcie.gen),
					c->driver);
			}
			if(rr < 0){
				return -1;
			}
			break;
		case BUS_VIRTUAL:
		case BUS_UNKNOWN:
			return 0;
		default:
			fprintf(stderr,"Unknown bus type: %d\n",c->bus);
			return -1;
	}
	r += rr = printf(" %s\n",c->name);
	if(rr < 0){
		return -1;
	}
	if(!descend){
		return r;
	}
	for(d = c->blockdevs ; d ; d = d->next){
		r += rr = print_drive(d,descend);
		if(rr < 0){
			return -1;
		}
	}
	return r;
}

static int
adapter(wchar_t * const *args,const char *arghelp){
	const controller *c;
	int descend;

	if(args[1] == NULL){
		descend = 0;
	}else if(wcscmp(args[1],L"-v") == 0 && args[2] == NULL){
		descend = 1;
	}else{
		if(args[2] && !args[3]){
			controller *c;

			if((c = lookup_wcontroller(args[2])) == NULL){
				return -1;
			}
			if(wcscmp(args[1],L"reset") == 0){
				if(reset_controller(c)){
					return -1;
				}
				return 0;
			}else if(wcscmp(args[1],L"rescan") == 0){
				if(rescan_controller(c)){
					return -1;
				}
				return 0;
			}
		}
		usage(args,arghelp);
		return -1;
	}
	for(c = get_controllers() ; c ; c = c->next){
		if(print_controller(c,descend) < 0){
			return -1;
		}
	}
	return 0;
}

// Walk the block devices, evaluating *fxn on each. The return value will be
// accumulated in r, unless -1 is ever returned, in which case we abort
// immediately and return -1.
static int
walk_devices(int (*fxn)(const device *,int),int descend){
	const controller *c;
	int rr,r = 0;

	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			r += rr = fxn(d,descend);
			if(rr < 0){
				return -1;
			}
			for(p = d->parts ; p ; p = p->next){
				r += rr = fxn(p,descend);
				if(rr < 0){
					return -1;
				}
			}
		}
	}
	return 0;
}

static int
zpool(wchar_t * const *args,const char *arghelp){
	int descend;

	if(args[1] == NULL){
		descend = 0;
	}else if(wcscmp(args[1],L"-v") == 0 && args[2] == NULL){
		descend = 1;
	}else{
		usage(args,arghelp);
		return -1;
	}
	printf("%-10.10s %-36.36s " PREFIXFMT " %5.5s %-6.6s%-6.6s%-6.6s\n",
			"Device","UUID","Bytes","PSect","Table","Disks","Level");
	if(walk_devices(print_zpool,descend)){
		return -1;
	}
	return 0;
}

static int
mdadm(wchar_t * const *args,const char *arghelp){
	const controller *c;
	int descend;

	if(args[1] == NULL){
		descend = 0;
	}else if(wcscmp(args[1],L"-v") == 0 && args[2] == NULL){
		descend = 1;
	}else{
		usage(args,arghelp);
		return -1;
	}
	printf("%-10.10s %-36.36s " PREFIXFMT " %5.5s %-6.6s%-6.6s%-6.6s\n",
			"Device","UUID","Bytes","PSect","Table","Disks","Level");
	for(c = get_controllers() ; c ; c = c->next){
		device *d;

		if(c->bus != BUS_VIRTUAL){
			continue;
		}
		for(d = c->blockdevs ; d ; d = d->next){
			if(d->layout == LAYOUT_MDADM){
				if(print_mdadm(d,0,descend) < 0){
					return -1;
				}
			}
		}
	}
	return 0;
}

static int
print_tabletypes(void){
	const char **types,*cr;
	int rr,r = 0;

	types = get_ptable_types();
	while( (cr = *types) ){
		unsigned last = !*++types;

		r += rr = printf("%s%c",cr,last ? '\n' : ',');
		if(rr < 0){
			return -1;
		}
	}
	return r;
}

static int
print_fstypes(void){
	const char **types,*cr;
	int rr,r = 0;

	types = get_fs_types();
	while( (cr = *types) ){
		unsigned last = !*++types;

		r += rr = printf("%s%c",cr,last ? '\n' : ',');
		if(rr < 0){
			return -1;
		}
	}
	return r;
}

static inline int
blockdev_dump(int descend){
	const controller *c;

	printf("%-10.10s %-16.16s %-4.4s " PREFIXFMT " %5.5s Flags %-6.6s%-16.16s %-3.3s\n",
			"Device","Model","Rev","Bytes","PSect","Table","WWN","PHY");
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			if(print_drive(d,descend) < 0){
				return -1;
			}
		}
	}
	use_terminfo_color(COLOR_WHITE,1);
	printf("\n\tFlags:\t(R)emovable, (V)irtual, (M)dadm, (Z)pool, r(O)tational,\n"
			"\t\t(W)ritecache enabled, (B)IOS bootable, (S)MART\n");
	return 0;
}

static inline int
blockdev_details(const device *d){
	unsigned z;

	if(print_drive(d,1) < 0){
		return -1;
	}
	printf("\n");
	if(d->blkdev.biossha1){
		if(printf("\nBIOS boot SHA-1: ") < 0){
			return -1;
		}
		for(z = 0 ; z < 19 ; ++z){
			if(printf("%02x:",((const unsigned char *)d->blkdev.biossha1)[z]) < 0){
				return -1;
			}
		}
		if(printf("%02x\n",((const unsigned char *)d->blkdev.biossha1)[z]) < 0){
			return -1;
		}
	}
	printf("Serial number: %s\n",d->blkdev.serial ? d->blkdev.serial : "n/a");
	printf("Transport: %s\n",
			d->blkdev.transport == SERIAL_ATAIII ? "SATA 3.0" :
			 d->blkdev.transport == SERIAL_ATAII ? "SATA 2.0" :
			 d->blkdev.transport == SERIAL_ATAI ? "SATA 1.0" :
			 d->blkdev.transport == SERIAL_ATA8 ? "ATA8-AST" :
			 d->blkdev.transport == SERIAL_UNKNOWN ? "Serial ATA" :
			 d->blkdev.transport == PARALLEL_ATA ? "Parallel ATA" :
			 d->blkdev.transport == AGGREGATE_MIXED ? "Mixed" :
			 "Unknown");
	return 0;
}

static int
blockdev(wchar_t * const *args,const char *arghelp){
	device *d;

	if(args[1] == NULL){
		return blockdev_dump(0);
	}
	if(args[2] == NULL){
		if(wcscmp(args[1],L"-v") == 0){
			return blockdev_dump(1);
		}else if(wcscmp(args[1],L"mktable") == 0){
			if(print_tabletypes() < 0){
				return -1;
			}
			return 0;
		}
		usage(args,arghelp);
		return -1;
	}
	// Everything else has a required device argument
	if((d = lookup_wdevice(args[2])) == NULL){
		return -1;
	}
	if(wcscmp(args[1],L"rescan") == 0){
		if(args[3]){
			usage(args,arghelp);
			return -1;
		}
		if(rescan_blockdev(d)){
			return -1;
		}
		return 0;
	}else if(wcscmp(args[1],L"badblocks") == 0){
		unsigned rw = 0;

		if(args[3]){
			if(args[4] || wcscmp(args[3],L"rw")){
				usage(args,arghelp);
				return -1;
			}
			rw = 1;
		}
		return badblock_scan(d,rw);
	}else if(wcscmp(args[1],L"rmtable") == 0){
		if(args[3]){
			usage(args,arghelp);
			return -1;
		}
		return wipe_ptable(d);
	}else if(wcscmp(args[1],L"wipebiosboot") == 0){
		if(args[3]){
			usage(args,arghelp);
			return -1;
		}
		return wipe_biosboot(d);
	}else if(wcscmp(args[1],L"wipedosmbr") == 0){
		if(args[3]){
			usage(args,arghelp);
			return -1;
		}
		return wipe_dosmbr(d);
	}else if(wcscmp(args[1],L"detail") == 0){
		if(args[3]){
			usage(args,arghelp);
			return -1;
		}
		return blockdev_details(d);
	}else if(wcscmp(args[1],L"mktable") == 0){
		if(args[3] == NULL || args[4]){
			usage(args,arghelp);
			return -1;
		}
		make_partition_wtable(d,args[3]);
		return 0;
	}
	usage(args,arghelp);
	return -1;
}

static int
print_partition_attributes(void){
	printf("GPT flags:\n");
	printf("\t%016llx %s\n",0x0000000000000001llu,"Required partition");
	printf("\t%016llx %s\n",0x0000000000000002llu,"Legacy BIOS bootable");
	printf("\t%016llx %s\n",0x1000000000000000llu,"Read-only");
	printf("\t%016llx %s\n",0x2000000000000000llu,"Shadow copy");
	printf("\t%016llx %s\n",0x4000000000000000llu,"Hidden");
	printf("\t%016llx %s\n",0x8000000000000000llu,"No automount");
	printf("MBR flags:\n");
	printf("\t%02x %s\n",0x80u,"Bootable");
	return 0;
}

static int
print_partition_types(void){
	printf("GPT codes:\n");
	printf(" 0700 Microsoft basic data  0c01 Microsoft reserved    2700 Windows RE\n"
		" 4200 Windows LDM data      4201 Windows LDM metadata  7501 IBM GPFS            \n"
		" 7f00 ChromeOS kernel       7f01 ChromeOS root         7f02 ChromeOS reserved   \n"
		" 8200 Linux swap            8300 Linux filesystem      8301 Linux reserved      \n"
		" 8e00 Linux LVM             a500 FreeBSD disklabel     a501 FreeBSD boot        \n"
		" a502 FreeBSD swap          a503 FreeBSD UFS           a504 FreeBSD ZFS         \n"
		" a505 FreeBSD Vinum/RAID    a580 Midnight BSD data     a581 Midnight BSD boot   \n"
		" a582 Midnight BSD swap     a583 Midnight BSD UFS      a584 Midnight BSD ZFS    \n"
		" a585 Midnight BSD Vinum    a800 Apple UFS             a901 NetBSD swap         \n"
		" a902 NetBSD FFS            a903 NetBSD LFS            a904 NetBSD concatenated \n"
		" a905 NetBSD encrypted      a906 NetBSD RAID           ab00 Apple boot          \n"
		" af00 Apple HFS/HFS+        af01 Apple RAID            af02 Apple RAID offline  \n"
		" af03 Apple label           af04 AppleTV recovery      af05 Apple Core Storage  \n"
		" be00 Solaris boot          bf00 Solaris root          bf01 Solaris /usr & Mac Z\n"
		" bf02 Solaris swap          bf03 Solaris backup        bf04 Solaris /var        \n"
		" bf05 Solaris /home         bf06 Solaris alternate se  bf07 Solaris Reserved 1  \n"
		" bf08 Solaris Reserved 2    bf09 Solaris Reserved 3    bf0a Solaris Reserved 4  \n"
		" bf0b Solaris Reserved 5    c001 HP-UX data            c002 HP-UX service       \n"
		" ef00 EFI System            ef01 MBR partition scheme  ef02 BIOS boot partition \n"
		" fd00 Linux RAID\n");
	printf("MBR flags:\n");
	printf(" 0  Empty           1e  Hidd FAT16 LBA  80  Minix <1.4a     bf  Solaris         \n"
		" 1  FAT12           24  NEC DOS         81  Minix >1.4b     c1  DRDOS/2 FAT12   \n"
		" 2  XENIX root      39  Plan 9          82  Linux swap      c4  DRDOS/2 smFAT16 \n"
		" 3  XENIX usr       3c  PMagic recovery 83  Linux           c6  DRDOS/2 FAT16   \n"
		" 4  Small FAT16     40  Venix 80286     84  OS/2 hidden C:  c7  Syrinx          \n"
		" 5  Extended        41  PPC PReP Boot   85  Linux extended  da  Non-FS data     \n"
		" 6  FAT16           42  SFS             86  NTFS volume set db  CP/M / CTOS     \n"
		" 7  HPFS/NTFS       4d  QNX4.x          87  NTFS volume set de  Dell Utility    \n"
		" 8  AIX             4e  QNX4.x 2nd part 88  Linux plaintext df  BootIt          \n"
		" 9  AIX bootable    4f  QNX4.x 3rd part 8e  Linux LVM       e1  DOS access      \n"
		" a  OS/2 boot mgr   50  OnTrack DM      93  Amoeba          e3  DOS R/O         \n"
		" b  FAT32           51  OnTrackDM6 Aux1 94  Amoeba BBT      e4  SpeedStor       \n"
		" c  FAT32 LBA       52  CP/M            9f  BSD/OS          eb  BeOS fs         \n"
		" e  FAT16 LBA       53  OnTrackDM6 Aux3 a0  Thinkpad hib    ee  GPT             \n"
		" f  Extended LBA    54  OnTrack DM6     a5  FreeBSD         ef  EFI FAT         \n"
		"10  OPUS            55  EZ Drive        a6  OpenBSD         f0  Lnx/PA-RISC bt  \n"
		"11  Hidden FAT12    56  Golden Bow      a7  NeXTSTEP        f1  SpeedStor       \n"
		"12  Compaq diag     5c  Priam Edisk     a8  Darwin UFS      f2  DOS secondary   \n"
		"14  Hidd Sm FAT16   61  SpeedStor       a9  NetBSD          f4  SpeedStor       \n"
		"16  Hidd FAT16      63  GNU HURD/SysV   ab  Darwin boot     fd  Lnx RAID auto   \n"
		"17  Hidd HPFS/NTFS  64  Netware 286     b7  BSDI fs         fe  LANstep         \n"
		"18  AST SmartSleep  65  Netware 386     b8  BSDI swap       ff  XENIX BBT       \n"
		"1b  Hidd FAT32      70  DiskSec MltBoot bb  Boot Wizard Hid \n"
		"1c  Hidd FAT32 LBA  75  PC/IX           be  Solaris boot    \n");
	return 0;
}

static int
partition(wchar_t * const *args,const char *arghelp){
	const controller *c;
	int descend;

	if(args[1] == NULL){
		descend = 0;
	}else if(wcscmp(args[1],L"-v") == 0 && args[2] == NULL){
		descend = 1;
	}else{
		device *d;

		if(wcscmp(args[1],L"setflag") == 0){
			unsigned long long ull;
			unsigned val;

			if(!args[2]){
				print_partition_attributes();
				return 0;
			}
			if((d = lookup_wdevice(args[2])) == NULL){
				usage(args,arghelp);
				return -1;
			}
			if(!args[3] || !args[4] || args[5]){
				usage(args,arghelp);
				return -1;
			}else if(wstrtoull(args[4],&ull)){
				usage(args,arghelp);
				return -1;
			}else if(ull > (1ull << 62) || ull == 0){
				usage(args,arghelp);
				return -1;
			}else if( (ull & (ull - 1u)) ){ // ought be power of 2
				usage(args,arghelp);
				return -1;
			}else if(wcscasecmp(args[3],L"on") == 0){
				val = 1;
			}else if(wcscasecmp(args[3],L"off") == 0){
				val = 0;
			}else{
				usage(args,arghelp);
				return -1;
			}
			if(partition_set_flag(d,ull,val)){
				return -1;
			}
			return 0;
		}else if(wcscmp(args[1],L"settype") == 0){
			unsigned long long ull;

			if(!args[2]){
				print_partition_types();
				return 0;
			}
			if((d = lookup_wdevice(args[2])) == NULL){
				usage(args,arghelp);
				return -1;
			}
			if(!args[3] || args[4]){
				usage(args,arghelp);
				return -1;
			}else if(wstrtoull(args[3],&ull)){
				usage(args,arghelp);
				return -1;
			}else if(ull > 0xffff || ull == 0){
				usage(args,arghelp);
				return -1;
			}
			if(partition_set_code(d,ull)){
				return -1;
			}
			return 0;
		}
		if(args[2] == NULL){ // the remainder always have an arg
			usage(args,arghelp);
			return -1;
		}
		if((d = lookup_wdevice(args[2])) == NULL){
			usage(args,arghelp);
			return -1;
		}
		if(wcscmp(args[1],L"add") == 0){
			unsigned long long ull;

			// target dev == 2, 3 == name, 4 == size
			if(!args[3] || !args[4] || args[5]){
				usage(args,arghelp);
				return -1;
			}
			if(wstrtoull(args[4],&ull)){
				usage(args,arghelp);
				return -1;
			}
			if(add_partition(d,args[3],ull)){
				return -1;
			}
			return 0;
		}else if(wcscmp(args[1],L"del") == 0){
			if(args[3]){
				usage(args,arghelp);
				return -1;
			}
			if(wipe_partition(d)){
				return -1;
			}
			return 0;
		}else if(wcscmp(args[1],L"fsck") == 0){
			if(args[3]){
				usage(args,arghelp);
				return -1;
			}
			if(check_partition(d)){
				return -1;
			}
			return 0;
		}else if(wcscmp(args[1],L"setname") == 0){
			if(!args[3] || args[4]){
				usage(args,arghelp);
				return -1;
			}
			if(name_partition(d,args[3])){
				return -1;
			}
			return 0;
		}
		usage(args,arghelp);
		return -1;
	}
	printf("%-10.10s %-36.36s " PREFIXFMT " %-4.4s %s\n",
			"Partition","UUID","Bytes","Role","Name");
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			for(p = d->parts ; p ; p = p->next){
				if(print_partition(p,descend) < 0){
					return -1;
				}
			}
		}
	}
	return 0;
}

static int
mounts(wchar_t * const *args,const char *arghelp){
	const controller *c;

	ZERO_ARG_CHECK(args,arghelp);
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			if(d->mnt){
				if(print_mount(d) < 0){
					return -1;
				}
			}else if(d->target){
				if(print_target(p->target) < 0){
					return -1;
				}
			}
			for(p = d->parts ; p ; p = p->next){
				if(p->mnt){
					if(print_mount(p) < 0){
						return -1;
					}
				}else if(p->target){
					if(print_target(p->target) < 0){
						return -1;
					}
				}
			}
		}
	}
	return 0;
}

static int
print_map(void){
	const controller *c;
	int rr,r = 0;

	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			if(d->target){
				r += rr = print_target(d->target);
				if(rr < 0){
					return -1;
				}
			}
			for(p = d->parts ; p ; p = p->next){
				if(p->target){
					r += rr = print_target(p->target);
					if(rr < 0){
						return -1;
					}
				}
			}
		}
	}
	return r;
}

static int
map(wchar_t * const *args,const char *arghelp){
	device *d;

	if(!args[1]){
		if(print_map() < 0){
			return -1;
		}
		return 0;
	}
	if(!args[2] || !args[3] || !args[4] || args[5]){
		usage(args,arghelp);
		return -1;
	}
	if((d = lookup_wdevice(args[1])) == NULL/* || (d = lookup_dentry(d,args[1])) == NULL*/){
		return -1;
	}
	if(args[2][0] != L'/'){
		fprintf(stderr,"Not an absolute path: %ls\n",args[2]);
		return -1;
	}
	if(prepare_wmount(d,args[2],args[3],args[4])){
		return -1;
	}
	return 0;
}

static int
print_swaps(const device *d,int descend){
	char buf[PREFIXSTRLEN + 1];
	int rr,r = 0;

	if(descend){
		fprintf(stderr,"Can't descend for swap!\n");
	}
	if(d->swapprio == SWAP_INVALID){
		return 0;
	}
	if(d->swapprio != SWAP_INACTIVE){
		r += rr = printf("%-*.*s %-5d %-36.36s " PREFIXFMT " %s\n",
				FSLABELSIZ,FSLABELSIZ,d->label ? d->label : "n/a",
				d->swapprio,d->uuid ? d->uuid : "n/a",
				qprefix(d->logsec * d->size,1,buf,sizeof(buf),0),
				d->name);
	}else{
		r += rr = printf("%-*.*s %-5.5s %-36.36s " PREFIXFMT " %s\n",
				FSLABELSIZ,FSLABELSIZ,d->label ? d->label : "n/a",
				"off",d->uuid ? d->uuid : "n/a",
				qprefix(d->logsec * d->size,1,buf,sizeof(buf),0),
				d->name);
	}
	if(rr < 0){
		return -1;
	}
	return r;
}

static inline int
fs_dump(int descend){
	printf("%-*.*s %-5.5s %-36.36s " PREFIXFMT " %s\n",
			FSLABELSIZ,FSLABELSIZ,"Label",
			"Type","UUID","Bytes","Device");
	if(walk_devices(print_fs,descend)){
		return -1;
	}
	return 0;
}

static int
fs(wchar_t * const *args,const char *arghelp){
	device *d;

	if(args[1] == NULL){
		return fs_dump(0);
	}
	if(args[2] == NULL){
		if(wcscmp(args[1],L"mkfs") == 0){
			if(print_fstypes() < 0){
				return -1;
			}
			return 0;
		}
		usage(args,arghelp);
		return -1;
	}
// Everything else has a required device argument
	if((d = lookup_wdevice(args[2])) == NULL){
		return -1;
	}
	if(wcscmp(args[1],L"mkfs") == 0){
		if(make_wfilesystem(d,args[3])){
			return -1;
		}
		return 0;
	}
	usage(args,arghelp);
	return -1;
}

static int
swap(wchar_t * const *args,const char *arghelp){
	device *d;
	if(!args[1]){
		if(printf("%-*.*s %-5.5s %-36.36s " PREFIXFMT " %s\n",FSLABELSIZ,FSLABELSIZ,
					"Label","Prio","UUID","Bytes","Device") < 0){
			return -1;
		}
		if(walk_devices(print_swaps,0)){
			return -1;
		}
		return 0;
	}
	TWO_ARG_CHECK(args,arghelp);
	if((d = lookup_wdevice(args[2])) == NULL){
		return -1;
	}
	if(wcscmp(args[1],L"on") == 0){
		if(swapondev(d)){
			return -1;
		}
	}else if(wcscmp(args[1],L"off") == 0){
		if(swapoffdev(d)){
			return -1;
		}
	}else{
		usage(args,arghelp);
		return -1;
	}
	return 0;
}

static int
benchmark(wchar_t * const *args,const char *arghelp){
	ZERO_ARG_CHECK(args,arghelp);
	fprintf(stderr,"Sorry, not yet implemented\n");
	// FIXME things to do:
	// FIXME run bonnie++?
	return -1;
}

static int
troubleshoot(wchar_t * const *args,const char *arghelp){
	ZERO_ARG_CHECK(args,arghelp);
	fprintf(stderr,"Sorry, not yet implemented\n");
	// FIXME things to do:
	// FIXME check PCIe bandwidth against SATA bandwidth
	// FIXME check for proper alignment of partitions
	// FIXME check for msdos, apm or bsd partition tables
	// FIXME check for filesystems without noatime
	// FIXME check for SSD erase block size alignment
	// FIXME check for GPT partition table validity
	return -1;
}

static int
uefiboot(wchar_t * const *args,const char *arghelp){
	ONE_ARG_CHECK(args,arghelp);
	// FIXME ensure the partition is a viable ESP
	// FIXME ensure kernel is in ESP
	// FIXME prepare protective MBR
	// FIXME install rEFIt to ESP
	// FIXME point rEFIt at kernel
	return -1;
}

static int
biosboot(wchar_t * const *args,const char *arghelp){
	ONE_ARG_CHECK(args,arghelp);
	// FIXME ensure the partition has its boot flag set
	// FIXME ensure it's a primary partition
	// FIXME install grub to MBR
	// FIXME point grub at kernel
	return -1;
}

static int
rescan(wchar_t * const *args,const char *arghelp){
	ZERO_ARG_CHECK(args,arghelp);

	if(rescan_devices()){
		return -1;
	}
	return 0;
}

static int
grubmap(wchar_t * const *args,const char *arghelp){
	ZERO_ARG_CHECK(args,arghelp);

	if(popen_drain("/usr/sbin/grub-mkdevicemap -m /dev/stdout")){
		return -1;
	}
	return 0;
}

static void
free_tokes(wchar_t **tokes){
	wchar_t **toke;

	if(tokes){
		for(toke = tokes ; *toke ; ++toke){
			free(*toke);
		}
		free(tokes);
	}
}

static int
tokenize(const char *line,wchar_t ***tokes){
	unsigned inquotes = 0;
	const char *s = NULL;
	unsigned wchars = 0;
	mbstate_t ps,sps;
	size_t len,conv;
	int t = 0;

	memset(&sps,0,sizeof(sps));
	memset(&ps,0,sizeof(ps));
	len = strlen(line);
	*tokes = NULL;
	do{
		wchar_t w,*n,**tmp;

		if((conv = mbrtowc(&w,line,len,&ps)) == (size_t)-1){
			fprintf(stderr,"Error converting multibyte: %s\n",line);
			break;
		}
		line += conv;
		len -= conv;
		if(s == NULL){
			/*if(conv == 0){
				break;
			}*/
			if(iswspace(w)){
				continue;
			}
			if(w == L'"'){
				inquotes = 1;
			}
			s = line - conv;
			sps = ps;
			wchars = 1;
		}else{
			const char *olds;

			if(!conv){
				if(inquotes){
					fprintf(stderr,"Unterminated quotes in %s\n",line);
					return -1;
				}
			}else{
				if(iswgraph(w)){
					if(w == L'"'){
						inquotes = !inquotes;
					}
					++wchars;
					continue;
				}else if(iswspace(w)){
					if(inquotes){
						++wchars;
						continue;
					}
				}
			}
			assert(wchars);
			if((n = malloc(sizeof(*n) * (wchars + 1))) == NULL){
				free_tokes(*tokes);
				return -1;
			}
			olds = s;
			if(mbsrtowcs(n,&s,wchars,&sps) != wchars){
				fprintf(stderr,"Couldn't convert %s\n",olds);
				free_tokes(*tokes);
				return -1;
			}
			n[wchars] = L'\0';
			// Use t + 2 because we must have space for a final NULL
			if((tmp = realloc(*tokes,sizeof(**tokes) * (t + 2))) == NULL){
				free(n);
				free_tokes(*tokes);
				return -1;
			}
			*tokes = tmp;
			(*tokes)[t++] = n;
			wchars = 0;
			s = NULL;
		}
	}while(conv);
	if(t){
		(*tokes)[t] = NULL;
	}
	return t;
}

static int
version(wchar_t * const *args,const char *arghelp){
	int ret = 0;

	ZERO_ARG_CHECK(args,arghelp);
	printf("+++++++++++++++++++++++++++++++++++++++++++++++++############+++++++++++++++++++"
"++++++++++++++++++++++++++++++++++++++++++++++++++###########+++++++++++++++++++"
"++++++++++++++++++++++++++++++++++++++++++++++++++###########+++++++++++++++++++"
"++++++++++++++++++++++++++++++++++++++++++++++++++###########+++++++++++++++++++"
"+++++++++++++++++++++++++++++++++++++++++++++++++++##########++++++''''+++++++++"
"+++++++++++++++++++++++++++++++++'+++++++++++++''+++#########+++++''''''''''''''"
"+++++++++++++++++++++++++++++++++'++''+++++++++''+++###++####+++++''''+';;';;''+"
"+++++++++++++++++++++++++++++++++'++''+++++++++''+++###++####+++++'''';''++'';;'"
"+++++++++++++++++++++++++++++++++'+++++++++++++''+##+##++####+++++''';+;+;'+++'+"
"+++++#'+++#++#++#+++#++++++++++#+'+++#++#+++''+''++####++#@@#+++++'''';+';''';++"
"+''''''#+''''''''''++''++''++'''+'++''#+'''''++''''++##++++##+++++'''+';+';+;'++"
"+''##''++''+++''++''+''++''+#''++'++''+''++''++'''+'+##++#####++++'''+:'+';'''++"
"+''++''++''++#''++''+'''+'''#''++'++''+''+#+'++''++''##++#####++++'''+''''''''''"
"+''++''++''+++'++++'++''''''''+++'++''+''++''++''++#+##++#####+++++''++'''+'''''"
"+''''''++'++++''++''++''''''''+++'++''+#'''''++''+++'##++#####+++++''''''+''''''"
"+''+++#++'++++''''''+++''++'''+++'++''+''+++#++''+++'##++##@##+++++'''''''''''''"
"+'''''++#'++++''''''+++''++''+++#'++''#'''''+++''+++'###++++###++++'''''''''''''"
"+''''''+#+#++++#+++++++++++++#++#+##+++'''''''+++++#+#+########+++++''''''''''''"
"+''++''#+++++++++++++++++++#+++++++++++''#+#''++++++++++#######+++++''''''''''''"
"+''++''++++++++++++++++++++++++++++++++''++'''+++++++++##########++++'''''''''''"
"++'''''#++++++++++++++++++++++++++++++++'''''++++++++++##########++++''++++++'''"
"++#++++++++++++++++++++++++++++++++++++++++#++++++++++++#########++++'+''''''+++\n");
	ret |= popen_drain("/usr/sbin/smartctl --version");
	printf("\n");
	ret |= popen_drain("/sbin/parted --version");
	printf("\n");
	ret |= popen_drain("/sbin/mkswap --version");
	printf("\n");
	ret |= popen_drain("/usr/sbin/grub-mkdevicemap --version");
	printf("\n");
	ret |= popen_drain("/sbin/sgdisk --version");
	if(print_zfs_version(stdout) < 0){
		ret |= -1;
	}
	printf("\n");
	printf("%s %s\n",PACKAGE,VERSION);
	return ret;
}

static int
quit(wchar_t * const *args,const char *arghelp){
	ZERO_ARG_CHECK(args,arghelp);
	lights_off = 1;
	return 0;
}

static const struct fxn {
	const wchar_t *cmd;
	int (*fxn)(wchar_t * const *,const char *);
	const char *arghelp;
} fxns[] = {
#define FXN(x,args) { .cmd = L###x, .fxn = x, .arghelp = args, }
	FXN(adapter,"[ \"reset\" adapter ]\n"
			"		  | [ \"rescan\" adapter ]\n"
			"                 | [ -v ] no arguments to list all host bus adapters"),
	FXN(blockdev,"[ \"rescan\" blockdev ]\n"
			"                 | [ \"badblocks\" blockdev [ \"rw\" ] ]\n"
			"                 | [ \"wipebiosboot\" blockdev ]\n"
			"                 | [ \"wipedosmbr\" blockdev ]\n"
			"                 | [ \"rmtable\" blockdev ]\n"
			"                 | [ \"mktable\" [ blockdev tabletype ] ]\n"
			"                    | no arguments to list supported table types\n"
			"                 | [ \"fsck\" blockdev ]\n"
			"                 | [ \"detail\" blockdev ]\n"
			"                 | [ -v ] no arguments to list all blockdevs"),
	FXN(partition,"[ \"del\" partition ]\n"
			"                 | [ \"fsck\" partition ]\n"
			"                 | [ \"add\" blockdev name size ]\n"
			"                 | [ \"setuuid\" partition uuid ]\n"
			"                 | [ \"setname\" partition name ]\n"
			"                 | [ \"setflag\" [ partition \"on\"|\"off\" flag ] ]\n"
			"                    | no arguments to list supported flags\n"
			"                 | [ -v ] no arguments to list all partitions"),
	FXN(fs,"[ \"mkfs\" [ partition fstype ] ]\n"
			"                 | no arguments to list supported fs types\n"
			"                 | [ \"wipefs\" fs ]\n"
			"                 | [ \"setuuid\" fs uuid ]\n"
			"                 | [ \"setlabel\" fs label ]\n"
			"                 | no arguments to list all filesystems"),
	FXN(swap,"[ \"on\"|\"off\" swapdevice ]\n"
			"                 | no arguments to list all swaps"),
	FXN(mdadm,"[ \"create\" mdname devcount level devices ]\n"
			"                 | [ -v ] no arguments to list all mdadm devices"),
	FXN(zpool,"[ \"create\" zname devcount level vdevs ]\n"
			"                 | [ -v ] no arguments to list all zpools"),
	FXN(map,"[ device mountpoint type options ]\n"
			"                 | [ mountdev \"swap\" ]\n"
			"                 | no arguments generates target fstab"),
	FXN(mounts,""),
	FXN(uefiboot,"device"),
	FXN(biosboot,"device"),
	FXN(grubmap,""),
	FXN(rescan,""),
	FXN(benchmark,"fs"),
	FXN(troubleshoot,""),
	FXN(version,""),
	FXN(help,"[ command ]"),
	FXN(quit,""),
	{ .cmd = NULL, .fxn = NULL, .arghelp = NULL, },
#undef FXN
};

static int
help(wchar_t * const *args,const char *arghelp){
	const struct fxn *fxn;

	if(args[1] == NULL){
		use_terminfo_color(COLOR_GREEN,1);
		printf("%-15.15s %s\n","Command","Arguments");
		for(fxn = fxns ; fxn->cmd ; ++fxn){
			printf("%-15.15ls %s\n",fxn->cmd,fxn->arghelp);
		}
	}else if(args[2] == NULL){
		for(fxn = fxns ; fxn->cmd ; ++fxn){
			if(wcscmp(fxn->cmd,args[1]) == 0){
				use_terminfo_color(COLOR_GREEN,1);
				printf("%15.15ls %s\n",args[1],fxn->arghelp);
				return 0;
			}
		}
		fprintf(stderr,"Unknown command: %ls\n",args[1]);
		return -1;
	}else{
		usage(args,arghelp);
		return -1;
	}
	return 0;
}

static int
tty_ui(void){
	char prompt[80] = "[" PACKAGE "](0)> ";
	char *l;

	use_terminfo_color(COLOR_WHITE,0);
	while( (l = readline(prompt)) ){
		const struct fxn *fxn;
		wchar_t **tokes;
		int z;
		
		fflush(stdout);
		add_history(l);
		z = tokenize(l,&tokes);
		free(l);
		if(z <= 0){
			continue;
		}
		for(fxn = fxns ; fxn->cmd ; ++fxn){
			if(wcscasecmp(fxn->cmd,tokes[0])){
				continue;
			}
			break;
		}
		if(fxn->fxn){
			use_terminfo_color(COLOR_WHITE,1);
			lock_growlight();
			z = fxn->fxn(tokes,fxn->arghelp);
			unlock_growlight();
			use_terminfo_color(COLOR_WHITE,0);
		}else{
			fprintf(stderr,"Unknown command: %ls\n",tokes[0]);
			z = -1;
		}
		free_tokes(tokes);
		snprintf(prompt,sizeof(prompt),"[" PACKAGE "](%d)> ",z);
		rl_set_prompt(prompt);
		if(lights_off){
			return 0;
		}
	}
	printf("\n");
	return 0;
}

// FIXME it'd be nice to do secondary completion (ie command-sensitive) for
// command arguments
static char *
completion_engine(const char *text,int state){
	static const struct fxn *fxn;
	static size_t len;

	// 'state' tells us whether readline has tried to complete already
	if(state == 0){
		len = strlen(text);
		fxn = fxns;
	}else{
		++fxn;
	}
	while(fxn->cmd){
		char fxnbuf[NAME_MAX];

		snprintf(fxnbuf,sizeof(fxnbuf),"%ls",fxn->cmd);
		if(strncmp(fxnbuf,text,len) == 0){

			return strdup(fxnbuf);
		}
		++fxn;
	}
	return NULL;
}

static char **
growlight_completion(const char *text,int start,int end __attribute__ ((unused))){
	if(start == 0){
		return rl_completion_matches(text,completion_engine);
	}
	return NULL;
}

int main(int argc,char * const *argv){
	fflush(stdout);
	if(growlight_init(argc,argv)){
		return EXIT_FAILURE;
	}
	rl_readline_name = PACKAGE;
	rl_attempted_completion_function = growlight_completion;
	rl_prep_terminal(1); // 1 == read 8-bit input
	if(isatty(STDOUT_FILENO)){
#ifdef HAVE_CURSES
		int errret;

		if(setupterm(NULL,STDOUT_FILENO,&errret) != OK){
			fprintf(stderr,"Couldn't set up terminfo db (errret %d)\n",errret);
		}else{
			use_terminfo = 1;
		}
	}
#endif
	if(tty_ui()){
		growlight_stop();
		return EXIT_FAILURE;
	}
// "default foreground color" according to http://bash-hackers.org/wiki/doku.php/scripting/terminalcodes
// but not defined in ncurses.h -- likely not fully portable :( FIXME
	use_terminfo_color(9,0);
	if(growlight_stop()){
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
