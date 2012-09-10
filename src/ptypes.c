#include <stdint.h>
#include <stdlib.h>
#include "ptypes.h"
#include "growlight.h"

const ptype ptypes[] = {
	{
		.code = 0x0005,
		.name = "DOS extended",
		.gpt_guid = {0},
		.mbr_code = 0x5,
	}, {
		.code = 0x0006,
		.name = "FAT16",
		.gpt_guid = {0},
		.mbr_code = 0x06,
	}, {
		.code = 0x0008,
		.name = "AIX",
		.gpt_guid = {0},
		.mbr_code = 0x08,
	}, {
		.code = 0x0009,
		.name = "AIX Bootable",
		.gpt_guid = {0},
		.mbr_code = 0x09,
	}, {
		.code = 0x000b,
		.name = "FAT32",
		.gpt_guid = {0},
		.mbr_code = 0x0b,
	}, {
		.code = 0x000c,
		.name = "FAT32 LBA",
		.gpt_guid = {0},
		.mbr_code = 0x0c,
	}, {
		.code = 0x000e,
		.name = "FAT16 LBA",
		.gpt_guid = {0},
		.mbr_code = 0x0e,
	}, {
		.code = 0x0085,
		.name = "Linux extended",
		.gpt_guid = {0},
		.mbr_code = 0x85,
	}, {
		.code = 0x00a6,
		.name = "OpenBSD",
		.gpt_guid = {0},
		.mbr_code = 0xa6,
	}, {
		.code = 0x00ee,
		.name = "MBR Protective",
		.gpt_guid = {0},
		.mbr_code = 0xee,
	}, {
		.code = 0x00ef,
		.name = "EFI FAT",
		.gpt_guid = {0},
		.mbr_code = 0xef,
	}, {
		.code = 0x0700,
		.name = "Microsoft basic data",
		.gpt_guid = "\xA2\xA0\xD0\xEB\xE5\xB9\x33\x44\x87\xC0\x68\xB6\xB7\x26\x99\xC7",
		.mbr_code = 0,
	}, {
		.code = 0x0c01,
		.name = "Microsoft reserved",
		.gpt_guid = "\x16\xE3\xC9\xE3\x5C\x0B\xB8\x4D\x81\x7D\xF9\x2D\xF0\x02\x15\xAE",
		.mbr_code = 0,
	}, {
		.code = 0x2700,
		.name = "Windows Recovery Environment",
		.gpt_guid = "\xA4\xBB\x94\xDE\xD1\x06\x40\x4D\xA1\x6A\xBF\xD5\x01\x79\xD6\xAC",
		.mbr_code = 0,
	}, {
		.code = 0x4200,
		.name = "Windows Logical Disk Manager data",
		.gpt_guid = "\xA0\x60\x9B\xAF\x31\x14\x62\x4F\xBC\x68\x33\x11\x71\x4A\x69\xAD",
		.mbr_code = 0,
	}, {
		.code = 0x4201,
		.name = "Windows Logical Disk Manager metadata",
		.gpt_guid = "\xAA\xC8\x08\x58\x8F\x7E\xE0\x42\x85\xD2\xE1\xE9\x04\x34\xCF\xB3",
		.mbr_code = 0,
	}, {
		.code = 0x7501,
		.name = "IBM General Parallel File System",
		.gpt_guid = "\x90\xFC\xAF\x37\x7D\xEF\x96\x4E\x91\xC3\x2D\x7A\xE0\x55\xB1\x74",
		.mbr_code = 0,
	}, {
		.code = 0x7f00,
		.name = "ChromeOS kernel",
		.gpt_guid = "\x5D\x2A\x3A\xFE\x32\x4F\xA7\x41\xB7\x25\xAC\xCC\x32\x85\xA3\x09",
		.mbr_code = 0,
	}, {
		.code = 0x7f01,
		.name = "ChromeOS root",
		.gpt_guid = "\x02\xE2\xB8\x3C\x7E\x3B\xDD\x47\x8A\x3C\x7F\xF2\xA1\x3C\xFC\xEC",
		.mbr_code = 0,
	}, {
		.code = 0x7f02,
		.name = "ChromeOS reserved",
		.gpt_guid = "\x3D\x75\x0A\x2E\x48\x9E\xB0\x43\x83\x37\xB1\x51\x92\xCB\x1B\x5E",
		.mbr_code = 0,
	}, {
		.code = 0x8200,
		.name = "Linux swap",
		.gpt_guid = "\x6D\xFD\x57\x06\xAB\xA4\xC4\x43\x84\xE5\x09\x33\xC8\x4B\x4F\x4F",
		.mbr_code = 0x82,
	}, {
		.code = PARTROLE_PRIMARY,
		.name = "Linux filesystem",
		.gpt_guid = "\xAF\x3D\xC6\x0F\x83\x84\x72\x47\x8E\x79\x3D\x69\xD8\x47\x7D\xE4",
		.mbr_code = 0x83,
	}, {
		.code = 0x8301,
		.name = "Linux reserved",
		.gpt_guid = "\x39\x33\xA6\x8D\x07\x00\xC0\x60\xC4\x36\x08\x3A\xC8\x23\x09\x08",
		.mbr_code = 0,
	}, {
		.code = 0x8e00,
		.name = "Linux Logical Volume Manager",
		.gpt_guid = "\x79\xD3\xD6\xE6\x07\xF5\xC2\x44\xA2\x3C\x23\x8F\x2A\x3D\xF9\x28",
		.mbr_code = 0x8e,
	}, /*{
		.code = 0xa500,
		.name = "FreeBSD disklabel",
		.gpt_guid = label

		.mbr_code = 0,
	},*/ {
		.code = 0xa501,
		.name = "FreeBSD boot",
		.gpt_guid = "\x9D\x6B\xBD\x83\x41\x7F\xDC\x11\xBE\x0B\x00\x15\x60\xB8\x4F\x0F",
		.mbr_code = 0xa5,
	}, {
		.code = 0xa502,
		.name = "FreeBSD swap",
		.gpt_guid = "\xB5\x7C\x6E\x51\xCF\x6E\xD6\x11\x8F\xF8\x00\x02\x2D\x09\x71\x2B",
		.mbr_code = 0xa5,
	}, {
		.code = 0xa503,
		.name = "FreeBSD UFS",
		.gpt_guid = "\xB6\x7C\x6E\x51\xCF\x6E\xD6\x11\x8F\xF8\x00\x02\x2D\x09\x71\x2B",
		.mbr_code = 0xa5,
	}, {
		.code = 0xa504,
		.name = "FreeBSD/Linux ZFS",
		.gpt_guid = "\xBA\x7C\x6E\x51\xCF\x6E\xD6\x11\x8F\xF8\x00\x02\x2D\x09\x71\x2B",
		.mbr_code = 0xa5,
		.aggregable = 1,
	}, {
		.code = 0xa505,
		.name = "FreeBSD Vinum/RAID",
		.gpt_guid = "\xB8\x7C\x6E\x51\xCF\x6E\xD6\x11\x8F\xF8\x00\x02\x2D\x09\x71\x2B",
		.mbr_code = 0xa5,
		.aggregable = 1,
	}, {
		.code = PARTROLE_ESP,
		.name = "EFI System Partition (ESP)",
		.gpt_guid = "\x28\x73\x2A\xC1\x1F\xF8\xD2\x11\xBA\x4B\x00\xA0\xC9\x3E\xC9\x3B",
		.mbr_code = 0,
	}, {
		.code = 0xef01,
		.name = "MBR partition scheme",
		.gpt_guid = "\x41\xEE\x4D\x02\xE7\x33\xD3\x11\x9D\x69\x00\x08\xC7\x81\xF3\x9F",
		.mbr_code = 0,
	}, {
		.code = 0xef02,
		.name = "BIOS boot partition",
		.gpt_guid = "\x48\x61\x68\x21\x49\x64\x6F\x6E\x74\x4E\x65\x65\x64\x45\x46\x49",
		.mbr_code = 0,
	}, {
		.code = 0xfd00,
		.name = "Linux MDRAID",
		.gpt_guid = "\x0F\x88\x9D\xA1\xFC\x05\x3B\x4D\xA0\x06\x74\x3F\x0F\x84\x91\x1E",
		.mbr_code = 0xfd,
		.aggregable = 1,
	}, {
		.code = 0xa580,
		.name = "Midnight BSD data",
		.gpt_guid = "\x5a\xe4\xd5\x85\x7c\x23\xe1\x11\xb4\xb3\xe8\x9a\x8f\x7f\xc3\xa7",
		.mbr_code = 0,
	}, {
		.code = 0xa581,
		.name = "Midnight BSD boot",
		.gpt_guid = "\x5e\xe4\xd5\x85\x7c\x23\xe1\x11\xb4\xb3\xe8\x9a\x8f\x7f\xc3\xa7",
		.mbr_code = 0,
	}, {
		.code = 0xa582,
		.name = "Midnight BSD swap",
		.gpt_guid = "\x5b\xe4\xd5\x85\x7c\x23\xe1\x11\xb4\xb3\xe8\x9a\x8f\x7f\xc3\xa7",
		.mbr_code = 0,
	}, {
		.code = 0xa583,
		.name = "Midnight BSD UFS",
		.gpt_guid = "\x8b\xef\x94\x03\x7e\x23\xe1\x11\xb4\xb3\xe8\x9a\x8f\x7f\xc3\xa7",
		.mbr_code = 0,
	}, {
		.code = 0xa584,
		.name = "Midnight BSD ZFS",
		.gpt_guid = "\x5d\xe4\xd5\x85\x7c\x23\xe1\x11\xb4\xb3\xe8\x9a\x8f\x7f\xc3\xa7",
		.mbr_code = 0,
		.aggregable = 1,
	}, {
		.code = 0xa585,
		.name = "Midnight BSD Vinum/RAID",
		.gpt_guid = "\x5c\xe4\xd5\x85\x7c\x23\xe1\x11\xb4\xb3\xe8\x9a\x8f\x7f\xc3\xa7",
		.mbr_code = 0,
		.aggregable = 1,
	}, {
		.code = 0xa800,
		.name = "Apple UFS",
		.gpt_guid = "\x00\x53\x46\x55\x00\x00\xAA\x11\xAA\x11\x00\x30\x65\x43\xEC\xAC",
		.mbr_code = 0xa8,
	}, {
		.code = 0xa901,
		.name = "NetBSD swap",
		.gpt_guid = "\x32\x8D\xF4\x49\x0E\xB1\xDC\x11\xB9\x9B\x00\x19\xD1\x87\x96\x48",
		.mbr_code = 0xa9,
	}, {
		.code = 0xa902,
		.name = "NetBSD FFS",
		.gpt_guid = "\x5A\x8D\xF4\x49\x0E\xB1\xDC\x11\xB9\x9B\x00\x19\xD1\x87\x96\x48",
		.mbr_code = 0xa9,
	}, {
		.code = 0xa903,
		.name = "NetBSD LFS",
		.gpt_guid = "\x82\x8D\xF4\x49\x0E\xB1\xDC\x11\xB9\x9B\x00\x19\xD1\x87\x96\x48",
		.mbr_code = 0xa9,
	}, {
		.code = 0xa904,
		.name = "NetBSD concatenated",
		.gpt_guid = "\xC4\x19\xB5\x2D\x0F\xB1\xDC\x11\xB9\x9B\x00\x19\xD1\x87\x96\x48",
		.mbr_code = 0xa9,
	}, {
		.code = 0xa905,
		.name = "NetBSD encrypted filesystem",
		.gpt_guid = "\xEC\x19\xB5\x2D\x0F\xB1\xDC\x11\xB9\x9B\x00\x19\xD1\x87\x96\x48",
		.mbr_code = 0xa9,
	}, {
		.code = 0xa906,
		.name = "NetBSD RAID",
		.gpt_guid = "\xAA\x8D\xF4\x49\x0E\xB1\xDC\x11\xB9\x9B\x00\x19\xD1\x87\x96\x48",
		.mbr_code = 0xa9,
		.aggregable = 1,
	}, {
		.code = 0xab00,
		.name = "Apple boot",
		.gpt_guid = "\x74\x6F\x6F\x42\x00\x00\xAA\x11\xAA\x11\x00\x30\x65\x43\xEC\xAC",
		.mbr_code = 0xab,
	}, {
		.code = 0xaf00,
		.name = "Apple HFS/HFS+",
		.gpt_guid = "\x00\x53\x46\x48\x00\x00\xAA\x11\xAA\x11\x00\x30\x65\x43\xEC\xAC",
		.mbr_code = 0,
	}, {
		.code = 0xaf01,
		.name = "Apple RAID",
		.gpt_guid = "\x44\x49\x41\x52\x00\x00\xAA\x11\xAA\x11\x00\x30\x65\x43\xEC\xAC",
		.mbr_code = 0,
		.aggregable = 1,
	}, {
		.code = 0xaf02,
		.name = "Apple RAID offline",
		.gpt_guid = "\x44\x49\x41\x52\x4F\x5F\xAA\x11\xAA\x11\x00\x30\x65\x43\xEC\xAC",
		.mbr_code = 0,
		.aggregable = 1,
	}, {
		.code = 0xaf03,
		.name = "Apple label",
		.gpt_guid = "\x65\x62\x61\x4C\x00\x6C\xAA\x11\xAA\x11\x00\x30\x65\x43\xEC\xAC",
		.mbr_code = 0,
	}, {
		.code = 0xaf04,
		.name = "AppleTV recovery",
		.gpt_guid = "\x6F\x63\x65\x52\x65\x76\xAA\x11\xAA\x11\x00\x30\x65\x43\xEC\xAC",
		.mbr_code = 0,
	}, {
		.code = 0xaf05,
		.name = "Apple Core Storage",
		.gpt_guid = "\x72\x6F\x74\x53\x67\x61\xAA\x11\xAA\x11\x00\x30\x65\x43\xEC\xAC",
		.mbr_code = 0,
	}, {
		.code = 0xbe00,
		.name = "Solaris boot",
		.gpt_guid = "\x45\xCB\x82\x6A\xD2\x1D\xB2\x11\x99\xA6\x08\x00\x20\x73\x66\x31",
		.mbr_code = 0xbe,
	}, {
		.code = 0xbf00,
		.name = "Solaris root",
		.gpt_guid = "\x4D\xCF\x85\x6A\xD2\x1D\xB2\x11\x99\xA6\x08\x00\x20\x73\x66\x31",
		.mbr_code = 0xbf,
	}, {
		.code = 0xbf01,
		.name = "Solaris /usr, Mac OS X ZFS",
		.gpt_guid = "\xC3\x8C\x89\x6A\xD2\x1D\xB2\x11\x99\xA6\x08\x00\x20\x73\x66\x31",
		.mbr_code = 0xbf,
		.aggregable = 1,
	}, {
		.code = 0xbf02,
		.name = "Solaris swap",
		.gpt_guid = "\x6F\xC4\x87\x6A\xD2\x1D\xB2\x11\x99\xA6\x08\x00\x20\x73\x66\x31",
		.mbr_code = 0xbf,
	}, {
		.code = 0xbf03,
		.name = "Solaris backup",
		.gpt_guid = "\x2B\x64\x8B\x6A\xD2\x1D\xB2\x11\x99\xA6\x08\x00\x20\x73\x66\x31",
		.mbr_code = 0xbf,
	}, {
		.code = 0xbf04,
		.name = "Solaris /var",
		.gpt_guid = "\xE9\xF2\x8E\x6A\xD2\x1D\xB2\x11\x99\xA6\x08\x00\x20\x73\x66\x31",
		.mbr_code = 0xbf,
	}, {
		.code = 0xbf05,
		.name = "Solaris /home",
		.gpt_guid = "\x39\xBA\x90\x6A\xD2\x1D\xB2\x11\x99\xA6\x08\x00\x20\x73\x66\x31",
		.mbr_code = 0xbf,
	}, {
		.code = 0xc001,
		.name = "HP/UX data",
		.gpt_guid = "\x1E\x4C\x89\x75\xEB\x3A\xD3\x11\xB7\xC1\x7B\x03\xA0\x00\x00\x00",
		.mbr_code = 0,
	}, {
		.code = 0xc002,
		.name = "HP/UX service partition",
		.gpt_guid = "\x28\xE7\xA1\xE2\xE3\x32\xD6\x11\xA6\x82\x7B\x03\xA0\x00\x00\x00",
		.mbr_code = 0,
	}, {
		.code = 0,
		.name = NULL,
		.gpt_guid = { 0 },
		.mbr_code = 0,
	},
};

