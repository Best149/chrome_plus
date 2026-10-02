#include "pakfile.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <vector>

#include "utils.h"

#pragma warning(disable : 4334)
#pragma warning(disable : 4267)
#pragma warning(disable : 4838)

extern "C" {
#include "..\mini_gzip\mini_gzip.h"
void* gzip_compress(uint8_t* data, size_t len, size_t* out_len);
}

namespace {
#pragma pack(push)
#pragma pack(1)

constexpr int kPack4FileVersion = 4;
constexpr int kPack5FileVersion = 5;

// Upper bound for one decompressed entry. The value comes from the entry's own
// gzip ISIZE, and MinSizeRel builds compile without exception handling, so a
// corrupt or hostile size must not reach the allocator unchecked.
constexpr uint32_t kMaxResourceSize = 64 * 1024 * 1024;

struct Pak4Header {
  uint32_t num_entries;
  uint8_t encoding;
};

struct Pak5Header {
  uint32_t encoding;
  uint16_t resource_count;
  uint16_t alias_count;
};

struct PakEntry {
  uint16_t resource_id;
  uint32_t file_offset;
};

struct PakAlias {
  uint16_t resource_id;
  uint16_t entry_index;
};
#pragma pack(pop)

// Validates the pak header and index against the size of the mapped view and
// returns the entry range (`end_entry` is the id-0 sentinel that terminates
// the walk). Every offset here is read from a file, so nothing is dereferenced
// before the buffer is known to cover it.
bool CheckHeader(uint8_t* buffer,
                 size_t buffer_size,
                 PakEntry*& pak_entry,
                 PakEntry*& end_entry) {
  if (!buffer || buffer_size < sizeof(uint32_t))
    return false;

  uint32_t version = *reinterpret_cast<uint32_t*>(buffer);

  if (version != kPack4FileVersion && version != kPack5FileVersion)
    return false;

  size_t entries_offset = 0;
  size_t entry_count = 0;

  if (version == kPack4FileVersion) {
    if (buffer_size < sizeof(uint32_t) + sizeof(Pak4Header))
      return false;

    auto* pak_header = reinterpret_cast<Pak4Header*>(buffer + sizeof(uint32_t));
    if (pak_header->encoding != 1)
      return false;

    entries_offset = sizeof(uint32_t) + sizeof(Pak4Header);
    entry_count = pak_header->num_entries;
  } else {
    if (buffer_size < sizeof(uint32_t) + sizeof(Pak5Header))
      return false;

    auto* pak_header = reinterpret_cast<Pak5Header*>(buffer + sizeof(uint32_t));
    if (pak_header->encoding != 1)
      return false;

    entries_offset = sizeof(uint32_t) + sizeof(Pak5Header);
    entry_count = pak_header->resource_count;
  }

  // The sentinel is read as the "next item" of the last entry, so the table
  // has to hold `entry_count` entries plus that sentinel.
  if (entry_count > (SIZE_MAX - entries_offset) / sizeof(PakEntry) - 1)
    return false;

  const size_t required =
      entries_offset + (entry_count + 1) * sizeof(PakEntry);
  if (required > buffer_size)
    return false;

  pak_entry = reinterpret_cast<PakEntry*>(buffer + entries_offset);
  end_entry = pak_entry + entry_count;

  // In order to save the "next item" of the last item,
  // the id of this special item must be 0
  if (end_entry->resource_id != 0) {
    return false;
  }

  return true;
}

// Validates the byte range an entry occupies: entries have to be ordered and
// the payload has to stay inside the mapping.
bool CheckEntryRange(const PakEntry* pak_entry, const PakEntry* next_entry,
                     size_t buffer_size, size_t& length) {
  const uint32_t start = pak_entry->file_offset;
  const uint32_t next = next_entry->file_offset;
  if (next <= start || next > buffer_size)
    return false;
  length = next - start;
  return true;
}

}  // namespace

