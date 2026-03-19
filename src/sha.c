// copyright 2012–2021 nick black
#include <stdio.h>

#include "sha.h"
#include "nettle/sha1.h"
#include "nettle/version.h"

void sha1(const void* src, size_t len, uint8_t* dst){
  struct sha1_ctx ctx;
  sha1_init(&ctx);
  sha1_update(&ctx, len, src);
#if(NETTLE_VERSION_MAJOR >= 4)
  sha1_digest(&ctx, dst);
#else
  sha1_digest(&ctx, SHA1_DIGEST_SIZE, dst);
#endif
}
