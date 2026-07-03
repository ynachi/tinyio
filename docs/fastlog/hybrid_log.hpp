// ─────────────────────────────────────────────────────────────────────────────
//  hybrid_log.hpp
//  Single-threaded FASTER-style hybrid log — data structure sketch.
//
//  Designed to be read alongside the deep-dive page (index.html in this dir).
//  Section numbers in comments refer to sections of that document.
//
//  Build:    g++ -std=c++20 -Wall -Wextra -O2 hybrid_log.hpp
//            (use as a header; #include it from a .cpp that exercises it)
//
//  What this file IS:
//      - A complete sketch of the data structures.
//      - Full method bodies for the load-bearing pieces
//        (Address, Record, InMemoryLog::locate, hash function, the
//        InMemoryLog append path).
//      - TODO() bodies for the pieces that are mechanical to fill in
//        (read_at, fold_over, checkpoint/replay).
//
//  What this file is NOT:
//      - Production code. No thread safety, no checksum impl, no crash
//        recovery beyond the shape. It's a learning artifact.
//
//  Design anchors:
//      §2  Address is a packed 64-bit descriptor, not a pointer.
//      §3  In-memory log is a circular buffer of fixed-size pages.
//      §4  Hash index is bucket array + overflow, with 16-bit tags.
//      §5  Record = 16-byte header + padded key + padded value.
//      §7  Read path: hash → addr → locate → read bytes.
//      §8  Delete is a tombstone (Op::Delete), not a physical remove.
//      §9  Freeze + flush; the page slot's current_page_index is the truth.
//     §10  Fold-over: only operation that moves a live record to a new addr.
//     §11  Recovery: hash index snapshot + tail replay.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hybrid_log {

// ─── Constants ─────────────────────────────────────────────────────────────
// Tunable. These are reasonable defaults for a learning build.
inline constexpr std::size_t kPageSize        = 32 * 1024;       // 32 KiB
inline constexpr std::size_t kSlotAlign       = 8;               // 8-byte record alignment
inline constexpr std::size_t kBucketCount     = 1u << 20;        // 1M buckets ≈ 32 MB index
inline constexpr std::size_t kPageCount       = 1024;            // 32 MiB in-memory log
inline constexpr std::size_t kSegmentPages    = 64;              // 2 MiB on-disk segments
inline constexpr std::size_t kReadCachePages  = 64;              // 2 MiB read cache
inline constexpr std::uint8_t kSegRamMutable  = 0;
inline constexpr std::uint8_t kSegRamReadOnly = 1;
inline constexpr std::uint8_t kSegDiskBase    = 2;               // disk segments are 2, 3, 4, …

// ─── Address (§2) ──────────────────────────────────────────────────────────
// 64-bit packed descriptor. NOT a pointer.
// Layout: [8 bits segment | 24 bits page | 32 bits offset].
// The address is stable across flushes — see the discussion of address
// vs. boundary pointers in the conversation that produced this sketch.
struct Address {
    std::uint64_t raw = 0;

    constexpr Address() noexcept = default;
    constexpr explicit Address(std::uint64_t r) noexcept : raw(r) {}

