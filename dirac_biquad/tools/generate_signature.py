#!/usr/bin/env python3
"""Generate the per-rate Dirac flat-EQ signature table.

Usage:  python3 tools/generate_signature.py                 # print the C++ table
        python3 tools/generate_signature.py --report        # fit report only
        python3 tools/generate_signature.py --dar PATH      # explicit .dar

Requires the proprietary Dirac resource blob, which is not part of this
repository: by default the generator reads

    vendor/oneplus/msm8998-common/proprietary/vendor/lib/rfsa/adsp/
        dirac_resource.dar

from the tree that ships it.  No vendor data is copied into this repository -
only the script that reads the blob and the fitted prototype table that comes
out of it, which is our own approximation, not a transcription.

Provenance: the target is the flat-EQ ("defaults/941") FIR magnitude response
of usecase/eheadset/defaults/941 in that blob, referenced to 1 kHz.  The blob
carries a real FIR for 44100 and 48000 Hz only, so those two rates are fitted
to their own target; 88200 and 96000 have no FIR and reuse the 48000
prototype, which is the same closed-form design - the DAR design is an analog
prototype sampled per rate, so its 48 kHz prototype already describes them.

Objective: sixteen RBJ sections evaluated on the digital axis of each rate,
against that rate's own target over the full 20 Hz - 20 kHz band (no clamped
reference).  Corners are analog prototypes realised through the pre-warped
bilinear transform w0 = 2*atan(pi*corner/fs) (Biquad::SetPrototype), with the
corner ceiling set per rate to the analog image of 0.45*fs, so the top sections
can follow the near-Nyquist cliff of that rate's own target.  The residual is
the dB error over a log grid, weighted 1.0 below 16 kHz and 0.5 above so the
cliff cannot mortgage the rest of the band; a minimax fit is reached by
continuation on a p-norm (p = 2, 4, 8, 16, 32) with the 1 kHz level pinned by
an extra row.  Constraints: peaking Q below 3.5, |gain| below 15 dB, and one
section per disjoint geometric band over [15 Hz, ceiling], which holds adjacent
corners apart by the band ratio (>= 1.7x here) so no pair can cancel.
"""

import argparse
import json
import math
import os
import struct
import sys

import numpy as np
from scipy.optimize import minimize

SECTION_COUNT = 16
BAND_LO_HZ = 15.0
CORNER_FRACTION = 0.45
Q_MAX = 3.5
GAIN_MAX_DB = 15.0
LEVEL_WEIGHT = 5.0
P_NORMS = (2, 4, 8, 16, 32)
# Behaviour guard. The tail length is set by the pole time constant, not by the
# radius alone: -60 dB takes about 6.9 time constants, so POLE_TAU_MS caps the
# impulse tail near RING_LIMIT_MS.  Because the sections are analog prototypes,
# one physical time constant holds at every rate, so the guard only has to be
# applied at the fitted rate; at 88200/96000 the same pole is simply nearer the
# unit circle by the rate ratio.
POLE_TAU_MS = 0.73
RING_LIMIT_MS = 5.0
RING_WEIGHT = 0.5
GUARD_RATES = {44100: (44100,), 48000: (48000,)}
SEEDS = int(os.environ.get("SEEDS", "24"))
COVERED_RATES = (44100, 48000, 88200, 96000)
FITTED_RATES = (44100, 48000)
DEFAULT_DAR = ("vendor/oneplus/msm8998-common/proprietary/vendor/lib/rfsa/adsp/"
               "dirac_resource.dar")
ENTRY = "usecase/eheadset/defaults/941"


# --- DAR0 container + ProtoFilter decoding ---------------------------------

def read_varint(buf, index):
    value = 0
    shift = 0
    while True:
        byte = buf[index]
        index += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, index


