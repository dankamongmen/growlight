#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <scsi/sg.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <linux/hdreg.h>

#include "sg.h"
#include "sysfs.h"
#include "growlight.h"

// Taken from hdparm-9.39's sgio.h.
enum {					// Features for CMDS_SUPP_0
	FEATURE_SMART = 1u,
	FEATURE_SECMODE = 1u << 1,	// Security Mode feature set
	FEATURE_REMMODE = 1u << 2,	// Removable Media feature set
	FEATURE_WRITE_CACHE = 1u << 4,
} scsi_features;

enum {					// Features for CMDS_SUPP_3 (119)
	FEATURE_READWRITEVERIFY = 1u << 1,
} scsi_feats3;

#define SG_ATA_16	0x85
#define SG_ATA_16_LEN	16
#define SG_ATA_PROTO_NON_DATA	( 3 << 1)
#define SG_ATA_LBA48            1
#define SG_ATA_PROTO_NON_DATA   ( 3 << 1)
#define SG_ATA_PROTO_PIO_IN     ( 4 << 1)
#define SG_ATA_PROTO_PIO_OUT    ( 5 << 1)
#define SG_ATA_PROTO_DMA        ( 6 << 1)
#define SG_ATA_PROTO_UDMA_IN    (11 << 1) /* not yet supported in libata */
#define SG_ATA_PROTO_UDMA_OUT   (12 << 1) /* not yet supported in libata */
#define SG_CHECK_CONDITION	0x02
#define SG_DRIVER_SENSE		0x08

#define CONFIG_ATAPI		0x4000
#define CONFIG_ATA		0x8000

#define GEN_CONFIG              0   /* general configuration */
#define LCYLS                   1   /* number of logical cylinders */
#define CONFIG                  2   /* specific configuration */
#define LHEADS                  3   /* number of logical heads */
#define TRACK_BYTES             4   /* number of bytes/track (ATA-1) */
#define SECT_BYTES              5   /* number of bytes/sector (ATA-1) */
#define LSECTS                  6   /* number of logical sectors/track */
#define START_SERIAL            10  /* ASCII serial number */
#define LENGTH_SERIAL           10  /* 10 words (20 bytes or characters) */
#define BUF_TYPE                20  /* buffer type (ATA-1) */
#define BUF_SIZE                21  /* buffer size (ATA-1) */
#define RW_LONG                 22  /* extra bytes in R/W LONG cmd ( < ATA-4)*/
#define START_FW_REV            23  /* ASCII firmware revision */
#define LENGTH_FW_REV            4  /*  4 words (8 bytes or characters) */
#define START_MODEL             27  /* ASCII model number */
#define LENGTH_MODEL            20  /* 20 words (40 bytes or characters) */
#define SECTOR_XFER_MAX         47  /* r/w multiple: max sectors xfered */
#define DWORD_IO                48  /* can do double-word IO (ATA-1 only) */
#define CAPAB_0                 49  /* capabilities */
#define CAPAB_1                 50
#define PIO_MODE                51  /* max PIO mode supported (obsolete)*/
#define DMA_MODE                52  /* max Singleword DMA mode supported (obs)*/
#define WHATS_VALID             53  /* what fields are valid */
#define LCYLS_CUR               54  /* current logical cylinders */
#define LHEADS_CUR              55  /* current logical heads */
#define LSECTS_CUR              56  /* current logical sectors/track */
#define CAPACITY_LSB            57  /* current capacity in sectors */
#define CAPACITY_MSB            58
#define SECTOR_XFER_CUR         59  /* r/w multiple: current sectors xfered */
#define LBA_SECTS_LSB           60  /* LBA: total number of user */
#define LBA_SECTS_MSB           61  /*      addressable sectors */
#define SINGLE_DMA              62  /* singleword DMA modes */
#define MULTI_DMA               63  /* multiword DMA modes */
#define ADV_PIO_MODES           64  /* advanced PIO modes supported */
                                    /* multiword DMA xfer cycle time: */
