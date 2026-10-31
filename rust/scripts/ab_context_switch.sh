#!/usr/bin/env bash
# A/B: C++ tinyio (baseline) vs tinyio-rs (candidate), context switch only.
# Follows AGENTS.md: rounds are interleaved (the arm order flips each round) and
# compared PAIRED per round, never by un-paired medians. Only release builds count.
#
#   CPP_BENCH=/path/to/context_switch_bench ROUNDS=15 CPU=2 scripts/ab_context_switch.sh
#
# CPP_BENCH comes from the C++ release preset (make release / make bench), e.g.
#   build/release/benchmarks/cpp/context_switch_bench
set -euo pipefail
cd "$(dirname "$0")/.."

CPP_BENCH=${CPP_BENCH:?set CPP_BENCH to the C++ context_switch_bench binary}
ROUNDS=${ROUNDS:-15}
ITERS=${ITERS:-10000000}
PIN=()
if [ -n "${CPU:-}" ] && command -v taskset >/dev/null; then PIN=(taskset -c "$CPU"); fi

cargo bench --bench context_switch --no-run 2>/dev/null
RUST_BENCH=$(ls -t target/release/deps/context_switch-* | grep -v '\.d$' | head -1)

cpp_once()  { "${PIN[@]}" "$CPP_BENCH" | awk '/ns\/switch/ {print $2}'; }
rust_once() { "${PIN[@]}" "$RUST_BENCH" --iters "$ITERS" --rounds 1 | awk -F= '/ns_per_switch/ {print $2}'; }

pairs=$(mktemp)
for r in $(seq 1 "$ROUNDS"); do
  if (( r % 2 )); then c=$(cpp_once); u=$(rust_once); else u=$(rust_once); c=$(cpp_once); fi
  echo "$r $c $u" | tee -a "$pairs" >&2
done

python3 -I - "$pairs" <<'PY'
import statistics as st, sys
rows = [tuple(map(float, l.split())) for l in open(sys.argv[1])]
ratios = [u / c for _, c, u in rows]          # < 1.0: Rust is faster in that round
print(f"rounds={len(rows)}")
print(f"C++  ns/switch: median {st.median(c for _, c, _ in rows):.2f}")
print(f"Rust ns/switch: median {st.median(u for _, _, u in rows):.2f}")
print(f"paired Rust/C++ ratio: median {st.median(ratios):.3f}  min {min(ratios):.3f}  max {max(ratios):.3f}")
print(f"rounds where Rust was faster: {sum(r < 1 for r in ratios)}/{len(ratios)}")
PY
rm -f "$pairs"
