#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <iconv.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "apm.h"
#include "ptypes.h"
#include "ptable.h"
#include "growlight.h"

#define DEFAULT_APM_ENTRIES 32
#define LBA_SIZE 512u

static const uint8_t APM_SIG[2] = { 0x4d, 0x50 };

// 512-byte apm partition entry. As many of these as can be fit between the
// Driver Description Block / Driver Description Record (sector 0) and the data
// partitions. They begin at sector 1.
typedef struct __attribute__ ((packed)) apm_entry {
	uint16_t signature;		// APM_SIG (0x504d)
	uint16_t reserved1;
	uint32_t partition_count;
	uint32_t fsector;
	uint32_t sectorcount;
	char pname[32];		// ASCIIZ
	char ptype[32];		// ASCIIZ
	uint32_t data_fsector;
	uint32_t data_sectorcount;
	uint32_t flags;
	uint32_t boot_fsector;
	uint32_t boot_sectorcount;
	uint32_t boot_address;
	uint32_t reserved2;
	uint32_t boot_entry;
	uint32_t reserved3;
	uint32_t boot_cksum;
	uint8_t proctype[16];
	uint8_t reserved4[376];
} apm_entry;

// Initialize an apple partition map having block 0 starting at |map|, on a
// disk having |sectors| |lba|B sectors. Reserve space for |entries| entries.
static int
initialize_apm(void *map, size_t lba, uintmax_t sectors, unsigned entries){
	apm_entry *apm;
	unsigned z;

	assert(map && lba && sectors && entries);
	if(entries + 1 > sectors){
		diag("Can't place %u partition entries in %ju sectors\n", entries, sectors);
		return -1;
	}
	if(lba != sizeof(*apm)){
		diag("Can't work with %zub LBA (need %zu)\n", lba, sizeof(*apm));
		return -1;
	}
	apm = map;
	// Zero out the first sector (Device Descriptor Block)
	memset(apm, 0, lba);
	// Zero out each entry, marking it as a free entry
	for(z = 0 ; z < entries ; ++z){
		apm = (apm_entry *)((char *)apm + lba);
		memset(apm, 0, lba);
		memcpy(&apm->signature, APM_SIG, sizeof(apm->signature));
		apm->partition_count = entries;
		strcpy(apm->ptype, "Apple_Extra");
	}
	// Span the entirety of the disk with the first partition
	apm = (apm_entry *)((char *)map + lba);
	apm->fsector = entries;
	apm->sectorcount = sectors - (entries + 1);
	strcpy(apm->pname, "Extra");
	strcpy(apm->ptype, "Apple_Free");
	// FIXME probably more to do here...
	return 0;
}

