#!/usr/bin/env python3
"""Sweep a corpus of .ll files through bpf-tv and report a per-function
outcome taxonomy.

Outcomes:
  verified          proved refinement ("1 correct transformations")
  INCORRECT         bpf-tv reports a miscompilation -- investigate!
  failed-to-prove   solver timeout / gave up
  unsupported:<r>   bpf-tv rejected the function (reason bucket)
  asm-parse-error   BPF MC asm parser rejected the backend's output
  input-error       .ll didn't parse / no functions found
  crash             process died on a signal / assertion
  timeout           wall-clock cap hit
  other             unclassified (inspect manually)

Emits a JSONL record per function and a markdown summary.
"""

import argparse
import concurrent.futures
import json
import pathlib
import re
import signal
import subprocess
import sys
import time
from collections import Counter, defaultdict

DEFINE_RE = re.compile(r"^define\b[^@]*@([A-Za-z0-9$._][-A-Za-z0-9$._]*)\s*\(",
                       re.MULTILINE)

ERROR_BUCKETS = [
    ("BPF backend reported an error", "backend-error"),
    ("Unsupported instruction: ", "unsupported-insn"),
    ("vectors not supported", "unsupported:vector"),
    ("floating point not supported", "unsupported:float"),
    ("varargs not supported", "unsupported:varargs"),
    ("more than 5 arguments", "unsupported:many-args"),
    ("calls with more than 5 arguments", "unsupported:many-args"),
    ("inline assembly not supported", "unsupported:inline-asm"),
    ("atomics not supported", "unsupported:atomics"),
    ("address spaces other than 0", "unsupported:addrspace"),
    ("integers wider than 64", "unsupported:wide-int"),
    ("only integer/pointer arguments", "unsupported:arg-type"),
    ("aggregate return", "unsupported:aggregate"),
    ("structures in arguments", "unsupported:aggregate"),
    ("arrays in arguments", "unsupported:aggregate"),
    ("structures in return", "unsupported:aggregate"),
    ("arrays in return", "unsupported:aggregate"),
    ("Stack frame too large", "unsupported:stack-size"),
    ("escapes to a callee", "unsupported:stack-escape"),
    ("thread_local not supported", "unsupported:thread-local"),
    ("volatiles not supported", "unsupported:volatile"),
    ("only static allocas", "unsupported:dynamic-alloca"),
    ("byval parameter", "unsupported:byval"),
    ("Unsupported Function Return", "unsupported:ret-type"),
    ("Scalar return values larger than 64", "unsupported:ret-type"),
    ("Unsupported function argument", "unsupported:arg-type"),
    ("personality functions", "unsupported:personality"),
    ("AsmParser failed", "asm-parse-error"),
    ("global symbol '", "unsupported:global-lookup"),
    ("Unknown global in", "unsupported:global-lookup"),
    ("can't locate corresponding source-side call", "unsupported:call-mapping"),
    ("recursion", "unsupported:recursion"),
]


def functions_in(ll_path: pathlib.Path):
    try:
        text = ll_path.read_text(errors="replace")
    except OSError:
        return None
    return DEFINE_RE.findall(text)


def classify(rc: int, out: str, timed_out: bool):
    if timed_out:
        return "timeout", ""
    if re.search(r"^  1 incorrect transformations", out, re.M):
        return "INCORRECT", ""
    if re.search(r"^  1 correct transformations", out, re.M):
        return "verified", ""
    if re.search(r"^  1 failed-to-prove transformations", out, re.M):
        return "failed-to-prove", ""
    for needle, bucket in ERROR_BUCKETS:
        if needle in out:
            if bucket == "unsupported-insn":
                # our lifter's visitError prints a bare opcode name;
                # alive2's llvm2alive prints the whole source IR
                # instruction (leading spaces) -- different bucket
                m = re.search(r"Unsupported instruction: (\w\S*)\s*$", out,
                              re.M)
                if m:
                    return "unsupported:insn", m.group(1)
                m = re.search(r"Unsupported instruction:\s+(\S+)", out)
                return ("unsupported:src-ir",
                        m.group(1)[:40] if m else "?")
            return bucket, ""
    if rc < 0 or "Assertion failed" in out or "Stack dump" in out \
            or "PLEASE submit a bug report" in out:
        return "crash", f"rc={rc}"
    if "Could not read bitcode" in out or "error: expected" in out:
        return "input-error", ""
    if "ERROR:" in out:
        m = re.search(r"ERROR: (.*)", out)
        return "other", (m.group(1)[:80] if m else "")
    return "other", f"rc={rc}"


