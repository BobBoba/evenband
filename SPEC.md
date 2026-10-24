# Evenband — design, calibration and verification

LV2 plugin (stereo, real time, any sample rate; tested at 48 kHz and 192 kHz) that
does the "tonal compression" of a broadcast multiband processor and nothing else:
five frequency bands, each with its own gain, driven either by a reference
spectrum (Balance mode) or by per-band absolute targets (AGC mode). Usage is in
`README.md`; this file records how it works, where the numbers come from, and how
well it matches the product that inspired it.

## 1. Provenance and legal position

Independent implementation. No code, binary or data structure of BreakawayOne was
read, disassembled or copied. What was used:

| Source | What it gave |
|---|---|
| US 6,940,987 B2 (Claesson; Octiv/Plantronics; filed 2000-12-20) | Architecture: AGC, crossover into n bands, per-band processing, mixer. Google Patents lists the patent as lapsed for non-payment of maintenance fees (2017-10-02; nominal expiry 2022-07-23). Google says this status is an assumption, not a legal conclusion: have a patent attorney confirm before any commercial release. |
| Breakaway Live manual, Telos Omnia docs, Claesson's IBC 2015 paper | Vocabulary and the idea that Power/Range/Speed and four tone controls steer a multiband AGC. |
| The product's own output | Everything numeric in section 5. An installation of BreakawayOne 3.32.55 in the vendor's own free trial mode was run under WINE with test noise on a private audio loop; only the audio coming out and the text files the application itself stores (`*.amx` presets, its state file) were read. |

Presets are the work of Breakaway and of their authors. `tools/amx2lv2.py`
converts *your own* installed copies for *your own* use; the generated files are
not part of this repository and must not be redistributed.

## 2. Signal flow

```
in ─ LR4 crossover (5 bands, all-pass compensated, flat sum) ─┬─ band signals
                                                              │ stereo-linked power detector per band
        gains (smoothed, interpolated per sample) ◄───────────┘   (control rate ≈ 1.5 kHz)
        │ × static band mix
        ▼ sum ─ look-ahead peak limiter (5 ms, release 80 ms) ─ out
```

Crossover shapes: LR4 (default, steep, flat sum through all-pass compensation) or
two gentle complementary types (6 and 12 dB/oct). Measured against the product,
LR4 fitted best (0.90 dB against 0.93 / 0.96 dB RMS over 31 presets), so it is the default.

### 2.1 Balance mode (default)

Pulls the spectrum toward a reference shape, independent of loudness.
With band power `p_i`, total `P = Σ p_i`:

```
reference_i = share of tilted pink noise in band i (integrated through the real crossover), × 10^(offset_i/10), normalised
error_i     = 10·log10(P · reference_i / p_i)
gain_i      = clamp(power · error_i, −range_cut, +range_boost)  (+ keep-level make-up, + band mix)
```

Pink noise passes unchanged for every crossover shape (tested). A band holding
less than 40..50 dB of the total is not boosted (no noise-floor lifting of
band-limited material).

### 2.2 AGC mode (BreakawayOne-style)

Every band is levelled toward its own absolute target power:

```
gain_i = trim + power · clamp(target + offset_i − level_i, −range_cut, +range_boost)    (then + mix_i)
```

* `level_i` is the band's mean-square power in dBFS (40 ms detector).
* The deviation is limited **first**, then scaled by `power` (ratio 50 % with a 13.9 dB limit gives −6 + 0.5·13.9 = +1.0 dB, as measured).
* Gain moves toward its goal with per-band attack (gain falls) and release (gain rises) time constants, exponential in dB.
* `gate`: below this **total** level all gains are held, so quiet passages and room noise are not pulled up.

## 3. User interface

`src/eb_ui_core.c` (toolkit independent, Cairo) draws the whole window and handles the pointer,
wheel and keyboard: bar sliders for every control port (linear or logarithmic as declared in
the port list, bipolar bars fill from zero), toggles, segmented choices, the five-row scrolling
preset list and the New / Save / Save as / Delete / Import buttons, a name dialog and a status
line. `eb_ui_x11.c` (ui:X11UI) and `eb_ui_gtk3.c` (ui:Gtk3UI) only feed it events and call
`eb_ui_poll` from a timer. Port names, units and kinds come from the generated `eb_ports.h`
(`lv2/gen_ttl.py`), so the window cannot drift from the plugin.

Presets are read from the installed LV2 preset files that apply to Evenband
(`src/eb_presets.c`), so nothing is bundled. User presets are written as ordinary LV2 preset
files into `~/.lv2/evenband-user.lv2` with a rebuilt manifest; deleting is limited to files
directly inside that bundle. The file chooser and the converter run as separate processes
without a shell (`src/eb_spawn.c`), so file names are never interpreted. The selected row is
derived from the current port values (the preset being worked on wins when several are
identical), so it also follows changes made in the host. Bypass is not part of a preset.

Verified: 94 interface checks (parser, store, sliders, drag, log scale, toggles, enumerations,
list scrolling and the five-row limit, save/overwrite/cancel/clash/delete, import with a fake
chooser and converter, missing helpers, drawing), both interfaces loaded like a host does
(`tests/ui_harness.c`, Xvfb, real mouse and keyboard events) and inside Carla's own UI bridge
(GTK3).

## 4. Ports

