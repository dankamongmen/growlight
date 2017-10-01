#include "nvme.h"
#include <errno.h>
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

int nvme_interrogate(struct device *d, int fd){
	struct nvme_admin_cmd nvmeio;
	struct nvme_id_ctrl ctrl;

	memset(&ctrl, 0, sizeof(ctrl));
	memset(&nvmeio, 0, sizeof(nvmeio));
	// FIXME where can we get this value from besides nvme-cli source?
	nvmeio.opcode = 6;
	nvmeio.addr = &ctrl;
	nvmeio.data_len = sizeof(ctrl);
	nvmeio.cdw10 = 1; // FIXME what is this?
	if(ioctl(fd, NVME_IOCTL_ADMIN_CMD, &nvmeio)){
		diag("Couldn't perform NVME_IOCTL_ADMIN_CMD on %s:%d (%s?)\n",
				d->name,fd,strerror(errno));
		return 0;
	}
	// FIXME implement
	d->blkdev.transport = DIRECT_NVME;
	d->blkdev.rotation = -1; // non-rotating store
	return 0;
}
