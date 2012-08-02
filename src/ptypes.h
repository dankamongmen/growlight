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
	uint8_t gpt_guid[GUIDSIZE];	// GPT Type GUID (16 bytes)
	uint8_t mbr_code;		// MBR code (1 byte)
} ptype;

extern const ptype ptypes[];

#ifdef __cplusplus
}
#endif

#endif
