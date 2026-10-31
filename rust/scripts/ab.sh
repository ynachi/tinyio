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
# CPP_DIR is where the C++ release preset put its benchmark binaries
# (context_switch_bench, pingpong_bench). ITERS scales the Rust arm only; the
# C++ benches have their own fixed sizes, so keep ITERS at the default unless
# you also change them. For disk benches: recreate the data file right before
# the run (AGENTS.md), not before the whole experiment.
set -euo pipefail
cd "$(dirname "$0")/.."

BENCH=${1:?usage: ab.sh context_switch|pingpong|sync|post}
CPP_DIR=${CPP_DIR:?set CPP_DIR to the C++ release benchmark directory}
ROUNDS=${ROUNDS:-15}
case "$BENCH" in
  context_switch) ITERS=${ITERS:-10000000}; CPP_ARGS=() ;;
  pingpong)       ITERS=${ITERS:-200000};   CPP_ARGS=("$ITERS") ;;
  sync|post)      ITERS=1;                  CPP_ARGS=() ;;
  *) echo "unknown bench: $BENCH" >&2; exit 2 ;;
esac
PIN=()
if [ -n "${CPU:-}" ] && command -v taskset >/dev/null; then PIN=(taskset -c "$CPU"); fi

cargo bench --bench "$BENCH" --no-run 2>/dev/null
RUST_BENCH=$(ls -t target/release/deps/"$BENCH"-* | grep -v '\.d$' | head -1)

# Each arm prints "name<TAB>ns/op" lines; single-metric benches use the name "ns/op".
labels() { sed -nE 's/^(.*): ([0-9.e+-]+) ns\/op$/\1\t\2/p'; }
cpp_once() {
  case "$BENCH" in
    sync|post) "${PIN[@]}" "$CPP_DIR/${BENCH}_bench" 2>/dev/null | labels ;;
    *)    "${PIN[@]}" "$CPP_DIR/${BENCH}_bench" "${CPP_ARGS[@]}" 2>/dev/null | awk '/^ns\// {print "ns/op\t"$2}' ;;
  esac
}
rust_once() {
  case "$BENCH" in
    sync|post) "${PIN[@]}" "$RUST_BENCH" | labels ;;
    *)    "${PIN[@]}" "$RUST_BENCH" --iters "$ITERS" --rounds 1 | awk -F= '/ns_per_op/ {print "ns/op\t"$2}' ;;
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
