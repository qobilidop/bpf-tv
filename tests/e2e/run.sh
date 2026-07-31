#!/usr/bin/env bash
# End-to-end test runner for bpf-tv.
#
# Positive tests: every tests/e2e/*.ll must validate as exactly
# "1 correct transformations".
#
# Negative control: a deliberately miscompiled assembly (wrong source
# register in one arm of branch.ll) must be reported as an incorrect
# transformation -- proving the oracle can actually fail.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-$ROOT/build}"
BPF_TV="${BPF_TV:-$BUILD_ROOT/alive2/bpf-tv/bpf-tv}"
TESTDIR="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$BPF_TV" ]; then
  echo "error: bpf-tv binary not found at $BPF_TV (set BPF_TV or BUILD_ROOT)" >&2
  exit 1
fi

fail=0

for ll in "$TESTDIR"/*.ll; do
  name="$(basename "$ll" .ll)"
  log="$WORK/$name.log"
  if "$BPF_TV" "$ll" >"$log" 2>&1 &&
     grep -q "^  1 correct transformations" "$log" &&
     grep -q "^  0 incorrect transformations" "$log" &&
     grep -q "^  0 failed-to-prove transformations" "$log"; then
    echo "PASS: $name"
  else
    echo "FAIL: $name (log: $log)"
    tail -6 "$log" | sed 's/^/    /'
    cp "$log" "$TESTDIR/$name.fail.log" 2>/dev/null || true
    fail=1
  fi
done

# negative control
neg_asm="$WORK/branch-mutated.s"
neg_log="$WORK/negative.log"
"$BPF_TV" --asm-only --asm-output="$neg_asm" "$TESTDIR/branch.ll" >/dev/null 2>&1
perl -0777 -pe 's/r0 = r1/r0 = r2/' "$neg_asm" > "$neg_asm.tmp"
if ! cmp -s "$neg_asm" "$neg_asm.tmp"; then
  mv "$neg_asm.tmp" "$neg_asm"
else
  echo "FAIL: negative-control (mutation did not apply)"
  fail=1
fi
if "$BPF_TV" --asm-input="$neg_asm" "$TESTDIR/branch.ll" >"$neg_log" 2>&1 &&
   grep -q "^  1 incorrect transformations" "$neg_log"; then
  echo "PASS: negative-control (miscompile detected)"
else
  echo "FAIL: negative-control (miscompile NOT detected; log: $neg_log)"
  tail -6 "$neg_log" | sed 's/^/    /'
  fail=1
fi

# second negative control: omit the zero-extension of a call result
# (the shape of the historical 2019 BPFMIPeephole wrong-code bug and of
# the -O3 freeze-refinement blind spot both) -- must be caught at the
# default optimization level
zx_asm="$WORK/zext-mutated.s"
zx_log="$WORK/negative-zext.log"
"$BPF_TV" --fn=f --asm-only --asm-output="$zx_asm" "$TESTDIR/zext_call.ll" >/dev/null 2>&1
grep -v "w0 = w0" "$zx_asm" > "$zx_asm.tmp" && mv "$zx_asm.tmp" "$zx_asm"
if "$BPF_TV" --fn=f --asm-input="$zx_asm" "$TESTDIR/zext_call.ll" >"$zx_log" 2>&1 &&
   grep -q "^  1 incorrect transformations" "$zx_log"; then
  echo "PASS: negative-control-zext (missing zext detected)"
else
  echo "FAIL: negative-control-zext (missing zext NOT detected; log: $zx_log)"
  tail -6 "$zx_log" | sed 's/^/    /'
  fail=1
fi

exit $fail