def run_one(bpf_tv, ll, fn, smt_to, wall_cap, extra_args=()):
    cmd = [str(bpf_tv), f"--fn={fn}", f"--smt-to={smt_to}",
           *extra_args, str(ll)]
    start = time.monotonic()
    timed_out = False
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=wall_cap)
        rc, out = p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        timed_out = True
        rc = -signal.SIGKILL
        out = ((e.stdout or b"").decode(errors="replace") if isinstance(
            e.stdout, bytes) else (e.stdout or "")) + \
              ((e.stderr or b"").decode(errors="replace") if isinstance(
                  e.stderr, bytes) else (e.stderr or ""))
    wall = time.monotonic() - start
    outcome, detail = classify(rc, out, timed_out)
    return {
        "file": str(ll),
        "fn": fn,
        "outcome": outcome,
        "detail": detail,
        "wall_s": round(wall, 3),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", type=pathlib.Path,
                    default=pathlib.Path(
                        "third_party/llvm-project/llvm/test/CodeGen/BPF"))
    ap.add_argument("--bpf-tv", type=pathlib.Path,
                    default=pathlib.Path("build/alive2/bpf-tv/bpf-tv"))
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--smt-to", type=int, default=10000, help="ms")
    ap.add_argument("--wall-cap", type=int, default=120, help="s")
    ap.add_argument("--recursive", action="store_true")
    ap.add_argument("--out", type=pathlib.Path, default=None,
                    help="output directory (default: build/eval)")
    ap.add_argument("--extra-arg", action="append", default=[],
                    help="extra bpf-tv flag (repeatable)")
    args = ap.parse_args()

    outdir = args.out or pathlib.Path("build/eval")
    outdir.mkdir(parents=True, exist_ok=True)

    files = sorted(args.corpus.rglob("*.ll") if args.recursive
                   else args.corpus.glob("*.ll"))
    if not files:
        print(f"no .ll files under {args.corpus}", file=sys.stderr)
        return 1

    work = []
    records = []
    for ll in files:
        fns = functions_in(ll)
        if not fns:
            records.append({"file": str(ll), "fn": None,
                            "outcome": "input-error",
                            "detail": "no parseable define lines",
                            "wall_s": 0.0})
            continue
        for fn in fns:
            work.append((ll, fn))

    print(f"{len(files)} files, {len(work)} functions, "
          f"{args.jobs} workers, smt-to={args.smt_to}ms, "
          f"wall-cap={args.wall_cap}s")

    done = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        futs = {ex.submit(run_one, args.bpf_tv, ll, fn,
                          args.smt_to, args.wall_cap,
                          tuple(args.extra_arg)): (ll, fn)
                for ll, fn in work}
        for fut in concurrent.futures.as_completed(futs):
            records.append(fut.result())
            done += 1
            if done % 50 == 0:
                print(f"  ...{done}/{len(work)}")

    jsonl = outdir / "corpus-results.jsonl"
    with open(jsonl, "w") as f:
        for r in sorted(records, key=lambda r: (r["file"], r["fn"] or "")):
            f.write(json.dumps(r) + "\n")

    # summary
    counts = Counter(r["outcome"] for r in records)
    insn_hist = Counter(r["detail"] for r in records
                        if r["outcome"] == "unsupported:insn")
    times = sorted(r["wall_s"] for r in records if r["outcome"] == "verified")
    incorrect = [r for r in records if r["outcome"] == "INCORRECT"]
    slow = sorted((r for r in records if r["outcome"] == "verified"),
                  key=lambda r: -r["wall_s"])[:10]

    lines = []
    lines.append(f"# bpf-tv corpus run: {args.corpus}")
    lines.append("")
    lines.append(f"- {len(files)} files, {len(records)} functions")
    lines.append(f"- smt-to={args.smt_to}ms, wall-cap={args.wall_cap}s")
    lines.append("")
    lines.append("| outcome | count | % |")
    lines.append("|---|---|---|")
    total = len(records)
    for outcome, n in counts.most_common():
        lines.append(f"| {outcome} | {n} | {100 * n / total:.1f}% |")
    if insn_hist:
        lines.append("")
        lines.append("## Unsupported-instruction histogram")
        lines.append("")
        lines.append("| opcode | count |")
        lines.append("|---|---|")
        for insn, n in insn_hist.most_common():
            lines.append(f"| {insn} | {n} |")
    if times:
        lines.append("")
        lines.append("## Verification time (verified functions)")
        lines.append("")
        mid = times[len(times) // 2]
        p90 = times[int(len(times) * 0.9)]
        lines.append(f"- n={len(times)}, median={mid:.2f}s, "
                     f"p90={p90:.2f}s, max={times[-1]:.2f}s")
        lines.append("")
        lines.append("Slowest verified:")
        for r in slow:
            lines.append(f"- {r['wall_s']:.1f}s "
                         f"{pathlib.Path(r['file']).name}:{r['fn']}")
    if incorrect:
        lines.append("")
        lines.append("## INCORRECT transformations (investigate!)")
        lines.append("")
        for r in incorrect:
            lines.append(f"- {r['file']}:{r['fn']}")
    report = "\n".join(lines) + "\n"
    (outdir / "corpus-report.md").write_text(report)
    print()
    print(report)
    print(f"records: {jsonl}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
