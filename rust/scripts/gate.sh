#!/usr/bin/env bash
# The review gate from docs/architecture-plan.md (section 9), as a script.
# Run from anywhere; exits non-zero with the offending lines on any violation.
#
#   1. no `pub unsafe` anywhere in src/ (Rule 0)
#   2. `unsafe` appears only in the modules the unsafe budget lists (section 5)
#   3. every `unsafe {` block has a `// SAFETY:` comment within 8 lines above it
#   4. no panic!, RefCell, catch_unwind or thread_local! outside #[cfg(test)] (Rules 5, 8)
#   5. tests/, benches/ and examples/ start with #![forbid(unsafe_code)] (Rule 0)
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0
report() { echo "gate: $1" >&2; fail=1; }

# The unsafe budget: the only files allowed to contain the word `unsafe` in code.
allowed='^src/(detail/(context|stack|fiber)|sched|ring|cx|sync/(waiters|scope))\.rs$'

# Strip everything from the first #[cfg(test)] to the end of the file: tests
# live at the bottom of their module and may use test-only constructs.
strip_tests() { awk '/^#\[cfg\(test\)\]/ {exit} {print}' "$1"; }

# 1
if grep -rnE 'pub +unsafe' src; then
  report "pub unsafe item found (Rule 0)"
fi

mapfile -t files < <(find src -name '*.rs' | sort)
for f in "${files[@]}"; do
  body="$(strip_tests "$f")"

  # 2: `unsafe` as code (ignore comments and doc lines)
  if echo "$body" | grep -vE '^\s*//' | grep -qE '\bunsafe\b'; then
    if ! [[ "$f" =~ $allowed ]]; then
      report "$f: contains unsafe but is not in the unsafe budget"
    fi
  fi

  # 3
  while IFS=: read -r n _; do
    start=$(( n > 8 ? n - 8 : 1 ))
    if ! sed -n "${start},${n}p" "$f" | grep -q 'SAFETY:'; then
      report "$f:$n: unsafe block without a // SAFETY: comment"
    fi
  done < <(echo "$body" | grep -nE '\bunsafe *\{' | grep -vE '^[0-9]+:\s*//' || true)

  # 4
  if echo "$body" | grep -vE '^\s*//' | grep -nE '\bpanic!|\bRefCell\b|\bcatch_unwind\b|\bthread_local!' ; then
    report "$f: forbidden construct (Rules 5/8)"
  fi
done

# 5
for f in $(find tests benches examples -name '*.rs' 2>/dev/null | sort); do
  # tests/common/mod.rs style helpers are not crate roots
  case "$f" in */common/*|*/support/*) continue ;; esac
  if ! head -n 5 "$f" | grep -q '#!\[forbid(unsafe_code)\]'; then
    report "$f: missing #![forbid(unsafe_code)]"
  fi
done

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "gate: ok"
