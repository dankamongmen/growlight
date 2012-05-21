#include <stdio.h>
#include <stdlib.h>
#include <src/config.h>

int main(void){
	printf("%s %s\n",PACKAGE,PACKAGE_VERSION);
	return EXIT_SUCCESS;
}
