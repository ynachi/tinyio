// smoke_test.cpp — minimal compilation + behavior check for hybrid_log.hpp.
// Build:   g++ -std=c++20 -O2 -Wall -Wextra smoke_test.cpp -o smoke_test
// Run:     ./smoke_test

#include "hybrid_log.hpp"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

using namespace hybrid_log;

int main() {
    // Use a per-run tempdir so re-runs are clean.
    auto dir = std::filesystem::temp_directory_path() /
               ("hl_smoke_" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);

    HybridLog store(dir / "log.bin");

    // ── 1. Basic upsert / read round-trip ──────────────────────────────
    store.upsert("hello", "world");
    auto got = store.read("hello");
    assert(got.has_value());
    assert(got->size() == 5);
    assert(std::string_view(reinterpret_cast<const char*>(got->data()), got->size()) == "world");
    std::printf("✓ basic upsert/read works\n");

    // ── 2. Overwrite keeps the latest value ────────────────────────────
    store.upsert("hello", "WORLD");
    got = store.read("hello");
    assert(got.has_value());
    assert(std::string_view(reinterpret_cast<const char*>(got->data()), got->size()) == "WORLD");
    std::printf("✓ overwrite returns latest value\n");

    // ── 3. Delete returns nullopt ──────────────────────────────────────
    store.remove("hello");
    got = store.read("hello");
    assert(!got.has_value());
    std::printf("✓ remove makes read return nullopt\n");

    // ── 4. Multiple keys, including hash-unfriendly ones ──────────────
    store.upsert("alpha", "1");
    store.upsert("beta",  "22");
    store.upsert("gamma", "333");
    store.upsert("a key with spaces and \xff\x00 binary", "binary value");
    assert(store.read("alpha").has_value());
    assert(store.read("beta").has_value());
    assert(store.read("gamma").has_value());
    assert(store.read("a key with spaces and \xff\x00 binary").has_value());
    std::printf("✓ multiple keys coexist\n");

    // ── 5. Many upserts, force page freezing ──────────────────────────
    for (int i = 0; i < 5000; ++i) {
        store.upsert("k" + std::to_string(i), "v" + std::to_string(i));
    }
    for (int i = 0; i < 5000; ++i) {
        auto v = store.read("k" + std::to_string(i));
        assert(v.has_value());
    }
    std::printf("✓ 5k upserts and reads\n");

    // ── 6. Locate() returns the right tier for the head of the log ─────
    auto loc = store.mem().locate(Address::encode(kSegRamMutable, store.tail_page(), 0));
    assert(loc.tier == Location::Tier::RamMutable);
    std::printf("✓ locate() identifies RamMutable for tail\n");

    store.sync();

    // ── 7. Clean shutdown via tempdir ─────────────────────────────────
    std::filesystem::remove_all(dir);
    std::printf("\nAll smoke tests passed.\n");
    return 0;
}
