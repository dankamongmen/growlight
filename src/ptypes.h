#ifndef GROWLIGHT_PTYPES
#define GROWLIGHT_PTYPES

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "gpt.h"

#define PARTROLE_ESP		0xef00
#define PARTROLE_BIOSBOOT	0xef02
#define PARTROLE_PRIMARY	0x8300

typedef struct ptype {
	uint16_t code;			// [fg]disk/parted code (2 bytes)
	const char *name;		// Human-readable name
	uint8_t gpt_guid[GUIDSIZE];	// GPT Type GUID (16 bytes) or 0s
	uint8_t mbr_code;		// MBR code (1 byte) or 0
	unsigned aggregable;		// Can it go into an aggregate?
} ptype;

extern const ptype ptypes[];

// Pass in the common code, get the scheme-specific identifier filled in.
// Returns 0 for a valid code, or -1 if there's no ident for the scheme.
int get_gpt_guid(unsigned,void *);
int get_mbr_code(unsigned,unsigned *);

// Pass in a libblkid-style string representation, and get the common code
unsigned get_str_code(const char *);

unsigned get_code_specific(const char *,unsigned);

static inline int
ptype_default_p(unsigned code){
	return code == PARTROLE_PRIMARY;
}

int ptype_supported(const char *,const ptype *);

#ifdef __cplusplus
}
#endif

#endif