def fields(buf):
    """(field_number, wire_type, value) of one protobuf message."""
    index = 0
    while index < len(buf):
        tag, index = read_varint(buf, index)
        field, wire = tag >> 3, tag & 7
        if wire == 0:
            value, index = read_varint(buf, index)
        elif wire == 1:
            value = struct.unpack_from("<d", buf, index)[0]
            index += 8
        elif wire == 2:
            length, index = read_varint(buf, index)
            value = buf[index:index + length]
            index += length
        elif wire == 5:
            value = struct.unpack_from("<f", buf, index)[0]
            index += 4
        else:
            return
        yield field, wire, value


def container_payload(path, name):
    """Length-prefixed payload of a named resource in a DAR0 container."""
    data = open(path, "rb").read()
    if data[:4] != b"DAR0":
        raise SystemExit("%s is not a DAR0 container" % path)
    count, names_size, data_size = struct.unpack_from("<III", data, 4)
    name_base = 16 + count * 8
    data_base = name_base + names_size
    pairs = [struct.unpack_from("<II", data, 16 + 8 * i) for i in range(count)]
    previous = 0
    for cumulative, offset in pairs:
        resource = data[name_base + previous:name_base + cumulative - 1].decode("latin1")
        previous = cumulative
        if resource == name:
            length = struct.unpack_from("<I", data, data_base + offset)[0]
            return data[data_base + offset + 4:data_base + offset + 4 + length]
    raise SystemExit("resource %r not found in %s" % (name, path))


