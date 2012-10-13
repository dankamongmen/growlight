#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <iconv.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include "mbr.h"
#include "msdos.h"
#include "crc32.h"
#include "ptypes.h"
#include "ptable.h"
#include "growlight.h"

#define LBA_SIZE 512u
#define MBR_SIZE (LBA_SIZE - MBR_OFFSET)
#define DISKSIG_LEN 4
#define MSDOS_ENTRIES 4

static const uint8_t MBR_SIG[2] = { 0x55, 0xaa };

// 16-byte msdos partition entry. A msdos table provides space for 4 of these,
// but they can be extended (subpartitioned).
typedef struct __attribute__ ((packed)) msdos_entry {
	uint8_t flags;
	uint8_t hstart,sstart,cstart; // 8-6-10 layout (8)-(2/6)-(8)
	uint8_t ptype;
	uint8_t hlast,slast,clast; // 8-6-10 layout (8)-(2/6)-(8)
	uint32_t lbafirst;	// little-endian
	uint32_t lbasect;	// little-endian
} msdos_entry;

// One LBA block, padded with zeroes at the end. 72 bytes, offset by 440.
typedef struct __attribute__ ((packed)) msdos_header {
	uint8_t bootstrap[MBR_OFFSET];
	unsigned char disksig[DISKSIG_LEN];
	uint16_t reserved;
	msdos_entry table[MSDOS_ENTRIES];
	uint16_t bootsig;
} msdos_header;

static int
initialize_msdos(msdos_header *mh){
	if(RAND_bytes(mh->disksig,sizeof(mh->disksig)) != 1){
		diag("%s",ERR_error_string(ERR_get_error(),NULL));
		return -1;
	}
	memset(&mh->reserved,0,sizeof(mh->reserved));
	memset(&mh->table,0,sizeof(mh->table));
	memcpy(&mh->bootsig,MBR_SIG,sizeof(mh->bootsig));
	return 0;
}

