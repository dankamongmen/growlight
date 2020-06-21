#include <stdio.h>

#include "sha.h"
#include "nettle/sha1.h"

void sha1(const void* src, size_t len, void* dst){
  struct sha1_ctx ctx;
  sha1_init(&ctx);
  sha1_update(&ctx, len, src);
  sha1_digest(&ctx, SHA1_DIGEST_SIZE, dst);
}
