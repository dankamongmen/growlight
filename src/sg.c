// copyright 2012–2020 nick black, except where otherwise noted below
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <scsi/sg.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <linux/hdreg.h>

#include "sg.h"
#include "sysfs.h"
#include "growlight.h"

// Consult T10/04-262r3 "ATA Comand Pass-Through" revision 3 2004-08-31
// The following defines and enums are taken from sgio.h from hdparm 9.58.
// That file is copyright Mark Lord (mlord@pobox.com), and licensed as follows:
//
// You may freely use, modify, and redistribute the hdparm program,
// as either binary or source, or both.
//
// The only condition is that my name and copyright notice
// remain in the source code as-is.
//
// Mark Lord (mlord@pobox.com)
static const int SG_ATA_16 = 0x85; // 16-byte ATA pass-though command
#define SG_ATA_16_LEN	16
static const int SG_ATA_PROTO_PIO_IN = 4;
#define SG_CHECK_CONDITION	0x02
#define SG_DRIVER_SENSE		0x08
#define START_SERIAL            10  // ASCII serial number
#define LENGTH_SERIAL           20
#define CMDS_SUPP_0             82  // command/feature set(s) supported
#define FEATURE_WRITE_CACHE     16  // use with CMDS_SUPP_1
#define CMDS_SUPP_1             83
#define CMDS_SUPP_2             84
#define CMDS_SUPP_3             119
#define FEATURE_READWRITEVERIFY 2   // use with CMDS_SUPP_3
#define CMDS_EN_0               85  // command/feature set(s) enabled
#define CMDS_EN_1               86
#define CMDS_EN_2               87
#define CMDS_EN_3               120
#define TRANSPORT_MAJOR         222
#define TRANSPORT_MINOR         223
#define NMRR                    217
#define WWN_SUP			0x100

enum {
        SG_CDB2_TLEN_NSECT      = 2 << 0,
        SG_CDB2_TLEN_SECTORS    = 1 << 2,
        SG_CDB2_TDIR_FROM_DEV   = 1 << 3,
};

enum {
        ATA_USING_LBA           = (1 << 6),
};

enum {
        ATA_OP_PIDENTIFY                = 0xa1,
        ATA_OP_IDENTIFY                 = 0xec,
};

struct scsi_sg_io_hdr {
        int                     interface_id;
        int                     dxfer_direction;
        unsigned char           cmd_len;
        unsigned char           mx_sb_len;
        unsigned short          iovec_count;
        unsigned int            dxfer_len;
        void *                  dxferp;
        unsigned char *         cmdp;
        void *                  sbp;
        unsigned int            timeout;
        unsigned int            flags;
        int                     pack_id;
        void *                  usr_ptr;
        unsigned char           status;
        unsigned char           masked_status;
        unsigned char           msg_status;
        unsigned char           sb_len_wr;
        unsigned short          host_status;
        unsigned short          driver_status;
        int                     resid;
        unsigned int            duration;
        unsigned int            info;
};
// Material taken from hdparm ends here

