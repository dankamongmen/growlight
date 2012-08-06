#ifndef GROWLIGHT_PTYPES
#define GROWLIGHT_PTYPES

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "growlight.h"

typedef struct ptype {
	uint16_t code;			// [fg]disk/parted code (2 bytes)
	const char *name;		// Human-readable name
	uint8_t gpt_guid[GUIDSIZE];	// GPT Type GUID (16 bytes) or 0s
	uint8_t mbr_code;		// MBR code (1 byte) or 0
} ptype;

extern const ptype ptypes[];

// Pass in the common code, get the scheme-specific identifier filled in.
// Returns 0 for a valid code, or -1 if there's no ident for the scheme.
int get_gpt_guid(unsigned,void *);
int get_mbr_code(unsigned,unsigned *);

#ifdef __cplusplus
}
#endif

#endif
