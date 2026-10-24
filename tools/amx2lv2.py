#!/usr/bin/env python3
"""Convert BreakawayOne processing presets (.amx) into Evenband LV2 presets.

usage: amx2lv2.py PRESET_DIR BUNDLE_DIR

PRESET_DIR  folder with the .amx files from your own BreakawayOne installation
BUNDLE_DIR  output bundle, normally ~/.lv2/evenband-presets.lv2. It is created
            (or rewritten) and contains only presets; the plugin itself stays where
            it is installed.

Only the multiband AGC and its per-band output mix are translated; every other
stage of the original chain (wide-band AGC, clippers, limiters, parametric EQs,
stereo enhancer, ...) is outside the scope of this plugin and is ignored.

The conversion rules were measured on BreakawayOne's output (see SPEC.md,
"Calibration"); nothing was read from the application's code. The presets
themselves belong to their authors: convert your own copies for your own use and
do not redistribute the generated files.
"""
import math
import os
import re
import sys

# ---- calibration (black-box measurements, see rig/data and SPEC.md) --------
CAL = {
    "target": -18.67,          # dBFS band power at threshold 50
    "k_threshold": 0.13,       # dB of target shift per threshold unit (higher threshold = lower target)
    "k_mix": 0.20,             # dB per outmix unit
    "range_to_db": 0.24,       # max deviation boost = 0.24 * mbagc_range
    "trim": -6.0,              # dB, gain of a band that sits exactly on its target
    "atk50": 0.088,            # s, attack time constant at unit 50
    "rel50": 1.21,             # s, release time constant at unit 50
    "slope": 0.083,            # 1/unit: time constants scale by exp(-slope * (unit - 50))
    "xover": (59.0, 170.0, 728.0, 4083.0),
}

BANDS = 5
URI = "https://github.com/BobBoba/evenband"
PRESET_URI = "https://github.com/BobBoba/evenband/preset/"
SYMBOLS = ["deep_bass", "warmth", "mids", "presence", "brilliance"]


def parse(path):
    d = {}
    with open(path, encoding="latin-1", newline="") as f:
        for line in f.read().split("\n"):
            line = line.rstrip("\r")
            m = re.match(r"([^=\[]+(?:\[\d+\])?)=(.*)$", line)
            if m:
                d[m.group(1)] = m.group(2)
    return d


def num(vals, key, default):
    try:
        return float(vals[key])
    except (KeyError, ValueError):
        return default


def band_value(vals, key, n_bands, j, default):
    """Value of my band j for a preset that has n_bands bands."""
    i = j if n_bands == BANDS else min(n_bands - 1, int(((j + 0.5) / BANDS) * n_bands))
    return num(vals, f"{key}[{i}]", default)


def tau(unit, base):
    return base * math.exp(-CAL["slope"] * (unit - 50.0))


def convert(vals):
    """Return {port symbol: value} for one preset."""
    nb = int(num(vals, "num_bands", 5))
    out = {
        "mode": 1.0,
        "power": max(0.0, min(100.0, num(vals, "mbagc_ratio", 50.0))),
        "range_boost": CAL["range_to_db"] * num(vals, "mbagc_range", 58.0),
        "range_cut": 40.0,
        "trim": CAL["trim"],
        "target": CAL["target"],
        "gate": max(-90.0, min(-20.0, num(vals, "freeze_threshold", -90.0))),
        "keep_level": 0.0,
        "xover_type": 0.0,
    }
    for k, f in enumerate(CAL["xover"], 1):
        out[f"xover{k}"] = f
    for j in range(BANDS):
        thr = band_value(vals, "mbagc_threshold", nb, j, 50.0)
        mix = band_value(vals, "outmix", nb, j, 50.0)
        out[SYMBOLS[j]] = -CAL["k_threshold"] * (thr - 50.0)
        out[f"mix{j}"] = CAL["k_mix"] * (mix - 50.0)
        out[f"atk{j}"] = tau(band_value(vals, "mbagc_atk", nb, j, 45.0), CAL["atk50"])
        out[f"rel{j}"] = tau(band_value(vals, "mbagc_rel", nb, j, 60.0), CAL["rel50"])
    return out


def slug(name):
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def preset_ttl(uri, label, ports):
    lines = [
        "@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .",
        "@prefix pset: <http://lv2plug.in/ns/ext/presets#> .",
        "@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .",
        "",
        f"<{uri}>",
        "    a pset:Preset ;",
        f"    lv2:appliesTo <{URI}> ;",
        f'    rdfs:label "{label}" ;',
        "    lv2:port",
    ]
    items = [f'        [ lv2:symbol "{k}" ; pset:value {v:.6g} ]' for k, v in sorted(ports.items())]
    lines.append(" ,\n".join(items) + " .")
    return "\n".join(lines) + "\n"


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    src, bundle = argv[1], argv[2]
    pdir = os.path.join(bundle, "presets")
    os.makedirs(pdir, exist_ok=True)
    for old in os.listdir(pdir):                      # regenerate from scratch
        if old.endswith(".ttl"):
            os.remove(os.path.join(pdir, old))
    manifest = os.path.join(bundle, "manifest.ttl")
    entries = []
    names = sorted(n for n in os.listdir(src) if n.lower().endswith(".amx"))
    for fn in names:
        vals = parse(os.path.join(src, fn))
        label = vals.get("presetname") or fn[:-4]
        if "mbagc_ratio" not in vals:
            print(f"skip {fn}: no multiband AGC settings")
            continue
        s = slug(label)
        uri = PRESET_URI + s
        with open(os.path.join(pdir, s + ".ttl"), "w", encoding="utf-8") as f:
            f.write(preset_ttl(uri, label + " (BreakawayOne)", convert(vals)))
        entries.append(
            f"<{uri}>\n    a <http://lv2plug.in/ns/ext/presets#Preset> ;\n"
            f"    <http://lv2plug.in/ns/lv2core#appliesTo> <{URI}> ;\n"
            f"    <http://www.w3.org/2000/01/rdf-schema#seeAlso> <presets/{s}.ttl> .\n")
    with open(manifest, "w", encoding="utf-8") as f:
        f.write("\n".join(entries))
    print(f"{len(entries)} presets written to {pdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
