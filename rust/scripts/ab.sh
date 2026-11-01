#!/usr/bin/env bash
# A/B: C++ tinyio (baseline) vs tinyio-rs (candidate) on one benchmark.
# Follows AGENTS.md: rounds are interleaved (the arm order flips each round) and
# compared PAIRED per round, never by un-paired medians. Only release builds count.
#
#   CPP_DIR=build/release/benchmarks/cpp ROUNDS=15 CPU=2 scripts/ab.sh context_switch
#   CPP_DIR=build/release/benchmarks/cpp ROUNDS=15 CPU=2 scripts/ab.sh pingpong
#   CPP_DIR=build/release/benchmarks/cpp ROUNDS=15 CPU=2 scripts/ab.sh sync
#   CPP_DIR=build/release/benchmarks/cpp ROUNDS=15 scripts/ab.sh post   # multi-threaded: do not pin
#
#   CPP_DIR=... DISK_DIR=/mnt/nvme ROUNDS=7 scripts/ab.sh disk_read    # 4 KiB O_DIRECT, IOPS and p99
#   CPP_DIR=... DISK_DIR=/mnt/nvme ROUNDS=5 scripts/ab.sh disk_write   # 64 KiB O_DIRECT, MiB/s
#
# Disk benches (value = time per op, so lower is better like the others): each
# arm runs in a fresh directory under DISK_DIR (default /tmp) and creates its
# own data file just before it measures, as AGENTS.md requires. Point DISK_DIR
# at the device you mean to measure; /tmp is often RAM. OPS, REPEATS, FILE_MIB
# (read) and BYTES (write, default 256M) set the sizes.
#
# EXTRA_RUST_ARGS / EXTRA_CPP_ARGS are appended to the disk_read arguments
# (e.g. EXTRA_RUST_ARGS=--no-verify with a C++ build that also skips the check).
# TIMEOUT_MS=30000 (pingpong only) puts that timeout on every read and write.
# CPP_DIR is where the C++ release preset put its benchmark binaries
# (context_switch_bench, pingpong_bench). ITERS scales the Rust arm only; the
# C++ benches have their own fixed sizes, so keep ITERS at the default unless
# you also change them. For disk benches: recreate the data file right before
# the run (AGENTS.md), not before the whole experiment.
set -euo pipefail
cd "$(dirname "$0")/.."

BENCH=${1:?usage: ab.sh context_switch|pingpong|sync|post|disk_read|disk_write}
CPP_DIR=${CPP_DIR:?set CPP_DIR to the C++ release benchmark directory}
ROUNDS=${ROUNDS:-15}
case "$BENCH" in
  context_switch) ITERS=${ITERS:-10000000}; CPP_ARGS=() ;;
  pingpong)       ITERS=${ITERS:-200000};   CPP_ARGS=("$ITERS" "${TIMEOUT_MS:-0}") ;;
  sync|post)      ITERS=1;                  CPP_ARGS=() ;;
  disk_read)      ITERS=1; CPP_ARGS=(--operations "${OPS:-20000}" --repeats "${REPEATS:-3}" --file-mib "${FILE_MIB:-64}") ;;
  disk_write)     ITERS=1; CPP_ARGS=() ;;
  *) echo "unknown bench: $BENCH" >&2; exit 2 ;;
esac
PIN=()
if [ -n "${CPU:-}" ] && command -v taskset >/dev/null; then PIN=(taskset -c "$CPU"); fi

cargo bench --bench "$BENCH" --no-run 2>/dev/null
RUST_BENCH=$PWD/$(ls -t target/release/deps/"$BENCH"-* | grep -v '\.d$' | head -1)

