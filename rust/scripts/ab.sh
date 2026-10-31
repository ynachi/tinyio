#!/usr/bin/env bash
# A/B: C++ tinyio (baseline) vs tinyio-rs (candidate) on one benchmark.
# Follows AGENTS.md: rounds are interleaved (the arm order flips each round) and
# compared PAIRED per round, never by un-paired medians. Only release builds count.
#
#   CPP_DIR=build/release/benchmarks/cpp ROUNDS=15 CPU=2 scripts/ab.sh context_switch
#   CPP_DIR=build/release/benchmarks/cpp ROUNDS=15 CPU=2 scripts/ab.sh pingpong
#
# CPP_DIR is where the C++ release preset put its benchmark binaries
# (context_switch_bench, pingpong_bench). ITERS scales the Rust arm only; the
# C++ benches have their own fixed sizes, so keep ITERS at the default unless
# you also change them. For disk benches: recreate the data file right before
# the run (AGENTS.md), not before the whole experiment.
set -euo pipefail
cd "$(dirname "$0")/.."

BENCH=${1:?usage: ab.sh context_switch|pingpong}
CPP_DIR=${CPP_DIR:?set CPP_DIR to the C++ release benchmark directory}
ROUNDS=${ROUNDS:-15}
case "$BENCH" in
  context_switch) ITERS=${ITERS:-10000000}; CPP_ARGS=() ;;
  pingpong)       ITERS=${ITERS:-200000};   CPP_ARGS=("$ITERS") ;;
  *) echo "unknown bench: $BENCH" >&2; exit 2 ;;
esac
PIN=()
if [ -n "${CPU:-}" ] && command -v taskset >/dev/null; then PIN=(taskset -c "$CPU"); fi

cargo bench --bench "$BENCH" --no-run 2>/dev/null
RUST_BENCH=$(ls -t target/release/deps/"$BENCH"-* | grep -v '\.d$' | head -1)

cpp_once()  { "${PIN[@]}" "$CPP_DIR/${BENCH}_bench" "${CPP_ARGS[@]}" | awk '/^ns\// {print $2}'; }
rust_once() { "${PIN[@]}" "$RUST_BENCH" --iters "$ITERS" --rounds 1 | awk -F= '/ns_per_op/ {print $2}'; }

pairs=$(mktemp)
for r in $(seq 1 "$ROUNDS"); do
  if (( r % 2 )); then c=$(cpp_once); u=$(rust_once); else u=$(rust_once); c=$(cpp_once); fi
  echo "$r $c $u" | tee -a "$pairs" >&2
done

python3 -I - "$pairs" "$BENCH" <<'PY'
import statistics as st, sys
rows = [tuple(map(float, l.split())) for l in open(sys.argv[1])]
ratios = [u / c for _, c, u in rows]          # < 1.0: Rust is faster in that round
print(f"bench={sys.argv[2]} rounds={len(rows)}")
print(f"C++  ns/op: median {st.median(c for _, c, _ in rows):.1f}")
print(f"Rust ns/op: median {st.median(u for _, _, u in rows):.1f}")
print(f"paired Rust/C++ ratio: median {st.median(ratios):.3f}  min {min(ratios):.3f}  max {max(ratios):.3f}")
print(f"rounds where Rust was faster: {sum(r < 1 for r in ratios)}/{len(ratios)}")
PY
rm -f "$pairs"
