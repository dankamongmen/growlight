#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <popen.h>
#include <string.h>

int popen_drain(const char *cmd){
	char buf[BUFSIZ];
	FILE *fd;

	if((fd = popen(cmd,"r")) == NULL){
		fprintf(stderr,"Couldn't run %s (%s?)\n",cmd,strerror(errno));
		return -1;
	}
	while(fgets(buf,sizeof(buf),fd)){
		printf("%s",buf);
	}
	if(!feof(fd)){
		fprintf(stderr,"Error running %s (%s?)\n",cmd,strerror(errno));
		fclose(fd);
		return -1;
	}
	if(fclose(fd)){
		fprintf(stderr,"Error closing %s (%s?)\n",cmd,strerror(errno));
	}
	return 0;
}