Inputs: Power, Speed, Range Boost, Range Cut, Tilt, five band offsets (Deep Bass,
Warmth, Mids, Presence, Brilliance, ±18 dB), four crossover frequencies, Keep Level,
Limiter, Ceiling, Bypass, Band Overlap (crossover shape), Mode, AGC Target, AGC Trim, AGC
Gate, five AGC attack and five release times, five static band mixes (±24 dB).
Outputs: Latency (samples, `lv2:latency`, 5 ms) and one gain meter per band.
Port order is generated from `lv2/gen_ttl.py`; keep it equal to `enum port` in `src/eb_lv2.c`.

## 5. Calibration: what was measured

All against BreakawayOne's output with the multiband AGC isolated (wide-band AGC,
clippers, limiters, second/third wide-band stage, stereo enhancer, sub-bass, parametric
EQ switched off through the application's own state file). The raw captures and the measurement scripts are not part of this repository.

| Finding | Evidence |
|---|---|
| Strength = `mbagc_ratio / 100`, linear | gain vs ratio at 0..100 is linear (probe_ratio_sweep) |
| Deviation limit = `0.24 · mbagc_range` dB, applied before the ratio | gains of +1.0 dB at ratio 50 / range 58, +3 at range 75, +6 at range 100; +18 at ratio 100 / range 100 |
| `trim` = −6 dB (gain with ratio 0, or a band exactly on target) | all bands −6.0 dB at ratio 0 |
| The target is absolute, not relative | at ratio 100 gain falls 1 dB per dB of input level (+18 at −44 dBFS … −10 at −4 dBFS) |
| Target ≈ −18.7 dBFS band power at threshold 50; threshold shifts it by −0.13 dB/unit (a higher threshold lowers the band) | global fit; single-band probes |
| Band mix: 0.20 dB/unit around 50 | outmix sweeps (±4 dB at ±20 units); global fit gives 0.200 |
| Time constants: `τ_attack = 0.088 s · e^(−0.083·(u−50))`, `τ_release = 1.21 s · e^(−0.083·(u−50))` | step response at u = 20…80, consistent at 4 points |
| Band layout ≈ 59 / 170 / 728 / 4083 Hz (LR4) | global fit over 31 presets; single-band probes give bell-shaped band responses ~2 octaves wide |
| `freeze_threshold` holds the gains below that total level | loud-then-quiet probe: gain stays ~11 dB at −50, climbs to the 18 dB limit at −90 |

## 6. Verification

### 5.1 Unit and measurement tests (`make test`): 252 DSP checks + 94 interface checks + 5 converter tests, 0 failures

LR4 and gentle crossovers sum flat (0.05 dB over 13 tones) and keep pink noise
unchanged; Balance mode halves/removes a spectral tilt as asked; ranges saturate
exactly; limiter never exceeds its ceiling under hostile input; NaN/Inf/1e30/DC/silence
and 300 random or invalid parameter sets stay finite; output is bit-identical for
any block size; AGC levelling slope, ratio 0, range semantics, attack/release times,
static mix, gate and 48/192 kHz independence; bypass (bit-exact copy of the input delayed by the
reported latency, no clicks when toggled every 0.5 s, processing keeps adapting underneath); 5 ms
reported latency.

### 5.2 Against BreakawayOne

| Test | Result |
|---|---|
| All 31 factory presets, converted by formula (no per-preset fitting), 5 spectra at −24 dBFS, absolute gain curves | 1.45 dB RMS overall, median 0.80 dB; 20 of 31 under 1 dB, 27 under 1.5 dB |
| Other input levels, never used in fitting (6 presets, −34 and −14 dBFS, 2 spectra) | 0.95 dB RMS, mean offset −0.15 dB |
| End to end inside the local Carla engine at 192 kHz, Plutonium preset loaded from the converted `.ttl`, pink-ish noise | 0.64 dB and 0.38 dB RMS against BreakawayOne's 48 kHz measurement |

### 5.3 Known limits (not hidden)

* Presets with strong noise reduction differ most: New York (5.1 dB), Neutral Voice (3.5 dB; also a 3-band layout), LEIFF (2.2 dB). BreakawayOne has a per-band downward expander (`ng_*`, "noise reduction") that this plugin does not implement.
* Presets with 2, 3 or 4 bands are mapped onto the five bands by position, because their band layout was not measured.
* Not translated at all: wide-band AGC and compressors, clippers and limiters, the parametric EQ and bass boost (`peq_*`), stereo enhancer, sub-bass synthesis, FM/AM filters, pre-emphasis. A converted preset therefore reproduces the *tonal compression and band mix* of the original, not the whole processor.
* Test signals were noise. Behaviour on music, speech and transients was not measured against the original, and the sound has so far been judged only informally by ear. Time constants were measured with a 100 ms resolution.
* The `alt` (speech) settings of some presets are ignored.
* Patent status: see section 1.

## 7. How the measurements were made

BreakawayOne ran under WINE in a private virtual display, its audio bound to two private
PipeWire null sinks. Parameters were changed by editing the application's own state file
while it was stopped; test noise went in, the processed audio was recorded from the output
sink with an explicit-device recorder, and the band gains were computed with an
independent FFT analysis. Single-parameter probes gave the rules in section 5; a global fit
over all 31 factory presets gave the constants. Details that bit during the work: a recorder
that silently falls back to the default microphone if its target is missing, and the need to
switch off every other stage of the original chain before measuring the multiband AGC.
