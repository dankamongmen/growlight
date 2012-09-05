#include <errno.h>
#include <assert.h>
#include <stdlib.h>

#include "ncurses.h"
#include "growlight.h"
#include "aggregate.h"
#include "ui-aggregate.h"

static const char AGGCOMP_TEXT[] =
"Bind devices to the new aggregate. To be eligible, a device must either be "
"unpartitioned, or be a partition having the appropriate component type. The "
"device furthermore must not have a valid filesystem signature.";

static const char AGGTYPE_TEXT[] =
"What kind of aggregate do you hope to create?";

static const char AGGNAME_TEXT[] =
"The chosen name will be the primary means by which the system makes use of "
"this new aggregate, so choose wisely, and plan for the future!";

static inline int
device_aggregablep(const device *d){
	if(d->mnt){
		return 0;
	}
	if(d->size == 0){
		return 0;
	}
	if(d->roflag){
		return 0;
	}
	switch(d->layout){
		case LAYOUT_NONE:
			if(d->blkdev.unloaded){
				return 0;
			}
			if(d->blkdev.pttable){
				return 0;
			}
			break;
		case LAYOUT_PARTITION:
			break;
		case LAYOUT_MDADM:
		case LAYOUT_DM:
		case LAYOUT_ZPOOL:
			break;
	}
	return 1;
}

typedef enum {
	FORM_SELECT,			// form_option[]
	FORM_STRING_INPUT,		// form_input
} form_enum;

struct form_option {
	char *option;			// option key (the string passed to cb)
	char *desc;			// longer description
};

struct form_input {
	const char *prompt;		// short prompt. currently aliases boxstr
	char *longprompt;		// longer prompt, not currently used
	char *buffer;			// input buffer, initialized to ""
};

struct form_state {
	PANEL *p;
	int ysize;			// number of lines of *text* (not win)
	void (*fxn)(const char *);	// callback once form is done
	int idx;			// selection index, [0..ysize)
	int longop;			// length of longest op
	char *boxstr;			// string for box label
	form_enum formtype;		// type of form
	union {
		struct {
			struct form_option *ops;// form_option array for *this instance*
			int scrolloff;		// scroll offset
			int opcount;		// total number of ops
		};
		struct form_input inp;	// form_input state for this instance
	};
};

static char *pending_aggname;
static char *pending_aggtype;

static void
destroy_agg_forms(void){
	free(pending_aggtype);
	pending_aggtype = NULL;
	free(pending_aggname);
	pending_aggname = NULL;
}