/*
	printf("MBR types:\n");
	printf(" 0  Empty           1e  Hidd FAT16 LBA  80  Minix <1.4a     \n"
		" 1  FAT12           24  NEC DOS         81  Minix >1.4b     c1  DRDOS/2 FAT12   \n"
		" 2  XENIX root      39  Plan 9          c4  DRDOS/2 smFAT16 \n"
		" 3  XENIX usr       3c  PMagic recovery c6  DRDOS/2 FAT16   \n"
		" 4  Small FAT16     40  Venix 80286     84  OS/2 hidden C:  c7  Syrinx          \n"
		" 41  PPC PReP Boot   da  Non-FS data     \n"
		" 42  SFS             86  NTFS volume set db  CP/M / CTOS     \n"
		" 7  HPFS/NTFS       4d  QNX4.x          87  NTFS volume set de  Dell Utility    \n"
		" 4e  QNX4.x 2nd part 88  Linux plaintext df  BootIt          \n"
		" 4f  QNX4.x 3rd part e1  DOS access      \n"
		" a  OS/2 boot mgr   50  OnTrack DM      93  Amoeba          e3  DOS R/O         \n"
		" 51  OnTrackDM6 Aux1 94  Amoeba BBT      e4  SpeedStor       \n"
		" 52  CP/M            9f  BSD/OS          eb  BeOS fs         \n"
		" 53  OnTrackDM6 Aux3 a0  Thinkpad hib    \n"
		" f  Extended LBA    54  OnTrack DM6     \n"
		" 10 OPUS            55  EZ Drive        f0  Lnx/PA-RISC bt  \n"
		" 11 Hidden FAT12    56  Golden Bow      a7  NeXTSTEP        f1  SpeedStor       \n"
		" 12 Compaq diag     5c  Priam Edisk     f2  DOS secondary   \n"
		" 14 Hidd Sm FAT16   61  SpeedStor       f4  SpeedStor       \n"
		" 16 Hidd FAT16      63  GNU HURD/SysV   "
		" 17 Hidd HPFS/NTFS  64  Netware 286     b7  BSDI fs         fe  LANstep         \n"
		" 18 AST SmartSleep  65  Netware 386     b8  BSDI swap       ff  XENIX BBT       \n"
		" 1b Hidd FAT32      70  DiskSec MltBoot bb  Boot Wizard Hid \n"
		" 1c Hidd FAT32 LBA  75  PC/IX           \n");
	return 0;*/

