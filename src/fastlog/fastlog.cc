#include "fastlog.hpp"

#include <absl/crc/crc32c.h>

namespace fastlog {

Partition::Partition(const FastLogSettings& settings, tinyio::IO& io)
    : io_(io),
      settings_(std::move(settings)),
      am_(settings_, 0),
      arena_(io.GetBufferPool(), settings_.PageSize(), settings_.ResidentPages()),
      ro_fd_cache_(settings_.ro_page_cache) {
  // ensure directory, terminate the program if we cannot
  if (auto res = EnsureDirectory(); !res.has_value()) {
    ALOG_FATAL("Cannot ensure the partition directory exist, error = {}", res.error());
    std::terminate();
  }
}

std::optional<LogAddress> Partition::AppendRecord(const RecordOp op,
                                                  std::span<const std::byte> record) {
  const std::size_t size = AlignUpTo8(k_header_size + record.size());

  assert(size <= settings_.PageSize() - k_header_size &&
         "Record too large for partition memory capacity");

  // Capacity Check
  if (!am_.HasCapacityFor(size)) {
    return std::nullopt;
  }

  // Allocate the space! (This handles padding and tail advancement)
  const LogAddress addr = am_.Allocate(size);

  // Do the physical writing
  const auto slot = am_.SlotOf(addr);
  const auto offset = am_.OffsetInPageOf(addr);
  const size_t start_offset = slot * settings_.PageSize() + offset;
  auto* ptr = arena_.Buffer().Ptr() + start_offset;

  const auto hdr = new (ptr) RecordHeader();
  hdr->op = static_cast<uint8_t>(op);
  hdr->flags = 0;
  hdr->payload_size = static_cast<uint32_t>(record.size());
  hdr->address = addr;

  arena_.Write(start_offset + k_header_size, record);

  // Compute CRC, skip the crc placeholder
  const auto crc_start = reinterpret_cast<const char*>(ptr + sizeof(uint32_t));
  const std::size_t crc_length = k_header_size - sizeof(uint32_t) + record.size();
  hdr->crc32c = static_cast<uint32_t>(
      absl::ComputeCrc32c(absl::string_view(crc_start, crc_length)));

  return addr;
}

tinyio::Result<void> Partition::Commit(const bool strong) {
  const LogAddress start = am_.GetCommitted();
  const LogAddress target = am_.GetTail();

  size_t len = (target - start) & ~4095ULL;  // Round down to nearest 4K boundary
  if (len <= 0) {
    return {};
  }

  // maybe rotate
  const size_t file_offset = am_.OffsetInSegmentOf(start);
  if (!active_segment_ || file_offset == 0) {
    // We crossed a 1GB boundary (or just booted up)! Time to rotate.
    TRY(RotateSegment(start));
  }

  // Ensure this flush stops exactly at the 1GB boundary of the current file.
  // If `len` is larger, the remainder will just get flushed on the NEXT cycle
  // (which will trigger the rotation logic above!)
  const size_t space_in_segment = settings_.SegmentSize() - file_offset;
  len = std::min(len, space_in_segment);

  // explicit two write split
  const size_t physical_start = am_.OffsetInMemoryOf(start);
  const size_t physical_end = am_.OffsetInMemoryOf(start + len);

  if (physical_start < physical_end) {
    TRY(io_.WriteFixed(*active_segment_, arena_.Buffer(), len, file_offset,
                       physical_start));
  } else {
    // non-continuous
    const size_t first_len = settings_.MemorySize() - physical_start;
    tinyio::TaskGroup<uint32_t> group(io_, 2);

    group.Spawn([&](tinyio::IO& io) noexcept {
      return io.WriteFixed(*active_segment_, arena_.Buffer(), first_len, file_offset,
                           physical_start);
    });

    group.Spawn([&](tinyio::IO& io) noexcept {
      return io.WriteFixed(*active_segment_, arena_.Buffer(), physical_end,
                           file_offset + first_len, 0);
    });

    TRY(group.Wait());

    // Don't forget to check if both writes actually succeeded!
    for (const auto& res : group.Results()) {
      if (!res->has_value()) {
        return std::unexpected(res->error());
      }
    }
  }

  TRY_LOG_ERR(io_.Fsync(*active_segment_, strong), "failed to fsync to disk");

  // advance
  am_.AdvanceCommitted(start + len);

  // Handle segment garbage collection
  const uint64_t old_lowest_segment = am_.SegmentNumberOf(persisted_begin_);
  const uint64_t new_lowest_segment = am_.SegmentNumberOf(am_.GetBegin());

  for (uint64_t i = old_lowest_segment; i < new_lowest_segment; ++i) {
    const auto filepath = settings_.GetSegmentPath(i);
    // Remove the FD from the read-only cache to prevent keeping the file open
    (void)ro_fd_cache_.Remove(i);
    
    if (auto res = io_.Remove(filepath); !res) {
      ALOG_ERROR("Failed to delete truncated segment file {}: {}", filepath.string(),
                 res.error());
    } else {
      ALOG_INFO("Deleted truncated segment file {}", filepath.string());
    }
  }

  persisted_begin_ = am_.GetBegin();

  return {};
}

tinyio::Result<void> Partition::RotateSegment(const LogAddress start) {
  // Close the current segment if it exists, do it in the background
  if (active_segment_) {
    (void)io_.Spawn([fd = std::move(*active_segment_)](tinyio::IO& io) mutable noexcept {
      if (auto expected = io.Close(std::move(fd)); !expected.has_value()) {
        ALOG_ERROR("Failed to close segment file: {}", expected.error());
      }
    });
    active_segment_.reset();
  }

  // Create filename like "00000001.log" based on mathematical segment ID
  const uint64_t segment_id = am_.SegmentNumberOf(start);
  const auto filepath = settings_.GetSegmentPath(segment_id);

  active_segment_.emplace(TRY(io_.Open(filepath, O_CREAT | O_RDWR | O_DIRECT, 0644)));

  // we need to fallocate
  TRY(io_.Fallocate(*active_segment_, 0, 0, static_cast<off_t>(settings_.SegmentSize())));

  return {};
}

tinyio::Result<uint32_t> Partition::Read(const LogAddress addr, std::span<std::byte> buffer) {
  if (addr < am_.GetBegin() || addr >= am_.GetTail()) {
    return tinyio::Fail(ENOENT, "Not found");
  }

  if (addr >= am_.GetHead()) {
    ALOG_DEBUG("Reading data from ram address={}", addr);
    return ReadFromRam(addr, buffer);
  }

  ALOG_DEBUG("Reading data from disk address={}", addr);
  return ReadFromDisk(addr, buffer);
}

tinyio::Result<std::shared_ptr<tinyio::Fd>> Partition::GetReadSegmentFd(
    uint64_t segment_id) {
  // is it cached
  if (auto cached_fd = ro_fd_cache_.Get(segment_id)) {
    return *cached_fd;
  }

  // open and register
  const auto filepath = settings_.GetSegmentPath(segment_id);
  auto raw_fd = TRY_LOG_ERR(io_.Open(filepath, O_RDONLY), "fail to open file for segment",
                            segment_id);
  auto direct_fd = TRY_LOG_ERR(io_.RegisterFile(std::move(raw_fd)),
                               "fail to register file for segment", segment_id);

  auto fd_deleter = [&io = io_](tinyio::Fd* ptr) {
    if (ptr && ptr->IsValid()) {
      (void)io.Spawn([old_fd = std::move(*ptr)](tinyio::IO& inner_io) mutable noexcept {
        if (auto res = inner_io.Close(std::move(old_fd)); !res) {
          ALOG_ERROR("Failed to close cached read segment: {}", res.error());
        }
      });
    }
    delete ptr;
  };
  auto shared_fd =
      std::shared_ptr<tinyio::Fd>(new tinyio::Fd(std::move(direct_fd)), fd_deleter);

  // Insert into Cache.
  // If the Sieve Cache evicts an element, `put` returns the old `shared_ptr`.
  // By discarding the return value `(void)`, the `shared_ptr` goes out of scope.
  // If no suspended fiber is actively reading from it, the custom deleter above
  // fires immediately and safely closes the FD in the background!
  (void)ro_fd_cache_.Put(segment_id, shared_fd);

  return shared_fd;
}

tinyio::Result<uint32_t> Partition::ReadFromRam(LogAddress addr,
                                             std::span<std::byte> buffer) {
  const auto slot = am_.SlotOf(addr);
  const auto offset = am_.OffsetInPageOf(addr);
  const size_t start_offset = slot * settings_.PageSize() + offset;
  auto* ptr = arena_.Buffer().Ptr() + start_offset;

  RecordHeader hdr;
  std::memcpy(&hdr, ptr, sizeof(RecordHeader));

  if (hdr.address != addr) {
    return tinyio::Fail(EBADMSG, "Address mismatch");
  }

  if (buffer.size() < hdr.payload_size) {
    return tinyio::Fail(ENOBUFS, "Buffer too small");
  }

  const auto crc_start = reinterpret_cast<const char*>(ptr + sizeof(uint32_t));
  const std::size_t crc_length = k_header_size - sizeof(uint32_t) + hdr.payload_size;
  const uint32_t computed_crc = static_cast<uint32_t>(
      absl::ComputeCrc32c(absl::string_view(crc_start, crc_length)));

  if (computed_crc != hdr.crc32c) {
    return tinyio::Fail(EBADMSG, "CRC mismatch");
  }

  std::memcpy(buffer.data(), ptr + k_header_size, hdr.payload_size);
  return static_cast<uint32_t>(hdr.payload_size);
}

tinyio::Result<uint32_t> Partition::ReadFromDisk(LogAddress addr,
                                             std::span<std::byte> buffer) {
  const uint64_t segment_id = am_.SegmentNumberOf(addr);
  const uint64_t file_offset = am_.OffsetInSegmentOf(addr);

  const std::shared_ptr<tinyio::Fd> fd_ptr = TRY(GetReadSegmentFd(segment_id));
  const auto& fd = *fd_ptr;

  RecordHeader hdr;
  auto hdr_span = std::span(reinterpret_cast<std::byte*>(&hdr), sizeof(hdr));
  const uint32_t hdr_bytes = TRY(io_.Read(fd, hdr_span, file_offset));

  if (hdr_bytes != sizeof(hdr)) {
    return tinyio::Fail(EIO, "Failed to read header completely");
  }

  if (hdr.address != addr) {
    return tinyio::Fail(EBADMSG, "Address mismatch");
  }

  if (buffer.size() < hdr.payload_size) {
    return tinyio::Fail(ENOBUFS, "Buffer too small");
  }

  auto payload_span = buffer.subspan(0, hdr.payload_size);
  const uint32_t payload_bytes =
      TRY(io_.Read(fd, payload_span, file_offset + sizeof(hdr)));

  if (payload_bytes != hdr.payload_size) {
    return tinyio::Fail(EIO, "Failed to read payload completely");
  }

  // Recompute CRC. We must hash the header and then the payload incrementally.
  const auto crc_start = reinterpret_cast<const char*>(&hdr) + sizeof(uint32_t);
  constexpr std::size_t hdr_crc_len = k_header_size - sizeof(uint32_t);

  absl::crc32c_t computed_crc =
      absl::ComputeCrc32c(absl::string_view(crc_start, hdr_crc_len));
  computed_crc = absl::ExtendCrc32c(
      computed_crc, absl::string_view(reinterpret_cast<const char*>(payload_span.data()),
                                      hdr.payload_size));

  if (static_cast<uint32_t>(computed_crc) != hdr.crc32c) {
    return tinyio::Fail(EBADMSG, "CRC mismatch");
  }

  return static_cast<uint32_t>(hdr.payload_size);
}

tinyio::Result<void> Partition::EnsureDirectory() noexcept {
  const auto dir_perm = static_cast<std::filesystem::perms>(settings_.dir_mode);

  try {
    if (!std::filesystem::exists(settings_.dir)) {
      std::filesystem::create_directories(settings_.dir);
      std::filesystem::permissions(settings_.dir, dir_perm);
    }
  } catch (const std::filesystem::filesystem_error& e) {
    return tinyio::Fail(e.code().value(), "EnsureDirectory");
  }

  return {};
}

tinyio::Result<void> Partition::TruncateUntil(LogAddress addr) {
  if (addr <= am_.GetBegin()) {
    return {};
  }

  // Cap truncation at tail
  if (addr > am_.GetTail()) {
    addr = am_.GetTail();
  }

  // Just advance the memory boundary. The actual segment files will be
  // unlinked asynchronously upon the next Commit().
  am_.AdvanceBegin(addr);

  return {};
}

}  // namespace fastlog