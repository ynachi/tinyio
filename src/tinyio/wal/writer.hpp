#pragma once
#include "tinyio/error.hpp"
#include "tinyio/runtime/runtime.hpp"
#include "tinyio/wal/wal.hpp"

namespace tinyio::wal {
class Writer {
public:
    // look at the folder to find the last segment id
    Result<Writer> Open(IO& io, Options options);
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer();
    /// Syncs and closes the active segment through the ring. Call it before
    /// dropping the writer; the destructor only falls back to ::close.
    [[nodiscard]] Result<void> Close() noexcept;

    /// Appends one record. Returns its LSN.
    [[nodiscard]] Result<Lsn> Append(Record record) noexcept;

    /// Appends @p records as consecutive LSNs, written in order.
    [[nodiscard]] Result<LsnRange> Append(std::span<const Record> records) noexcept;

private:
    Writer(IO& io, UniqueFd fd, Options options) noexcept;
    // Can't be null once the writer is created
    IO* io_ = nullptr;
    UniqueFd fd_;
    // get it from the IO. used as scratch buffer for direct IO
    // this is why Writer must go out of scope BEFORE the IO. But this should normally
    // be the case since Writer does not own the IO.
    FixedBuffer scratch_buffer_;
    Lsn last_lsn_ = 0;
    Lsn durable_lsn_ = 0;
};

}  // namespace tinyio::wal