# Each arm prints "name<TAB>ns/op" lines; single-metric benches use the name "ns/op".
labels() { sed -nE 's/^(.*): ([0-9.e+-]+) ns\/op$/\1\t\2/p'; }
# The disk benches print tables; turn each row into "name<TAB>ns" (lower is better).
disk_rows() {
  awk '
    $1 == "shared" || $1 == "independent" { printf "%s QD%s iops\t%.1f\n", $1, $3, 1e9 / $4;
                                            printf "%s QD%s p99\t%.1f\n", $1, $3, $7 * 1000 }
    $1 ~ /^fibers=/ && $4 == "MiB/s"      { printf "%s\t%.1f\n", $1, 1e9 / $3 }'
}
# Run an arm in a fresh directory so each starts from a freshly created data file.
in_fresh_dir() {
  local dir; dir=$(mktemp -d "${DISK_DIR:-/tmp}/tinyio-ab.XXXXXX")
  ( cd "$dir" && "$@" ) 2>/dev/null
  local rc=$?; rm -rf "$dir"; return $rc
}
cpp_once() {
  case "$BENCH" in
    disk_read)  in_fresh_dir "${PIN[@]}" "$CPP_DIR/tinyio_disk_read_bench" "${CPP_ARGS[@]}" ${EXTRA_CPP_ARGS:-} | disk_rows ;;
    disk_write) local d; d=$(mktemp -d "${DISK_DIR:-/tmp}/tinyio-ab.XXXXXX")
                "${PIN[@]}" "$CPP_DIR/tinyio_disk_bench" --path="$d/w.dat" --bytes="${BYTES:-256M}" 2>/dev/null | disk_rows; rm -rf "$d" ;;
    sync|post) "${PIN[@]}" "$CPP_DIR/${BENCH}_bench" 2>/dev/null | labels ;;
    *)    "${PIN[@]}" "$CPP_DIR/${BENCH}_bench" "${CPP_ARGS[@]}" 2>/dev/null | awk '/^ns\// {print "ns/op\t"$2}' ;;
  esac
}
rust_once() {
  case "$BENCH" in
    disk_read)  in_fresh_dir "${PIN[@]}" "$RUST_BENCH" --operations "${OPS:-20000}" --repeats "${REPEATS:-3}" --file-mib "${FILE_MIB:-64}" ${EXTRA_RUST_ARGS:-} | disk_rows ;;
    disk_write) local d; d=$(mktemp -d "${DISK_DIR:-/tmp}/tinyio-ab.XXXXXX")
                "${PIN[@]}" "$RUST_BENCH" --path="$d/w.dat" --bytes="${BYTES:-256M}" 2>/dev/null | disk_rows; rm -rf "$d" ;;
    sync|post) "${PIN[@]}" "$RUST_BENCH" | labels ;;
    *)    "${PIN[@]}" "$RUST_BENCH" --iters "$ITERS" --rounds 1 --timeout-ms "${TIMEOUT_MS:-0}" | awk -F= '/ns_per_op/ {print "ns/op\t"$2}' ;;
  esac
}

pairs=$(mktemp)
for r in $(seq 1 "$ROUNDS"); do
  if (( r % 2 )); then c=$(cpp_once); u=$(rust_once); else u=$(rust_once); c=$(cpp_once); fi
  { echo "$c" | sed "s/^/$r\tC\t/"; echo "$u" | sed "s/^/$r\tR\t/"; } >> "$pairs"
  echo "round $r done" >&2
done

python3 -I - "$pairs" "$BENCH" <<'PY'
import statistics as st, sys
from collections import defaultdict
runs = defaultdict(lambda: defaultdict(dict))        # name -> round -> {"C": v, "R": v}
order = []
for line in open(sys.argv[1]):
    rnd, arm, name, val = line.rstrip("\n").split("\t")
    if name not in order:
        order.append(name)
    runs[name][int(rnd)][arm] = float(val)
print(f"bench={sys.argv[2]}")
for name in order:
    rows = [v for v in runs[name].values() if "C" in v and "R" in v]
    c = st.median(v["C"] for v in rows)
    u = st.median(v["R"] for v in rows)
    ratios = [v["R"] / v["C"] for v in rows]          # < 1.0: Rust is faster that round
    # A whole operation under ~1 ns in C++ may mean the compiler deleted the loop.
    note = "  [C++ < 1 ns: check the loop was not optimised away]" if c < 1.0 else ""
    print(f"{name}: C++ {c:.2f}  Rust {u:.2f} ns/op  paired Rust/C++ median {st.median(ratios):.3f} "
          f"(min {min(ratios):.3f}, max {max(ratios):.3f})  Rust faster {sum(r < 1 for r in ratios)}/{len(ratios)}{note}")
PY
rm -f "$pairs"
