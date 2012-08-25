#include <assert.h>
#include <stdlib.h>
#include "growlight.h"
#include "aggregate.h"
#include "ui-aggregate.h"

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

static void
agg_callback(const char *fn){
	if(fn == NULL){
		locked_diag("aggregate creation was cancelled");
		return;
	}
	// FIXME handle aggregate type
	locked_diag("not yet implemented FIXME");
}

static char *pending_aggtype;

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

static void
destroy_agg_forms(void){
}

int raise_aggregate_form(WINDOW *w){
	struct form_option *ops_agg;
	int opcount,defidx;

	if((ops_agg = agg_table(&opcount,pending_aggtype,&defidx)) == NULL){
		destroy_agg_forms();
		return -1;
	}
	raise_form("select an aggregate type",agg_callback,ops_agg,opcount,defidx);
	assert(w);
	return -1;
}