    constexpr std::uint8_t  segment() const noexcept { return static_cast<std::uint8_t>((raw >> 56) & 0xFF); }
    constexpr std::uint32_t page()    const noexcept { return static_cast<std::uint32_t>((raw >> 32) & 0x00FF'FFFF); }
    constexpr std::uint32_t offset()  const noexcept { return static_cast<std::uint32_t>(raw & 0xFFFF'FFFF); }

    static constexpr Address encode(std::uint8_t seg,
                                    std::uint32_t page,
                                    std::uint32_t off) noexcept {
        return Address{(static_cast<std::uint64_t>(seg) << 56) |
                       (static_cast<std::uint64_t>(page) << 32) |
                       static_cast<std::uint64_t>(off)};
    }
};

inline constexpr Address kInvalidAddress{0};

// ─── Location (§7) ─────────────────────────────────────────────────────────
// What "where do I read this from?" returns. Computed from an Address by
// comparing against the current in-memory window and asking the page slot
// whether it still holds that page.
struct Location {
    enum class Tier { RamMutable, RamReadOnly, Disk };

    Tier          tier;
    std::uint8_t  disk_segment;   // meaningful only if tier == Disk
    std::uint32_t page;
    std::uint32_t offset;

    static constexpr Location ram(std::uint32_t p, std::uint32_t o, bool is_mutable) noexcept {
        return Location{is_mutable ? Tier::RamMutable : Tier::RamReadOnly, 0, p, o};
    }
    static constexpr Location disk(std::uint8_t seg, std::uint32_t p, std::uint32_t o) noexcept {
        return Location{Tier::Disk, seg, p, o};
    }
};

// ─── Operation (§5, §8) ───────────────────────────────────────────────────
enum class Op : std::uint8_t { Upsert = 0, Delete = 1, Copy = 2 };

// ─── Record (§5) ───────────────────────────────────────────────────────────
// In-memory and on-disk layout are byte-for-byte identical.
//
// +0   : version   (u64)  — = the record's own Address
// +8   : key_len   (u16)
// +10  : val_len   (u32)
// +14  : op        (u8)
// +15  : checksum  (u8)
// +16  : key bytes (key_len, padded up to kSlotAlign)
// then : val bytes (val_len, padded up to kSlotAlign)
struct Record {
    std::uint64_t version  = 0;
    std::uint16_t key_len  = 0;
    std::uint32_t val_len  = 0;
    Op            op       = Op::Upsert;
    std::uint8_t  checksum = 0;   // TODO: use xxh3 of the body in production

    static constexpr std::size_t header_size() noexcept { return 16; }

    static constexpr std::size_t align_up(std::size_t n) noexcept {
        return (n + kSlotAlign - 1) & ~(kSlotAlign - 1);
    }

    static constexpr std::size_t slot_size(std::size_t k, std::size_t v) noexcept {
        return header_size() + align_up(k) + align_up(v);
    }
    constexpr std::size_t slot_size() const noexcept {
        return slot_size(key_len, val_len);
    }

    // Where the key body starts within the slot.
    constexpr std::size_t key_offset() const noexcept { return header_size(); }
    // Where the value body starts within the slot.
    constexpr std::size_t val_offset() const noexcept {
        return header_size() + align_up(key_len);
    }

    // Serialize into a pre-allocated slot. The slot must be at least
    // slot_size() bytes. Key and value are copied.
    void write_to(std::span<std::byte> slot,
                  std::span<const std::byte> key,
                  std::span<const std::byte> val) const noexcept;

    // Deserialize the header from a slot. The full record is just a span
    // over the slot — the header lives in the first 16 bytes.
    static Record read_header(std::span<const std::byte> slot) noexcept {
        Record r;
        std::memcpy(&r.version,  slot.data() + 0,  8);
        std::memcpy(&r.key_len,  slot.data() + 8,  2);
        std::memcpy(&r.val_len,  slot.data() + 10, 4);
        std::memcpy(&r.op,       slot.data() + 14, 1);
        std::memcpy(&r.checksum, slot.data() + 15, 1);
        return r;
    }
};

inline void Record::write_to(std::span<std::byte> slot,
                             std::span<const std::byte> key,
                             std::span<const std::byte> val) const noexcept {
    assert(slot.size() >= slot_size());
    std::memcpy(slot.data() + 0,  &version,  8);
    std::memcpy(slot.data() + 8,  &key_len,  2);
    std::memcpy(slot.data() + 10, &val_len,  4);
    std::memcpy(slot.data() + 14, &op,       1);
    std::memcpy(slot.data() + 15, &checksum, 1);
    std::memcpy(slot.data() + key_offset(),
                key.data(), std::min<std::size_t>(key.size(), key_len));
    std::memcpy(slot.data() + val_offset(),
                val.data(), std::min<std::size_t>(val.size(), val_len));
}

// ─── Pages (§3, §9) ───────────────────────────────────────────────────────
enum class PageStatus : std::uint8_t { Free = 0, Mutable = 1, ReadOnly = 2 };

struct Page {
    std::array<std::byte, kPageSize> bytes{};
    std::uint32_t   current_page_index = 0;   // which logical page this slot currently holds
    PageStatus      status             = PageStatus::Free;
    std::uint32_t   used_bytes         = 0;   // valid when status != Free

    // The "truth" the read path asks. If the slot's current_page_index
    // matches the requested page, the data is still here in RAM.
    bool holds(std::uint32_t page) const noexcept { return current_page_index == page; }
};

// ─── In-memory log (§3, §9, §10) ──────────────────────────────────────────
// A circular buffer of pages. The window is [head_page_, tail_page_).
// Both pointers advance monotonically EXCEPT during fold-over, which
// resets both (see reset_for_fold_over()).
class InMemoryLog {
public:
    InMemoryLog() : pages_(kPageCount) {
        for (std::uint32_t i = 0; i < kPageCount; ++i) {
            pages_[i].current_page_index = i;
            pages_[i].status             = PageStatus::Free;
        }
        // Open page 0 as the initial mutable page. The first append
        // would otherwise trip the "p.status == Mutable" assert.
        pages_[0].status = PageStatus::Mutable;
    }

    // ── Boundaries ────────────────────────────────────────────────────────
    std::uint32_t head_page() const noexcept { return head_page_; }
    std::uint32_t tail_page() const noexcept { return tail_page_; }
    std::uint32_t tail_off()  const noexcept { return tail_off_; }
    Address head_addr() const noexcept { return Address::encode(kSegRamReadOnly, head_page_, 0); }
    Address tail_addr() const noexcept { return Address::encode(kSegRamMutable,  tail_page_, tail_off_); }

    // ── The core read helper (§7) ─────────────────────────────────────────
    // Given an address that came from the hash index, decide where the
    // bytes actually live RIGHT NOW. The crucial step: ask the page slot
    // whether it still holds that page. If not, the data was flushed and
    // lives on disk.
    [[nodiscard]] Location locate(Address addr) const noexcept {
        const auto seg = addr.segment();

        // Segment == 0: in-memory mutable region
        if (seg == kSegRamMutable) {
            return Location::ram(addr.page(), addr.offset(), /*is_mutable=*/true);
        }
        // Segment == 1: in-memory read-only region (or already flushed)
        if (seg == kSegRamReadOnly) {
            const auto& slot = slot_for(addr.page());
            if (slot.holds(addr.page())) {
                return Location::ram(addr.page(), addr.offset(), /*is_mutable=*/false);
            }
            // Slot got recycled — the data is on disk.
            // TODO: compute disk segment from the page index. For a sketch,
            // we assume the on-disk log mirrors the in-memory log 1:1.
            return Location::disk(/*seg=*/kSegDiskBase, addr.page(), addr.offset());
        }
        // Segment >= 2: it was written straight to disk (e.g. a Copy record
        // placed there). In this sketch we never do that; treat as disk.
        return Location::disk(seg, addr.page(), addr.offset());
    }

    // ── Append path (§6) ──────────────────────────────────────────────────
    // Returns the address of the new record. The record is fully written
    // by the time this returns. If the current mutable page can't fit the
    // record, it is frozen and a new mutable page is opened.
    [[nodiscard]] Address append(Record rec,
                                 std::span<const std::byte> key,
                                 std::span<const std::byte> val) {
        const std::size_t slot = rec.slot_size();

        // Ensure the current mutable page has room.
        if (tail_off_ + slot > kPageSize) {
            freeze_current();
        }

        // Open the next mutable page if we don't have one.
        Page& p = pages_[tail_page_ % pages_.size()];
        assert(p.status == PageStatus::Mutable);
        assert(p.current_page_index == tail_page_);

        // Compute the address BEFORE writing the body (so we can stamp it
        // into the record's version field).
        const Address addr = Address::encode(kSegRamMutable, tail_page_, tail_off_);
        rec.version = addr.raw;

        // Write the record into the page.
        rec.write_to(std::span<std::byte>(p.bytes).subspan(tail_off_, slot), key, val);
        tail_off_ += static_cast<std::uint32_t>(slot);
        p.used_bytes = tail_off_;
        return addr;
    }

    // ── Freeze (§9) ───────────────────────────────────────────────────────
    // Mark the current mutable page as ReadOnly. The NEXT page becomes
    // mutable (allocated if it was Free, or reclaimed from disk if it
    // was holding a flushed page).
    void freeze_current() noexcept {
        Page& cur = pages_[tail_page_ % pages_.size()];
        assert(cur.status == PageStatus::Mutable);
        cur.status = PageStatus::ReadOnly;

        // Advance to next page.
        tail_page_ += 1;
        tail_off_  = 0;

        Page& nxt = pages_[tail_page_ % pages_.size()];
        // In a full implementation, if nxt is ReadOnly, we need to make
        // sure its data has been flushed to disk before overwriting it.
        // For single-threaded, we assert the invariant.
        assert(nxt.status == PageStatus::Free);
        nxt.status             = PageStatus::Mutable;
        nxt.current_page_index = tail_page_;
        nxt.used_bytes         = 0;
    }

    // ── Reclaim (§9) ──────────────────────────────────────────────────────
    // Mark a contiguous range of pages as Free. The caller (HybridLog)
    // has confirmed that no hash index entry still references any address
    // in this range. Typically called after a page range has been flushed
    // to disk and is no longer the "hot" window.
    void reclaim_range(std::uint32_t from_page, std::uint32_t to_page) noexcept {
        for (std::uint32_t p = from_page; p < to_page; ++p) {
            Page& slot = pages_[p % pages_.size()];
            slot.status             = PageStatus::Free;
            slot.used_bytes         = 0;
            // Note: we keep current_page_index set to its old value so that
            // a stale hash lookup (which would be a bug elsewhere) cannot
            // accidentally read wrong data from this slot.
        }
        if (to_page > head_page_) head_page_ = to_page;
    }

    // ── Fold-over reset (§10) ─────────────────────────────────────────────
    // After fold-over compacts the in-memory log, the live records are
    // at new addresses in a fresh window. The old window is abandoned.
    // We reset the log to start writing at `new_head_page`.
    void reset_for_fold_over(std::uint32_t new_head_page) noexcept {
        // Free everything in the current window. In a full impl, fold-over
        // would copy live records into the new window before calling this.
        for (auto& pg : pages_) {
            pg.status     = PageStatus::Free;
            pg.used_bytes = 0;
        }
        head_page_   = new_head_page;
        tail_page_   = new_head_page;
        tail_off_    = 0;
        Page& p = pages_[tail_page_ % pages_.size()];
        p.status             = PageStatus::Mutable;
        p.current_page_index = tail_page_;
        p.used_bytes         = 0;
    }

    // ── Read the full page bytes (kPageSize), assuming the slot still
    //    holds addr.page(). Use this from the read path: it always
    //    returns a full page so callers can index by record offset.
    std::span<const std::byte, kPageSize> read_page(Address addr) const noexcept {
        assert(addr.segment() == kSegRamMutable || addr.segment() == kSegRamReadOnly);
        const auto& slot = slot_for(addr.page());
        assert(slot.holds(addr.page()));
        return std::span<const std::byte, kPageSize>(slot.bytes);
    }

private:
    std::vector<Page> pages_;
    std::uint32_t     head_page_ = 0;
    std::uint32_t     tail_page_ = 0;
    std::uint32_t     tail_off_  = 0;

    const Page& slot_for(std::uint32_t page) const noexcept {
        return pages_[page % pages_.size()];
    }
    Page& slot_for(std::uint32_t page) noexcept {
        return pages_[page % pages_.size()];
    }
};

// ─── Hash index (§4) ──────────────────────────────────────────────────────
// Bucket array + overflow chains. The 16-bit tag is what makes the common
// case (no collision) a single cache-line load + one compare.
struct HashEntry {
    std::uint16_t tag = 0;       // top 16 bits of the hash
    Address       address{};     // where the latest version lives

    constexpr HashEntry() noexcept = default;
    constexpr HashEntry(std::uint16_t t, Address a) noexcept : tag(t), address(a) {}
};

class Bucket {
public:
    std::optional<HashEntry> find(std::uint64_t hash) const noexcept {
        const std::uint16_t tag = static_cast<std::uint16_t>(hash >> 48);
        if (primary_used_ && primary_.tag == tag) return primary_;
        for (const auto& e : overflow_) if (e.tag == tag) return e;
        return std::nullopt;
    }

    void upsert(std::uint64_t hash, Address addr) noexcept {
        const std::uint16_t tag = static_cast<std::uint16_t>(hash >> 48);
        if (primary_used_ && primary_.tag == tag) { primary_.address = addr; return; }
        for (auto& e : overflow_) if (e.tag == tag) { e.address = addr; return; }
        if (!primary_used_) { primary_ = {tag, addr}; primary_used_ = true; return; }
        overflow_.push_back({tag, addr});
    }

    // Read-only accessors for the iteration in HashIndex::for_each_live.
    bool                   primary_used()      const noexcept { return primary_used_; }
    const HashEntry&       primary_entry()     const noexcept { return primary_; }
    const std::vector<HashEntry>& overflow()  const noexcept { return overflow_; }

private:
    HashEntry              primary_{};
    bool                   primary_used_ = false;
    std::vector<HashEntry> overflow_;
};

class HashIndex {
public:
    explicit HashIndex(std::size_t buckets = kBucketCount) : buckets_(buckets) {}

    // FNV-1a 64-bit. NOT cryptographic. Replace with xxh3 in production.
    static std::uint64_t hash(std::span<const std::byte> key) noexcept {
        std::uint64_t h = 0xcbf29ce484222325ULL;
        for (auto b : key) {
            h ^= static_cast<std::uint8_t>(b);
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    [[nodiscard]] std::optional<Address> lookup(std::uint64_t hash) const {
        if (auto e = bucket_for(hash).find(hash); e) return e->address;
        return std::nullopt;
    }

    void upsert(std::uint64_t hash, Address addr) {
        bucket_for(hash).upsert(hash, addr);
    }

    // Iterate every live entry — used by fold-over (§10).
    template <typename F>
    void for_each_live(F&& f) const {
        for (const auto& b : buckets_) {
            if (b.primary_used()) f(b.primary_entry());
            for (const auto& e : b.overflow()) f(e);
        }
    }

    // Expose bucket internals for fold-over (read-only).
    const Bucket& bucket_at(std::size_t i) const { return buckets_[i]; }
    std::size_t   bucket_count() const noexcept { return buckets_.size(); }

private:
    std::vector<Bucket> buckets_;

    const Bucket& bucket_for(std::uint64_t hash) const {
        return buckets_[hash % buckets_.size()];
    }
    Bucket& bucket_for(std::uint64_t hash) {
        return buckets_[hash % buckets_.size()];
    }
};

// ─── On-disk log (§9, §11) ─────────────────────────────────────────────────
// A single append-only file for the sketch; production FASTER uses multiple
// segment files. The file layout is a flat sequence of kPageSize pages.
// The mapping from "page index in the address" to "byte offset in the file"
// is page * kPageSize — the on-disk log mirrors the in-memory log 1:1 in
// this simplified model.
class OnDiskLog {
public:
    explicit OnDiskLog(const std::filesystem::path& path)
        : path_(path), writer_(path, std::ios::binary | std::ios::app) {
        assert(writer_.is_open());
    }

    void append_page(std::span<const std::byte, kPageSize> page_bytes) {
        writer_.write(reinterpret_cast<const char*>(page_bytes.data()), kPageSize);
        pages_on_disk_ += 1;
    }

    [[nodiscard]] std::array<std::byte, kPageSize>
    read_page(std::uint32_t page) const {
        std::array<std::byte, kPageSize> out{};
        std::ifstream in(path_, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(page) * kPageSize);
        in.read(reinterpret_cast<char*>(out.data()), kPageSize);
        return out;
    }

    void sync() { writer_.flush(); }

    std::uint32_t pages_on_disk() const noexcept { return pages_on_disk_; }

private:
    std::filesystem::path              path_;
    std::ofstream                      writer_;
    std::uint32_t                      pages_on_disk_ = 0;
};

// ─── Read cache (§7 callout) ───────────────────────────────────────────────
// Plain LRU. The sketch returns copies to avoid lifetime issues; a
// production version would use a span over a stable backing store.
class ReadCache {
public:
    explicit ReadCache(std::size_t capacity = kReadCachePages) : capacity_(capacity) {}

    [[nodiscard]] std::optional<std::array<std::byte, kPageSize>>
    get(std::uint32_t page) {
        auto it = map_.find(page);
        if (it == map_.end()) return std::nullopt;
        // Move the key to the front of the LRU list.
        lru_.remove(page);
        lru_.push_front(page);
        return it->second;
    }

    void put(std::uint32_t page, const std::array<std::byte, kPageSize>& bytes) {
        if (map_.size() >= capacity_) {
            std::uint32_t evict = lru_.back();
            lru_.pop_back();
            map_.erase(evict);
        }
        map_[page] = bytes;
        lru_.push_front(page);
    }

private:
    std::size_t                                              capacity_;
    std::unordered_map<std::uint32_t, std::array<std::byte, kPageSize>> map_;
    std::list<std::uint32_t>                                 lru_;
};

// ─── The hybrid log store (§6, §7, §8, §10) ────────────────────────────────
// The top-level type. The user calls upsert/remove/read on this.
class HybridLog {
public:
    explicit HybridLog(const std::filesystem::path& disk_path)
        : disk_(disk_path) {}

    // ── Write (§6, §8) ────────────────────────────────────────────────────
    void upsert(std::string_view key, std::span<const std::byte> value) {
        const auto h = HashIndex::hash(as_bytes(key));
        Address addr = append_record(Op::Upsert, key, value);
        index_.upsert(h, addr);
    }

    void upsert(std::string_view key, std::string_view value) {
        upsert(key, as_bytes(value));
    }

    void remove(std::string_view key) {
        const auto h = HashIndex::hash(as_bytes(key));
        Address addr = append_record(Op::Delete, key, std::span<const std::byte>{});
        index_.upsert(h, addr);
    }

    // ── Read (§7) ─────────────────────────────────────────────────────────
    [[nodiscard]] std::optional<std::vector<std::byte>>
    read(std::string_view key) {
        const auto h = HashIndex::hash(as_bytes(key));
        auto addr = index_.lookup(h);
        if (!addr) return std::nullopt;
        return read_at(*addr, as_bytes(key));
    }

    // ── Test hooks (public for the smoke test) ──────────────────────────
    // In a real build, hide these behind a test-only friend declaration
    // or a debug-only interface. For a sketch, public is fine.
    [[nodiscard]] const InMemoryLog& mem()      const noexcept { return mem_; }
    [[nodiscard]] std::uint32_t      head_page() const noexcept { return mem_.head_page(); }
    [[nodiscard]] std::uint32_t      tail_page() const noexcept { return mem_.tail_page(); }

    // ── Maintenance (§9, §10) ─────────────────────────────────────────────
    // Freeze the current mutable page and flush it to disk.
    void maybe_flush() {
        // TODO: this is the freeze+flush path. In a full impl, you would
        // (1) freeze the current mutable page, (2) write its bytes to
        // disk via disk_.append_page(), (3) reclaim the in-memory slot
        // if no hash entry still references it. See §9.
    }

    void fold_over_if_needed() {
        // TODO: scan index_.for_each_live(), copy each live record into
        // a fresh in-memory region, update the hash index, and call
        // mem_.reset_for_fold_over(new_head_page). See §10.
    }

    void sync() { disk_.sync(); }

private:
    // ── Internal helpers ─────────────────────────────────────────────────

    static std::span<const std::byte> as_bytes(std::string_view s) noexcept {
        return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
    }

    // Build a record, write it into the in-memory log, return its Address.
    Address append_record(Op op, std::string_view key, std::span<const std::byte> val) {
        Record r;
        r.key_len = static_cast<std::uint16_t>(key.size());
        r.val_len = static_cast<std::uint32_t>(val.size());
        r.op      = op;
        // checksum = TODO: xxh3 of (key || val) once we wire it up
        return mem_.append(r, as_bytes(key), val);
    }

    // Read at an Address, chasing Copy records (§10) and resolving RAM
    // vs disk via mem_.locate().
    std::optional<std::vector<std::byte>>
    read_at(Address addr, std::span<const std::byte> want_key) {
        // Bound the chase — in a sketch, 4 is plenty.
        for (int hop = 0; hop < 4; ++hop) {
            const Location loc = mem_.locate(addr);

            // Read the page bytes.
            std::array<std::byte, kPageSize> page_buf{};
            switch (loc.tier) {
                case Location::Tier::RamMutable:
                case Location::Tier::RamReadOnly: {
                    const auto full_page = mem_.read_page(addr);
                    page_buf = {};
                    std::memcpy(page_buf.data(), full_page.data(), kPageSize);
                    break;
                }
                case Location::Tier::Disk: {
                    if (auto cached = read_cache_.get(loc.page)) {
                        page_buf = *cached;
                    } else {
                        page_buf = disk_.read_page(loc.page);
                        read_cache_.put(loc.page, page_buf);
                    }
                    break;
                }
            }

            // Parse the record header at the offset.
            const Record rec = Record::read_header(
                std::span<const std::byte>(page_buf).subspan(loc.offset));
            const std::span<const std::byte> slot_bytes(
                page_buf.data() + loc.offset,
                rec.slot_size());

            // Version check: the record's own address must match what
            // we read it from. If not, the page slot got recycled and
            // we'd be reading garbage — bail. (Production also handles
            // torn writes from the multi-threaded case; irrelevant here.)
            if (rec.version != addr.raw) {
                return std::nullopt;
            }

            const std::span<const std::byte> key_view(
                slot_bytes.data() + rec.key_offset(), rec.key_len);

            // Verify the key matches — catches hash collisions.
            if (!std::equal(key_view.begin(), key_view.end(),
                            want_key.begin(), want_key.end())) {
                return std::nullopt;
            }

            if (rec.op == Op::Upsert) {
                std::vector<std::byte> out(rec.val_len);
                std::memcpy(out.data(),
                            slot_bytes.data() + rec.val_offset(), rec.val_len);
                return out;
            }
            if (rec.op == Op::Delete) {
                return std::nullopt;
            }
            if (rec.op == Op::Copy) {
                // The Copy record's value body holds the original Address.
                // Read it and chase.
                Address target;
                std::memcpy(&target.raw,
                            slot_bytes.data() + rec.val_offset(), sizeof(Address));
                addr = target;
                continue;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    InMemoryLog  mem_;
    HashIndex    index_{kBucketCount};
    OnDiskLog    disk_;
    ReadCache    read_cache_;
};

} // namespace hybrid_log
