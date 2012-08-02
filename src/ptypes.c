#include <stdint.h>
#include "ptypes.h"
#include "growlight.h"

const ptype ptypes[] = {
	{
		.code = 0x0700,
		.name = "Microsoft basic data",
		.gpt_guid = "\xEB\xD0\xA0\xA2\xB9\xE5\x44\x33\x87\xC0\x68\xB6\xB7\x26\x99\xC7",
		.mbr_code = 0,
	}, {
		.code = 0x0c01,
		.name = "Microsoft reserved",
		.gpt_guid = "\xE3\xC9\xE3\x16\x0B\x5C\x4D\xB8\x81\x7D\xF9\x2D\xF0\x02\x15\xAE",
		.mbr_code = 0,
	}, {
		.code = 0x2700,
		.name = "Windows Recovery Environment",
		.gpt_guid = "\xDE\x94\xBB\xA4\x06\xD1\x4D\x40\xA1\x6A\xBF\xD5\x01\x79\xD6\xAC",
		.mbr_code = 0,
	}, {
		.code = 0x4200,
		.name = "Windows Logical Disk Manager data",
		.gpt_guid = "\xAF\x9B\x60\xA0\x14\x31\x4F\x62\xBC\x68\x33\x11\x71\x4A\x69\xAD",
		.mbr_code = 0,
	}, {
		.code = 0x4201,
		.name = "Windows Logical Disk Manager metadata",
		.gpt_guid = "\x58\x08\xC8\xAA\x7E\x8F\x42\xE0\x85\xD2\xE1\xE9\x04\x34\xCF\xB3",
		.mbr_code = 0,
	}, {
		.code = 0x7501,
		.name = "IBM General Parallel File System",
		.gpt_guid = "\x37\xAF\xFC\x90\xEF\x7D\x4E\x96\x91\xC3\x2D\x7A\xE0\x55\xB1\x74",
		.mbr_code = 0,
	}, {
		.code = 0x7f00,
		.name = "ChromeOS kernel",
		.gpt_guid = "\xFE\x3A\x2A\x5D\x4F\x32\x41\xA7\xB7\x25\xAC\xCC\x32\x85\xA3\x09",
		.mbr_code = 0,
	}, {
		.code = 0x7f01,
		.name = "ChromeOS root",
		.gpt_guid = "\x3C\xB8\xE2\x02\x3B\x7E\x47\xDD\x8A\x3C\x7F\xF2\xA1\x3C\xFC\xEC",
		.mbr_code = 0,
	}, {
		.code = 0x7f02,
		.name = "ChromeOS reserved",
		.gpt_guid = "\x2E\x0A\x75\x3D\x9E\x48\x43\xB0\x83\x37\xB1\x51\x92\xCB\x1B\x5E",
		.mbr_code = 0,
	}, {
		.code = 0x8200,
		.name = "Linux swap",
		.gpt_guid = "\x06\x57\xFD\x6D\xA4\xAB\x43\xC4\x84\xE5\x09\x33\xC8\x4B\x4F\x4F",
		.mbr_code = 0,
	}, {
		.code = 0x8300,
		.name = "Linux filesystem",
		.gpt_guid = "\x0F\xC6\x3D\xAF\x84\x83\x47\x72\x8E\x79\x3D\x69\xD8\x47\x7D\xE4",
		.mbr_code = 0,
	}, {
		.code = 0x8301,
		.name = "Linux reserved",
		.gpt_guid = "\x8D\xA6\x33\x39\x00\x07\x60\xC0\xC4\x36\x08\x3A\xC8\x23\x09\x08",
		.mbr_code = 0,
	}, {
		.code = 0x8e00,
		.name = "Linux Logical Volume Manager",
		.gpt_guid = "\xE6\xD6\xD3\x79\xF5\x07\x44\xC2\xA2\x3C\x23\x8F\x2A\x3D\xF9\x28",
		.mbr_code = 0,
	}, /*{
		.code = 0xa500,
		.name = "FreeBSD disklabel",
		.gpt_guid = label

		.mbr_code = 0,
	},*/ {
		.code = 0xa501,
		.name = "FreeBSD boot",
		.gpt_guid = "\x83\xBD\x6B\x9D\x7F\x41\x11\xDC\xBE\x0B\x00\x15\x60\xB8\x4F\x0F",
		.mbr_code = 0,
	}, {
		.code = 0xa502,
		.name = "FreeBSD swap",
		.gpt_guid = "\x51\x6E\x7C\xB5\x6E\xCF\x11\xD6\x8F\xF8\x00\x02\x2D\x09\x71\x2B",
		.mbr_code = 0,
	}, {
		.code = 0xa503,
		.name = "FreeBSD UFS",
		.gpt_guid = "\x51\x6E\x7C\xB6\x6E\xCF\x11\xD6\x8F\xF8\x00\x02\x2D\x09\x71\x2B",
		.mbr_code = 0,
	}, {
		.code = 0xa504,
		.name = "FreeBSD ZFS",
		.gpt_guid = "\x51\x6E\x7C\xBA\x6E\xCF\x11\xD6\x8F\xF8\x00\x02\x2D\x09\x71\x2B",
		.mbr_code = 0,
	}, {
		.code = 0xa505,
		.name = "FreeBSD Vinum/RAID",
		.gpt_guid = "\x51\x6E\x7C\xB8\x6E\xCF\x11\xD6\x8F\xF8\x00\x02\x2D\x09\x71\x2B",
		.mbr_code = 0,
	}, {
		.code = 0xef00,
		.name = "EFI System Partition",
		.gpt_guid = "\xC1\x2A\x73\x28\xF8\x1F\x11\xD2\xBA\x4B\x00\xA0\xC9\x3E\xC9\x3B",
		.mbr_code = 0,
	}, {
		.code = 0xef01,
		.name = "MBR partition scheme",
		.gpt_guid = "\x02\x4D\xEE\x41\x33\xE7\x11\xD3\x9D\x69\x00\x08\xC7\x81\xF3\x9F",
		.mbr_code = 0,
	}, {
		.code = 0xef02,
		.name = "BIOS boot partition",
		.gpt_guid = "\x21\x68\x61\x48\x64\x49\x6E\x6F\x74\x4E\x65\x65\x64\x45\x46\x49",
		.mbr_code = 0,
	}, {
		.code = 0xfd00,
		.name = "Linux MDRAID",
		.gpt_guid = "\xA1\x9D\x88\x0F\x05\xFC\x4D\x3B\xA0\x06\x74\x3F\x0F\x84\x91\x1E",
		.mbr_code = 0,
	}, {
		.code = 0xa580,
		.name = "Midnight BSD data",
		.gpt_guid = "\x85\xd5\xe4\x5a\x23\x7c\x11\xe1\xb4\xb3\xe8\x9a\x8f\x7f\xc3\xa7",
		.mbr_code = 0,
	}, {
		.code = 0xa581,
		.name = "Midnight BSD boot",
		.gpt_guid = "\x85\xd5\xe4\x5e\x23\x7c\x11\xe1\xb4\xb3\xe8\x9a\x8f\x7f\xc3\xa7",
		.mbr_code = 0,
	}, {
		.code = 0xa582,
		.name = "Midnight BSD swap",
		.gpt_guid = "\x85\xd5\xe4\x5b\x23\x7c\x11\xe1\xb4\xb3\xe8\x9a\x8f\x7f\xc3\xa7",
		.mbr_code = 0,
	}, {
		.code = 0xa583,
		.name = "Midnight BSD UFS",
		.gpt_guid = "\x03\x94\xef\x8b\x23\x7e\x11\xe1\xb4\xb3\xe8\x9a\x8f\x7f\xc3\xa7",
		.mbr_code = 0,
	}, {
		.code = 0xa584,
		.name = "Midnight BSD ZFS",
		.gpt_guid = "\x85\xd5\xe4\x5d\x23\x7c\x11\xe1\xb4\xb3\xe8\x9a\x8f\x7f\xc3\xa7",
		.mbr_code = 0,
	}, {
		.code = 0xa585,
		.name = "Midnight BSD Vinum/RAID",
		.gpt_guid = "\x85\xd5\xe4\x5c\x23\x7c\x11\xe1\xb4\xb3\xe8\x9a\x8f\x7f\xc3\xa7",
		.mbr_code = 0,
	}, {
		.code = 0xa800,
		.name = "Apple UFS",
		.gpt_guid = "\x55\x46\x53\x00\x00\x00\x11\xAA\xAA\x11\x00\x30\x65\x43\xEC\xAC",
		.mbr_code = 0,
	},
};

