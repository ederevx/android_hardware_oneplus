#!/usr/bin/env python3
# Copyright (C) 2026 The LineageOS Project
# Copyright (C) 2026 Edrick Sinsuan <evcsinsuan@gmail.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Measure the module against the Dirac DAR target.

The DAR is the shipped hdsound-filters entry: a slot-8 container whose ProtoFilter
payload carries the FIR this module's design fits. This tool decodes it, measures a
recorded design against it, and verifies a generated table against both.
"""

import hashlib
import json
import os
import re
import struct
import sys

import numpy as np

DEFAULT_DAR = ("vendor/oneplus/msm8998-common/proprietary/vendor/lib/rfsa/adsp/"
               "dirac_resource.dar")
ENTRY = ("usecase/eheadset/hdsound-filters/"
         "09-Oneplus-Earphone_General_Bluetooth_170928v02")


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


def container_resources(path):
    """(name, payload) of every resource in a DAR0 container, in file order."""
    data = open(path, "rb").read()
    if data[:4] != b"DAR0":
        raise SystemExit("%s is not a DAR0 container" % path)
    count, names_size, _ = struct.unpack_from("<III", data, 4)
    name_base = 16 + count * 8
    data_base = name_base + names_size
    pairs = [struct.unpack_from("<II", data, 16 + 8 * i) for i in range(count)]
    resources, previous = [], 0
    for cumulative, offset in pairs:
        resource = data[name_base + previous:name_base + cumulative - 1].decode("latin1")
        previous = cumulative
        length = struct.unpack_from("<I", data, data_base + offset)[0]
        resources.append(
            (resource, data[data_base + offset + 4:data_base + offset + 4 + length]))
    return resources


def filter_identity(text):
    """Filter identity: a leading NN- slot ordinal and every separator removed,
    lowercased.  A DAR key and the ProtoFilter's own stored name then compare on
    their common text, even though the stored name is truncated and the key
    carries a date suffix."""
    return re.sub(r"[^a-z0-9]", "", re.sub(r"^\d\d-", "", text).lower())


def proto_name(payload):
    """The ProtoFilter message's own name field, or "" when it has none.  A
    scalar resource is not a ProtoFilter, so a short payload is simply unnamed."""
    try:
        for field, wire, value in fields(payload):
            if field == 1 and wire == 2:
                return value.decode("latin1")
    except (struct.error, IndexError):
        pass
    return ""


def identity_matches(wanted, identity):
    return bool(identity) and (wanted.startswith(identity) or identity.startswith(wanted))


def container_payload(path, name):
    """Length-prefixed payload of a named resource in a DAR0 container.

    The eheadset hdsound-filters name table runs one entry ahead of the payload
    table, so the entry a hdsound-filters key names holds the *next* filter's
    payload.  Resolve those keys by the ProtoFilter's own identity so the named
    filter is decoded, not its neighbour; every other resource keeps its entry.
    """
    resources = container_resources(path)
    own = next((payload for resource, payload in resources if resource == name), None)
    if own is None:
        raise SystemExit("resource %r not found in %s" % (name, path))
    if "hdsound-filters/" not in name:
        return own
    wanted = filter_identity(name.rsplit("/", 1)[-1])
    if identity_matches(wanted, filter_identity(proto_name(own))):
        return own
    for resource, payload in resources:
        if identity_matches(wanted, filter_identity(proto_name(payload))):
            return payload
    return own


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


def dc_anchor_db(rate, taps):
    """Reference DC gain re 1 kHz: 20*log10(|sum(taps)| / |H(1 kHz)|)."""
    taps = np.array(taps, float)
    n = np.arange(len(taps))

    def magnitude(f):
        f = np.atleast_1d(np.asarray(f, float))
        return np.abs(np.exp(-2j * np.pi * f[:, None] / rate * n[None, :]) @ taps)

    return float(20.0 * np.log10(magnitude([0.0])[0] / magnitude([1000.0])[0]))


# The accuracy metric the design record and the header both name: the design
# against the DAR target, on the dense grid, in dB, referenced to 1 kHz.
DESIGN_METRIC = "max |dB(H(f)/H(1 kHz)) - T(f)|, f = 1..20040 Hz step 1"


def frequency_grid():
    return np.arange(1, 20041, dtype=float)


def fir_db(rate, taps, freqs):
    taps = np.array(taps, float)
    n = np.arange(len(taps))

    def magnitude(f):
        f = np.atleast_1d(np.asarray(f, float))
        return np.abs(np.exp(-2j * np.pi * f[:, None] / rate * n[None, :]) @ taps)

    return 20.0 * np.log10(magnitude(freqs) + 1e-30) - 20.0 * np.log10(magnitude(1000.0) + 1e-30)


def sections_db(rate, sections, freqs):
    freqs = np.asarray(freqs, float)
    delay = np.exp(-2j * np.pi * freqs / rate)
    total = np.ones(len(freqs), complex)
    for b0, b1, b2, a1, a2 in sections:
        total *= (b0 + b1 * delay + b2 * delay * delay) / (1 + a1 * delay + a2 * delay * delay)
    level = np.abs(total[np.argmin(np.abs(np.asarray(freqs, float) - 1000.0))])
    total = total / complex(level, 0.0) if level else total
    return 20.0 * np.log10(np.abs(total) + 1e-30)


def parallel_db(rate, direct, terms, freqs):
    freqs = np.asarray(freqs, float)
    delay = np.exp(-2j * np.pi * freqs / rate)
    total = np.full(len(freqs), complex(direct))
    for e0, e1, a1, a2 in terms:
        total += (e0 * delay + e1 * delay * delay) / (1 + a1 * delay + a2 * delay * delay)
    level = np.abs(total[np.argmin(np.abs(np.asarray(freqs, float) - 1000.0))])
    total = total / complex(level, 0.0) if level else total
    return 20.0 * np.log10(np.abs(total) + 1e-30)


def worst_against_target(rate, got_db, taps):
    """(max deviation in dB, the frequency it occurs at) on the dense grid."""
    freqs = frequency_grid()
    error = np.abs(got_db - fir_db(rate, taps, freqs))
    index = int(np.argmax(error))
    return float(error[index]), int(freqs[index])


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def update_record(path, dar, channel):
    """Fill the target hashes and per-rate measurements; leaves the designs alone."""
    with open(path) as handle:
        record = json.load(handle)
    payload = container_payload(dar, ENTRY)
    with open(dar, "rb") as handle:
        dar_sha = sha256_bytes(handle.read())
    record["target"] = {
        "entry": ENTRY,
        "channel": channel,
        "dar_sha256": dar_sha,
        "entry_payload_sha256": sha256_bytes(payload),
    }
    taps_by_rate = fir_taps(dar, channel)
    for rate, design in record["designs"].items():
        sections = [[float.fromhex(value) for value in row] for row in design["sections"]]
        got_db = sections_db(int(rate), sections, frequency_grid())
        max_db, at_hz = worst_against_target(int(rate), got_db, taps_by_rate[int(rate)])
        design["measured"] = {"metric": DESIGN_METRIC,
                              "max_db": "%.6f" % max_db,
                              "at_hz": at_hz}
        print("%s Hz  %s  max %.6f dB at %d Hz" % (rate, design["source"], max_db, at_hz))
    with open(path, "w") as handle:
        handle.write(json.dumps(record, sort_keys=True, indent=2) + "\n")
    print("target %s\n  dar sha256 %s" % (ENTRY, dar_sha))


def parse_header(path):
    """(terms per rate, direct per rate) from the header."""
    with open(path) as handle:
        text = handle.read()
    terms, direct = {}, {}
    for rate, body in re.findall(r"kTerms(\d+)\[\d+\] = \{(.*?)\n\};", text, re.S):
        rows = re.findall(r"\{([^{}]*)\},", body)
        terms[int(rate)] = [[float.fromhex(value.strip()) for value in row.split(",")]
                            for row in rows]
    for rate, value in re.findall(r"\{(\d+)u, (0x[0-9a-fp+.\-]+),", text):
        direct[int(rate)] = float.fromhex(value)
    return terms, direct


def verify_header(path, dar, channel, record_path):
    """Check a generated header against the design record it was folded from."""
    with open(record_path) as handle:
        record = json.load(handle)
    with open(dar, "rb") as handle:
        dar_sha256 = sha256_bytes(handle.read())
    payload_sha256 = sha256_bytes(container_payload(dar, ENTRY))
    failures = 0
    if dar_sha256 != record["target"]["dar_sha256"]:
        print("dar sha256 %s does not match the record" % dar_sha256)
        failures += 1
    if payload_sha256 != record["target"]["entry_payload_sha256"]:
        print("entry payload sha256 %s does not match the record" % payload_sha256)
        failures += 1

    terms, direct = parse_header(path)
    taps_by_rate = fir_taps(dar, channel)
    for rate in sorted(terms):
        parallel = parallel_db(rate, direct[rate], terms[rate], frequency_grid())
        max_db, at_hz = worst_against_target(rate, parallel, taps_by_rate[rate])
        recorded = float(record["designs"][str(rate)]["measured"]["max_db"])
        print("%d Hz  %2d terms  max %.6f dB at %d Hz  recorded %.6f"
              % (rate, len(terms[rate]), max_db, at_hz, recorded))
        if abs(max_db - recorded) > 1e-6:
            print("  does not match the recorded maximum")
            failures += 1
    return 1 if failures else 0


def main(argv):
    """Usage: dar_target.py [--dar PATH] [--channel NAME] [--design JSON]

    [--verify-header HEADER]. With no mode it prints the target's identity and DC
    anchor, which is how a caller sees which filter is being measured.
    """
    dar, entry, channel = DEFAULT_DAR, ENTRY, "Left"
    measure_path = header_path = None
    arguments = list(argv)
    for flag in ("--dar", "--channel", "--design", "--verify-header"):
        if flag in arguments:
            at = arguments.index(flag)
            value = arguments[at + 1]
            del arguments[at:at + 2]
            if flag == "--dar":
                dar = value
            elif flag == "--channel":
                channel = value
            elif flag == "--design":
                measure_path = value
            else:
                header_path = value

    if measure_path is not None:
        update_record(measure_path, dar, channel)
        return 0

    if header_path is not None:
        record_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                   "signature_design.json")
        return verify_header(header_path, dar, channel, record_path)

    taps_by_rate = fir_taps(dar, channel)
    rates = sorted(taps_by_rate)
    for rate in rates:
        print("%s  %s  %d Hz  %d taps" % (entry, channel, rate, len(taps_by_rate[rate])))
    print("dc anchor %.3f dB at %d Hz" % (dc_anchor_db(rates[0], taps_by_rate[rates[0]]), rates[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