// Pass in the common code, get the scheme-specific identifier filled in.
// Returns 0 for a valid code, or -1 if there's no ident for the scheme.
int get_gpt_guid(unsigned code,void *guid){
	const ptype *pt;

	for(pt = ptypes ; pt->name ; ++pt){
		if(pt->code == code){
			static const uint8_t zguid[GUIDSIZE] = {0};

			if(memcmp(pt->gpt_guid,zguid,sizeof(zguid)) == 0){
				return -1;
			}
			memcpy(guid,pt->gpt_guid,GUIDSIZE);
			return 0;
		}
	}
	return -1;
}

int get_mbr_code(unsigned code,unsigned *mbr){
	const ptype *pt;

	for(pt = ptypes ; pt->name ; ++pt){
		if(pt->code == code){
			if(pt->mbr_code == 0){
				return -1;
			}
			*mbr = pt->mbr_code;
			return 0;
		}
	}
	return -1;
}

int ptype_supported(const char *pttype,const ptype *pt){
	if(strcmp(pttype,"gpt") == 0){
		static const uint8_t zguid[GUIDSIZE] = {0};

		if(memcmp(pt->gpt_guid,zguid,sizeof(zguid)) == 0){
			return 0;
		}
		return 1;
	}else if(strcmp(pttype,"mbr") == 0){
		if(pt->mbr_code == 0){
			return 0;
		}
		return 1;
	}else if(strcmp(pttype,"mdp") == 0){
		return 0;
	}
	diag("No support for pttype %s\n",pttype);
	return 0;
}

unsigned get_str_code(const char *str){
	unsigned long ul;
	const ptype *pt;
	char *e;

	// libblkid (currently) uses "0x%2x" as a format specifier. by using
	// strtoul, we ought be fairly future-proof.
	if((ul = strtoul(str,&e,0)) > 0xff){
		ul = 0;
	}
	for(pt = ptypes ; pt->name ; ++pt){
		char tstr[GUIDSTRLEN + 1];

		guidstr_be(pt->gpt_guid,tstr);
		if(strcmp(tstr,str) == 0){
			return pt->code;
		}
		if(ul && ul == pt->mbr_code){
			return pt->code;
		}
	}
	return 0;
}
