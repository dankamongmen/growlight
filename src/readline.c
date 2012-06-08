#include <assert.h>
#include <stdlib.h>
#include <locale.h>
#include <readline/history.h>
#include <readline/readline.h>

#include <growlight.h>

#define ZERO_ARG_CHECK(args) \
 if(args[1]){ fprintf(stderr,"Usage: %s\n",*args); return -1 ; }

static int help(char * const *);

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
print_mdadm(const device *d){
	int r = 0,rr;
	mdslave *md;

	r += rr = printf("%-10.10s %4uB %4uB %-6.6s%5lu %-7.7s\n",
			d->name,
			d->logsec,d->physsec,
			d->pttable ? d->pttable : "none",
			d->mddev.disks,d->mddev.level
			);
	if(rr < 0){
		return -1;
	}
	for(md = d->mddev.slaves ; md ; md = md->next){
		r += rr = printf(" %s\n",md->name);
		if(rr < 0){
			return -1;
		}
	}
	return r;
}

static int
print_drive(const device *d){
	const device *p;
	int r = 0,rr;

	r += rr = printf("%-10.10s %-16.16s %-4.4s %4uB %4uB %c%c%c%c  %-6.6s%-20.20s\n",d->name,
			d->model ? d->model : "n/a",
			d->revision ? d->revision : "n/a",
			d->logsec,d->physsec,
			d->blkdev.removable ? 'R' : '.',
			d->blkdev.realdev ? '.' : 'V',
			d->layout == LAYOUT_MDADM ? 'M' : '.',
			d->blkdev.realdev ? d->blkdev.rotate ? 'O' : '.' : '.',
			d->pttable ? d->pttable : "none",
			d->wwn ? d->wwn : "n/a"
			);
	if(rr < 0){
		return -1;
	}
	for(p = d->parts ; p ; p = p->next){
		r += rr = printf(" %s\n",p->name);
		if(rr < 0){
			return -1;
		}
	}
	return r;
}

static int
print_controller(const controller *c){
	int r = 0,rr;
	device *d;

	switch(c->bus){
		case BUS_PCIe:
			r += rr = printf("PCI Express device %04hx:%02x.%02x.%x (x%u, gen %s)\n",
					c->pcie.domain,c->pcie.bus,
					c->pcie.dev,c->pcie.func,
					c->pcie.lanes_neg,pcie_gen(c->pcie.gen));
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
	for(d = c->blockdevs ; d ; d = d->next){
		r += rr = printf(" %s\n",d->name);
		if(rr < 0){
			return -1;
		}
	}
	return r;
}

static int
initiators(char * const *args){
	const controller *c;

	ZERO_ARG_CHECK(args);
	for(c = get_controllers() ; c ; c = c->next){
		if(print_controller(c) < 0){
			return -1;
		}
	}
	return 0;
}

static int
mdadm(char * const *args){
	const controller *c;

	ZERO_ARG_CHECK(args);
	printf("%-10.10s %5.5s %5.5s %-6.6s%-6.6s%-7.7s\n",
			"Device","Log","Phys","Table","Disks","Level");
	for(c = get_controllers() ; c ; c = c->next){
		device *d;

		if(c->bus != BUS_VIRTUAL){
			continue;
		}
		for(d = c->blockdevs ; d ; d = d->next){
			if(d->layout == LAYOUT_MDADM){
				if(print_mdadm(d) < 0){
					return -1;
				}
			}
		}
	}
	return 0;
}

static int
blockdevs(char * const *args){
	const controller *c;

	ZERO_ARG_CHECK(args);
	printf("%-10.10s %-16.16s %-4.4s %5.5s %5.5s Flags %-6.6s%-20.20s\n",
			"Device","Model","Rev","Log","Phys","Table","WWN");
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			if(print_drive(d) < 0){
				return -1;
			}
		}
	}
	printf("\n  Flags: (r)emovable, (v)irtual, (m)dadm, r(o)tational\n\n");
	return 0;
}

static int
print_partition(const device *p){
	int r = 0,rr;

	r += rr = printf("%-10.10s %-37.37s %s\n",p->name,
			p->partdev.uuid ? p->partdev.uuid : "n/a",
			p->partdev.pname ? p->partdev.pname : "n/a");
	if(rr < 0){
		return -1;
	}
	return r;
}

static int
partitions(char * const *args){
	const controller *c;

	ZERO_ARG_CHECK(args);
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			for(p = d->parts ; p ; p = p->next){
				if(print_partition(p) < 0){
					return -1;
				}
			}
		}
	}
	return 0;
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

static const struct fxn {
	const char *cmd;
	int (*fxn)(char * const *);
} fxns[] = {
#define FXN(x) { .cmd = #x, .fxn = x, }
	FXN(initiators),
	FXN(blockdevs),
	FXN(partitions),
	FXN(mdadm),
	FXN(help),
	{ .cmd = NULL,		.fxn = NULL, },
#undef FXN
};

static int
help(char * const *args){
	const struct fxn *fxn;

	ZERO_ARG_CHECK(args);
	printf("\n\tAvailable commands:\n\n");
	for(fxn = fxns ; fxn->cmd ; ++fxn){
		printf("\t  %s\n",fxn->cmd);
	}
	printf("\t  quit\n\n");
	return 0;
}

static int
tty_ui(void){
	const char prompt[] = "[growlight]> ";
	char *l;

	// FIXME need command line completion!
	while( (l = readline(prompt)) ){
		const struct fxn *fxn;
		char **tokes;
		int z;

		z = tokenize(l,&tokes);
		free(l);
		if(z == 0){
			continue;
		}else if(z < 0){
			return -1;
		}
		if(strcasecmp(tokes[0],"quit") == 0){
			free_tokes(tokes);
			break;
		}
		for(fxn = fxns ; fxn->cmd ; ++fxn){
			if(strcasecmp(fxn->cmd,tokes[0])){
				continue;
			}
			break;
		}
		if(fxn->fxn){
			fxn->fxn(tokes);
		}else{
			fprintf(stderr,"Unknown command: %s\n",tokes[0]);
		}
		free_tokes(tokes);
	}
	printf("\n");
	return 0;
}

int main(int argc,char * const *argv){
	if(growlight_init(argc,argv)){
		return EXIT_FAILURE;
	}
	rl_prep_terminal(1); // 1 == read 8-bit input
	if(tty_ui()){
		growlight_stop();
		return EXIT_FAILURE;
	}
	if(growlight_stop()){
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
