#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libudev.h>

#include <udev.h>
#include <growlight.h>

int monitor_udev(void){
	struct udev_monitor *udmon;
	struct udev *udev;

	if((udev = udev_new()) == NULL){
		fprintf(stderr,"Couldn't get udev instance (%s?)\n",strerror(errno));
		return -1;
	}
	if((udmon = udev_monitor_new_from_netlink(udev,"udev")) == NULL){
		fprintf(stderr,"Couldn't get udev monitor (%s?)\n",strerror(errno));
		udev_unref(udev);
		return -1;
	}
	return 0;
}
