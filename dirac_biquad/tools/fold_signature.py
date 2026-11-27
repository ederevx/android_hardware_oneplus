#!/usr/bin/env python3
"""Fold the recorded sections into the single filter the module runs.

The sections are multiplied into one filter and written in its parallel form:
a direct term plus one real second-order term per pole, so nothing is cascaded
at runtime. Each term keeps its own section's denominator verbatim, so no
polynomial is expanded and no root is re-derived from coefficients.

The fold is computed in mpmath at 60 digits and rounded to double once, so the
emitted bytes are the same on every host. `--check` regenerates in memory and
compares the result with the committed header byte for byte.
"""

import argparse
import hashlib
import json
import os
import sys

import mpmath as mp

mp.mp.dps = 60

RECOMPOSITION_TOLERANCE = 1e-12
GRID_POINTS = 4096
GRID_HIGH_FRACTION = 0.4999
LEVEL_HZ = 1000.0


def sha256_of(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def sha256_of_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sections_of(hex_rows):
    return [[mp.mpf(float.fromhex(value)) for value in row] for row in hex_rows]


def roots(a1, a2):
    """The two poles of z^2 + a1 z + a2, as exact mpmath numbers."""
    discriminant = a1 * a1 - 4 * a2
    if discriminant == 0:
        raise SystemExit("double pole in a section: the fold needs distinct poles")
    root = mp.sqrt(discriminant)
    return (-a1 + root) / 2, (-a1 - root) / 2


def residue(sections, index, pole):
    """The residue of section `index` at `pole`, from the factored product.

    Uses the (2z + a1) form of the derivative, so nothing divides by the pole.
    """
    numerator = mp.fprod(b0 * pole * pole + b1 * pole + b2 for b0, b1, b2, _, _ in sections)
    denominator = 2 * pole + sections[index][3]
    for other, (_, _, _, c1, c2) in enumerate(sections):
        if other != index:
            denominator *= pole * pole + c1 * pole + c2
    return numerator / denominator


def fold(sections):
    """Return (direct, terms), each term (e0, e1, a1, a2) as doubles."""
    direct = mp.fprod(section[0] for section in sections)
    terms = []
    for index, (_, _, _, a1, a2) in enumerate(sections):
        first, second = roots(a1, a2)
        if mp.im(first) != 0:
            rho = residue(sections, index, first)
            e0 = 2 * mp.re(rho)
            e1 = -2 * mp.re(rho * mp.conj(first))
        else:
            rho_first = residue(sections, index, first)
            rho_second = residue(sections, index, second)
            e0 = rho_first + rho_second
            e1 = -(rho_first * second + rho_second * first)
        terms.append((float(e0), float(e1), float(sections[index][3]),
                      float(sections[index][4])))
    for e0, e1, _, _ in terms:
        if not (abs(e0) < mp.inf and abs(e1) < mp.inf):
            raise SystemExit("a folded term is not finite")
    return float(direct), terms


def unit_delay(rate, freq):
    return mp.e ** (-2j * mp.pi * freq / rate)


def parallel_response(direct, terms, rate, freq):
    delay = unit_delay(rate, freq)
    total = mp.mpf(direct)
    for e0, e1, a1, a2 in terms:
        e0, e1, a1, a2 = mp.mpf(e0), mp.mpf(e1), mp.mpf(a1), mp.mpf(a2)
        total += (e0 * delay + e1 * delay * delay) / (1 + a1 * delay + a2 * delay * delay)
    return total


def sections_response(sections, rate, freq):
    delay = unit_delay(rate, freq)
    total = mp.mpc(1)
    for b0, b1, b2, a1, a2 in sections:
        total *= (b0 + b1 * delay + b2 * delay * delay) / (1 + a1 * delay + a2 * delay * delay)
    return total


def check_gates(sections, direct, terms, rate):
    """Stability of every term, then the fold against the exact section product."""
    for index, (_, _, a1, a2) in enumerate(terms):
        if not (abs(a2) < 1 and abs(a1) < 1 + a2):
            raise SystemExit("term %d is not stable: a1 %.6g a2 %.6g" % (index, a1, a2))
    frequencies = [mp.mpf(0), mp.mpf(rate) / 2]
    ratio = (GRID_HIGH_FRACTION * rate) ** (mp.mpf(1) / (GRID_POINTS - 1))
    frequencies += [ratio ** point for point in range(GRID_POINTS)]
    worst = mp.mpf(0)
    for freq in frequencies:
        want = sections_response(sections, rate, freq)
        got = parallel_response(direct, terms, rate, freq)
        worst = max(worst, abs(got / want - 1))
    if worst > RECOMPOSITION_TOLERANCE:
        raise SystemExit("recomposition error %.3e exceeds %.0e at %d Hz"
                         % (float(worst), RECOMPOSITION_TOLERANCE, rate))
    return worst


def level_deviation(direct, terms, rate):
    return abs(parallel_response(direct, terms, rate, mp.mpf(LEVEL_HZ))) - 1


def pole_metrics(sections):
    poles = [pole for a1, a2 in ((section[3], section[4]) for section in sections)
             for pole in roots(a1, a2)]
    largest = max(abs(pole) for pole in poles)
    gap = min((abs(first - second) for index, first in enumerate(poles)
               for second in poles[index + 1:]), default=mp.mpf(0))
    return largest, gap


def hex_literal(value):
    return value.hex()


def render(design_path, design):
    lines = []
    design_bytes = open(design_path, "rb").read()
    folder = open(os.path.abspath(__file__), "rb").read()
    extractor = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dar_target.py")
    lines.append("/*")
    lines.append(" * Copyright (C) 2026 The LineageOS Project")
    lines.append(" * Copyright (C) 2026 Edrick Sinsuan <evcsinsuan@gmail.com>")
    lines.append(" *")
    lines.append(" * Licensed under the Apache License, Version 2.0 (the \"License\");")
    lines.append(" * you may not use this file except in compliance with the License.")
    lines.append(" * You may obtain a copy of the License at")
    lines.append(" *")
    lines.append(" *      http://www.apache.org/licenses/LICENSE-2.0")
    lines.append(" *")
    lines.append(" * Unless required by applicable law or agreed to in writing, software")
    lines.append(" * distributed under the License is distributed on an \"AS IS\" BASIS,")
    lines.append(" * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.")
    lines.append(" * See the License for the specific language governing permissions and")
    lines.append(" * limitations under the License.")
    lines.append(" */")
    lines.append("// GENERATED by tools/fold_signature.py -- do not edit.")
    lines.append("// Regenerate: python3 tools/fold_signature.py")
    lines.append("//   --design tools/signature_design.json --out DiracSignatureTable.h")
    lines.append("//")
    lines.append("// The fixed Dirac signature as one filter: the recorded sections multiplied")
    lines.append("// into a direct term plus one real second-order term per pole.")
    lines.append("//")
    lines.append("// design sha256 %s" % sha256_of_bytes(design_bytes))
    lines.append("// folder sha256 %s" % sha256_of_bytes(folder))
    lines.append("// dar_target sha256 %s" % sha256_of(extractor))
    target = design.get("target")
    if target:
        lines.append("// target %s" % target["entry"])
        lines.append("// channel %s" % target["channel"])
        lines.append("// dar sha256 %s" % target["dar_sha256"])
        lines.append("// entry payload sha256 %s" % target["entry_payload_sha256"])
    lines.append("// mpmath %s  recomposition tolerance %.1e"
                 % (mp.__version__, RECOMPOSITION_TOLERANCE))

    folded = []
    for rate in sorted(design["designs"], key=int):
        entry = design["designs"][rate]
        sections = sections_of(entry["sections"])
        direct, terms = fold(sections)
        worst = check_gates(sections, direct, terms, int(rate))
        largest, gap = pole_metrics(sections)
        folded.append((int(rate), direct, terms))
        lines.append("// %s Hz  %d sections  source %s"
                     % (rate, len(sections), entry["source"]))
        lines.append("//     source sha256 %s" % entry["source_sha256"])
        lines.append("//     recomposition %.1e against the exact section product"
                     % float(worst))
        measured = entry.get("measured")
        if measured:
            lines.append("//     measured %s" % measured["metric"])
            lines.append("//     -> %s dB at %s Hz" % (measured["max_db"], measured["at_hz"]))
        lines.append("//     |H(1 kHz)| - 1 %.1e  |p|max %.6f  min pole gap %.2e"
                     % (float(level_deviation(direct, terms, int(rate))), float(largest),
                        float(gap)))

    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <stddef.h>")
    lines.append("")
    lines.append("// One real term of the parallel form, on a delayed input:")
    lines.append("// t[n] = e0 x[n-1] + e1 x[n-2] - a1 t[n-1] - a2 t[n-2].")
    lines.append("struct DiracSignatureTerm {")
    lines.append("    double e0;")
    lines.append("    double e1;")
    lines.append("    double a1;")
    lines.append("    double a2;")
    lines.append("};")
    lines.append("")
    lines.append("struct DiracSignatureSpec {")
    lines.append("    unsigned rateHz;")
    lines.append("    double direct;")
    lines.append("    const DiracSignatureTerm *terms;")
    lines.append("    size_t termCount;")
    lines.append("};")
    lines.append("")
    lines.append("namespace dirac_signature_table_detail {")
    for rate, _, terms in folded:
        lines.append("")
        lines.append("inline constexpr DiracSignatureTerm kTerms%d[%d] = {" % (rate, len(terms)))
        for e0, e1, a1, a2 in terms:
            lines.append("        {%s, %s," % (hex_literal(e0), hex_literal(e1)))
            lines.append("         %s, %s}," % (hex_literal(a1), hex_literal(a2)))
        lines.append("};")
    lines.append("")
    lines.append("}  // namespace dirac_signature_table_detail")
    lines.append("")
    lines.append("struct DiracSignatureTable {")
    largest_count = max(len(terms) for _, _, terms in folded)
    lines.append("    static constexpr size_t kMaxTermCount = %d;" % largest_count)
    lines.append("    static constexpr size_t kDesignCount = %d;" % len(folded))
    lines.append("")
    lines.append("    static constexpr DiracSignatureSpec kDesigns[kDesignCount] = {")
    for rate, direct, terms in folded:
        lines.append("        {%du, %s, dirac_signature_table_detail::kTerms%d, %d},"
                     % (rate, hex_literal(direct), rate, len(terms)))
    lines.append("    };")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--design", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--check", action="store_true",
                        help="compare the committed header with a fresh fold")
    args = parser.parse_args()

    with open(args.design) as handle:
        design = json.load(handle)
    rendered = render(args.design, design)

    if args.check:
        try:
            with open(args.out) as handle:
                committed = handle.read()
        except FileNotFoundError:
            raise SystemExit("%s: not generated yet" % args.out)
        if committed != rendered:
            raise SystemExit("%s does not match a fresh fold" % args.out)
        print("%s matches a fresh fold" % args.out)
        return

    with open(args.out, "w") as handle:
        handle.write(rendered)
    print("%s written, %d bytes" % (args.out, len(rendered.encode())))


if __name__ == "__main__":
    main()