uint16_t TraversalGZIPFile(uint8_t* buffer,
                           size_t buffer_size,
                           std::function<bool(uint8_t*, uint32_t, size_t&)>&& f,
                           uint16_t target_resource_id) {
  PakEntry* pak_entry = nullptr;
  PakEntry* end_entry = nullptr;

  if (!CheckHeader(buffer, buffer_size, pak_entry, end_entry)) {
    return 0;
  }

  do {
    PakEntry* next_entry = pak_entry + 1;
    if (target_resource_id != 0 &&
        pak_entry->resource_id != target_resource_id) {
      pak_entry = next_entry;
      continue;
    }

    size_t old_size = 0;
    if (!CheckEntryRange(pak_entry, next_entry, buffer_size, old_size)) {
      // Unordered offsets or an entry past the end of the mapping.
      pak_entry = next_entry;
      continue;
    }

    if (old_size < 10 * 1024) {
      pak_entry = next_entry;
      continue;
    }

    constexpr uint8_t kGzipMagic[] = {0x1F, 0x8B, 0x08};
    std::span<uint8_t> entry_data(buffer + pak_entry->file_offset, old_size);
    if (entry_data.size() < sizeof(kGzipMagic) ||
        !std::ranges::equal(entry_data.subspan(0, sizeof(kGzipMagic)),
                            kGzipMagic)) {
      // Not a GZIP file, skipping
      pak_entry = next_entry;
      continue;
    }

    uint32_t original_size =
        *reinterpret_cast<uint32_t*>(buffer + next_entry->file_offset - 4);

    if (original_size == 0 || original_size > kMaxResourceSize) {
      pak_entry = next_entry;
      continue;
    }

    // `new (std::nothrow)` rather than `make_unique_for_overwrite`: MinSizeRel
    // builds have no exception handling, where a failed allocation terminates
    // the host browser instead of being reported.
    std::unique_ptr<uint8_t[]> unpack_buffer(
        new (std::nothrow) uint8_t[original_size]);

    if (!unpack_buffer) {
      pak_entry = next_entry;
      continue;
    }

    struct mini_gzip gz;
    if (mini_gz_start(&gz, buffer + pak_entry->file_offset, old_size) != 0) {
      // A malformed header leaves the stream unset, and `mini_gz_unpack` would
      // make no progress on it.
      pak_entry = next_entry;
      continue;
    }
    uint32_t unpack_len =
        mini_gz_unpack(&gz, unpack_buffer.get(), original_size);

    if (original_size == unpack_len) {
      size_t new_len = old_size;
      bool changed = f(unpack_buffer.get(), unpack_len, new_len);

      if (changed) {
        // The patched entry is rewritten inside its original slot, so the
        // recompressed gzip member (10-byte header + deflate + 8-byte trailer)
        // has to leave room for the FEXTRA field that pads the slot out:
        //   12 + extra_length + (compress_size - 10) == old_size
        // Both preconditions are checked before anything is written: the
        // subtraction wraps for size_t when the result does not fit, and a
        // truncated extra_length makes the payload copy overrun the slot.
        constexpr size_t kGzipHeaderSize = 10;
        constexpr size_t kExtraFieldHeaderSize = 2;
        constexpr size_t kMaxExtraFieldSize = 0xFFFF;

        size_t compress_size = 0;
        // `gzip_compress` is written in C style, so we free it using
        // `std::free`
        std::unique_ptr<void, decltype(&std::free)> compress_buffer_ptr(
            gzip_compress(unpack_buffer.get(), new_len, &compress_size),
            std::free);

        auto* compress_buffer =
            static_cast<uint8_t*>(compress_buffer_ptr.get());

        const size_t slot_slack =
            compress_size > old_size ? 0 : old_size - compress_size;

        if (compress_buffer && compress_size >= kGzipHeaderSize &&
            slot_slack >= kExtraFieldHeaderSize &&
            slot_slack - kExtraFieldHeaderSize <= kMaxExtraFieldSize) {
          const size_t extra_length = slot_slack - kExtraFieldHeaderSize;
          std::span<uint8_t> src_span(compress_buffer, compress_size);
          std::ranges::copy(src_span.subspan(0, kGzipHeaderSize),
                            entry_data.begin());
          entry_data[3] = 0x04;
          const uint16_t extra_length_le =
              static_cast<uint16_t>(extra_length);
          std::memcpy(&entry_data[10], &extra_length_le,
                      sizeof(extra_length_le));
          std::ranges::fill(entry_data.subspan(12, extra_length), 0);
          std::ranges::copy(src_span.subspan(kGzipHeaderSize),
                            entry_data.begin() + 12 +
                                static_cast<std::ptrdiff_t>(extra_length));
          // Only one resource is the patch target; once the callback has
          // handled it there is nothing left to find, so stop scanning the
          // rest of the pak to avoid decompressing every remaining entry in
          // each renderer process.
          return pak_entry->resource_id;
        }

        // The recompressed entry cannot be represented in the original slot;
        // leaving it alone reports "nothing patched" (the documented return
        // value) instead of publishing an entry that does not decode.
        DebugLog(L"PakPatch: resource {} not written back ({} bytes -> {})",
                 pak_entry->resource_id, old_size, compress_size);
        return 0;
      }
    }
    pak_entry = next_entry;
  } while (pak_entry->resource_id != 0);

  return 0;
}

std::optional<PakResourceSlot> FindResourceSlot(uint8_t* buffer,
                                                size_t buffer_size,
                                                uint16_t resource_id) {
  PakEntry* pak_entry = nullptr;
  PakEntry* end_entry = nullptr;

  if (!CheckHeader(buffer, buffer_size, pak_entry, end_entry)) {
    return std::nullopt;
  }

  do {
    PakEntry* next_entry = pak_entry + 1;
    if (pak_entry->resource_id == resource_id) {
      size_t length = 0;
      if (!CheckEntryRange(pak_entry, next_entry, buffer_size, length)) {
        return std::nullopt;
      }
      return PakResourceSlot{pak_entry->file_offset,
                             static_cast<uint32_t>(length)};
    }
    pak_entry = next_entry;
  } while (pak_entry->resource_id != 0);

  return std::nullopt;
}
