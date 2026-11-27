#!/usr/bin/env python3
"""Import fitted sections into the design record.

Reads the fitter's section files, one row of `b0 b1 b2 a1 a2` per section under a
`# rate <Hz>` header, and records them as exact hex doubles together with each
source file's sha256. Run once; the JSON it writes is the design of record.
"""

import argparse
import hashlib
import json
import os


def load(path):
    rate = None
    sections = []
    with open(path) as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                if " rate " in line:
                    rate = int(line.split()[-1])
                continue
            sections.append([float(token).hex() for token in line.split()])
    if rate is None or not sections:
        raise SystemExit("%s: no rate or no sections" % path)
    for row in sections:
        if len(row) != 5:
            raise SystemExit("%s: a section needs five coefficients" % path)
    return rate, sections


def sha256_of(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True, help="the design record to write")
    parser.add_argument("sources", nargs="+", help="section files, one per rate")
    args = parser.parse_args()

    designs = {}
    for path in sorted(args.sources):
        rate, sections = load(path)
        designs[str(rate)] = {
            "source": os.path.basename(path),
            "source_sha256": sha256_of(path),
            "fit": "LP minimax on |H|^2, out-of-tree scratch fitter; parameters unrecorded",
            "sections": sections,
        }

    record = {"schema": 2, "designs": designs}
    if os.path.exists(args.out):
        with open(args.out) as handle:
            previous = json.load(handle)
        if "target" in previous:
            record["target"] = previous["target"]
        for rate, design in designs.items():
            if rate in previous.get("designs", {}) and "measured" in previous["designs"][rate]:
                design["measured"] = previous["designs"][rate]["measured"]

    with open(args.out, "w") as handle:
        handle.write(json.dumps(record, sort_keys=True, indent=2) + "\n")
    for rate in sorted(designs, key=int):
        print("%s Hz  %d sections  %s" % (rate, len(designs[rate]["sections"]),
                                          designs[rate]["source_sha256"][:16]))


if __name__ == "__main__":
    main()