// Write out a msdos partition map on the device represented by fd, using
// lbasize-byte LBA. We will write to the first sector only. We can either zero
// it all out, or create a new empty msdos. Set realdata not equal to 0 to
// perform the latter.
static int
write_msdos(int fd,ssize_t lbasize,unsigned realdata){
	int pgsize = getpagesize();
	msdos_header *mhead;
	size_t mapsize;
	void *map;

	assert(pgsize > 0 && pgsize % lbasize == 0);
	mapsize = lbasize;
	mapsize = ((mapsize / pgsize) + !!(mapsize % pgsize)) * pgsize;
	assert(mapsize % pgsize == 0 && mapsize);
	map = mmap(NULL,mapsize,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
	if(map == MAP_FAILED){
		diag("Error mapping %zub at %d (%s?)\n",mapsize,fd,strerror(errno));
		return -1;
	}
	mhead = (msdos_header *)map;
	if(!realdata){
		memset(mhead,0,MBR_OFFSET + MBR_SIZE);
	}else{
		if(initialize_msdos(mhead)){
			munmap(map,mapsize);
			return -1;
		}
	}
	if(msync(map,mapsize,MS_SYNC|MS_INVALIDATE)){
		diag("Error syncing %d (%s?)\n",fd,strerror(errno));
		munmap(map,mapsize);
		return -1;
	}
	if(munmap(map,mapsize)){
		diag("Error unmapping %d (%s?)\n",fd,strerror(errno));
		return -1;
	}
	return 0;
}

int new_msdos(device *d){
	int fd;

	if(d->layout != LAYOUT_NONE){
		diag("Won't create partition table on non-disk %s\n",d->name);
		return -1;
	}
	if(d->size % LBA_SIZE){
		diag("Won't create msdos on (%ju %% %u == %juB) disk %s\n",
			d->size,LBA_SIZE,d->size % LBA_SIZE,d->name);
		return -1;
	}
	if(d->size < LBA_SIZE){
		diag("Won't create msdos on empty disk %s\n",d->name);
		return -1;
	}
	if((fd = openat(devfd,d->name,O_RDWR|O_CLOEXEC|O_DIRECT)) < 0){
		diag("Couldn't open %s (%s?)\n",d->name,strerror(errno));
		return -1;
	}
	if(write_msdos(fd,LBA_SIZE,1)){
		diag("Couldn't write msdos on %s (%s?)\n",d->name,strerror(errno));
		close(fd);
		return -1;
	}
	if(fsync(fd)){
		diag("Warning: error syncing %d for %s (%s?)\n",fd,d->name,strerror(errno));
	}
	if(close(fd)){
		diag("Error closing %d for %s (%s?)\n",fd,d->name,strerror(errno));
		return -1;
	}
	return 0;
}

int zap_msdos(device *d){
	if(d->layout != LAYOUT_NONE){
		diag("Won't zap partition table on non-disk %s\n",d->name);
		return -1;
	}
	if(d->blkdev.pttable == NULL || strcmp(d->blkdev.pttable,"dos")){
		diag("No msdos on disk %s\n",d->name);
		return -1;
	}
	return wipe_dos_ptable(d);
}

// Map the primary msdos header, its table, and the MBR boot sector.
static void *
map_msdos(device *d,size_t *mapsize,int *fd,size_t lbasize){
	const int pgsize = getpagesize();
	void *map;

	if(pgsize < 0){
		diag("Bad pgsize for msdos: %d\n",pgsize);
		return MAP_FAILED;
	}
	if((*fd = openat(devfd,d->name,O_RDWR|O_CLOEXEC|O_DIRECT)) < 0){
		diag("Couldn't open %s (%s?)\n",d->name,strerror(errno));
		return MAP_FAILED;
	}
	*mapsize = lbasize;
	*mapsize = ((*mapsize / pgsize) + !!(*mapsize % pgsize)) * pgsize;
	assert(*mapsize % pgsize == 0);
	map = mmap(NULL,*mapsize,PROT_READ|PROT_WRITE,MAP_SHARED,*fd,0);
	if(map == MAP_FAILED){
		diag("Couldn't map msdos header (%s?)\n",strerror(errno));
		close(*fd);
		return map;
	}
	return map;
}

// Pass the return from map_msdos(), ie the MBR boot sector
static int
unmap_msdos(const device *parent,void *map,size_t mapsize,int fd){
	const int pgsize = getpagesize();

	assert(parent->layout == LAYOUT_NONE);
	if(pgsize < 0){
		diag("Warning: bad pgsize for msdos: %d\n",pgsize);
	}
	if(munmap(map,mapsize)){
		int e = errno;

		diag("Error munmapping %s (%s?)\n",parent->name,strerror(errno));
		close(fd);
		errno = e;
		return -1;
	}
	return 0;
}

int add_msdos(device *d,const wchar_t *name,uintmax_t fsec,uintmax_t lsec,unsigned long long code){
	static unsigned char zmpe[16] = "";
	const size_t lbasize = LBA_SIZE;
	char cname[BUFSIZ];
	unsigned z,partno;
	msdos_entry *mpe;
	unsigned mbrcode;
	size_t mapsize;
	uint64_t lbas;
	void *map;
	int fd,r;

	if(name){
		diag("msdos partitions don't support names\n");
		return -1;
	}
	if((lsec - fsec) * d->logsec > 2ull * 1000ull * 1000ull * 1000ull * 1000ull){
		diag("msdos partitions may not exceed 2TB\n");
		return -1;
	}
	if(!d){
		diag("Passed a NULL device\n");
		return -1;
	}
	if(get_mbr_code(code,&mbrcode)){
		diag("Illegal code for DOS/BIOS/MBR: %llu\n",code);
		return -1;
	}
	if(d->layout != LAYOUT_NONE){
		diag("Won't add partition to non-disk %s\n",d->name);
		return -1;
	}
	if(d->blkdev.pttable == NULL || strcmp(d->blkdev.pttable,"dos")){
		diag("No msdos on disk %s\n",d->name);
		return -1;
	}
	if(d->size % lbasize){
		diag("Disk size is not a multiple of LBA size, aborting\n");
		return -1;
	}
	lbas = d->size / lbasize;
	// Align it properly
	if(fsec % (d->physsec / d->logsec)){
		fsec += (d->physsec / d->logsec) - (fsec % (d->physsec / d->logsec));
		assert(fsec % (d->physsec / d->logsec) == 0);
	}
	if(lsec < fsec || lsec > last_usable_sector(d) || fsec < first_usable_sector(d)){
		diag("Bad sector spec (%ju:%ju) on %ju disk\n",fsec,lsec,lbas);
		return -1;
	}
	if((map = map_msdos(d,&mapsize,&fd,LBA_SIZE)) == MAP_FAILED){
		return -1;
	}
	mpe = (msdos_entry *)((char *)map + MBR_OFFSET + 6);
	// Determine the next available partition number, and verify that no
	// existing partitions overlap with this one.
	partno = MSDOS_ENTRIES;
	for(z = 0 ; z < MSDOS_ENTRIES ; ++z){
		// if there're any non-zero bits, assume it's being used.
		if(memcmp(&mpe[z],zmpe,sizeof(zmpe))){
			if((mpe[z].lbafirst >= fsec && mpe[z].lbafirst <= lsec) ||
					(mpe[z].lbafirst + mpe[z].lbasect - 1 <= lsec && mpe[z].lbafirst + mpe[z].lbasect - 1 >= fsec)){
				diag("Partition overlap (%ju:%ju) ([%u]%u:%u)\n",fsec,lsec,
						z,mpe[z].lbafirst,mpe[z].lbafirst + mpe[z].lbasect - 1);
			}
			continue;
		}
		if(partno == MSDOS_ENTRIES){
			partno = z;
		}
	}
	if((z = partno) == MSDOS_ENTRIES){
		diag("no entry for a new partition in %s\n",d->name);
		munmap(map,mapsize);
		close(fd);
		return -1;
	}
	diag("First sector: %ju last sector: %ju count: %ju size: %ju\n",
			(uintmax_t)fsec,
			(uintmax_t)lsec,
			(uintmax_t)(lsec - fsec),
			(uintmax_t)((lsec - fsec) * d->logsec));
	memset(&mpe[z],0,sizeof(*mpe));
	mpe[z].ptype = mbrcode;
	mpe[z].lbafirst = fsec;
	mpe[z].lbasect = lsec - fsec + 1;
	if(unmap_msdos(d,map,mapsize,fd)){
		close(fd);
		return -1;
	}
	snprintf(cname,sizeof(cname) - 1,"%ls",name);
	if(fsync(fd)){
		diag("Couldn't sync %d for %s\n",fd,d->name);
	}
	r = blkpg_add_partition(fd,fsec * LBA_SIZE,
			(lsec - fsec + 1) * LBA_SIZE,z + 1,cname);
	if(close(fd)){
		int e = errno;

		diag("Error closing %s (%s?)\n",d->name,strerror(errno));
		errno = e;
		return -1;
	}
	return r;
}

int flag_msdos(device *d,uint64_t flag,unsigned status){
	msdos_entry *mpe;
	size_t mapsize;
	unsigned g;
	void *map;
	int fd;

	if(flag != 0x80){
		diag("Invalid flag for BIOS/MBR: 0x%016jx\n",(uintmax_t)flag);
		return -1;
	}
	assert(d->layout == LAYOUT_PARTITION);
	if(d->partdev.ptype != PARTROLE_PRIMARY || d->partdev.ptstate.logical || d->partdev.ptstate.extended){
		diag("Flags are only set on primary partitions\n");
		return -1;
	}
	if(d->partdev.pnumber == 0 || d->partdev.pnumber > MSDOS_ENTRIES){
		diag("No support for partnumber %u\n",d->partdev.pnumber);
		return -1;
	}
	g = d->partdev.pnumber - 1;
	if((map = map_msdos(d->partdev.parent,&mapsize,&fd,LBA_SIZE)) == MAP_FAILED){
		return -1;
	}
	mpe = (msdos_entry *)((char *)map + MBR_OFFSET + 6);
	if(status){
		mpe[g].flags |= flag;
	}else{
		mpe[g].flags &= ~flag;
	}
	if(unmap_msdos(d->partdev.parent,map,mapsize,fd)){
		close(fd);
		return -1;
	}
	if(close(fd)){
		diag("Error closing %s (%s?)\n",d->name,strerror(errno));
		return -1;
	}
	return 0;
}

int code_msdos(device *d,unsigned long long code){
	msdos_entry *mpe;
	unsigned mbrcode;
	size_t mapsize;
	unsigned g;
	void *map;
	int fd;

	if(get_mbr_code(code,&mbrcode)){
		diag("Illegal code for DOS/BIOS/MBR: %llu\n",code);
		return -1;
	}
	assert(d->layout == LAYOUT_PARTITION);
	if(d->partdev.pnumber == 0 || d->partdev.pnumber > MSDOS_ENTRIES){
		diag("No support for partnumber %u\n",d->partdev.pnumber);
		return -1;
	}
	g = d->partdev.pnumber - 1;
	if((map = map_msdos(d->partdev.parent,&mapsize,&fd,LBA_SIZE)) == MAP_FAILED){
		return -1;
	}
	mpe = (msdos_entry *)((char *)map + MBR_OFFSET + 6);
	if(mpe[g].lbafirst == 0 || mpe[g].lbasect == 0){
		diag("Not a valid msdos partition: %s\n",d->name);
		unmap_msdos(d->partdev.parent,map,mapsize,fd);
		close(fd);
		return -1;
	}
	mpe[g].ptype = mbrcode;
	if(unmap_msdos(d->partdev.parent,map,mapsize,fd)){
		close(fd);
		return -1;
	}
	if(close(fd)){
		diag("Error closing %s (%s?)\n",d->name,strerror(errno));
		return -1;
	}
	return 0;
}

int del_msdos(const device *p){
	msdos_entry *mpe;
	size_t mapsize;
	unsigned g;
	void *map;
	int fd,r;

	assert(p->layout == LAYOUT_PARTITION);
	if(p->partdev.pnumber == 0 || p->partdev.pnumber > MSDOS_ENTRIES){
		diag("No support for partnumber %u\n",p->partdev.pnumber);
		return -1;
	}
	g = p->partdev.pnumber - 1;
	if((map = map_msdos(p->partdev.parent,&mapsize,&fd,LBA_SIZE)) == MAP_FAILED){
		return -1;
	}
	mpe = (msdos_entry *)((char *)map + MBR_OFFSET + 6);
	memset(&mpe[g],0,sizeof(*mpe));
	if(unmap_msdos(p->partdev.parent,map,mapsize,fd)){
		close(fd);
		return -1;
	}
	if(fsync(fd)){
		diag("Couldn't sync %d for %s\n",fd,p->name);
	}
	r = blkpg_del_partition(fd,p->partdev.fsector * LBA_SIZE,
				p->size,p->partdev.pnumber,
				p->partdev.parent->name);
	if(close(fd)){
		diag("Couldn't close %s (%s?)\n",p->partdev.parent->name,strerror(errno));
		return -1;
	}
	return r;
}

uintmax_t first_msdos(const device *d __attribute__ ((unused))){
	return 1;
}

uintmax_t last_msdos(const device *d){
	return d->logsec && d->size ? d->size / d->logsec - 1 : 0;
}
