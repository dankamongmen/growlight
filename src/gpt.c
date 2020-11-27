// copyright 2012–2020 nick black
#include <assert.h>
#include <zlib.h>
#include <fcntl.h>
#include <errno.h>
#include <iconv.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/random.h>

#include "gpt.h"
#include "ptypes.h"
#include "ptable.h"
#include "growlight.h"

#define LBA_SIZE 512u
#define MBR_SIZE (LBA_SIZE - MBR_OFFSET)

static const unsigned char GPT_PROTECTIVE_MBR[LBA_SIZE - MBR_OFFSET] =
 "\x00\x00\x00\x00\x00\x00"  // 6 bytes of zeros
 "\x80"                      // bootable (violation of GPT spec, but some
                             //  BIOS/MBR *and* UEFI won't boot otherwise)
 "\x00\x00\x00"              // CHS of first absolute sector
 "\xee"                      // Protective partition type
 "\xff\xff\xff"              // CHS of last absolute sector
 "\x01\x00\x00\x00"          // LBA of first absolute sector
 "\xff\xff\xff\xff"          // Sectors in partition
 "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
 "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
 "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
 "\x55\xaa";                 // MBR signature

static const unsigned char gpt_signature[8] =
 "\x45\x46\x49\x20\x50\x41\x52\x54";

#define MINIMUM_GPT_ENTRIES 128
#define CRCPOLY 0x04C11DB7

int update_crc(gpt_header *head, const gpt_entry *gpes){
  size_t hs = head->headsize; // FIXME little-endian; swap on BE machines
  // partition entry size must be a positive multiple of 128 (usually 128)
  if(head->partsize == 0 || head->partsize % 128){
    return -1;
  }
  if(head->partcount < MINIMUM_GPT_ENTRIES){
    return -1;
  }
  head->partcrc = crc32(CRCPOLY, (const void*)gpes, head->partcount * head->partsize);
  head->crc = 0;
  head->crc = crc32(CRCPOLY, (void*)head, hs);
  return 0;
}

static int
update_backup(int fd, const gpt_header *ghead, unsigned gptlbas, uint64_t lbas,
              unsigned lbasize, unsigned pgsize, int realdata){
  // Cannot look to ghead->backuplba, because we might be zeroing things
  // out, and have already lost it in the primary.
  const uint64_t backuplba = lbas - 1;
  const uint64_t absdevoff = (backuplba - (gptlbas - 1)) * lbasize;
  const size_t mapoff = absdevoff % pgsize;
  gpt_header *gh;
  size_t mapsize;
  void *map;

  mapsize = lbasize * gptlbas + mapoff;
  mapsize = pgsize * (mapsize / pgsize + !!mapoff);
  if((map = mmap(NULL, mapsize, PROT_READ|PROT_WRITE, MAP_SHARED, fd,
        absdevoff - mapoff)) == MAP_FAILED){
    diag("Error mapping %zub at %d (%s?)\n", mapsize, fd, strerror(errno));
    return -1;
  }
  verbf("Mapped %zub at %p:%zu devoff %ju\n", mapsize, map, mapoff, (uintmax_t)(absdevoff - mapoff));
  if(realdata){
    // Copy the partition table entries -- all but the first of the
    // primary header's sectors to all but the last of the backup
    // header's sectors.
    memcpy((char *)map + mapoff, (char *)ghead + lbasize,
      (gptlbas - 1) * lbasize);
    // Copy the header, always a single LBA sector
    gh = (gpt_header *)((char *)map + lbasize * (gptlbas - 1) + mapoff);
    memcpy(gh, ghead, lbasize);
    gh->lba = gh->backuplba;
    gh->backuplba = 1;
    gh->partlba = gh->lba - (gptlbas - 1);
    update_crc(gh, (const gpt_entry *)((char *)map + mapoff));
  }else{
    memset(map + mapoff, 0, gptlbas * lbasize);
  }
  if(msync(map, mapsize, MS_SYNC|MS_INVALIDATE)){
    diag("Error syncing %d (%s)\n", fd, strerror(errno));
    munmap(map, mapsize);
    return -1;
  }
  if(munmap(map, mapsize)){
    diag("Error unmapping %d (%s)\n", fd, strerror(errno));
    return -1;
  }
  return 0;
}