#define DMA_TIME_MIN            65  /*   - minimum */
#define DMA_TIME_NORM           66  /*   - manufacturer's recommended   */
                                    /* minimum PIO xfer cycle time: */
#define PIO_NO_FLOW             67  /*   - without flow control */
#define PIO_FLOW                68  /*   - with IORDY flow control */
#define PKT_REL                 71  /* typical #ns from PKT cmd to bus rel */
#define SVC_NBSY                72  /* typical #ns from SERVICE cmd to !BSY */
#define CDR_MAJOR               73  /* CD ROM: major version number */
#define CDR_MINOR               74  /* CD ROM: minor version number */
#define QUEUE_DEPTH             75  /* queue depth */
#define SATA_CAP_0              76  /* Serial ATA Capabilities */
#define SATA_RESERVED_77        77  /* reserved for future Serial ATA definitions */
#define SATA_SUPP_0             78  /* Serial ATA features supported */
#define SATA_EN_0               79  /* Serial ATA features enabled */
#define MAJOR                   80  /* major version number */
#define MINOR                   81  /* minor version number */
#define CMDS_SUPP_0             82  /* command/feature set(s) supported */
#define CMDS_SUPP_1             83
#define CMDS_SUPP_2             84
#define CMDS_EN_0               85  /* command/feature set(s) enabled */
#define CMDS_EN_1               86
#define CMDS_EN_2               87
#define ULTRA_DMA               88  /* ultra DMA modes */
                                    /* time to complete security erase */
#define ERASE_TIME              89  /*   - ordinary */
#define ENH_ERASE_TIME          90  /*   - enhanced */
#define ADV_PWR                 91  /* current advanced power management level
				                                              in low byte, 0x40 in high byte. */  
