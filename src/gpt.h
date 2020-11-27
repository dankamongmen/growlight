// copyright 2012–2020 nick black
#ifndef GROWLIGHT_GPT
#define GROWLIGHT_GPT

#ifdef __cplusplus
extern "C" {
#endif

#include <wchar.h>
#include <stdint.h>

#define GUIDSIZE 16	// 128 opaque bits

struct device;

// Pass the block device
int new_gpt(struct device *);
int zap_gpt(struct device *);
int add_gpt(struct device *,const wchar_t *,uintmax_t,uintmax_t,unsigned long long);

// Pass the partition
int del_gpt(const struct device *);
int name_gpt(struct device *,const wchar_t *);
int uuid_gpt(struct device *,const void *);
int flag_gpt(struct device *,uint64_t,unsigned);
int flags_gpt(struct device *,uint64_t);
int code_gpt(struct device *,unsigned long long);

uintmax_t first_gpt(const struct device *);
uintmax_t last_gpt(const struct device *);

// One LBA block, padded with zeroes at the end. 92 bytes.
typedef struct __attribute__ ((packed)) gpt_header {
  uint64_t signature;        // "EFI PART", 45 46 49 20 50 41 52 54
  uint32_t revision;         // Through UEFI 2.3.1: 00 00 01 00
  uint32_t headsize;         // Header size in little endian,
                             // excludes padding: 5c 00 00 00 == 92
// byte 0x10
  uint32_t crc;              // crc32, 0 through headsize
  uint32_t reserved;         // must be 0
  uint64_t lba;              // location of this header
// byte 0x20
  uint64_t backuplba;        // location of backup header (should be
                             //  last sector of disk)
  uint64_t first_usable;     // first usable lba
// byte 0x30
  uint64_t last_usable;      // last usable lba
  unsigned char disk_guid[GUIDSIZE];
  uint64_t partlba;          // partition entries lba for this copy
// byte 0x50
  uint32_t partcount;        // supported partition entry count
  uint32_t partsize;         // size of partition entries
  uint32_t partcrc;          // crc32 of partition array
} gpt_header;

// 128-byte GUID partition entry. A GPT table must provide space for at least
// MINIMUM_GPT_ENTRIES (128) of these, equal to 16k (32 512-byte sectors, or
// 4 4096-byte sectors) in both copies of the GPT.
typedef struct __attribute__ ((packed)) gpt_entry {
  unsigned char type_guid[GUIDSIZE];
  unsigned char part_guid[GUIDSIZE];
  uint64_t first_lba;
  uint64_t last_lba;
  uint64_t flags;
  uint16_t name[36];  // 36 UTF-16LE code units
} gpt_entry;

void update_crc(gpt_header *head, const gpt_entry *gpes);

#ifdef __cplusplus
}
#endif

#endif
