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

static struct form_state *
create_form(const char *str,void (*fxn)(const char *),form_enum ftype){
	struct form_state *fs;

	if( (fs = malloc(sizeof(*fs))) ){
		memset(fs,0,sizeof(*fs));
		if((fs->boxstr = strdup(str)) == NULL){
			locked_diag("Couldn't create input dialog (%s?)",strerror(errno));
			free(fs);
			return NULL;
		}
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

		fsw = panel_window(fs->p);
		hide_panel(fs->p);
		assert(del_panel(fs->p) == OK);
		fs->p = NULL;
		assert(delwin(fsw) == OK);
		switch(fs->formtype){
			case FORM_SELECT:
				for(z = 0 ; z < fs->ysize ; ++z){
					free(fs->ops[z].option);
					free(fs->ops[z].desc);
				}
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
		}
		fs->ysize = -1;
	}
}

static void
free_form(struct form_state *fs){
	if(fs){
		free(fs->boxstr);
		destroy_form_locked(fs);
		free(fs);
	}
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
	for(z = 0 ; z < fs->ysize ; ++z){
		int op = (z + fs->scrolloff) % fs->opcount;

		assert(op >= 0);
		assert(op < fs->opcount);
		wcolor_set(fsw,11,NULL);
		mvwprintw(fsw,z + 1,1,"%-*.*s ",
			fs->longop,fs->longop,opstrs[op].option);
		if(z == fs->idx){
			wattron(fsw,A_REVERSE);
		}
		wcolor_set(fsw,12,NULL);
		wprintw(fsw,"%-*.*s",cols - fs->longop - 1 - 2,
			cols - fs->longop - 1 - 2,opstrs[op].desc);
		if(z == fs->idx){
			wattroff(fsw,A_REVERSE);
		}
	}
}

#define FORM_Y_OFFSET 5
#define FORM_X_OFFSET 5
static void
raise_form(const char *str,void (*fxn)(const char *),struct form_option *opstrs,int ops,int defidx){
	size_t longop,longdesc;
	struct form_state *fs;
	int cols,rows;
	WINDOW *fsw;
	int x,y;

	if(opstrs == NULL || !ops){
		locked_diag("Passed empty %u-option string table",ops);
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
	rows = ops + 2;
	getmaxyx(stdscr,y,x);
	if(x < cols + 2){
		locked_diag("Window too thin for form, uh-oh");
		return;
	}
	if(y < FORM_Y_OFFSET + 2 + 1){ // two boundaries, at least 1 selection
		locked_diag("Window too short for form, uh-oh");
		return;
	}
	if(y <= rows + FORM_Y_OFFSET){
		rows = y - FORM_Y_OFFSET - 1;
	}
	if((fs = create_form(str,fxn,FORM_SELECT)) == NULL){
		return;
	}
	if((fsw = newwin(rows,cols + 2,FORM_Y_OFFSET,FORM_X_OFFSET)) == NULL){
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
	// FIXME adapt for scrolling
	if((fs->idx = defidx) < 0){
		fs->idx = defidx = 0;
	}
	fs->opcount = ops;
	fs->ysize = rows - 2;
	wattroff(fsw,A_BOLD);
	wcolor_set(fsw,COLOR_GREEN,NULL);
	//bevel(fsw);
	wattron(fsw,A_BOLD);
	mvwprintw(fsw,0,cols - strlen(fs->boxstr),fs->boxstr);
	mvwprintw(fsw,fs->ysize + 1,cols - strlen("⎋esc returns"),"⎋esc returns");
	wattroff(fsw,A_BOLD);
	fs->longop = longop;
	fs->ops = opstrs;
	form_options(fs);
	//actform = fs;
	//screen_update();
}

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
