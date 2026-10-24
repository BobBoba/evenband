#!/usr/bin/env python3
"""Generate the LV2 description files and the port header from one port list.

usage: gen_ttl.py ttl|manifest|header [--x11] [--gtk3]
  --x11 / --gtk3   describe that user interface as well
"""
import sys

MODE = sys.argv[1] if len(sys.argv) > 1 else "ttl"
X11 = "--x11" in sys.argv
GTK3 = "--gtk3" in sys.argv
URI = "https://github.com/BobBoba/evenband"

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
ctl("range_boost", "Range Boost", 0, 36, 9, "db")
ctl("range_cut", "Range Cut", 0, 60, 9, "db")
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
ctl("bypass", "Bypass", 0, 1, 0, toggled=True, integer=True)
ctl("xover_type", "Band Overlap", 0, 2, XOVER_DEFAULT, integer=True,
    extra="lv2:portProperty lv2:enumeration ;",
    points=[("Steep", 0), ("Gentle", 1), ("Medium", 2)])
ctl("mode", "Mode", 0, 1, 0, integer=True, extra="lv2:portProperty lv2:enumeration ;",
    points=[("Balance", 0), ("AGC", 1)])
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

PREFIXES = """@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix doap:  <http://usefulinc.com/ns/doap#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix units: <http://lv2plug.in/ns/extensions/units#> .
@prefix ui:    <http://lv2plug.in/ns/extensions/ui#> .
"""


def emit_ttl():
    print(PREFIXES)
    print("<%s>" % URI)
    print("""    a lv2:Plugin, lv2:CompressorPlugin ;
    doap:name "Evenband" ;
    doap:license <http://opensource.org/licenses/MIT> ;
    lv2:optionalFeature lv2:hardRTCapable ;
    rdfs:comment "Five-band spectral balance compressor: pulls the tonal balance of the programme toward a reference spectrum, with a look-ahead output limiter." ;""")
    if X11:
        print("    ui:ui <%s#ui> ;" % URI)
    if GTK3:
        print("    ui:ui <%s#ui-gtk3> ;" % URI)
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
    if X11:
        print("""
<%s#ui>
    a ui:X11UI ;
    lv2:requiredFeature ui:idleInterface ;
    lv2:optionalFeature ui:resize ;
    lv2:extensionData ui:idleInterface .""" % URI)
    if GTK3:
        print("""
<%s#ui-gtk3>
    a ui:Gtk3UI .""" % URI)


def emit_manifest():
    print("""@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
@prefix ui:   <http://lv2plug.in/ns/extensions/ui#> .
""")
    print("<%s>\n    a lv2:Plugin ;\n    lv2:binary <evenband.so> ;\n    rdfs:seeAlso <evenband.ttl> ." % URI)
    if X11:
        print("\n<%s#ui>\n    a ui:X11UI ;\n    ui:binary <evenband_ui_x11.so> ;\n    rdfs:seeAlso <evenband.ttl> ." % URI)
    if GTK3:
        print("\n<%s#ui-gtk3>\n    a ui:Gtk3UI ;\n    ui:binary <evenband_ui_gtk3.so> ;\n    rdfs:seeAlso <evenband.ttl> ." % URI)


def emit_header():
    print("/* Generated by lv2/gen_ttl.py; do not edit. One list feeds the TTL, the plugin and the UI. */")
    print("#ifndef EB_PORTS_H\n#define EB_PORTS_H\n")
    print('#define EB_PLUGIN_URI "%s"\n' % URI)
    print("enum eb_port {")
    for p in ports:
        print("    P_%s," % p["sym"].upper())
    print("    P_COUNT\n};\n")
    print("static const char *const eb_port_symbol[P_COUNT] = {")
    for p in ports:
        print('    "%s",' % p["sym"])
    print("};\n")

    def table(name, key, kind="float"):
        print("static const %s eb_port_%s[P_COUNT] = {" % (kind, name))
        for p in ports:
            v = p[key] if p["kind"] == "ctl" else 0
            print("    %s," % (repr(float(v)) + "f" if kind == "float" else int(v)))
        print("};\n")

    table("default", "df"); table("min", "mn"); table("max", "mx")
    print("static const unsigned char eb_port_is_output[P_COUNT] = {")
    for p in ports:
        print("    %d," % (1 if p["out"] else 0))
    print("};\n")
    print("/* How the user interface presents a port. */")
    print("typedef struct {")
    print("    const char *name;")
    print("    const char *unit;           /* pc, db, hz, s or empty */")
    print("    unsigned char log;          /* logarithmic slider */")
    print("    unsigned char toggle;       /* on/off button */")
    print("    unsigned char integer;")
    print("    unsigned char in_preset;    /* saved in and applied by presets (bypass is not) */")
    print("    unsigned char n_labels;     /* enumeration: number of labels */")
    print("    const char *labels[4];")
    print("} eb_port_meta;\n")
    print("static const eb_port_meta eb_meta[P_COUNT] = {")
    for p in ports:
        if p["kind"] != "ctl":
            print('    {"%s", "", 0, 0, 0, 0, 0, {0}},' % p["name"])
            continue
        labels = [l for l, _ in (p["points"] or [])]
        lab = ", ".join('"%s"' % l for l in labels) if labels else "0"
        print('    {"%s", "%s", %d, %d, %d, %d, %d, {%s}},' % (
            p["name"], p["unit"] or "", 1 if "logarithmic" in p["extra"] else 0, 1 if p["toggled"] else 0,
            1 if p["integer"] else 0, 0 if p["sym"] == "bypass" else 1, len(labels), lab))
    print("};\n\n#endif")

{"ttl": emit_ttl, "manifest": emit_manifest, "header": emit_header}[MODE]()
