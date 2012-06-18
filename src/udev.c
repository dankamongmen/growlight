#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libudev.h>

#include <udev.h>
#include <pthread.h>
#include <growlight.h>

static struct udev *udev;
struct udev_monitor *udmon;

int udev_event(void){
	fprintf(stderr,"UDEV EVENT!\n");
	return -1;
}

int monitor_udev(void){
	int r;

	if((udev = udev_new()) == NULL){
		fprintf(stderr,"Couldn't get udev instance (%s?)\n",strerror(errno));
		return -1;
	}
	if((udmon = udev_monitor_new_from_netlink(udev,"udev")) == NULL){
		fprintf(stderr,"Couldn't get udev monitor (%s?)\n",strerror(errno));
		udev_unref(udev);
		return -1;
	}
	if(udev_monitor_filter_add_match_subsystem_devtype(udmon,"block",NULL)){
		fprintf(stderr,"Couldn't filter block events\n");
		udev_monitor_unref(udmon);
		udev_unref(udev);
		return -1;
	}
	if(udev_monitor_enable_receiving(udmon)){
		fprintf(stderr,"Couldn't receive events from udev\n");
		udev_monitor_unref(udmon);
		udev_unref(udev);
		return -1;
	}
	if((r = udev_monitor_get_fd(udmon)) < 0){
		fprintf(stderr,"Couldn't get udev fd\n");
		udev_monitor_unref(udmon);
		udev_unref(udev);
		return -1;
	}
	return r;
}

int shutdown_udev(void){
	udev_monitor_unref(udmon);
	udev_unref(udev);
	udmon = NULL;
	udev = NULL;
	return 0;
}
