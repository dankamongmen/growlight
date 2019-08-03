#include "nvme.h"
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <stdbool.h>
#include <atasmart.h>
#include "growlight.h"
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>

// copied from nvme-cli :/

struct nvme_id_power_state {
        __le16                  max_power;      /* centiwatts */
        __u8                    rsvd2;
        __u8                    flags;
        __le32                  entry_lat;      /* microseconds */
        __le32                  exit_lat;       /* microseconds */
        __u8                    read_tput;
        __u8                    read_lat;
        __u8                    write_tput;
        __u8                    write_lat;
        __le16                  idle_power;
        __u8                    idle_scale;
        __u8                    rsvd19;
        __le16                  active_power;
        __u8                    active_work_scale;
        __u8                    rsvd23[9];
};

struct nvme_id_ctrl {
        __le16                  vid;
        __le16                  ssvid;
        char                    sn[20];
        char                    mn[40];
        char                    fr[8];
        __u8                    rab;
        __u8                    ieee[3];
        __u8                    cmic;
        __u8                    mdts;
        __le16                  cntlid;
        __le32                  ver;
        __le32                  rtd3r;
        __le32                  rtd3e;
        __le32                  oaes;
        __le32                  ctratt;
        __u8                    rsvd100[156];
        __le16                  oacs;
        __u8                    acl;
        __u8                    aerl;
        __u8                    frmw;
        __u8                    lpa;
        __u8                    elpe;
        __u8                    npss;
        __u8                    avscc;
        __u8                    apsta;
        __le16                  wctemp;
        __le16                  cctemp;
        __le16                  mtfa;
        __le32                  hmpre;
        __le32                  hmmin;
        __u8                    tnvmcap[16];
        __u8                    unvmcap[16];
        __le32                  rpmbs;
        __le16                  edstt;
        __u8                    dsto;
        __u8                    fwug;
        __le16                  kas;
        __le16                  hctma;
        __le16                  mntmt;
        __le16                  mxtmt;
        __le32                  sanicap;
        __u8                    rsvd332[180];
        __u8                    sqes;
        __u8                    cqes;
        __le16                  maxcmd;
        __le32                  nn;
        __le16                  oncs;
        __le16                  fuses;
        __u8                    fna;
        __u8                    vwc;
        __le16                  awun;
        __le16                  awupf;
        __u8                    nvscc;
        __u8                    rsvd531;
        __le16                  acwu;
        __u8                    rsvd534[2];
        __le32                  sgls;
        __u8                    rsvd540[228];
        char                    subnqn[256];
        __u8                    rsvd1024[768];
        __le32                  ioccsz;
        __le32                  iorcsz;
        __le16                  icdoff;
        __u8                    ctrattr;
        __u8                    msdbd;
        __u8                    rsvd1804[244];
        struct nvme_id_power_state      psd[32];
        __u8                    vs[1024];
};

struct nvme_smart_log {
        __u8                    critical_warning;
        __u8                    temperature[2];
        __u8                    avail_spare;
        __u8                    spare_thresh;
        __u8                    percent_used;
        __u8                    rsvd6[26];
        __u8                    data_units_read[16];
        __u8                    data_units_written[16];
        __u8                    host_reads[16];
        __u8                    host_writes[16];
        __u8                    ctrl_busy_time[16];
        __u8                    power_cycles[16];
        __u8                    power_on_hours[16];
        __u8                    unsafe_shutdowns[16];
        __u8                    media_errors[16];
        __u8                    num_err_log_entries[16];
        __le32                  warning_temp_time;
        __le32                  critical_comp_time;
        __le16                  temp_sensor[8];
        __le32                  thm_temp1_trans_count;
        __le32                  thm_temp2_trans_count;
        __le32                  thm_temp1_total_time;
        __le32                  thm_temp2_total_time;
        __u8                    rsvd232[280];
};

#define NVME_LOG_SMART 2
#define NVME_ADMIN_GET_LOG_PAGE 2
#define NVME_ADMIN_IDENTIFY 6

