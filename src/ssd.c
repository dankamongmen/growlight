#include "ssd.h"
#include "popen.h"
#include "growlight.h"

int fstrim(const char *mnt){
	return vspopen_drain("fstrim -v %s",mnt);
}