// Write out a apm on the device represented by fd, using lbasize-byte LBA. We
// can either zero it all out, or create a new empty apm. Set realdata not
// equal to 0 to perform the latterj
static int
write_apm(int fd, ssize_t lbasize, uintmax_t sectors, unsigned realdata){
	int pgsize = getpagesize();
	apm_entry *mhead;
	size_t mapsize;
	void *map;

	assert(pgsize > 0 && pgsize % lbasize == 0);
	mapsize = lbasize * (DEFAULT_APM_ENTRIES + 1);
	mapsize = ((mapsize / pgsize) + !!(mapsize % pgsize)) * pgsize;
	assert(mapsize % pgsize == 0 && mapsize);
	map = mmap(NULL, mapsize, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if(map == MAP_FAILED){
		diag("Error mapping %zub at %d (%s?)\n", mapsize, fd, strerror(errno));
		return -1;
	}
	mhead = (apm_entry *)map;
	if(!realdata){
		memset(mhead, 0, lbasize * (sectors > DEFAULT_APM_ENTRIES + 1 ?
				DEFAULT_APM_ENTRIES + 1 : sectors));
	}else{
		if(initialize_apm(mhead, lbasize, sectors, DEFAULT_APM_ENTRIES)){
			munmap(map, mapsize);
			return -1;
		}
	}
	if(msync(map, mapsize, MS_SYNC|MS_INVALIDATE)){
		diag("Error syncing %d (%s?)\n", fd, strerror(errno));
		munmap(map, mapsize);
		return -1;
	}
	if(munmap(map, mapsize)){
		diag("Error unmapping %d (%s?)\n", fd, strerror(errno));
		return -1;
	}
	return 0;
}

int new_apm(device *d){
	int fd;

	if(d->layout != LAYOUT_NONE){
		diag("Won't create partition table on non-disk %s\n", d->name);
		return -1;
	}
	if(d->size % LBA_SIZE){
		diag("Won't create apm on (%ju %% %u == %juB) disk %s\n",
			d->size, LBA_SIZE, d->size % LBA_SIZE, d->name);
		return -1;
	}
	if(d->size < LBA_SIZE){
		diag("Won't create apm on empty disk %s\n", d->name);
		return -1;
	}
	if((fd = openat(devfd, d->name, O_RDWR|O_CLOEXEC|O_DIRECT)) < 0){
		diag("Couldn't open %s (%s?)\n", d->name, strerror(errno));
		return -1;
	}
	if(write_apm(fd, LBA_SIZE, d->size / LBA_SIZE, 1)){
		close(fd);
		return -1;
	}
	if(fsync(fd)){
		diag("Warning: error syncing %d for %s (%s?)\n", fd, d->name, strerror(errno));
	}
	if(close(fd)){
		diag("Error closing %d for %s (%s?)\n", fd, d->name, strerror(errno));
		return -1;
	}
	return 0;
}

int zap_apm(device *d){
	int fd;

	if(d->layout != LAYOUT_NONE){
		diag("Won't zap partition table on non-disk %s\n", d->name);
		return -1;
	}
	if(d->blkdev.pttable == NULL || strcmp(d->blkdev.pttable, "apm")){
		diag("No apm on disk %s\n", d->name);
		return -1;
	}
	if(d->size < LBA_SIZE || d->size % LBA_SIZE){
		diag("Won't zap apm on (%ju %% %u == %juB) disk %s\n",
			d->size, LBA_SIZE, d->size % LBA_SIZE, d->name);
		return -1;
	}
	if((fd = openat(devfd, d->name, O_RDWR|O_CLOEXEC|O_DIRECT)) < 0){
		diag("Couldn't open %s (%s?)\n", d->name, strerror(errno));
		return -1;
	}
	if(write_apm(fd, LBA_SIZE, d->size / LBA_SIZE, 0)){
		close(fd);
		return -1;
	}
	if(fsync(fd)){
		diag("Warning: error syncing %d for %s (%s?)\n", fd, d->name, strerror(errno));
	}
	if(close(fd)){
		diag("Error closing %d for %s (%s?)\n", fd, d->name, strerror(errno));
		return -1;
	}
	return 0;
}

// Map the first Apple Partition Map entry, and the disk's first sector
static void *
map_apm(const device *d, size_t *mapsize, int *fd, size_t lbasize){
	const int pgsize = getpagesize();
	void *map;

	if(pgsize < 0){
		diag("Bad pgsize for apm: %d\n", pgsize);
		return MAP_FAILED;
	}
	if((*fd = openat(devfd, d->name, O_RDWR|O_CLOEXEC|O_DIRECT)) < 0){
		diag("Couldn't open %s (%s?)\n", d->name, strerror(errno));
		return MAP_FAILED;
	}
	*mapsize = lbasize * (DEFAULT_APM_ENTRIES + 1);
	*mapsize = ((*mapsize / pgsize) + !!(*mapsize % pgsize)) * pgsize;
	assert(*mapsize % pgsize == 0);
	map = mmap(NULL, *mapsize, PROT_READ|PROT_WRITE, MAP_SHARED, *fd, 0);
	if(map == MAP_FAILED){
		diag("Couldn't map apm header (%s?)\n", strerror(errno));
		close(*fd);
		return map;
	}
	return map;
}

// Pass the return from map_apm(), ie the MBR boot sector
static int
unmap_apm(const device *parent, void *map, size_t mapsize, int fd){
	const int pgsize = getpagesize();

	assert(parent->layout == LAYOUT_NONE);
	if(pgsize < 0){
		diag("Warning: bad pgsize for apm: %d\n", pgsize);
	}
	if(munmap(map, mapsize)){
		int e = errno;

		diag("Error munmapping %s (%s?)\n", parent->name, strerror(errno));
		close(fd);
		errno = e;
		return -1;
	}
	return 0;
}

uintmax_t first_apm(const device *d){
	uintmax_t fsector;
	size_t mapsize;
	apm_entry *apm;
	void *map;
	int fd;

	if((map = map_apm(d, &mapsize, &fd, LBA_SIZE)) == MAP_FAILED){
		return -1;
	}
	if(mapsize < LBA_SIZE * (DEFAULT_APM_ENTRIES + 1)){
		diag("APM size too small (%zu < %u)\n", mapsize, LBA_SIZE * (DEFAULT_APM_ENTRIES + 1));
		unmap_apm(d, map, mapsize, fd);
		return -1;
	}
	apm = (apm_entry *)((char *)map + LBA_SIZE);
	fsector = 1 + apm->partition_count;
	unmap_apm(d, map, mapsize, fd);
	return fsector;
}

uintmax_t last_apm(const device *d){
	return d->logsec && d->size ? d->size / d->logsec - 1 : 0;
}