static int
nvme_smart_log(struct device *d, int fd){
	struct nvme_admin_cmd nvmeio;
	struct nvme_smart_log smart;

	memset(&smart, 0, sizeof(smart));
	memset(&nvmeio, 0, sizeof(nvmeio));
	nvmeio.opcode = NVME_ADMIN_GET_LOG_PAGE;
	nvmeio.addr = (uintptr_t)&smart;
	nvmeio.data_len = sizeof(smart);
	// FIXME black magics stolen from nvme_get_log()
	nvmeio.nsid = 0xffffffffu;
	uint32_t numd = (nvmeio.data_len >> 2) - 1;
	uint16_t numdu = numd >> 16;
	uint16_t numdl = numd & 0xffff;
	nvmeio.cdw10 = NVME_LOG_SMART | (numdl << 16);
	nvmeio.cdw11 = numdu;
	if(ioctl(fd, NVME_IOCTL_ADMIN_CMD, &nvmeio)){
		diag("Couldn't perform nvme_admin_get_log_page on %s:%d (%s?)\n",
				d->name, fd, strerror(errno));
		return -1;
	}
	if(smart.critical_warning){
		d->blkdev.smart = SK_SMART_OVERALL_BAD_STATUS;
	}else{
		d->blkdev.smart = SK_SMART_OVERALL_GOOD;
	}
	// nvme smart reports temp in kelvin integer degrees, huh
	d->blkdev.celsius = ((smart.temperature[1] << 8) | smart.temperature[0]) - 273;
	return 0;
}

int nvme_interrogate(struct device *d, int fd){
	struct nvme_admin_cmd nvmeio;
	struct nvme_id_ctrl ctrl;

	memset(&ctrl, 0, sizeof(ctrl));
	memset(&nvmeio, 0, sizeof(nvmeio));
	// FIXME where can we get this value from besides nvme-cli source?
	nvmeio.opcode = NVME_ADMIN_IDENTIFY;
	nvmeio.addr = (uintptr_t)&ctrl;
	nvmeio.data_len = sizeof(ctrl);
	nvmeio.cdw10 = 1; // FIXME what is this?
	if(ioctl(fd, NVME_IOCTL_ADMIN_CMD, &nvmeio)){
		diag("Couldn't perform nvme_admin_identify on %s:%d (%s?)\n",
				d->name, fd, strerror(errno));
		return -1;
	}
	// These serials sometimes have weird whitespace; normalize it
	size_t snlen = 0; // number of copied characters <= sizeof(ctrl.sn)
	size_t endtrim = 0; // where did trailing whitespace start?
	bool inspace = 1; // trim out leading/repeated whitespace
	size_t iter; // iterator in original string
	for(iter = 0 ; iter < sizeof(ctrl.sn) ; ++iter){
		if(!ctrl.sn[iter]){
			break;
		}else if(isspace(ctrl.sn[iter]) || !isprint(ctrl.sn[iter])){
			if(!inspace){ // trims early or repeated whitespace
				ctrl.sn[snlen++] = ' ';
				inspace = true;
				endtrim = snlen - 1;
			}
		}else{
			ctrl.sn[snlen++] = ctrl.sn[iter];
			endtrim = 0;
			inspace = 0;
		}
	}
	if(endtrim){ // space included at the end, cannot be 0-length output
		snlen = endtrim;
	}
	d->blkdev.serial = malloc(snlen + 1);
	memcpy(d->blkdev.serial, ctrl.sn, snlen);
	d->blkdev.serial[snlen] = '\0';
	// NVMe devices don't appear to have WWNs...? NGUIDs are all 0s on mine?
	d->blkdev.wwn = strdup(d->blkdev.serial);
	d->blkdev.transport = DIRECT_NVME;
	d->blkdev.rotation = -1; // non-rotating store
	nvme_smart_log(d, fd);
	return 0;
}