int initialize_gpt(gpt_header *gh, size_t lbasize, uint64_t backuplba, uint64_t firstusable){
  if(firstusable == 0){
    diag("Illegal first usable LBA %ju\n", (uintmax_t)firstusable);
    return -1;
  }
  if(lbasize == 0){
    diag("Illegal LBA size %zu\n", lbasize);
    return -1;
  }
  memcpy(&gh->signature, gpt_signature, sizeof(gh->signature));
  gh->revision = 0x10000u;
  gh->headsize = sizeof(*gh);
  gh->reserved = 0;
  gh->lba = 1;
  // ->crc is set by update_crc()
  gh->backuplba = backuplba;
  gh->first_usable = firstusable;
  gh->last_usable = backuplba - (firstusable - 1);
  if(getrandom(gh->disk_guid, GUIDSIZE, GRND_NONBLOCK) != GUIDSIZE){
    diag("Couldn't get %d random bytes (%s)\n", GUIDSIZE, strerror(errno));
    return -1;
  }
  gh->partlba = gh->lba + 1;
  gh->partcount = MINIMUM_GPT_ENTRIES;
  gh->partsize = sizeof(gpt_entry);
  // ->partcrc is set by update_crc()
  if(lbasize > sizeof(*gh)){
    memset((char *)gh + sizeof(*gh), 0, lbasize - sizeof(*gh));
  }
  return 0;
}