#define PSWD_CODE               92  /* master password revision code    */
#define HWRST_RSLT              93  /* hardware reset result */
#define ACOUSTIC                94  /* acoustic mgmt values ( >= ATA-6) */
#define LBA_LSB                 100 /* LBA: maximum.  Currently only 48 */
#define LBA_MID                 101 /*      bits are used, but addr 103 */
#define LBA_48_MSB              102 /*      has been reserved for LBA in */
#define LBA_64_MSB              103 /*      the future. */
#define CMDS_SUPP_3             119
#define CMDS_EN_3               120
#define RM_STAT                 127 /* removable media status notification feature */
#define SECU_STATUS             128 /* security status */
#define CFA_PWR_MODE            160 /* CFA power mode 1 */
#define START_MEDIA             176 /* media serial number */
#define LENGTH_MEDIA            20  /* 20 words (40 bytes or characters)*/
#define START_MANUF             196 /* media manufacturer I.D. */
#define LENGTH_MANUF            10  /* 10 words (20 bytes or characters) */
#define SCT_SUPP                206 /* SMART command transport (SCT) support */
#define TRANSPORT_MAJOR         222 /* PATA vs. SATA etc.. */
#define TRANSPORT_MINOR         223 /* minor revision number */
#define NMRR                    217 /* nominal media rotation rate */
#define INTEGRITY               255 /* integrity word */
#define WWN_SUP			0x100

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
#define IDSECTORS 1
	unsigned char cdb[SG_ATA_16_LEN];
	uint16_t buf[512 / 2],maj,min; // FIXME
	struct scsi_sg_io_hdr io;
	char sb[32];
	unsigned n;

	memset(buf,0,sizeof(buf));
	memset(cdb,0,sizeof(cdb));
	cdb[0]= SG_ATA_16;
	cdb[1] = SG_ATA_PROTO_PIO_IN;
	cdb[2] = SG_CDB2_TLEN_NSECT | SG_CDB2_TLEN_SECTORS | SG_CDB2_TDIR_FROM_DEV;
	cdb[6] = IDSECTORS;
	cdb[13] = ATA_USING_LBA;
	cdb[14] = ATA_OP_IDENTIFY;
	// data size: 512
	memset(&io,0,sizeof(io));
	io.interface_id = 'S';
	io.mx_sb_len = sizeof(sb);
	io.dxfer_direction = SG_DXFER_FROM_DEV;
	io.dxfer_len = sizeof(buf);
	io.dxferp = buf;
	io.cmdp = cdb;
	io.sbp = sb;
	io.cmd_len = sizeof(cdb);
	if(ioctl(fd,SG_IO,&io)){
		diag("Couldn't perform SG_IO ioctl on %s:%d (%s?)\n",d->name,fd,strerror(errno));
		return -1;
	}
	if(io.driver_status && io.driver_status != SG_DRIVER_SENSE){
		verbf("Bad driver status 0x%x on %s\n",io.driver_status,d->name);
		cdb[14] = ATA_OP_PIDENTIFY;
		if(ioctl(fd,SG_IO,&io)){
			diag("Couldn't perform PIDENTIFY ioctl on %s:%d (%s?)\n",d->name,fd,strerror(errno));
			return -1;
		}
		if(io.driver_status && io.driver_status != SG_DRIVER_SENSE){
			verbf("Bad PIDENTIFY status 0x%x on %s\n",io.driver_status,d->name);
			return -1;
		}
	}
	if(io.status && io.status != SG_CHECK_CONDITION){
		verbf("Bad check condition 0x%x on %s\n",io.status,d->name);
		return 0; // FIXME
	}
	if(io.host_status){
		verbf("Bad host status 0x%x on %s\n",io.host_status,d->name);
		return 0; // FIXME
	}
	/*conf = ntohs(buf[GEN_CONFIG]);
	if(conf == 0x848a || conf == 0x844a
			|| ntohs(buf[83] & 0xc004) == 0x4004){
		// CFA, not ATA-4...? see hdparm. FIXME
	}
	if(!(conf & CONFIG_ATA)){
	}else if(!(conf & CONFIG_ATAPI)){
	}
	*/
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
			diag("Unknown transport type %hu on %s\n",maj,d->name);
			break;
	}
	if((d->blkdev.rotation = buf[NMRR]) == 1){
		d->blkdev.rotation = -1; // non-rotating store
	}else if(d->blkdev.rotation <= 0x401){
		d->blkdev.rotation = 0; // unknown rate
	}
	if(ntohs(buf[CMDS_SUPP_0]) & FEATURE_WRITE_CACHE){
		d->blkdev.wcache = !!(ntohs(buf[CMDS_EN_0]) & FEATURE_WRITE_CACHE);
		verbf("\t%s write-cache: %s\n",d->name,d->blkdev.wcache ? "Enabled" : "Disabled/not present");
	}
	if(ntohs(buf[CMDS_SUPP_2]) & WWN_SUP){
		free(d->wwn);
		if((d->wwn = malloc(17)) == NULL){
			return -1;
		}
		snprintf(d->wwn,17,"%04x%04x%04x%04x",buf[108],buf[109],buf[110],buf[111]);
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
	verbf("\t%s read-write-verify: %s\n",d->name,
			d->blkdev.rwverify == RWVERIFY_UNSUPPORTED ? "Not present" :
			d->blkdev.rwverify == RWVERIFY_SUPPORTED_OFF ? "Disabled" : "Enabled");
	for(n = START_SERIAL ; n < START_SERIAL + LENGTH_SERIAL ; ++n){
		unsigned char c1 = (buf[n] & 0xff00) >> 8u;
		unsigned char c2 = (buf[n] & 0xff);

		if(!isprint(c1) || !isprint(c2)){
			break;
		}
		buf[n] = ntohs(buf[n]);
	}
	if(n == START_SERIAL + LENGTH_SERIAL){
		d->blkdev.serial = malloc(LENGTH_SERIAL * sizeof(*buf) + 1);
		if(d->blkdev.serial){
			// FIXME this copies over whitespace
			memcpy(d->blkdev.serial,buf + START_SERIAL,LENGTH_SERIAL * sizeof(*buf));
			d->blkdev.serial[LENGTH_SERIAL * sizeof(*buf)] = '\0';
		}
	}else{
		verbf("Got bad data on SG_IO for %s\n",d->name);
		//return 0;
	}
	return 0;
}