static struct form_option *
agg_table(int *count,char *match,int *defidx){
	struct form_option *fo = NULL,*tmp;
	const aggregate_type *types;
	int z;

	*defidx = -1;
	if((types = get_aggregate_types(count)) == NULL){
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
			if(aggregate_default_p(key)){
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

static struct form_option *
grow_component_table(const device *d,int *count,const char *match,int *defidx,
			char ***selarray,int *selections,struct form_option *fo){
	struct form_option *tmp;
	char *key,*desc;

	if((key = strdup(d->name)) == NULL){
		return NULL;
	}
	if(match){
		if(strcmp(key,match) == 0){
			int z;

			*defidx = *count;
			for(z = 0 ; z < *selections ; ++z){
				if(strcmp(key,(*selarray)[z]) == 0){
					free((*selarray)[z]);
					(*selarray)[z] = NULL;
					if(z < *selections - 1){
						memmove(&(*selarray)[z],&(*selarray)[z + 1],sizeof(**selarray) * (*selections - 1 - z));
					}
					--*selections;
					z = -1;
					break;
				}
			}
			if(z >= *selections){
				typeof(*selarray) tmp;

				if((tmp = realloc(*selarray,sizeof(**selarray) * (*selections + 1))) == NULL){
					free(key);
					return NULL;
				}
				*selarray = tmp;
				(*selarray)[*selections] = strdup(match);
				++*selections;
			}
		}
	}
	if((desc = strdup(d->bypath ? d->bypath : d->model ? d->model :
				d->wwn ? d->wwn : d->name)) == NULL){
		free(key);
		return NULL;
	}
	if((tmp = realloc(fo,sizeof(*fo) * (*count + 1))) == NULL){
		free(key);
		free(desc);
		return NULL;
	}
	fo = tmp;
	fo[*count].option = key;
	fo[*count].desc = desc;
	++*count;
	return fo;
}

static struct form_option *
component_table(const aggregate_type *at,int *count,const char *match,int *defidx,
		char ***selarray,int *selections){
	struct form_option *fo = NULL,*tmp;
	const controller *c;

	*count = 0;
	*defidx = -1;
	for(c = get_controllers() ; c ; c = c->next){
		const device *d;

		for(d = c->blockdevs ; d ; d = d->next){
			const device *p;

			if(device_aggregablep(d)){
				if((tmp = grow_component_table(d,count,match,defidx,selarray,selections,fo)) == NULL){
					goto err;
				}
				fo = tmp;
			}
			for(p = d->parts ; p ; p = p->next){
				if(device_aggregablep(p)){
					if((tmp = grow_component_table(p,count,match,defidx,selarray,selections,fo)) == NULL){
						goto err;
					}
					fo = tmp;
				}
			}
		}
	}
	*defidx = (*defidx + 1) % *count;
	if(at->maxfaulted){
		if((tmp = realloc(fo,sizeof(*fo) * (*count + 1))) == NULL){
			goto err;
		}
		fo = tmp;
		fo[*count].option = strdup("missing");
		fo[*count].desc = strdup("force construction of degraded array");
		++*count;
	}
	return fo;

err:
	// FIXME free up selarray?
	while(*count--){
		free(fo[*count].option);
		free(fo[*count].desc);
	}
	free(fo);
	return NULL;
}

static void agg_callback(const char *);
static void aggname_callback(const char *);

static void
do_agg(const aggregate_type *at,char * const *selarray,int selections){
	struct panel_state *ps;
	int r;

	if(at->makeagg == NULL){
		locked_diag("FIXME %s creation is not yet implemented",pending_aggtype);
		return;
	}

	ps = show_splash(L"Creating aggregate...");
	r = at->makeagg(pending_aggname,selarray,selections);
	if(ps){
		kill_splash(ps);
	}
	if(r == 0){
		locked_diag("Successfully created %s",pending_aggtype);
	}
}

static void
aggcomp_callback(const char *fn,char **selarray,int selections,int scroll){
	struct form_option *comps_agg;
	const aggregate_type *at;
	int opcount,defidx;

	if(fn == NULL){
		raise_str_form("enter aggregate name",aggname_callback,
				pending_aggname,AGGNAME_TEXT);
		return;
	}
	if((at = get_aggregate(pending_aggtype)) == NULL){
		destroy_agg_forms();
		return;
	}
	if(strcmp(fn,"") == 0){
		if(selections >= at->mindisks){
			do_agg(at,selarray,selections);
			destroy_agg_forms();
			return;
		}
	}
	if((comps_agg = component_table(at,&opcount,fn,&defidx,&selarray,&selections)) == NULL){
		destroy_agg_forms();
		return;
	}
	raise_multiform("select aggregate components",aggcomp_callback,comps_agg,
			opcount,defidx,at->mindisks,selarray,selections,AGGCOMP_TEXT,scroll);
	locked_diag("%s needs at least %d devices",pending_aggtype,at->mindisks);
}

static void
aggname_callback(const char *fn){
	struct form_option *comps_agg;
	const aggregate_type *at;
	int selections = 0;
	int opcount,defidx;
	char **selarray;

	if(fn == NULL){
		struct form_option *ops_agg;

		if( (ops_agg = agg_table(&opcount,pending_aggtype,&defidx)) ){
			raise_form("select an aggregate type",agg_callback,ops_agg,
					opcount,defidx,AGGTYPE_TEXT);
		}
		return;
	}
	if((pending_aggname = strdup(fn)) == NULL){
		destroy_agg_forms();
		return;
	}
	if((at = get_aggregate(pending_aggtype)) == NULL){
		destroy_agg_forms();
		return;
	}
	selarray = NULL;
	if((comps_agg = component_table(at,&opcount,NULL,&defidx,&selarray,&selections)) == NULL){
		destroy_agg_forms();
		return;
	}
	raise_multiform("select aggregate components",aggcomp_callback,comps_agg,
			opcount,defidx,at->mindisks,selarray,selections,AGGCOMP_TEXT,0);
}

static void
agg_callback(const char *fn){
	const aggregate_type *at;

	if(fn == NULL){
		locked_diag("aggregate creation was cancelled");
		return;
	}
	if((at = get_aggregate(fn)) == NULL){
		destroy_agg_forms();
		return;
	}
	if((pending_aggtype = strdup(fn)) == NULL){
		destroy_agg_forms();
		return;
	}
	raise_str_form("enter aggregate name",aggname_callback,at->defname,
			AGGNAME_TEXT);
}

int raise_aggregate_form(void){
	struct form_option *ops_agg;
	int opcount,defidx;

	if((ops_agg = agg_table(&opcount,pending_aggtype,&defidx)) == NULL){
		destroy_agg_forms();
		return -1;
	}
	raise_form("select an aggregate type",agg_callback,ops_agg,opcount,
			defidx,AGGTYPE_TEXT);
	return 0;
}
