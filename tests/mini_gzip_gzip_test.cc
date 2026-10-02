// Offline round-trip checks for the pak write-back path.
//
// `src/pakfile.cc` replaces a patched `resources.pak` entry by re-compressing
// the HTML with `gzip_compress()` and splicing the result back into the
// original slot, padding the difference into a gzip FEXTRA field. Pak entries
// carry no compression flag -- the reader detects gzip by its magic bytes --
// so if `gzip_compress()` ever emits something that is not a complete gzip
// member (a raw zlib stream, say), the entry is served as raw bytes and
// `chrome://settings` breaks in the browser and in every renderer that applies
// the published bytes. That regression is silent, so it is pinned here:
//
//   chrome_plus_gzip_test
//
// Also covers `mini_gz_start()` skipping the FEXTRA payload, without which a
// padded member would be inflated from its own padding.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "../mini_gzip/mini_gzip.h"
void* gzip_compress(uint8_t* data, size_t len, size_t* out_len);
}

namespace {

int failures = 0;

void Expect(const char* what, bool condition) {
  if (condition) {
    std::printf("  ok   %s\n", what);
  } else {
    std::printf("  FAIL %s\n", what);
    ++failures;
  }
}

// Deterministic pseudo-random text, so the deflate stream is realistic and the
// test stays byte-for-byte reproducible.
std::string MakePayload(size_t size) {
  static const char kAlphabet[] =
      "abcdefghijklmnopqrstuvwxyz<>/=\"' \n\t0123456789ABCDEF";
  std::string out;
  out.reserve(size);
  uint32_t state = 0x12345678u;
  for (size_t i = 0; i < size; ++i) {
    state = state * 1664525u + 1013904223u;
    out.push_back(kAlphabet[(state >> 16) % (sizeof(kAlphabet) - 1)]);
  }
  return out;
}

// Inflates a whole gzip member with the mini_gzip API the pak reader uses.
bool Inflate(const uint8_t* member, size_t member_len, std::string* out) {
  struct mini_gzip gz;
  if (mini_gz_start(&gz, const_cast<uint8_t*>(member), member_len) != 0) {
    return false;
  }
  const uint32_t original_size =
      *reinterpret_cast<const uint32_t*>(member + member_len - 4);
  std::vector<char> buffer(original_size);
  const int unpacked =
      mini_gz_unpack(&gz, buffer.data(), static_cast<size_t>(original_size));
  if (unpacked < 0 || static_cast<uint32_t>(unpacked) != original_size) {
    return false;
  }
  out->assign(buffer.data(), buffer.size());
  return true;
}

// The exact splice src/pakfile.cc performs, using its constants and its
// preconditions: keep the member's own 10-byte header, turn on FEXTRA, stash
// the entire slack in the extra field, and relocate the payload behind it.
// `slot_size` is the original pak slot, so the result is what the pak reader
// will later hand to mini_gz_start()/mini_gz_unpack().
//
//   pakfile.cc: 12 + extra_length + (compress_size - 10) == old_size
bool SpliceIntoSlot(const std::vector<uint8_t>& member, size_t slot_size,
                    std::vector<uint8_t>* out) {
  constexpr size_t kGzipHeaderSize = 10;
  constexpr size_t kExtraFieldHeaderSize = 2;
  constexpr size_t kMaxExtraFieldSize = 0xFFFF;

  const size_t old_size = slot_size;
  const size_t compress_size = member.size();
  const size_t slot_slack =
      compress_size > old_size ? 0 : old_size - compress_size;

  if (compress_size < kGzipHeaderSize || slot_slack < kExtraFieldHeaderSize ||
      slot_slack - kExtraFieldHeaderSize > kMaxExtraFieldSize) {
    return false;
  }

  const size_t extra_length = slot_slack - kExtraFieldHeaderSize;
  out->assign(old_size, 0);
  std::memcpy(out->data(), member.data(), kGzipHeaderSize);
  (*out)[3] = 0x04;
  const uint16_t extra_length_le = static_cast<uint16_t>(extra_length);
  std::memcpy(out->data() + 10, &extra_length_le, sizeof(extra_length_le));
  std::memcpy(out->data() + 12 + extra_length, member.data() + kGzipHeaderSize,
              compress_size - kGzipHeaderSize);
  return true;
}

void RunUnpaddedRoundTrip() {
  for (size_t size : {size_t{64}, size_t{4096}, size_t{200 * 1024}}) {
    const std::string payload = MakePayload(size);
    size_t compressed_len = 0;
    void* compressed = gzip_compress(
        reinterpret_cast<uint8_t*>(const_cast<char*>(payload.data())),
        payload.size(), &compressed_len);
    if (!compressed) {
      std::printf("  FAIL gzip_compress returned NULL for %zu bytes\n", size);
      ++failures;
      continue;
    }
    const auto* bytes = static_cast<const uint8_t*>(compressed);

    // It must be a gzip member, not a zlib stream.
    char label[128];
    std::snprintf(label, sizeof(label), "%zu: magic is 1f 8b 08", size);
    Expect(label, compressed_len >= 18 && bytes[0] == 0x1F && bytes[1] == 0x8B &&
                      bytes[2] == 8);
    std::snprintf(label, sizeof(label), "%zu: FLG has no optional fields",
                  size);
    Expect(label, bytes[3] == 0);

    std::string restored;
    std::snprintf(label, sizeof(label), "%zu: unpadded round-trip", size);
    Expect(label, Inflate(bytes, compressed_len, &restored) &&
                      restored == payload);
    std::free(compressed);
  }
}

void RunPaddedRoundTrip() {
  const std::string payload = MakePayload(150 * 1024);
  size_t compressed_len = 0;
  void* compressed = gzip_compress(
      reinterpret_cast<uint8_t*>(const_cast<char*>(payload.data())),
      payload.size(), &compressed_len);
  if (!compressed) {
    std::printf("  FAIL gzip_compress returned NULL\n");
    ++failures;
    return;
  }
  std::vector<uint8_t> member(static_cast<uint8_t*>(compressed),
                              static_cast<uint8_t*>(compressed) +
                                  compressed_len);
  std::free(compressed);

  // Pad into a slot with room to spare, exactly like pakfile.cc.
  const size_t slot_size = member.size() + 64;
  std::vector<uint8_t> slot;
  Expect("padded: splice accepted the slot",
         SpliceIntoSlot(member, slot_size, &slot) && slot.size() == slot_size);
  if (slot.size() != slot_size) {
    return;
  }

  Expect("padded: FEXTRA flag set", (slot[3] & 0x04) != 0);

  // mini_gz_start must skip the extra field, so the payload inflates back to
  // the original HTML instead of being read from the padding.
  std::string restored;
  Expect("padded: round-trip through FEXTRA",
         Inflate(slot.data(), slot.size(), &restored) && restored == payload);

  // A slot too tight for the member must be declined, not overrun.
  std::vector<uint8_t> too_tight;
  Expect("padded: over-long member declined",
         !SpliceIntoSlot(member, member.size() - 1, &too_tight));
  Expect("padded: no slack declined",
         !SpliceIntoSlot(member, member.size(), &too_tight));
}

// A member whose optional fields run past the end of the mapping must be
// rejected rather than walked off the end. The optional fields are sized by
// the member itself, so each one is a potential overrun.
void RunTruncatedHeaderRejected() {
  // 0xFF fill, not 0x00: a zero-filled member would supply the NUL terminator
  // that FNAME/FCOMMENT are supposed to carry, hiding an unterminated field.
  const auto make_header = [](uint8_t flags, uint16_t extra_len) {
    std::vector<uint8_t> member(24, 0xFF);
    member[0] = 0x1F;
    member[1] = 0x8B;
    member[2] = 8;
    member[3] = flags;
    member[10] = static_cast<uint8_t>(extra_len & 0xFF);
    member[11] = static_cast<uint8_t>((extra_len >> 8) & 0xFF);
    return member;
  };

  struct mini_gzip gz;

  // FEXTRA claims 64 KiB inside a 24-byte member.
  std::vector<uint8_t> member = make_header(0x04, 0xFFFF);
  Expect("FEXTRA running past the end rejected",
         mini_gz_start(&gz, member.data(), member.size()) != 0);

  // The extra-field length itself is truncated away.
  member = make_header(0x04, 0);
  Expect("truncated FEXTRA length rejected",
         mini_gz_start(&gz, member.data(), 11) != 0);

  // FNAME/FCOMMENT are NUL-terminated, so the member must still be walked to
  // find that terminator. Filled with 0xFF so nothing but the terminator the
  // header promises can end the scan.
  member = make_header(0x08, 0);
  member[10] = 'a';
  member[11] = 'b';
  member[12] = 'c';
  Expect("unterminated FNAME rejected",
         mini_gz_start(&gz, member.data(), member.size()) != 0);

  // FCOMMENT with no terminator.
  member = make_header(0x10, 0);
  member[10] = 'a';
  member[11] = 'b';
  Expect("unterminated FCOMMENT rejected",
         mini_gz_start(&gz, member.data(), member.size()) != 0);

  // FCRC needs two bytes; only one is left.
  member = make_header(0x02, 0);
  Expect("truncated FCRC rejected",
         mini_gz_start(&gz, member.data(), 11) != 0);

  // A member with no payload at all.
  member = make_header(0, 0);
  Expect("member with no payload rejected",
         mini_gz_start(&gz, member.data(), 10) != 0);

  // Shorter than the fixed 10-byte header.
  member.assign(8, 0);
  member[0] = 0x1F;
  member[1] = 0x8B;
  member[2] = 8;
  Expect("member shorter than the header rejected",
         mini_gz_start(&gz, member.data(), member.size()) != 0);

  // A well-formed empty-ish member is still accepted, so the checks above are
  // not simply rejecting everything.
  member = make_header(0, 0);
  Expect("truncated-header guard does not reject a valid header",
         mini_gz_start(&gz, member.data(), member.size()) == 0);
}

}  // namespace

int main() {
  std::printf("mini_gzip round-trip\n");
  RunUnpaddedRoundTrip();
  RunPaddedRoundTrip();
  RunTruncatedHeaderRejected();

  if (failures == 0) {
    std::printf("all checks passed\n");
    return 0;
  }
  std::printf("%d check(s) failed\n", failures);
  return 1;
}
