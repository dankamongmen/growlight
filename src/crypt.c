#include <libcryptsetup.h>

#include "crypt.h"
#include "growlight.h"

// Create LUKS on the device
int cryptondev(device *d){
	struct crypt_params_luks1 params;
	struct crypt_device *cctx;
	char path[PATH_MAX + 1];

	if((unsigned)snprintf(path,sizeof(path),"/dev/%s",d->name) >= sizeof(path)){
		diag("Bad path: /dev/%s\n",d->name);
		return -1;
	}
	if(crypt_init(&cctx,path)){
		diag("Couldn't create LUKS context for %s\n",d->name);
		return -1;
	}
	memset(&params,0,sizeof(params));
	params.hash = "sha1";
	if(crypt_format(cctx,CRYPT_LUKS1,"aes","xts-plain64",NULL,NULL,256 / CHAR_BIT,&params)){
		diag("Couldn't format LUKS on %s\n",d->name);
		return -1;
	}
	// FIXME still need to set up keyslots and activate, including
	// acquisition of a passphrase
	crypt_free(cctx);
	return 0;
}

int crypt_start(void){
	return 0;
}

int crypt_stop(void){
	return 0;
}
