#include <stdint.h>
#include "ptypes.h"
#include "growlight.h"

const ptype ptypes[] = {
	{
		.code = 0x0700,
		.name = "Microsoft basic data",
		.gpt_guid = EBD0A0A2-B9E5-4433-87C0-68B6B72699C7,
		.mbr_code = 0,
	}, {
		.code = 0x0c01,
		.name = "Microsoft reserved",
		.gpt_guid = E3C9E316-0B5C-4DB8-817D-F92DF00215AE,
		.mbr_code = 0,
	}, {
		.code = 0x2700,
		.name = "Windows Recovery Environment",
		.gpt_guid = DE94BBA4-06D1-4D40-A16A-BFD50179D6AC,
		.mbr_code = 0,
	}, {
		.code = 0x4200,
		.name = "Windows Logical Disk Manager data",
		.gpt_guid = AF9B60A0-1431-4F62-BC68-3311714A69AD,
		.mbr_code = 0,
	}, {
		.code = 0x4201,
		.name = "Windows Logical Disk Manager metadata",
		.gpt_guid = 5808C8AA-7E8F-42E0-85D2-E1E90434CFB3,
		.mbr_code = 0,
	}, {
		.code = 0x7501,
		.name = "IBM General Parallel File System",
		.gpt_guid = 37AFFC90-EF7D-4E96-91C3-2D7AE055B174,
		.mbr_code = 0,
	}, {
		.code = 0x7f00,
		.name = "ChromeOS kernel",
		.gpt_guid = FE3A2A5D-4F32-41A7-B725-ACCC3285A309,
		.mbr_code = 0,
	}, {
		.code = 0x7f01,
		.name = "ChromeOS root",
		.gpt_guid = 3CB8E202-3B7E-47DD-8A3C-7FF2A13CFCEC,
		.mbr_code = 0,
	}, {
		.code = 0x7f02,
		.name = "ChromeOS reserved",
		.gpt_guid = 2E0A753D-9E48-43B0-8337-B15192CB1B5E,
		.mbr_code = 0,
	}, {
		.code = 0x8200,
		.name = "Linux swap",
		.gpt_guid = 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F,
		.mbr_code = 0,
	}, {
		.code = 0x8300,
		.name = "Linux filesystem",
		.gpt_guid = 0FC63DAF-8483-4772-8E79-3D69D8477DE4,
		.mbr_code = 0,
	}, {
		.code = 0x8301,
		.name = "Linux reserved",
		.gpt_guid = 8DA63339-0007-60C0-C436-083AC8230908,
		.mbr_code = 0,
	}, {
		.code = 0x8e00,
		.name = "Linux Logical Volume Manager",
		.gpt_guid = E6D6D379-F507-44C2-A23C-238F2A3DF928,
		.mbr_code = 0,
	}, /*{
		.code = 0xa500,
		.name = "FreeBSD disklabel",
		.gpt_guid = label

		.mbr_code = 0,
	},*/ {
		.code = 0xa501,
		.name = "FreeBSD boot",
		.gpt_guid = 83BD6B9D-7F41-11DC-BE0B-001560B84F0F,
		.mbr_code = 0,
	}, {
		.code = 0xa502,
		.name = "FreeBSD swap",
		.gpt_guid = 516E7CB5-6ECF-11D6-8FF8-00022D09712B,
		.mbr_code = 0,
	}, {
		.code = 0xa503,
		.name = "FreeBSD UFS",
		.gpt_guid = 516E7CB6-6ECF-11D6-8FF8-00022D09712B,
		.mbr_code = 0,
	}, {
		.code = 0xa504,
		.name = "FreeBSD ZFS",
		.gpt_guid = 516E7CBA-6ECF-11D6-8FF8-00022D09712B,
		.mbr_code = 0,
	}, {
		.code = 0xa505,
		.name = "FreeBSD Vinum/RAID",
		.gpt_guid = 516E7CB8-6ECF-11D6-8FF8-00022D09712B,
		.mbr_code = 0,
	}, {
		.code = 0xef00,
		.name = "EFI System Partition",
		.gpt_guid = C12A7328-F81F-11D2-BA4B-00A0C93EC93B,
		.mbr_code = 0,
	}, {
		.code = 0xef01,
		.name = "MBR partition scheme",
		.gpt_guid = 024DEE41-33E7-11D3-9D69-0008C781F39F,
		.mbr_code = 0,
	}, {
		.code = 0xef02,
		.name = "BIOS boot partition",
		.gpt_guid = 21686148-6449-6E6F-744E-656564454649,
		.mbr_code = 0,
	}, {
		.code = 0xfd00,
		.name = "Linux MDRAID",
		.gpt_guid = A19D880F-05FC-4D3B-A006-743F0F84911E,
		.mbr_code = 0,
	},
};

/*
	printf("GPT types:\n");
		" a505 FreeBSD Vinum/RAID    a580 Midnight BSD data     a581 Midnight BSD boot   \n"
		" a582 Midnight BSD swap     a583 Midnight BSD UFS      a584 Midnight BSD ZFS    \n"
		" a585 Midnight BSD Vinum    a800 Apple UFS             a901 NetBSD swap         \n"
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
