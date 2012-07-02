#include <assert.h>
#include <wchar.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "popen.h"

static char *
sanitize_cmd(const char *cmd){
	char *tmp,*san = NULL;
	size_t left,len = 0;
	mbstate_t ps;
	size_t conv;

	memset(&ps,0,sizeof(ps));
	left = strlen(cmd);
	do{
		unsigned escape;
		wchar_t w;

		if((conv = mbrtowc(&w,cmd,left,&ps)) == (size_t)-1){
			fprintf(stderr,"Error converting multibyte: %s\n",cmd);
			free(san);
		}
		left -= conv;
		if(w == L'(' || w == L')'){
			escape = 1;
		}else if(w == '$'){
			escape = 1;
		}else{
			escape = 0;
		}
		if((tmp = realloc(san,sizeof(*san) * (len + conv + escape))) == NULL){
			free(san);
			return NULL;
		}
		san = tmp;
		if(escape){
			san[len] = '\\';
			++len;
		}
		memcpy(san + len,cmd,conv);
		len += conv;
		cmd += conv;
	}while(conv);
	if((tmp = realloc(san,sizeof(*san) * (len + 1))) == NULL){
		free(san);
		return NULL;
	}
	san = tmp;
	san[len] = '\0';
	return san;
}

int popen_drain(const char *cmd){
	char buf[BUFSIZ],*safecmd;
	FILE *fd;

	if((safecmd = sanitize_cmd(cmd)) == NULL){
		return -1;
	}
	if((fd = popen(safecmd,"re")) == NULL){
		fprintf(stderr,"Couldn't run %s (%s?)\n",safecmd,strerror(errno));
		free(safecmd);
		return -1;
	}
	while(fgets(buf,sizeof(buf),fd)){
		printf("%s",buf);
	}
	if(!feof(fd)){
		fprintf(stderr,"Error reading from '%s' (%s?)\n",cmd,strerror(errno));
		fclose(fd);
		return -1;
	}
	if(fclose(fd)){
		fprintf(stderr,"Error running '%s'\n",cmd);
		return -1;
	}
	return 0;
}

int vpopen_drain(const char *cmd,wchar_t * const *args){
	char buf[BUFSIZ];
	int r;

	if((r = snprintf(buf,sizeof(buf),"%s ",cmd)) >= (int)sizeof(buf)){
		return -1;
	}
	while(*args){
		int rr;

		if((rr = snprintf(buf + r,sizeof(buf) - r,"%ls ",*args)) >= (int)(sizeof(buf) - r)){
			return -1;
		}
		r += rr;
		++args;
	}
	return popen_drain(buf);
}
