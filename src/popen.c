// copyright 2012–2020 nick black
#include <assert.h>
#include <wchar.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "popen.h"
#include "growlight.h"

#define REDIRECTERR " 2>&1 < /dev/null"
static char *
sanitize_cmd(const char *cmd){
	char *tmp,*san = NULL;
	size_t left,len = 0;
	mbstate_t ps;
	size_t conv;

	memset(&ps,0,sizeof(ps));
	left = strlen(cmd) + 1;
	do{
		unsigned escape;
		wchar_t w;

		if((conv = mbrtowc(&w,cmd,left,&ps)) == (size_t)-1){
			diag("Error converting multibyte: %s\n",cmd);
			free(san);
			return NULL;
		}
		if(conv == (size_t)-2){
			// FIXME ended unexpectedly...are we feeding bad data?
			diag("Multibyte ended unexpectedly: %s\n",cmd);
			break;
		}
		if(conv == 0){ // done!
			break;
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
	if((tmp = realloc(san,sizeof(*san) * (len + 1 + strlen(REDIRECTERR)))) == NULL){
		free(san);
		return NULL;
	}
	san = tmp;
	strcpy(san + len,REDIRECTERR);
	return san;
}

int popen_drain(const char *cmd){
	char buf[128],*safecmd;
	FILE *fd;

	if((safecmd = sanitize_cmd(cmd)) == NULL){
		return -1;
	}
	diag("Running \"%s\"...\n",safecmd);
	if((fd = popen(safecmd,"re")) == NULL){
		diag("Couldn't run %s (%s?)\n",safecmd,strerror(errno));
		free(safecmd);
		return -1;
	}
	while(fgets(buf,sizeof(buf),fd)){
		diag("%s",buf);
	}
	if(!feof(fd)){
		diag("Error reading from '%s' (%s?)\n",safecmd,strerror(errno));
		pclose(fd);
		return -1;
	}
	if(pclose(fd)){
		diag("Error running '%s'\n",safecmd);
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

int vspopen_drain(const char *fmt,...){
	char buf[BUFSIZ];
	va_list va;

	va_start(va,fmt);
	if(vsnprintf(buf,sizeof(buf),fmt,va) >= (int)sizeof(buf)){
		va_end(va);
		diag("Bad command: %s ...\n",fmt);
		return -1;
	}
	va_end(va);
	return popen_drain(buf);
}
