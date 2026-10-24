#!/usr/bin/env python3
"""Generate evenband.ttl. Port order must match enum port in src/eb_lv2.c."""
import sys

XOVER_DEFAULT = 0  # keep equal to the default in eb_defaults()
ports = []


def audio(sym, name, out=False):
    ports.append(dict(kind="audio", sym=sym, name=name, out=out))


def ctl(sym, name, mn, mx, df, unit=None, toggled=False, integer=False, out=False, extra="", points=None):
    ports.append(dict(kind="ctl", sym=sym, name=name, out=out, mn=mn, mx=mx, df=df, unit=unit,
                      toggled=toggled, integer=integer, extra=extra, points=points))


audio("in_l", "In L"); audio("in_r", "In R")
audio("out_l", "Out L", True); audio("out_r", "Out R", True)
ctl("power", "Power", 0, 100, 60, "pc")
ctl("speed", "Speed", 0.25, 4, 1)
ctl("range_boost", "Range Boost", 0, 18, 9, "db")
ctl("range_cut", "Range Cut", 0, 18, 9, "db")
ctl("tilt", "Tilt", -3, 3, 0)
for sym, nm in [("deep_bass", "Deep Bass"), ("warmth", "Warmth"), ("mids", "Mids"),
                ("presence", "Presence"), ("brilliance", "Brilliance")]:
    ctl(sym, nm, -18, 18, 0, "db")
for i, f in enumerate([120, 500, 2000, 6000], 1):
    ctl("xover%d" % i, "Crossover %d" % i, 30, 18000, f, "hz",
        extra="lv2:portProperty <http://lv2plug.in/ns/ext/port-props#logarithmic> ;")
ctl("keep_level", "Keep Level", 0, 1, 1, toggled=True, integer=True)
ctl("limiter", "Limiter", 0, 1, 1, toggled=True, integer=True)
ctl("ceiling", "Ceiling", -6, 0, -1, "db")
ctl("xover_type", "Band Overlap", 0, 2, XOVER_DEFAULT, integer=True,
    extra="lv2:portProperty lv2:enumeration ;",
    points=[("Steep (LR4)", 0), ("Gentle (6 dB/oct)", 1), ("Medium (12 dB/oct)", 2)])
ctl("mode", "Mode", 0, 1, 0, integer=True, extra="lv2:portProperty lv2:enumeration ;",
    points=[("Balance (relative)", 0), ("AGC (absolute targets)", 1)])
ctl("target", "AGC Target", -40, 0, -12, "db")
ctl("trim", "AGC Trim", -12, 12, -6, "db")
ctl("gate", "AGC Gate", -90, -20, -60, "db")
BAND_NAMES = ["Deep Bass", "Warmth", "Mids", "Presence", "Brilliance"]
LOG = "lv2:portProperty <http://lv2plug.in/ns/ext/port-props#logarithmic> ;"
for i, nm in enumerate(BAND_NAMES):
    ctl("atk%d" % i, "AGC Attack " + nm, 0.005, 60, 0.2, "s", extra=LOG)
for i, nm in enumerate(BAND_NAMES):
    ctl("rel%d" % i, "AGC Release " + nm, 0.005, 60, 1.0, "s", extra=LOG)
for i, nm in enumerate(BAND_NAMES):
    ctl("mix%d" % i, "Mix " + nm, -24, 24, 0, "db")
ctl("latency", "Latency", 0, 192000, 0, integer=True, out=True,
    extra="lv2:designation lv2:latency ;\n        lv2:portProperty lv2:reportsLatency ;")
for sym, nm in [("meter_deep_bass", "Gain Deep Bass"), ("meter_warmth", "Gain Warmth"),
                ("meter_mids", "Gain Mids"), ("meter_presence", "Gain Presence"),
                ("meter_brilliance", "Gain Brilliance")]:
    ctl(sym, nm, -40, 40, 0, "db", out=True)

print("""@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix doap:  <http://usefulinc.com/ns/doap#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix units: <http://lv2plug.in/ns/extensions/units#> .


<https://github.com/BobBoba/evenband>
    a lv2:Plugin, lv2:CompressorPlugin ;
    doap:name "Evenband" ;
    doap:license <http://opensource.org/licenses/MIT> ;
    lv2:optionalFeature lv2:hardRTCapable ;
    rdfs:comment "Five-band spectral balance compressor: pulls the tonal balance of the programme toward a reference spectrum, with a look-ahead output limiter." ;""")
chunks = []
for i, p in enumerate(ports):
    s = "    [\n        a lv2:%s, lv2:%s ;\n        lv2:index %d ;\n        lv2:symbol \"%s\" ;\n        lv2:name \"%s\" ;\n" % (
        "OutputPort" if p["out"] else "InputPort", "AudioPort" if p["kind"] == "audio" else "ControlPort",
        i, p["sym"], p["name"])
    if p["kind"] == "ctl":
        if not p["out"]:
            s += "        lv2:default %s ;\n" % p["df"]
        s += "        lv2:minimum %s ;\n        lv2:maximum %s ;\n" % (p["mn"], p["mx"])
        if p["out"]:
            s += "        lv2:default %s ;\n" % p["df"]
        if p["toggled"]:
            s += "        lv2:portProperty lv2:toggled ;\n"
        if p["integer"]:
            s += "        lv2:portProperty lv2:integer ;\n"
        if p["unit"]:
            s += "        units:unit units:%s ;\n" % p["unit"]
        if p["extra"]:
            s += "        " + p["extra"] + "\n"
        for label, value in (p["points"] or []):
            s += "        lv2:scalePoint [ rdfs:label \"%s\" ; rdf:value %d ] ;\n" % (label, value)
    s += "    ]"
    chunks.append(s)
print("    lv2:port\n" + " ,\n".join(chunks) + " .")