/*
	printf("GPT types:\n");
		" a901 NetBSD swap         \n"
		" a902 NetBSD FFS            a903 NetBSD LFS            a904 NetBSD concatenated \n"
		" a905 NetBSD encrypted      a906 NetBSD RAID           ab00 Apple boot          \n"
		" af00 Apple HFS/HFS+        af01 Apple RAID            af02 Apple RAID offline  \n"
		" af03 Apple label           af04 AppleTV recovery      af05 Apple Core Storage  \n"
		" be00 Solaris boot          bf00 Solaris root          bf01 Solaris /usr & Mac Z\n"
		" bf02 Solaris swap          bf03 Solaris backup        bf04 Solaris /var        \n"
		" bf05 Solaris /home         bf06 Solaris alternate se  bf07 Solaris Reserved 1  \n"
		" bf08 Solaris Reserved 2    bf09 Solaris Reserved 3    bf0a Solaris Reserved 4  \n"
		" bf0b Solaris Reserved 5    c001 HP-UX data            c002 HP-UX service       \n"
	printf("MBR types:\n");
	printf(" 0  Empty           1e  Hidd FAT16 LBA  80  Minix <1.4a     bf  Solaris         \n"
		" 1  FAT12           24  NEC DOS         81  Minix >1.4b     c1  DRDOS/2 FAT12   \n"
		" 2  XENIX root      39  Plan 9          82  Linux swap      c4  DRDOS/2 smFAT16 \n"
		" 3  XENIX usr       3c  PMagic recovery 83  Linux           c6  DRDOS/2 FAT16   \n"
		" 4  Small FAT16     40  Venix 80286     84  OS/2 hidden C:  c7  Syrinx          \n"
		" 5  Extended        41  PPC PReP Boot   85  Linux extended  da  Non-FS data     \n"
		" 6  FAT16           42  SFS             86  NTFS volume set db  CP/M / CTOS     \n"
		" 7  HPFS/NTFS       4d  QNX4.x          87  NTFS volume set de  Dell Utility    \n"
		" 8  AIX             4e  QNX4.x 2nd part 88  Linux plaintext df  BootIt          \n"
		" 9  AIX bootable    4f  QNX4.x 3rd part 8e  Linux LVM       e1  DOS access      \n"
		" a  OS/2 boot mgr   50  OnTrack DM      93  Amoeba          e3  DOS R/O         \n"
		" b  FAT32           51  OnTrackDM6 Aux1 94  Amoeba BBT      e4  SpeedStor       \n"
		" c  FAT32 LBA       52  CP/M            9f  BSD/OS          eb  BeOS fs         \n"
		" e  FAT16 LBA       53  OnTrackDM6 Aux3 a0  Thinkpad hib    ee  GPT             \n"
		" f  Extended LBA    54  OnTrack DM6     a5  FreeBSD         ef  EFI FAT         \n"
		" 10 OPUS            55  EZ Drive        a6  OpenBSD         f0  Lnx/PA-RISC bt  \n"
		" 11 Hidden FAT12    56  Golden Bow      a7  NeXTSTEP        f1  SpeedStor       \n"
		" 12 Compaq diag     5c  Priam Edisk     a8  Darwin UFS      f2  DOS secondary   \n"
		" 14 Hidd Sm FAT16   61  SpeedStor       a9  NetBSD          f4  SpeedStor       \n"
		" 16 Hidd FAT16      63  GNU HURD/SysV   ab  Darwin boot     fd  Lnx RAID auto   \n"
		" 17 Hidd HPFS/NTFS  64  Netware 286     b7  BSDI fs         fe  LANstep         \n"
		" 18 AST SmartSleep  65  Netware 386     b8  BSDI swap       ff  XENIX BBT       \n"
		" 1b Hidd FAT32      70  DiskSec MltBoot bb  Boot Wizard Hid \n"
		" 1c Hidd FAT32 LBA  75  PC/IX           be  Solaris boot    \n");
	return 0;*/
