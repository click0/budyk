#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Line coverage per module from an lcov .info file.

Prints a table of src/<module>/ coverage, writes it as Markdown to
$GITHUB_STEP_SUMMARY when that is set, and exits 1 if any module given
with --min is below its threshold. Spec §5 item 10 asks for >= 85% on
core/ and storage/:

    tests/coverage_report.py coverage.info --min core=85 --min storage=85
"""
import argparse
import os
import sys
from collections import defaultdict


def read_lines(path):
    """{source file: {line: hits}}; a line hit by any test counts."""
    files = {}
    current = None
    with open(path) as f:
        for raw in f:
            line = raw.strip()
            if line.startswith("SF:"):
                current = files.setdefault(line[3:], {})
            elif line.startswith("DA:") and current is not None:
                number, hits = line[3:].split(",")[:2]
                number, hits = int(number), int(hits)
                current[number] = max(current.get(number, 0), hits)
            elif line == "end_of_record":
                current = None
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("info")
    ap.add_argument("--min", action="append", default=[], metavar="MODULE=PCT",
                    help="fail if src/MODULE/ line coverage is below PCT")
    args = ap.parse_args()

    modules = defaultdict(lambda: [0, 0])   # module -> [hit, total]
    for path, lines in read_lines(args.info).items():
        if "/src/" not in path or not lines:
            continue
        rel = path.split("/src/", 1)[1]
        module = rel.split("/")[0] if "/" in rel else rel   # main.cpp stays itself
        modules[module][0] += sum(1 for h in lines.values() if h > 0)
        modules[module][1] += len(lines)

    thresholds = {}
    for item in args.min:
        name, _, pct = item.partition("=")
        thresholds[name] = float(pct)

    rows = []
    failed = []
    for name, (hit, total) in sorted(modules.items()):
        pct = 100.0 * hit / total
        need = thresholds.get(name)
        status = ""
        if need is not None:
            status = "ok" if pct >= need else "BELOW %.0f%%" % need
            if pct < need:
                failed.append(name)
        rows.append((name, pct, hit, total, status))
    for name in thresholds:
        if name not in modules:
            rows.append((name, 0.0, 0, 0, "NO DATA"))
            failed.append(name)

    hit = sum(r[2] for r in rows)
    total = sum(r[3] for r in rows)
    print("%-14s %7s %13s  %s" % ("module", "lines", "hit/total", ""))
    for name, pct, h, t, status in rows:
        print("%-14s %6.1f%% %13s  %s" % (name, pct, "%d/%d" % (h, t), status))
    print("%-14s %6.1f%% %13s" % ("total", 100.0 * hit / max(total, 1), "%d/%d" % (hit, total)))

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write("### Line coverage\n\n| module | lines | hit/total | |\n|---|---:|---:|---|\n")
            for name, pct, h, t, status in rows:
                f.write("| %s | %.1f%% | %d/%d | %s |\n" % (name, pct, h, t, status))

    if failed:
        print("coverage below threshold: " + ", ".join(failed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