def fir_taps(path, channel="Left"):
    """{rate: taps} for one channel of ENTRY, decoded from the DAR blob."""
    taps = {}
    for field, wire, value in fields(container_payload(path, ENTRY)):
        if field != 4 or wire != 2:
            continue
        name, rates = "", []
        for sub_field, sub_wire, sub in fields(value):
            if sub_field == 4 and sub_wire == 2:
                name = sub.decode("latin1")
            elif sub_field == 5 and sub_wire == 2:
                rate, packed = 0, b""
                for deep_field, deep_wire, deep in fields(sub):
                    if deep_field == 1 and deep_wire == 0:
                        rate = deep
                    elif deep_field == 5 and deep_wire == 2:
                        packed = deep
                taps.setdefault(name, {})[rate] = list(
                    struct.unpack("<%df" % (len(packed) // 4), packed))
    if channel not in taps:
        raise SystemExit("channel %r not in %s" % (channel, ENTRY))
    return taps[channel]


def target(rate, taps):
    """(freq_hz, db) of the FIR, referenced to 1 kHz, 20 Hz - 20 kHz."""
    freqs = 20.0 * (20000.0 / 20.0) ** (np.arange(1201) / 1200.0)
    taps = np.array(taps, float)
    n = np.arange(len(taps))

    def magnitude(f):
        f = np.atleast_1d(np.asarray(f, float))
        return np.abs(np.exp(-2j * np.pi * f[:, None] / rate * n[None, :]) @ taps)

    return freqs, 20.0 * np.log10(magnitude(freqs) + 1e-30) - 20.0 * np.log10(
        magnitude(1000.0) + 1e-30)


# --- the RBJ prototype and the fit -----------------------------------------

def kinds(n):
    return ["ls"] + ["peak"] * (n - 2) + ["hs"]


def corner_ceiling(rate):
    """Analog corner whose digital image at `rate` is CORNER_FRACTION*rate."""
    return (rate / math.pi) * math.tan(math.pi * CORNER_FRACTION)


def bands(n, rate):
    """Disjoint geometric bands; the boundaries hold adjacent corners apart."""
    hi = corner_ceiling(rate)
    span = hi / BAND_LO_HZ
    edges = [BAND_LO_HZ * span ** (i / float(n)) for i in range(n + 1)]
    return list(zip(edges[:-1], edges[1:]))


def rbj(kind, f0, q, g, rate):
    """RBJ coefficients with the analog-prototype corner pre-warped for rate."""
    w0 = 2.0 * math.atan(math.pi * f0 / rate)
    cw, sw = math.cos(w0), math.sin(w0)
    a = 10.0 ** (g / 40.0)
    if kind == "peak":
        alpha = sw / (2.0 * q)
        b0, b1, b2 = 1 + alpha * a, -2 * cw, 1 - alpha * a
        a0, a1, a2 = 1 + alpha / a, -2 * cw, 1 - alpha / a
    else:
        alpha = (sw / 2.0) * math.sqrt(max(0.0, (a + 1.0 / a) * (1.0 / q - 1.0) + 2.0))
        beta = 2.0 * math.sqrt(a) * alpha
        if kind == "ls":
            b0 = a * ((a + 1) - (a - 1) * cw + beta)
            b1 = 2 * a * ((a - 1) - (a + 1) * cw)
            b2 = a * ((a + 1) - (a - 1) * cw - beta)
            a0 = (a + 1) + (a - 1) * cw + beta
            a1 = -2 * ((a - 1) + (a + 1) * cw)
            a2 = (a + 1) + (a - 1) * cw - beta
        else:
            b0 = a * ((a + 1) + (a - 1) * cw + beta)
            b1 = -2 * a * ((a - 1) + (a + 1) * cw)
            b2 = a * ((a + 1) + (a - 1) * cw - beta)
            a0 = (a + 1) - (a - 1) * cw + beta
            a1 = 2 * ((a - 1) - (a + 1) * cw)
            a2 = (a + 1) - (a - 1) * cw - beta
    return b0 / a0, b1 / a0, b2 / a0, 1.0, a1 / a0, a2 / a0


def pole_radius(kind, f0, q, g, rate):
    """Largest pole modulus of one section at `rate`."""
    return math.sqrt(abs(rbj(kind, f0, q, g, rate)[5]))


def pole_cap(rate):
    """Pole modulus whose time constant is POLE_TAU_MS at `rate`."""
    return math.exp(-1.0 / (rate * POLE_TAU_MS * 1e-3))


def q_ceiling(kind, f0, g, rate):
    """Largest Q whose poles stay inside POLE_CAP at `rate`.

    Peaking sections solve this in closed form (the pole modulus follows from
    alpha = sin(w0)/(2Q)); the shelves are monotone in the slope, so a short
    bisection is exact enough.
    """
    cap = pole_cap(rate)
    if pole_radius(kind, f0, 0.2, g, rate) > cap:
        return 0.2
    if pole_radius(kind, f0, Q_MAX, g, rate) <= cap:
        return Q_MAX
    lo, hi = 0.2, Q_MAX
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        if pole_radius(kind, f0, mid, g, rate) <= cap:
            lo = mid
        else:
            hi = mid
    return lo


def sections(x, n, rate, guard_rates=None):
    """Design parameters with the pole guard applied, so every returned section
    satisfies POLE_CAP at the rate it will be realised at."""
    k = kinds(n)
    out = []
    for i in range(n):
        kind, f0, q, g = k[i], x[3 * i], x[3 * i + 1], x[3 * i + 2]
        cap = min([q_ceiling(kind, f0, g, r) for r in (guard_rates or (rate,))])
        out.append((kind, f0, min(q, cap), g))
    return out


def ring_ms(secs, rate):
    """Time until the impulse response falls 60 dB below its peak."""
    import numpy as np
    from scipy.signal import sosfilt
    sos = np.array([[c[0], c[1], c[2], 1.0, c[4], c[5]] for c in
                    [rbj(*sec, rate) for sec in secs]])
    impulse = np.zeros(4800)
    impulse[0] = 1.0
    y = np.abs(sosfilt(sos, impulse))
    peak = y.max()
    if peak <= 0.0:
        return 0.0
    index = np.nonzero(y > peak * 1e-3)[0]
    return 1000.0 * float(index[-1]) / rate if len(index) else 0.0


def response(secs, freqs, rate):
    z = np.exp(-2j * np.pi * np.asarray(freqs) / rate)
    h = np.ones_like(z, complex)
    for s in secs:
        b0, b1, b2, _, a1, a2 = rbj(*s, rate)
        h *= (b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z)
    return 20.0 * np.log10(np.abs(h) + 1e-30)


def residual(x, n, rate, F, T, W, guard_rates=None):
    secs = sections(x, n, rate, guard_rates)
    level = float(response(secs, [1000.0], rate)[0])
    return np.concatenate([W * (response(secs, F, rate) - level - T), [LEVEL_WEIGHT * level]])


def pnorm(x, n, p, rate, F, T, W, guard_rates=None):
    r = np.abs(residual(x, n, rate, F, T, W, guard_rates))
    m = float(r.max())
    base = 0.0 if m <= 0.0 else m * float(np.mean((r / m) ** p)) ** (1.0 / p)
    tail = ring_ms(sections(x, n, rate, guard_rates), rate)
    return base + RING_WEIGHT * max(0.0, tail - RING_LIMIT_MS)


def guard_for(rate):
    """Rates whose pole cap one entry must respect (an entry is realised at its
    own rate; the aliased prototype carries the same physical time constant)."""
    return GUARD_RATES.get(rate, (rate,))


def fit(rate, seed, T=None, F=None):
    if F is None:
        F, T = target(rate, TAPS[rate])
    W = np.where(F <= 16000.0, 1.0, 0.5)
    bnds = []
    for lo, hi in bands(SECTION_COUNT, rate):
        bnds += [(lo, hi), (0.2, Q_MAX), (-GAIN_MAX_DB, GAIN_MAX_DB)]
    lo = np.array([b[0] for b in bnds])
    hi = np.array([b[1] for b in bnds])
    rng = np.random.default_rng(seed)
    bd = bands(SECTION_COUNT, rate)
    x0 = np.stack([
        np.array([10.0 ** rng.uniform(math.log10(b[0]), math.log10(b[1])) for b in bd]),
        np.exp(rng.uniform(math.log(0.3), math.log(Q_MAX), SECTION_COUNT)),
        rng.uniform(-GAIN_MAX_DB, GAIN_MAX_DB, SECTION_COUNT)], 1).reshape(-1)
    u = np.clip((x0 - lo) / (hi - lo), 0.0, 1.0)

    def tou(v):
        return lo + np.asarray(v, float) * (hi - lo)

    guard = guard_for(rate)
    for p in P_NORMS:
        res = minimize(lambda v: pnorm(tou(v), SECTION_COUNT, p, rate, F, T, W, guard), u,
                       method="L-BFGS-B", bounds=[(0.0, 1.0)] * len(u),
                       options=dict(maxiter=500, maxfun=int(6e5), ftol=1e-14, gtol=1e-12))
        u = res.x
    x = tou(u)
    secs = sections(x, SECTION_COUNT, rate, guard)
    r = response(secs, F, rate) - float(response(secs, [1000.0], rate)[0]) - T
    b1, b2 = F <= 16000.0, F > 16000.0
    return x, dict(ring_ms=float(ring_ms(secs, rate)),
                   pole=float(max(pole_radius(*sec, rate) for sec in secs)),
                   rms_lo=float(np.sqrt(np.mean(r[b1] ** 2))),
                   max_lo=float(np.max(np.abs(r[b1]))),
                   at_lo=float(F[b1][np.argmax(np.abs(r[b1]))]),
                   rms_hi=float(np.sqrt(np.mean(r[b2] ** 2))),
                   max_hi=float(np.max(np.abs(r[b2]))),
                   at_hi=float(F[b2][np.argmax(np.abs(r[b2]))]))


def canonical_order(secs, rate):
    """Order sections so no intermediate node peaks above the output response.

    Deterministic greedy: repeatedly append the remaining section that keeps the
    running peak of the prefix response smallest; candidates are tried in
    ascending index order and a step is only taken on a strict improvement, so
    ties keep the lowest index and the result is reproducible bit for bit.  The
    step is a pure permutation, so the transfer function is unchanged - only the
    intermediate headroom improves.  Reaches worst intermediate 1.23x at
    44100 Hz and 1.21x at 48000 Hz (the output response peak) from 10.40x and
    3.10x for the raw fit order.  No block scaling is used: scaling a block
    injects transients of its own.
    """
    freqs = 20.0 * (20000.0 / 20.0) ** (np.arange(4001) / 4000.0)
    carrying = list(range(len(secs)))
    order = []
    while carrying:
        chosen, chosen_peak = None, None
        for index in carrying:
            h = response([secs[k] for k in order + [index]], freqs, rate)
            peak = float(np.max(np.abs(10.0 ** (h / 20.0))))
            if chosen_peak is None or peak < chosen_peak - 1e-12:
                chosen, chosen_peak = index, peak
        order.append(chosen)
        carrying.remove(chosen)
    return [secs[index] for index in order]


def emit(secs_by_rate):
    names = {"ls": "BiquadType::kLowShelf", "peak": "BiquadType::kPeaking",
             "hs": "BiquadType::kHighShelf"}
    for rate in COVERED_RATES:
        print("// %d Hz" % rate)
        for kind, f0, q, g in secs_by_rate[rate]:
            print("        {%-25s %9.2f, %6.3f, %+7.2f}," % (names[kind], f0, q, g))
        print()


TAPS = {}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dar", default=DEFAULT_DAR, help="path to dirac_resource.dar")
    parser.add_argument("--rate", type=int, default=0, help="fit this rate only")
    parser.add_argument("--seed", type=int, default=0, help="fit this seed only")
    parser.add_argument("--report", action="store_true", help="do not print the table")
    parser.add_argument("--sections", help="JSON file of fitted sections to emit")
    args = parser.parse_args()

    if args.sections:
        data = json.load(open(args.sections, encoding="utf-8"))
        secs = {int(k): [tuple(s) for s in v] for k, v in data.items()}
        secs.setdefault(88200, secs[48000])
        secs.setdefault(96000, secs[48000])
        emit(secs)
        return

    global TAPS
    TAPS = fir_taps(args.dar)
    missing = [r for r in FITTED_RATES if r not in TAPS]
    if missing:
        raise SystemExit("no %s FIR in %s" % (missing, args.dar))

    if args.rate:
        x, m = fit(args.rate, args.seed)
        m["rate"], m["seed"] = args.rate, args.seed
        m["sections"] = canonical_order(
                sections(x, SECTION_COUNT, args.rate, guard_for(args.rate)), args.rate)
        print(json.dumps(m), flush=True)
        return

    secs = {}
    for rate in FITTED_RATES:
        out = []
        for seed in range(SEEDS):
            x, m = fit(rate, seed)
            m["sections"] = canonical_order(
                    sections(x, SECTION_COUNT, rate, guard_for(rate)), rate)
            out.append(m)
        out.sort(key=lambda r: (r["rms_lo"], r["rms_hi"]))
        m = out[0]
        secs[rate] = [tuple(s) for s in m["sections"]]
        print("%6d Hz  own target: 20-16k RMS=%.4f max=%.4f @%.1f Hz | "
              "16-20k RMS=%.4f max=%.4f @%.1f Hz | ring=%.2f ms max|pole|=%.6f"
              % (rate, m["rms_lo"], m["max_lo"], m["at_lo"], m["rms_hi"], m["max_hi"], m["at_hi"],
                 m["ring_ms"], m["pole"]),
              file=sys.stderr, flush=True)
    secs[88200] = secs[48000]
    secs[96000] = secs[48000]
    if not args.report:
        emit(secs)


if __name__ == "__main__":
    main()
