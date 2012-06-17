#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <locale.h>
#include <readline/history.h>
#include <readline/readline.h>

#include <fs.h>
#include <mbr.h>
#include <swap.h>
#include <sysfs.h>
#include <config.h>
#include <target.h>
#include <growlight.h>

#ifdef HAVE_NCURSES_CURSES_H
#include <ncurses/curses.h>
#else
#ifdef HAVE_NCURSESW_CURSES_H
#include <ncursesw/curses.h>
#else
#ifdef HAVE_NCURSES_H
#include <ncurses.h>
#else
#ifdef HAVE_CURSES_H
#include <curses.h>
#else
#define NOCURSES
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

static inline int
usage(char * const *args,const char *arghelp){
	fprintf(stderr,"Usage: %s %s\n",*args,arghelp);
	return -1;
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

#define TWO_ARG_CHECK(args,arghelp) \
 if(!args[1] || !args[2] || args[3]){ usage(args,arghelp); return -1 ; }

static int help(char * const *,const char *);
static int print_mdadm(const device *,int,int);

static int
print_target(const mount *m,int prefix){
	int r = 0,rr;

	r += rr = printf("%*.*s%-*.*s %-5.5s %-36.36s " PREFIXFMT " %-6.6s\n%*.*s %s %s\n",
			prefix,prefix,"",
			FSLABELSIZ,FSLABELSIZ,m->label ? m->label : "n/a",
			m->fs,
			m->uuid ? m->uuid : "n/a",
			"-1", // FIXME
			m->dev,
			prefix,prefix,"",
			m->path,m->ops);
	if(rr < 0){
		return -1;
	}
	return r;
}

static int
print_mount(const device *d,int prefix){
	char buf[PREFIXSTRLEN + 1];
	int r = 0,rr;

	r += rr = printf("%*.*s%-*.*s %-5.5s %-36.36s " PREFIXFMT " %-6.6s\n%*.*s %s %s\n",
			prefix,prefix,"",
			FSLABELSIZ,FSLABELSIZ,d->label ? d->label : "n/a",
			d->mnttype,
			d->uuid ? d->uuid : "n/a",
			qprefix(d->size * d->logsec,1,buf,sizeof(buf),0),
			d->name,
			prefix,prefix,"",
			d->mnt,d->mntops);
	if(rr < 0){
		return -1;
	}
	return r;
}

static int
print_unmount(const device *d,int prefix){
	char buf[PREFIXSTRLEN + 1];
	int r = 0,rr;

	r += rr = printf("%*.*s%-*.*s %-5.5s %-36.36s " PREFIXFMT " %-6.6s\n",
			prefix,prefix,"",
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
print_swap(const device *p,int prefix){
	int r = 0,rr;

	assert(p->mnttype);
	r += rr = printf("%*.*s%-*.*s %-5.5s %-36.36s %-6.6s",prefix,prefix,"",
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
print_fs(const device *p){
	int r = 0,rr;

	if(p->mnttype == NULL){
		return 0;
	}
	if(p->swapprio != SWAP_INVALID){
		return 0;
	}
	if(p->target){
		r += rr = print_target(p->target,0);
		if(rr < 0){
			return -1;
		}
	}
	if(p->mnt){
		r += rr = print_mount(p,0);
	}else{
		r += rr = print_unmount(p,0);
	}
	if(rr < 0){
		return -1;
	}
	return r;
}

static int
print_partition(const device *p,int prefix,int descend){
	char buf[PREFIXSTRLEN + 1];
	int r = 0,rr;

	r += rr = printf("%*.*s%-10.10s %-36.36s " PREFIXFMT " %-4.4s %-17.17s\n",
			prefix,prefix,"",p->name,
			p->partdev.uuid ? p->partdev.uuid : "n/a",
			qprefix(p->size * p->logsec,1,buf,sizeof(buf),0),
			((p->partdev.partrole == PARTROLE_PRIMARY || p->partdev.partrole == PARTROLE_GPT) && (p->partdev.flags & 0xffu) == 0x80) ? "Boot" :
				p->partdev.partrole == PARTROLE_PRIMARY ? "Pri" :
				p->partdev.partrole == PARTROLE_EXTENDED ? "Ext" :
				p->partdev.partrole == PARTROLE_LOGICAL ? "Log" :
				p->partdev.partrole == PARTROLE_GPT ? "GPT" :
				p->partdev.partrole == PARTROLE_EPS ? "ESP" : "Unk",
				p->label ? p->label : p->partdev.label ? p->partdev.label : "n/a");
	if(rr < 0){
		return -1;
	}
	if(!descend){
		return r;
	}
	if(p->mnt){
		r += rr = print_mount(p,prefix + 1);
	}else if(p->swapprio >= SWAP_INACTIVE){
		r += rr = print_swap(p,prefix + 1);
	}else if(p->mnttype){
		r += rr = print_unmount(p,prefix + 1);
	}
	if(p->target){
		r += rr = print_target(p->target,prefix + 1);
	}
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

static int
print_drive(const device *d,int prefix,int descend){
	char buf[PREFIXSTRLEN + 1];
	const device *p;
	int r = 0,rr;

	switch(d->layout){
	case LAYOUT_NONE:{
		r += rr = printf("%*.*s%-10.10s %-16.16s %-4.4s " PREFIXFMT " %4uB %c%c%c%c%c%c %-6.6s%-19.19s\n",
			prefix,prefix,"",d->name,
			d->model ? d->model : "n/a",
			d->revision ? d->revision : "n/a",
			qprefix(d->logsec * d->size,1,buf,sizeof(buf),0),
			d->physsec,
			d->blkdev.removable ? 'R' : '.',
			'.',
			'.',
			d->blkdev.rotate ? 'O' : '.',
			d->blkdev.wcache ? 'W' : '.',
			d->blkdev.biosboot ? 'B' : '.',
			d->pttable ? d->pttable : "none",
			d->wwn ? d->wwn : "n/a"
			);
		break;
	}case LAYOUT_MDADM:{
		r += rr = printf("%*.*s%-10.10s %-16.16s %-4.4s " PREFIXFMT " %4uB %c%c%c%c%c%c %-6.6s%-19.19s\n",
			prefix,prefix,"",d->name,
			d->model ? d->model : "n/a",
			d->revision ? d->revision : "n/a",
			qprefix(d->logsec * d->size,1,buf,sizeof(buf),0),
			d->physsec, '.', 'V', 'M', '.', '.', '.',
			d->pttable ? d->pttable : "none",
			d->wwn ? d->wwn : "n/a"
			);
		break;
	}case LAYOUT_ZPOOL:{
		r += rr = printf("%*.*s%-10.10s %-16.16s %-4.4s " PREFIXFMT " %4uB %c%c%c%c%c%c %-6.6s%-19.19s\n",
			prefix,prefix,"",d->name,
			d->model ? d->model : "n/a",
			d->revision ? d->revision : "n/a",
			qprefix(d->logsec * d->size,1,buf,sizeof(buf),0),
			d->physsec, '.', 'V', '.', '.', '.', '.',
			d->pttable ? d->pttable : "none",
			d->wwn ? d->wwn : "n/a"
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
	if(d->mnt){
		r += rr = print_mount(d,prefix + 1);
	}else if(d->mnttype){
		r += rr = print_unmount(d,prefix + 1);
	}else if(d->swapprio >= SWAP_INACTIVE){
		r += rr = print_swap(d,prefix + 1);
	}
	if(d->target){
		r += rr = print_target(d->target,prefix + 1);
	}
	if(rr < 0){
		return -1;
	}
	if(!prefix){
		for(p = d->parts ; p ; p = p->next){
			r += rr = print_partition(p,prefix + 1,descend);
			if(rr < 0){
				return -1;
			}
		}
		
	}
	return r;
}

static int
print_dev_mplex(const device *d,int prefix,int descend){
	switch(d->layout){
		case LAYOUT_NONE:
			return print_drive(d,prefix,descend);
		case LAYOUT_PARTITION:
			return print_partition(d,prefix,descend);
		case LAYOUT_MDADM:
			return print_mdadm(d,prefix,descend);
		case LAYOUT_ZPOOL:
			// FIXME return print_zpool(d,prefix,descend);
		default:
			return -1;
	}
}

static int
print_mdadm(const device *d,int prefix,int descend){
	char buf[PREFIXSTRLEN + 1];
	const mdslave *md;
	int r = 0,rr;

	r += rr = printf("%-*.*s%-10.10s %-36.36s " PREFIXFMT " %4uB %-6.6s%5lu %-6.6s\n",
			prefix,prefix,"",
			d->name,
			d->uuid ? d->uuid : "n/a",
			qprefix(d->logsec * d->size,1,buf,sizeof(buf),0),
			d->physsec,
			d->pttable ? d->pttable : "none",
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
				r += rr = print_partition(p,1,descend);
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

	switch(c->bus){
		case BUS_PCIe:
			if(c->pcie.lanes_neg == 0){
				r += rr = printf("Southbridge device %04hx:%02x.%02x.%x\n",
					c->pcie.domain,c->pcie.bus,
					c->pcie.dev,c->pcie.func);
			}else{
				r += rr = printf("PCI Express device %04hx:%02x.%02x.%x (x%u, gen %s)\n",
					c->pcie.domain,c->pcie.bus,
					c->pcie.dev,c->pcie.func,
					c->pcie.lanes_neg,pcie_gen(c->pcie.gen));
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
		r += rr = print_drive(d,1,descend);
		if(rr < 0){
			return -1;
		}
	}
	return r;
}

static int
adapter(char * const *args,const char *arghelp){
	const controller *c;
	int descend;

	if(args[1] == NULL){
		descend = 1;
	}else if(strcmp(args[1],"-q") == 0 && args[2] == NULL){
		descend = 0;
	}else{
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

static int
zpool(char * const *args,const char *arghelp){
	const controller *c;

	ZERO_ARG_CHECK(args,arghelp);
	printf("%-10.10s " PREFIXFMT " %5.5s %-6.6s%-6.6s%-7.7s\n",
			"Device","Bytes","PSect","Table","Disks","Level");
	for(c = get_controllers() ; c ; c = c->next){
		device *d;

		if(c->bus != BUS_VIRTUAL){
			continue;
		}
		for(d = c->blockdevs ; d ; d = d->next){
			if(d->layout == LAYOUT_ZPOOL){
				// FIXME
			}
		}
	}
	return 0;
}

static int
mdadm(char * const *args,const char *arghelp){
	const controller *c;
	int descend;

	if(args[1] == NULL){
		descend = 1;
	}else if(strcmp(args[1],"-q") == 0 && args[2] == NULL){
		descend = 0;
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

	printf("%-10.10s %-16.16s %-4.4s " PREFIXFMT " %5.5s Flags  %-6.6s%-19.19s\n",
			"Device","Model","Rev","Bytes","PSect","Table","WWN");
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			if(print_drive(d,0,descend) < 0){
				return -1;
			}
		}
	}
	printf("\n\tFlags:\t(R)emovable, (V)irtual, (M)dadm, r(O)tational\n"
			"\t\t(W)ritecache enabled, (B)IOS boot\n");
	return 0;
}

static inline int
blockdev_details(const device *d){
	unsigned z;

	if(print_drive(d,0,1) < 0){
		return -1;
	}
	if(d->blkdev.biossha1){
		if(printf("BIOS boot code SHA-1:\n") < 0){
			return -1;
		}
		for(z = 0 ; z < 2 ; ++z){
			unsigned y;

			if(printf("\t%02x:",((const unsigned char *)d->blkdev.biossha1)[10 * z]) < 0){
				return -1;
			}
			for(y = 1 ; y < 9 ; ++y){
				if(printf("%02x:",((const unsigned char *)d->blkdev.biossha1)[(10 * z) + y]) < 0){
					return -1;
				}
			}
			if(printf("%02x\n",((const unsigned char *)d->blkdev.biossha1)[10 * (z + 1) - 1]) < 0){
				return -1;
			}
		}
	}
	return 0;
}

static int
blockdev(char * const *args,const char *arghelp){
	device *d;

	if(args[1] == NULL){
		return blockdev_dump(1);
	}
	if(args[2] == NULL){
		if(strcmp(args[1],"-q") == 0){
			return blockdev_dump(0);
		}
		if(strcmp(args[1],"mktable") == 0){
			if(print_tabletypes() < 0){
				return -1;
			}
			return 0;
		}else if(strcmp(args[1],"mkfs") == 0){
			if(print_fstypes() < 0){
				return -1;
			}
			return 0;
		}
		usage(args,arghelp);
		return -1;
	}
	// Everything else has a required device argument
	if((d = lookup_device(args[2])) == NULL){
		return -1;
	}
	if(strcmp(args[1],"reset") == 0){
		if(args[3]){
			usage(args,arghelp);
			return -1;
		}
		if(reset_blockdev(d)){
			return -1;
		}
		return 0;
	}else if(strcmp(args[1],"wipebiosboot") == 0){
		if(args[3]){
			usage(args,arghelp);
			return -1;
		}
		return wipe_biosboot(d);
	}else if(strcmp(args[1],"wipedosmbr") == 0){
		if(args[3]){
			usage(args,arghelp);
			return -1;
		}
		return wipe_dosmbr(d);
	}else if(strcmp(args[1],"detail") == 0){
		if(args[3]){
			usage(args,arghelp);
			return -1;
		}
		return blockdev_details(d);
	}else if(strcmp(args[1],"mktable") == 0){
		if(args[3] == NULL || args[4]){
			usage(args,arghelp);
			return -1;
		}
		if(make_partition_table(d,args[3])){
			return -1;
		}
		return 0;
	}else if(strcmp(args[1],"mkfs") == 0){
		if(args[3] == NULL || args[4]){
			usage(args,arghelp);
			return -1;
		}
		if(make_filesystem(d,args[3])){
			return -1;
		}
		return 0;
	}
	usage(args,arghelp);
	return -1;
}

static int
partition(char * const *args,const char *arghelp){
	const controller *c;
	int descend;

	if(args[1] == NULL){
		descend = 1;
	}else if(strcmp(args[1],"-q") == 0 && args[2] == NULL){
		descend = 0;
	}else{
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
				if(print_partition(p,0,descend) < 0){
					return -1;
				}
			}
		}
	}
	return 0;
}

static int
mounts(char * const *args,const char *arghelp){
	const controller *c;

	ZERO_ARG_CHECK(args,arghelp);
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			if(d->mnt){
				if(print_mount(d,0) < 0){
					return -1;
				}
			}else if(d->target){
				if(print_target(p->target,0) < 0){
					return -1;
				}
			}
			for(p = d->parts ; p ; p = p->next){
				if(p->mnt){
					if(print_mount(p,0) < 0){
						return -1;
					}
				}else if(p->target){
					if(print_target(p->target,0) < 0){
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
				r += rr = print_target(d->target,0);
				if(rr < 0){
					return -1;
				}
			}
			for(p = d->parts ; p ; p = p->next){
				if(p->target){
					r += rr = print_target(p->target,0);
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
map(char * const *args,const char *arghelp){
	device *d;

	if(!args[1]){
		if(print_map() < 0){
			return -1;
		}
		return 0;
	}
	if(!args[2] || !args[3] || !args[4] || args[5]){
		fprintf(stderr,"Usage:\t%s\t%s\n",*args,arghelp);
		return -1;
	}
	if((d = lookup_device(args[1])) == NULL || (d = lookup_dentry(d,args[1])) == NULL){
		fprintf(stderr,"Couldn't find device %s\n",args[1]);
		return -1;
	}
	if(args[2][0] != '/'){
		fprintf(stderr,"Not an absolute path: %s\n",args[2]);
		return -1;
	}
	if(prepare_mount(d,args[2],args[3],args[4])){
		return -1;
	}
	return 0;
}

// Walk the block devices, evaluating *fxn on each. The return value will be
// accumulated in r, unless -1 is ever returned, in which case we abort
// immediately and return -1.
static int
walk_devices(int (*fxn)(const device *)){
	const controller *c;
	int rr,r = 0;

	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			r += rr = fxn(d);
			if(rr < 0){
				return -1;
			}
			for(p = d->parts ; p ; p = p->next){
				r += rr = fxn(p);
				if(rr < 0){
					return -1;
				}
			}
		}
	}
	return 0;
}

static int
print_swaps(const device *d){
	char buf[PREFIXSTRLEN + 1];
	int rr,r = 0;

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

static int
fs(char * const *args,const char *arghelp){
	ZERO_ARG_CHECK(args,arghelp);
	printf("%-*.*s %-5.5s %-36.36s " PREFIXFMT " %s\n",
			FSLABELSIZ,FSLABELSIZ,"Label",
			"Type","UUID","Bytes","Device");
	if(walk_devices(print_fs)){
		return -1;
	}
	return 0;
}

static int
swap(char * const *args,const char *arghelp){
	device *d;
	if(!args[1]){
		if(printf("%-*.*s %-5.5s %-36.36s " PREFIXFMT " %s\n",FSLABELSIZ,FSLABELSIZ,
					"Label","Prio","UUID","Bytes","Device") < 0){
			return -1;
		}
		if(walk_devices(print_swaps)){
			return -1;
		}
		return 0;
	}
	TWO_ARG_CHECK(args,arghelp);
	if((d = lookup_device(args[1])) == NULL){
		fprintf(stderr,"Couldn't find device %s\n",args[2]);
		return -1;
	}
	if(strcmp(args[2],"on") == 0){
		if(swapondev(d)){
			return -1;
		}
	}else if(strcmp(args[2],"off") == 0){
		if(swapoffdev(d)){
			return -1;
		}
	}else{
		fprintf(stderr,"Invalid command to %s: %s\n",args[0],args[1]);
		return -1;
	}
	return 0;
}

static int
badblocks(char * const *args,const char *arghelp){
	device *d;

	if(!args[1]){
		fprintf(stderr,"Usage:\t%s\t%s\n",*args,arghelp);
		return -1;
	}
	if(args[2] == NULL){
		d = lookup_device(args[1]);
	}else if(args[3]){
		fprintf(stderr,"Usage:\t%s\t%s\n",*args,arghelp);
		return -1;
	}else{
		if(strcmp(args[1],"rw")){
			fprintf(stderr,"Usage:\t%s\t%s\n",*args,arghelp);
			return -1;
		}
		d = lookup_device(args[2]);
	}
	if(d == NULL){
		return -1;
	}
	// FIXME perform check
	return 0;
}

static int
benchmark(char * const *args,const char *arghelp){
	ZERO_ARG_CHECK(args,arghelp);
	fprintf(stderr,"Sorry, not yet implemented\n");
	// FIXME things to do:
	// FIXME run bonnie++?
	return -1;
}

static int
troubleshoot(char * const *args,const char *arghelp){
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
uefiboot(char * const *args,const char *arghelp){
	ZERO_ARG_CHECK(args,arghelp);
	// FIXME ensure the partition is a viable ESP
	// FIXME ensure kernel is in ESP
	// FIXME prepare protective MBR
	// FIXME install rEFIt to ESP
	// FIXME point rEFIt at kernel
	return -1;
}

static int
biosboot(char * const *args,const char *arghelp){
	ZERO_ARG_CHECK(args,arghelp);
	// FIXME ensure the partition has its boot flag set
	// FIXME ensure it's a primary partition
	// FIXME install grub to MBR
	// FIXME point grub at kernel
	return -1;
}

static void
free_tokes(char **tokes){
	char **toke;

	if(tokes){
		for(toke = tokes ; *toke ; ++toke){
			free(*toke);
		}
		free(tokes);
	}
}

static int
tokenize(const char *line,char ***tokes){
	int t = 0;

	*tokes = NULL;
	do{
		const char *s;
		char *n,**tmp;

		while(isspace(*line)){
			++line;
		}
		s = line;
		while(isgraph(*line)){
			++line;
		}
		if(line == s){
			break;
		}
		if((n = strndup(s,line - s)) == NULL){
			free_tokes(*tokes);
			return -1;
		}
		// Use t + 2 because we must have space for a final NULL
		if((tmp = realloc(*tokes,sizeof(**tokes) * (t + 2))) == NULL){
			free(n);
			free_tokes(*tokes);
			return -1;
		}
		*tokes = tmp;
		(*tokes)[t++] = n;
	}while(*line);
	if(t){
		(*tokes)[t] = NULL;
	}
	return t;
}

static int
quit(char * const *args,const char *arghelp){
	ZERO_ARG_CHECK(args,arghelp);
	lights_off = 1;
	return 0;
}

static const struct fxn {
	const char *cmd;
	int (*fxn)(char * const *,const char *);
	const char *arghelp;
} fxns[] = {
#define FXN(x,args) { .cmd = #x, .fxn = x, .arghelp = args, }
	FXN(adapter,"[ adapter \"reset\" ]\n"
			"                 | [ -q ] no arguments to detail all host bus adapters"),
	FXN(blockdev,"[ \"reset\" blockdev ]\n"
			"                 | [ \"wipebiosboot\" blockdev ]\n"
			"                 | [ \"wipedosmbr\" blockdev ]\n"
			"                 | [ \"mktable\" [ blockdev tabletype ] ]\n"
			"                    | no arguments to list supported table types\n"
			"                 | [ \"mkfs\" [ blockdev fstype ] ]\n"
			"                    | no arguments to list supported fs types\n"
			"                 | [ \"detail\" blockdev ]\n"
			"                 | [ -q ] no arguments to list all blockdevs"),
	FXN(partition,"[ partition \"delete\" ]\n"
			"                 | [ partition \"mkfs\" [ fstype ] ]\n"
			"                    | no argument to list supported fs types\n"
			"                 | [ -q ] no arguments to detail all partitions"),
	FXN(fs,""),
	FXN(mdadm,"[ mdname \"create\" devcount level devices ]\n"
			"                 | [ -q ] no arguments to detail all mdadm devices"),
	FXN(zpool,"[ zname \"create\" devcount level vdevs ]\n"
			"                 | [ -q ] no arguments to detail all zpools"),
	FXN(swap,"[ swapdevice \"on\"|\"off\" ]\n"
			"                 | no arguments to detail all swaps"),
	FXN(map,"[ device mountpoint type options ]\n"
			"                 | [ mountdev \"swap\" ]\n"
			"                 | no arguments generates target fstab"),
	FXN(mounts,""),
	FXN(uefiboot,"device"),
	FXN(biosboot,"device"),
	FXN(badblocks,"[ \"rw\" ] device"),
	FXN(benchmark,"fs"),
	FXN(troubleshoot,""),
	FXN(help,"[ command ]"),
	FXN(quit,""),
	{ .cmd = NULL, .fxn = NULL, .arghelp = NULL, },
#undef FXN
};

static int
help(char * const *args,const char *arghelp){
	const struct fxn *fxn;

	if(args[1] == NULL){
		printf("%-15.15s %s\n","Command","Arguments");
		for(fxn = fxns ; fxn->cmd ; ++fxn){
			printf("%-15.15s %s\n",fxn->cmd,fxn->arghelp);
		}
	}else if(args[2] == NULL){
		for(fxn = fxns ; fxn->cmd ; ++fxn){
			if(strcmp(fxn->cmd,args[1]) == 0){
				printf("%15.15s %s\n",args[1],fxn->arghelp);
				return 0;
			}
		}
		printf("Unknown command: %s\n",args[1]);
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

	while( (l = readline(prompt)) ){
		const struct fxn *fxn;
		char **tokes;
		int z;
		
		fflush(stdout);
		add_history(l);
		z = tokenize(l,&tokes);
		free(l);
		if(z == 0){
			continue;
		}else if(z < 0){
			return -1;
		}
		for(fxn = fxns ; fxn->cmd ; ++fxn){
			if(strcasecmp(fxn->cmd,tokes[0])){
				continue;
			}
			break;
		}
		if(fxn->fxn){
			z = fxn->fxn(tokes,fxn->arghelp);
		}else{
			fprintf(stderr,"Unknown command: %s\n",tokes[0]);
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
		if(strncmp(fxn->cmd,text,len) == 0){
			return strdup(fxn->cmd);
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

#ifndef NOCURSES
static SCREEN *
init_ncurses(void){
	SCREEN *s;

	if((s = newterm(NULL,stdout,stdin)) == NULL){
		return NULL;
	}
	if(start_color()){
		return NULL;
	}
	printf("libncurses %s\n",curses_version());
}

static int
stop_ncurses(SCREEN *s){
	delscreen(s);
	return 0;
}
#else
typedef void SCREEN;

static SCREEN *init_ncurses(void){ return init_ncurses; }
static int stop_ncurses(SCREEN *s __attribute__ ((unused))){ return 0; }
#endif

int main(int argc,char * const *argv){
	SCREEN *s;

	fflush(stdout);
	if(growlight_init(argc,argv)){
		return EXIT_FAILURE;
	}
	rl_readline_name = PACKAGE;
	rl_attempted_completion_function = growlight_completion;
	rl_prep_terminal(1); // 1 == read 8-bit input
	if((s = init_ncurses()) == NULL){
		return EXIT_FAILURE;
	}
	if(tty_ui()){
		growlight_stop();
		stop_ncurses(s);
		return EXIT_FAILURE;
	}
	if(growlight_stop()){
		stop_ncurses(s);
		return EXIT_FAILURE;
	}
	if(stop_ncurses(s)){
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