// Write out a GPT and its backup on the device represented by fd, using
// lbasize-byte LBA. The device ought have lbas lbasize-byte sectors. We will
// write to the second-from-the-first, and the final, groups of sectors. lbas
// must be greater than or equal to 1 + 2 * (1 + ceil(16k / lbasize).
//
// We can either zero it all out, or create a new empty GPT. Set realdata not
// equal to 0 to perform the latter.
static int
write_gpt(int fd, ssize_t lbasize, uint64_t lbas, unsigned realdata){
  ssize_t s = lbasize - (MINIMUM_GPT_ENTRIES * sizeof(gpt_entry) % lbasize);
  const size_t gptlbas = 1 + !!s + (MINIMUM_GPT_ENTRIES * sizeof(gpt_entry) / lbasize);
  uint64_t backuplba = lbas - 1;
  int pgsize = getpagesize();
  gpt_header *ghead;
  size_t mapsize;
  void *map;
  off_t off;

  if(pgsize <= 0 || (pgsize % lbasize)){
    return -1;
  }
  // The first copy goes into LBA 1. Calculate offset into map due to
  // lbasize possibly (probably) not equalling the page size.
  if((off = lbasize % pgsize) == 0){
    mapsize = 0;
  }else{
    mapsize = lbasize;
  }
  mapsize += gptlbas * lbasize;
  mapsize = ((mapsize / pgsize) + !!(mapsize % pgsize)) * pgsize;
  if(mapsize % pgsize){
    return -1;
  }
  map = mmap(NULL, mapsize, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  if(map == MAP_FAILED){
    diag("Error mapping %zub at %d (%s?)\n", mapsize, fd, strerror(errno));
    return -1;
  }
  ghead = (gpt_header *)((char *)map + off);
  if(!realdata){
    memset(ghead, 0, gptlbas * lbasize);
  }else{
    if(initialize_gpt(ghead, lbasize, backuplba, gptlbas)){
      munmap(map, mapsize);
      return -1;
    }
    if(update_crc(ghead, (const gpt_entry *)((char *)ghead + lbasize))){
      munmap(map, mapsize);
      return -1;;
    }
  }
  if(msync(map, mapsize, MS_SYNC|MS_INVALIDATE)){
    diag("Error syncing %d (%s?)\n", fd, strerror(errno));
    munmap(map, mapsize);
    return -1;
  }
  if(update_backup(fd, ghead, gptlbas, lbas, lbasize, pgsize, realdata)){
    munmap(map, mapsize);
    return -1;
  }
  if(munmap(map, mapsize)){
    diag("Error unmapping %d (%s?)\n", fd, strerror(errno));
    return -1;
  }
  return 0;
}

int new_gpt(device *d){
  size_t mapsize;
  void *map;
  int fd;

  if(d->layout != LAYOUT_NONE){
    diag("Won't create partition table on non-disk %s\n", d->name);
    return -1;
  }
  if(d->size % LBA_SIZE){
    diag("Won't create GPT on (%ju %% %u == %juB) disk %s\n",
      d->size, LBA_SIZE, d->size % LBA_SIZE, d->name);
    return -1;
  }
  if(d->size < LBA_SIZE + 2 * (LBA_SIZE + MINIMUM_GPT_ENTRIES * sizeof(gpt_entry))){
    diag("Won't create GPT on %juB disk %s\n",d->size,d->name);
    return -1;
  }
  if((fd = openat(devfd, d->name, O_RDWR|O_CLOEXEC|O_DIRECT)) < 0){
    diag("Couldn't open %s (%s?)\n", d->name, strerror(errno));
    return -1;
  }
  // protective MBR in first LBA
  mapsize = getpagesize(); // FIXME check for insanity
  if((map = mmap(NULL, mapsize, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED){
    diag("Couldn't map %s (%s?)\n", d->name, strerror(errno));
    close(fd);
    return -1;
  }
  memcpy((char *)map + MBR_OFFSET, GPT_PROTECTIVE_MBR, MBR_SIZE);
  if(munmap(map, mapsize)){
    diag("Couldn't unmap MBR for %s (%s?)\n", d->name, strerror(errno));
    close(fd);
    return -1;
  }
  if(write_gpt(fd, LBA_SIZE, d->size / LBA_SIZE, 1)){
    diag("Couldn't write GPT on %s (%s?)\n", d->name, strerror(errno));
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

int zap_gpt(device *d){
  size_t mapsize;
  void *map;
  int fd;

  if(d->layout != LAYOUT_NONE){
    diag("Won't zap partition table on non-disk %s\n", d->name);
    return -1;
  }
  if(d->blkdev.pttable == NULL || strcmp(d->blkdev.pttable, "gpt")){
    diag("No GPT on disk %s\n", d->name);
    return -1;
  }
  if((fd = openat(devfd, d->name, O_RDWR|O_CLOEXEC|O_DIRECT)) < 0){
    diag("Couldn't open %s (%s?)\n", d->name, strerror(errno));
    return -1;
  }
  mapsize = getpagesize(); // FIXME check for insanity
  if((map = mmap(NULL, mapsize, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED){
    diag("Couldn't map %s (%s?)\n", d->name, strerror(errno));
    close(fd);
    return -1;
  }
  memset((char *)map + MBR_OFFSET, 0, MBR_SIZE);
  if(munmap(map, mapsize)){
    diag("Couldn't unmap MBR for %s (%s?)\n", d->name, strerror(errno));
    close(fd);
    return -1;
  }
  if(write_gpt(fd, LBA_SIZE, d->size / LBA_SIZE, 0)){
    diag("Couldn't write GPT on %s (%s?)\n", d->name, strerror(errno));
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

static int
gpt_name(const wchar_t *name, void *name16le, size_t olen){
  iconv_t icv;
  size_t len;
  char *n16;

  errno = 0;
  if((icv = iconv_open("UTF16LE", "WCHAR_T")) == (iconv_t)-1 && errno){
    diag("Can't convert WCHAR_T to UTF16LE (%s?)\n", strerror(errno));
    return -1;
  }
  n16 = name16le;
  len = wcslen(name) * sizeof(*name);
  errno = 0;
  if(iconv(icv, (char **)&name, &len, &n16, &olen) == (size_t)-1 && errno){
    diag("Error converting name (%s? %zu, %zu left)\n", strerror(errno), len, olen);
    iconv_close(icv);
    return -1;
  }
  if(iconv_close(icv)){
    diag("Error closing iconv (%s?)\n", strerror(errno));
    return -1;
  }
  return 0;
}

static void *
const_map_gpt(const device *d, size_t *mapsize, int *fd, size_t lbasize){
  const int pgsize = getpagesize();
  const unsigned gptlbas = 1 + (MINIMUM_GPT_ENTRIES * sizeof(struct gpt_entry) / lbasize);
  uint64_t off;
  void *map;

  if(pgsize < 0){
    diag("Bad pgsize for GPT: %d\n", pgsize);
    return MAP_FAILED;
  }
  if(MINIMUM_GPT_ENTRIES * sizeof(struct gpt_entry) % lbasize){
    diag("Bad lbasize for GPT: %zu\n", lbasize);
    return MAP_FAILED;
  }
  // The first copy goes into LBA 1. Calculate offset into map due to
  // lbasize possibly (probably) not equalling the page size.
  if((off = lbasize % pgsize) == 0){
    *mapsize = 0;
  }else{
    *mapsize = lbasize;
  }
  *mapsize += gptlbas * lbasize;
  *mapsize = ((*mapsize / pgsize) + !!(*mapsize % pgsize)) * pgsize;
  if(*mapsize % pgsize){
    diag("Bad mapsize %zu for page size %d\n", *mapsize, pgsize);
    return MAP_FAILED;
  }
  if((*fd = openat(devfd, d->name, O_RDONLY|O_CLOEXEC|O_DIRECT)) < 0){
    diag("Couldn't open %s (%s?)\n", d->name, strerror(errno));
    return MAP_FAILED;
  }
  map = mmap(NULL, *mapsize, PROT_READ, MAP_SHARED, *fd, 0);
  if(map == MAP_FAILED){
    diag("Couldn't map GPT header (%s?)\n", strerror(errno));
    close(*fd);
    return map;
  }
  return map;
}

// Map the primary GPT header, its table, and the MBR boot sector.
static void *
map_gpt(device *d, size_t *mapsize, int *fd, size_t lbasize){
  const int pgsize = getpagesize();
  const unsigned gptlbas = 1 + (MINIMUM_GPT_ENTRIES * sizeof(struct gpt_entry) / lbasize);
  uint64_t off;
  void *map;

  if(pgsize < 0){
    diag("Bad pgsize for GPT: %d\n", pgsize);
    return MAP_FAILED;
  }
  if(MINIMUM_GPT_ENTRIES * sizeof(struct gpt_entry) % lbasize){
    diag("Bad lbasize for GPT: %zu\n", lbasize);
    return MAP_FAILED;
  }
  if((*fd = openat(devfd, d->name, O_RDWR|O_CLOEXEC|O_DIRECT)) < 0){
    diag("Couldn't open %s (%s?)\n", d->name, strerror(errno));
    return MAP_FAILED;
  }
  // The first copy goes into LBA 1. Calculate offset into map due to
  // lbasize possibly (probably) not equalling the page size.
  if((off = lbasize % pgsize) == 0){
    *mapsize = 0;
  }else{
    *mapsize = lbasize;
  }
  *mapsize += gptlbas * lbasize;
  *mapsize = ((*mapsize / pgsize) + !!(*mapsize % pgsize)) * pgsize;
  assert(*mapsize % pgsize == 0);
  map = mmap(NULL, *mapsize, PROT_READ|PROT_WRITE, MAP_SHARED, *fd, 0);
  if(map == MAP_FAILED){
    diag("Couldn't map GPT header (%s?)\n", strerror(errno));
    close(*fd);
    return map;
  }
  return map;
}

// Pass the return from map_gpt(), ie the MBR boot sector + primary GPT
static int
const_unmap_gpt(const device *parent, void *map, size_t mapsize, int fd){
  assert(parent->layout == LAYOUT_NONE);
  if(munmap(map, mapsize)){
    int e = errno;

    diag("Error munmapping %s (%s?)\n", parent->name, strerror(errno));
    close(fd);
    errno = e;
    return -1;
  }
  return 0;
}

// Pass the return from map_gpt(), ie the MBR boot sector + primary GPT
static int
unmap_gpt(const device *parent, void *map, size_t mapsize, int fd, size_t lbasize){
  const uint64_t gptlbas = 1 + (MINIMUM_GPT_ENTRIES * sizeof(gpt_entry) / lbasize);
  gpt_header *gpt = (gpt_header *)((char *)map + lbasize);
  gpt_entry *gpe = (gpt_entry *)((char *)map + 2 * lbasize);
  const uint64_t lbas = parent->size / lbasize;
  const int pgsize = getpagesize();

  assert(parent->layout == LAYOUT_NONE);
  if(pgsize < 0){
    diag("Warning: bad pgsize for GPT: %d\n", pgsize);
  }
  if(update_crc(gpt, gpe)){
    munmap(map, mapsize);
    close(fd);
    return -1;
  }
  if(update_backup(fd, gpt, gptlbas, lbas, LBA_SIZE, pgsize, 1)){
    munmap(map, mapsize);
    close(fd);
    return -1;
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

int add_gpt(device *d, const wchar_t *name, uintmax_t fsec, uintmax_t lsec, unsigned long long code){
  static const uint8_t zguid[GUIDSIZE];
  const size_t lbasize = LBA_SIZE;
  unsigned char tguid[GUIDSIZE];
  char cname[BUFSIZ];
  gpt_header *ghead;
  unsigned z, partno;
  gpt_entry *gpe;
  size_t mapsize;
  uint64_t lbas;
  void *map;
  int fd, r;

  if(!name){
    diag("GPT partitions ought be named!\n");
    return -1;
  }
  if(!d){
    diag("Passed a NULL device\n");
    return -1;
  }
  if(d->layout != LAYOUT_NONE){
    diag("Won't add partition to non-disk %s\n", d->name);
    return -1;
  }
  if(d->blkdev.pttable == NULL || strcmp(d->blkdev.pttable, "gpt")){
    diag("No GPT on disk %s\n", d->name);
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
    diag("Bad sector spec (%ju:%ju) on %ju disk\n", fsec, lsec, lbas);
    return -1;
  }
  if(get_gpt_guid(code, tguid)){
    diag("Not a valid GPT typecode: %llu\n", code);
    return -1;
  }
  if((map = map_gpt(d, &mapsize, &fd, LBA_SIZE)) == MAP_FAILED){
    return -1;
  }
  ghead = (gpt_header *)((char *)map + LBA_SIZE);
  gpe = (gpt_entry *)((char *)map + 2 * LBA_SIZE);
  // Determine the next available partition number, and verify that no
  // existing partitions overlap with this one.
  partno = ghead->partcount;
  for(z = 0 ; z < ghead->partcount ; ++z){
    // if there're any non-zero bits in either the type or
    // partition guid, assume it's being used.
    if(memcmp(gpe[z].type_guid, zguid, sizeof(zguid)) || memcmp(gpe[z].part_guid, zguid, sizeof(zguid))){
      if((gpe[z].first_lba >= fsec && gpe[z].first_lba <= lsec) ||
          (gpe[z].last_lba <= lsec && gpe[z].last_lba >= fsec)){
        diag("Partition overlap (%ju:%ju) ([%u]%ju:%ju)\n", fsec, lsec,
            z, gpe[z].first_lba, gpe[z].last_lba);
      }
      continue;
    }
    if(partno == ghead->partcount){
      partno = z;
    }
  }
  if((z = partno) == ghead->partcount){
    diag("no entry for a new partition in %s\n", d->name);
    munmap(map, mapsize);
    close(fd);
    return -1;
  }
  diag("First sector: %ju last sector: %ju count: %ju size: %ju\n",
      (uintmax_t)fsec,
      (uintmax_t)lsec,
      (uintmax_t)(lsec - fsec),
      (uintmax_t)((lsec - fsec) * d->logsec));
  memcpy(gpe[z].type_guid, tguid, sizeof(tguid));
  if(gpt_name(name, gpe[z].name, sizeof(gpe[z].name))){
    memset(gpe + z, 0, sizeof(*gpe));
    munmap(map, mapsize);
    close(fd);
    return -1;
  }
  if(getrandom(gpe[z].part_guid, GUIDSIZE, GRND_NONBLOCK) != GUIDSIZE){
    diag("Couldn't get %d random bytes (%s)\n", GUIDSIZE, strerror(errno));
    memset(gpe + z, 0, sizeof(*gpe));
    munmap(map, mapsize);
    close(fd);
    return -1;
  }
  gpe[z].flags = 0;
  gpe[z].first_lba = fsec;
  gpe[z].last_lba = lsec;
  if(unmap_gpt(d, map, mapsize, fd, LBA_SIZE)){
    close(fd);
    return -1;
  }
  snprintf(cname, sizeof(cname) - 1, "%ls", name);
  if(fsync(fd)){
    diag("Couldn't sync %d for %s\n", fd, d->name);
  }
  r = blkpg_add_partition(fd, fsec * LBA_SIZE,
      (lsec - fsec + 1) * LBA_SIZE, z + 1, cname);
  if(close(fd)){
    int e = errno;

    diag("Error closing %s (%s?)\n", d->name, strerror(errno));
    errno = e;
    return -1;
  }
  return r;
}

int name_gpt(device *d, const wchar_t *name){
  gpt_entry *gpe;
  size_t mapsize;
  void *map;
  int fd;

  assert(d->layout == LAYOUT_PARTITION);
  if((map = map_gpt(d->partdev.parent, &mapsize, &fd, LBA_SIZE)) == MAP_FAILED){
    return -1;
  }
  gpe = (gpt_entry *)((char *)map + 2 * LBA_SIZE);
  if(gpt_name(name, gpe[d->partdev.pnumber].name, sizeof(gpe->name))){
    munmap(map, mapsize);
    close(fd);
    return -1;
  }
  if(unmap_gpt(d->partdev.parent, map, mapsize, fd, LBA_SIZE)){
    close(fd);
    return -1;
  }
  if(close(fd)){
    diag("Error closing %s (%s?)\n", d->name, strerror(errno));
    return -1;
  }
  return 0;
}

int uuid_gpt(device *d, const void *uuid){
  gpt_entry *gpe;
  size_t mapsize;
  void *map;
  int fd;

  assert(d->layout == LAYOUT_PARTITION);
  if((map = map_gpt(d->partdev.parent, &mapsize, &fd, LBA_SIZE)) == MAP_FAILED){
    return -1;
  }
  gpe = (gpt_entry *)((char *)map + 2 * LBA_SIZE);
  memcpy(gpe[d->partdev.pnumber].name, uuid, GUIDSIZE);
  if(unmap_gpt(d->partdev.parent, map, mapsize, fd, LBA_SIZE)){
    close(fd);
    return -1;
  }
  if(close(fd)){
    diag("Error closing %s (%s?)\n", d->name, strerror(errno));
    return -1;
  }
  return 0;
}

int flags_gpt(device *d, uint64_t flag){
  gpt_entry *gpe;
  size_t mapsize;
  void *map;
  int fd;

  assert(d->layout == LAYOUT_PARTITION);
  if((map = map_gpt(d->partdev.parent, &mapsize, &fd, LBA_SIZE)) == MAP_FAILED){
    return -1;
  }
  gpe = (gpt_entry *)((char *)map + 2 * LBA_SIZE);
  gpe[d->partdev.pnumber].flags = flag;
  if(unmap_gpt(d->partdev.parent, map, mapsize, fd, LBA_SIZE)){
    close(fd);
    return -1;
  }
  if(close(fd)){
    diag("Error closing %s (%s?)\n", d->name, strerror(errno));
    return -1;
  }
  return 0;
}

int flag_gpt(device *d, uint64_t flag, unsigned status){
  gpt_entry *gpe;
  size_t mapsize;
  void *map;
  int fd;

  assert(d->layout == LAYOUT_PARTITION);
  if((map = map_gpt(d->partdev.parent, &mapsize, &fd, LBA_SIZE)) == MAP_FAILED){
    return -1;
  }
  gpe = (gpt_entry *)((char *)map + 2 * LBA_SIZE);
  if(status){
    gpe[d->partdev.pnumber].flags |= flag;
  }else{
    gpe[d->partdev.pnumber].flags &= ~flag;
  }
  if(unmap_gpt(d->partdev.parent, map, mapsize, fd, LBA_SIZE)){
    close(fd);
    return -1;
  }
  if(close(fd)){
    diag("Error closing %s (%s?)\n", d->name, strerror(errno));
    return -1;
  }
  return 0;
}

int code_gpt(device *d, unsigned long long code){
  unsigned g = d->partdev.pnumber;
  unsigned char tguid[GUIDSIZE];
  gpt_entry *gpe;
  size_t mapsize;
  void *map;
  int fd;

  assert(d->layout == LAYOUT_PARTITION);
  if(get_gpt_guid(code, tguid)){
    diag("Not a valid GPT typecode: %llu\n", code);
    return -1;
  }
  if((map = map_gpt(d->partdev.parent, &mapsize, &fd, LBA_SIZE)) == MAP_FAILED){
    return -1;
  }
  gpe = (gpt_entry *)((char *)map + 2 * LBA_SIZE);
  if(gpe[g].first_lba == 0 && gpe[g].last_lba == 0){
    diag("Not a valid GPT partition: %s\n", d->name);
    unmap_gpt(d->partdev.parent, map, mapsize, fd, LBA_SIZE);
    close(fd);
    return -1;
  }
  memcpy(gpe[g].type_guid, tguid, sizeof(tguid));
  if(unmap_gpt(d->partdev.parent, map, mapsize, fd, LBA_SIZE)){
    close(fd);
    return -1;
  }
  if(close(fd)){
    diag("Error closing %s (%s?)\n", d->name, strerror(errno));
    return -1;
  }
  return 0;
}

int del_gpt(const device *p){
  gpt_entry *gpe;
  size_t mapsize;
  unsigned g;
  void *map;
  int fd, r;

  assert(p->layout == LAYOUT_PARTITION);
  g = p->partdev.pnumber - 1;
  if((map = map_gpt(p->partdev.parent, &mapsize, &fd, LBA_SIZE)) == MAP_FAILED){
    return -1;
  }
  gpe = (gpt_entry *)((char *)map + 2 * LBA_SIZE);
  memset(&gpe[g], 0, sizeof(*gpe));
  if(unmap_gpt(p->partdev.parent, map, mapsize, fd, LBA_SIZE)){
    close(fd);
    return -1;
  }
  if(fsync(fd)){
    diag("Couldn't sync %d for %s\n", fd, p->name);
  }
  r = blkpg_del_partition(fd, p->partdev.fsector * LBA_SIZE,
        p->size, p->partdev.pnumber,
        p->partdev.parent->name);
  if(close(fd)){
    diag("Couldn't close %s (%s?)\n", p->partdev.parent->name, strerror(errno));
    return -1;
  }
  return r;
}

uintmax_t first_gpt(const device *d){
  gpt_header *ghead;
  size_t mapsize;
  uintmax_t r;
  void *map;
  int fd;

  assert(d->layout == LAYOUT_NONE);
  if((map = const_map_gpt(d, &mapsize, &fd, LBA_SIZE)) == MAP_FAILED){
    return 0;
  }
  ghead = (gpt_header *)((char *)map + LBA_SIZE);
  r = ghead->first_usable;
  assert(r);
  if(const_unmap_gpt(d, map, mapsize, fd)){
    close(fd);
    return 0;
  }
  if(close(fd)){
    diag("Couldn't close GPT map %d on %s (%s?)\n", fd, d->name, strerror(errno));
    return 0;
  }
  assert(r);
  return r;
}

uintmax_t last_gpt(const device *d){
  gpt_header *ghead;
  size_t mapsize;
  uintmax_t r;
  void *map;
  int fd;

  assert(d->layout == LAYOUT_NONE);
  if((map = const_map_gpt(d, &mapsize, &fd, LBA_SIZE)) == MAP_FAILED){
    return 0;
  }
  ghead = (gpt_header *)((char *)map + LBA_SIZE);
  r = ghead->last_usable;
  if(const_unmap_gpt(d, map, mapsize, fd)){
    close(fd);
    return 0;
  }
  if(close(fd)){
    diag("Couldn't close GPT map %d on %s (%s?)\n", fd, d->name, strerror(errno));
    return 0;
  }
  return r;
}
