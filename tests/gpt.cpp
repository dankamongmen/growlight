#include "main.h"
#include "gpt.h"
#include <cstring>

TEST_CASE("GPT") {

  // First eight bytes must be "EFI PART"
  SUBCASE("Signature") {
    gpt_header head;
    initialize_gpt(&head, 0, 0, 0); // FIXME
    CHECK(0 == memcmp(&head, "EFI PART", 8));
  }

  // Bytes 0x8--0xb must be 00 00 01 00 (1.00 by UEFI logic)
  SUBCASE("Revision") {
    gpt_header head;
    initialize_gpt(&head, 0, 0, 0); // FIXME
    CHECK(0 == memcmp(reinterpret_cast<char*>(&head) + 8, "\x00\x00\x01\x00", 4));
  }

  SUBCASE("CRC32") {
    gpt_header head;
    initialize_gpt(&head, 0, 0, 0); // FIXME
    // partition entry size must be a positive multiple of 128 (usually 128)
    CHECK(0 < head.partsize);
    CHECK(0 == (head.partsize % 128));
    // number of partition entries, usually 128 (MINIMUM_GPT_ENTRIES)
    CHECK(128 <= head.partcount);
    auto entries = new gpt_entry[head.partcount];
    update_crc(&head, entries);
    // FIXME verify crc
    delete[] entries;
  }

}
