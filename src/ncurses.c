#include <assert.h>
#include <stdlib.h>

#include "growlight.h"

int main(int argc,char * const *argv){
	if(growlight_init(argc,argv)){
		return EXIT_FAILURE;
	}
	// FIXME do stuff
	if(growlight_stop()){
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