int sg_interrogate(device *d, int fd){
#define IDSECTORS 1
	unsigned char cdb[SG_ATA_16_LEN];
	uint16_t buf[512 / 2], maj, min; // FIXME
	struct scsi_sg_io_hdr io;
	char sb[32];
	unsigned n;

	assert(d->layout == LAYOUT_NONE);
	memset(buf, 0, sizeof(buf));
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = SG_ATA_16;
	cdb[1] = SG_ATA_PROTO_PIO_IN << 1u;
	cdb[2] = SG_CDB2_TLEN_NSECT | SG_CDB2_TLEN_SECTORS | SG_CDB2_TDIR_FROM_DEV;
	cdb[6] = IDSECTORS;
	cdb[13] = ATA_USING_LBA;
	cdb[14] = ATA_OP_IDENTIFY;
	// data size: 512
	memset(&io, 0, sizeof(io));
	io.interface_id = 'S';
	io.mx_sb_len = sizeof(sb);
	io.dxfer_direction = SG_DXFER_FROM_DEV;
	io.dxfer_len = sizeof(buf);
	io.dxferp = buf;
	io.cmdp = cdb;
	io.sbp = sb;
	io.cmd_len = sizeof(cdb);
	if(ioctl(fd, SG_IO, &io)){
		diag("Couldn't perform SG_IO ioctl on %s:%d (%s?)\n", d->name, fd, strerror(errno));
		return -1;
	}
	if(io.driver_status && io.driver_status != SG_DRIVER_SENSE){
		verbf("Bad driver status 0x%x on %s\n", io.driver_status, d->name);
		cdb[14] = ATA_OP_PIDENTIFY;
		if(ioctl(fd, SG_IO, &io)){
			diag("Couldn't perform PIDENTIFY ioctl on %s:%d (%s?)\n", d->name, fd, strerror(errno));
			return -1;
		}
		if(io.driver_status && io.driver_status != SG_DRIVER_SENSE){
			verbf("Bad PIDENTIFY status 0x%x on %s\n", io.driver_status, d->name);
			return -1;
		}
	}
	if(io.status && io.status != SG_CHECK_CONDITION){
		verbf("Bad check condition 0x%x on %s\n", io.status, d->name);
		return 0; // FIXME
	}
	if(io.host_status){
		verbf("Bad host status 0x%x on %s\n", io.host_status, d->name);
		return 0; // FIXME
	}
	maj = buf[TRANSPORT_MAJOR] >> 12u;
	min = buf[TRANSPORT_MAJOR] & 0xfffu;
	switch(maj){
		case 0:
			d->blkdev.transport = PARALLEL_ATA;
		break;
		case 1:
			if(min & (1u << 5u)){
				d->blkdev.transport = SERIAL_ATAIII;
			}else if(min & (1u << 2u)){
				d->blkdev.transport = SERIAL_ATAII;
			}else if(min & (1u << 1u)){
				d->blkdev.transport = SERIAL_ATAI;
			}else if(min & 1u){
				d->blkdev.transport = SERIAL_ATA8;
			}else{
				d->blkdev.transport = SERIAL_UNKNOWN;
			}
		break;
		default:
			diag("Unknown transport type %hu on %s\n", maj, d->name);
			break;
	}
	if((d->blkdev.rotation = buf[NMRR]) == 1){
		d->blkdev.rotation = SSD_ROTATION; // special value for solidstate
	}else if(d->blkdev.rotation <= 0x401){
		d->blkdev.rotation = 0; // unknown rate
	}
	if(ntohs(buf[CMDS_SUPP_0]) & FEATURE_WRITE_CACHE){
		d->blkdev.wcache = !!(ntohs(buf[CMDS_EN_0]) & FEATURE_WRITE_CACHE);
		verbf("\t%s write-cache: %s\n", d->name, d->blkdev.wcache ? "Enabled" : "Disabled/not present");
	}
	if(ntohs(buf[CMDS_SUPP_2]) & WWN_SUP){
		free(d->blkdev.wwn);
		if((d->blkdev.wwn = malloc(17)) == NULL){
			return -1;
		}
		snprintf(d->blkdev.wwn, 17, "%04x%04x%04x%04x", buf[108], buf[109], buf[110], buf[111]);
	}
	if(buf[CMDS_SUPP_3] & FEATURE_READWRITEVERIFY){
		if(ntohs(buf[CMDS_EN_3]) & FEATURE_READWRITEVERIFY){
			d->blkdev.rwverify = RWVERIFY_SUPPORTED_ON;
		}else{
			d->blkdev.rwverify = RWVERIFY_SUPPORTED_OFF;
		}
	}else{
		d->blkdev.rwverify = RWVERIFY_UNSUPPORTED;
	}
	verbf("\t%s read-write-verify: %s\n", d->name,
			d->blkdev.rwverify == RWVERIFY_UNSUPPORTED ? "Not present" :
			d->blkdev.rwverify == RWVERIFY_SUPPORTED_OFF ? "Disabled" : "Enabled");
	for(n = START_SERIAL ; n < START_SERIAL + LENGTH_SERIAL ; ++n){
		buf[n] = ntohs(buf[n]);
	}
	free(d->blkdev.serial);
	d->blkdev.serial = cleanup_serial(buf + START_SERIAL, LENGTH_SERIAL * sizeof(*buf));
	if(d->blkdev.serial == NULL){
		return -1;
	}
	return 0;
}

// Serial numbers with weird whitespace are surprisingly common. Clean 'em up.
void *cleanup_serial(const void *vserial, size_t snmax) {
	char *clean;
	size_t snlen = 0; // number of copied characters <= snmax
	size_t iter; // iterator in original string
	bool inspace = true; // trim out leading/repeated whitespace
	if((clean = malloc(snmax + 1)) == NULL){
		return NULL;
	}
	const char *serial = vserial;
	for(iter = 0 ; iter < snmax ; ++iter){
		if(!serial[iter]){
			break;
		}else if(isspace(serial[iter]) || !isprint(serial[iter])){
			if(!inspace){ // trims early or repeated whitespace
				clean[snlen++] = ' ';
				inspace = true;
			}
		}else{
			clean[snlen++] = serial[iter];
			inspace = false;
		}
	}
	if(inspace && snlen){ // space was at the end, after actual data
		--snlen;
	}
	clean[snlen] = '\0';
	return clean;
}
