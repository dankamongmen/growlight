#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <mdadm.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>

#include <sg.h>
#include <sysfs.h>
#include <growlight.h>

#define SG_ATA_16	0x85
#define SG_ATA_16_LEN	16
#define SG_ATA_PROTO_NON_DATA	( 3 << 1)

// Taken from hdparm-9.39's sgio.h
enum {
        SG_CDB2_TLEN_NODATA     = 0 << 0,
        SG_CDB2_TLEN_FEAT       = 1 << 0,
        SG_CDB2_TLEN_NSECT      = 2 << 0,
        SG_CDB2_TLEN_BYTES      = 0 << 2,
        SG_CDB2_TLEN_SECTORS    = 1 << 2,
        SG_CDB2_TDIR_TO_DEV     = 0 << 3,
        SG_CDB2_TDIR_FROM_DEV   = 1 << 3,
        SG_CDB2_CHECK_COND      = 1 << 5,
};

enum {
        ATA_USING_LBA           = (1 << 6),
        ATA_STAT_DRQ            = (1 << 3),
        ATA_STAT_ERR            = (1 << 0),
};

enum {
        ATA_OP_DSM                      = 0x06, // Data Set Management (TRIM)
        ATA_OP_READ_PIO                 = 0x20,
        ATA_OP_READ_PIO_ONCE            = 0x21,
        ATA_OP_READ_LONG                = 0x22,
        ATA_OP_READ_LONG_ONCE           = 0x23,
        ATA_OP_READ_PIO_EXT             = 0x24,
        ATA_OP_READ_DMA_EXT             = 0x25,
        ATA_OP_READ_FPDMA               = 0x60, // NCQ
        ATA_OP_WRITE_PIO                = 0x30,
        ATA_OP_WRITE_LONG               = 0x32,
        ATA_OP_WRITE_LONG_ONCE          = 0x33,
        ATA_OP_WRITE_PIO_EXT            = 0x34,
        ATA_OP_WRITE_DMA_EXT            = 0x35,
        ATA_OP_WRITE_FPDMA              = 0x61, // NCQ
        ATA_OP_READ_VERIFY              = 0x40,
        ATA_OP_READ_VERIFY_ONCE         = 0x41,
        ATA_OP_READ_VERIFY_EXT          = 0x42,
        ATA_OP_WRITE_UNC_EXT            = 0x45, // lba48, no data, uses feat reg
        ATA_OP_FORMAT_TRACK             = 0x50,
        ATA_OP_DOWNLOAD_MICROCODE       = 0x92,
        ATA_OP_STANDBYNOW2              = 0x94,
        ATA_OP_CHECKPOWERMODE2          = 0x98,
        ATA_OP_SLEEPNOW2                = 0x99,
        ATA_OP_PIDENTIFY                = 0xa1,
        ATA_OP_READ_NATIVE_MAX          = 0xf8,
        ATA_OP_READ_NATIVE_MAX_EXT      = 0x27,
        ATA_OP_SMART                    = 0xb0,
        ATA_OP_DCO                      = 0xb1,
        ATA_OP_ERASE_SECTORS            = 0xc0,
        ATA_OP_READ_DMA                 = 0xc8,
        ATA_OP_WRITE_DMA                = 0xca,
        ATA_OP_DOORLOCK                 = 0xde,
        ATA_OP_DOORUNLOCK               = 0xdf,
        ATA_OP_STANDBYNOW1              = 0xe0,
        ATA_OP_IDLEIMMEDIATE            = 0xe1,
        ATA_OP_SETIDLE                  = 0xe3,
        ATA_OP_SET_MAX                  = 0xf9,
        ATA_OP_SET_MAX_EXT              = 0x37,
        ATA_OP_SET_MULTIPLE             = 0xc6,
        ATA_OP_CHECKPOWERMODE1          = 0xe5,
        ATA_OP_SLEEPNOW1                = 0xe6,
        ATA_OP_FLUSHCACHE               = 0xe7,
        ATA_OP_FLUSHCACHE_EXT           = 0xea,
        ATA_OP_IDENTIFY                 = 0xec,
        ATA_OP_SETFEATURES              = 0xef,
        ATA_OP_SECURITY_SET_PASS        = 0xf1,
        ATA_OP_SECURITY_UNLOCK          = 0xf2,
        ATA_OP_SECURITY_ERASE_PREPARE   = 0xf3,
        ATA_OP_SECURITY_ERASE_UNIT      = 0xf4,
        ATA_OP_SECURITY_FREEZE_LOCK     = 0xf5,
        ATA_OP_SECURITY_DISABLE         = 0xf6,
        ATA_OP_VENDOR_SPECIFIC_0x80     = 0x80,
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

int sg_interrogate(device *d,int fd){
	unsigned char cdb[SG_ATA_16_LEN];
	struct scsi_sg_io_hdr io;
	char sb[32];

	memset(cdb,0,sizeof(cdb));
	cdb[0]= SG_ATA_16;
	cdb[1] = SG_ATA_PROTO_NON_DATA;
	cdb[2] = SG_CDB2_CHECK_COND;
	cdb[8] = ATA_USING_LBA;
	cdb[9] = ATA_OP_IDENTIFY;
	// data size: 512
	memset(&io,0,sizeof(io));
	io.interface_id = 'S';
	io.mx_sb_len = sizeof(sb);
	io.sbp = sb;
	io.cmd_len = sizeof(cdb);
	io.cmdp = cdb;
	io.dxfer_direction = SG_DXFER_NONE;
	if(ioctl(fd,SG_IO,&io)){
		fprintf(stderr,"Couldn't perform SG_IO ioctl on %d (%s?)\n",fd,strerror(errno));
		return -1;
	}
	assert(d); // FIXME
	return 0;
}
