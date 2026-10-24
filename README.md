# Evenband

A five-band tonal compressor as an LV2 plugin for Linux (Carla, Ardour, Qtractor and any
other LV2 host). Real time, stereo, any sample rate (192 kHz included), 5 ms latency
(reported to the host), look-ahead output limiter.

Evenband evens out the tonal balance of music: tracks with too little or too much bass,
or too much or too little treble, come out balanced. It has two modes:

* **Balance** (default, simple): pulls the spectrum toward a reference shape, independent
  of how loud the music is.
* **AGC**: every band is levelled toward its own absolute target power, the way a
  broadcast multiband processor does it. Its settings are laid out so that presets of
  the Windows processor BreakawayOne can be converted (see below).

## Install

Arch Linux, with the packaging recipe from this repository (works today):

```
git clone https://github.com/BobBoba/evenband && cd evenband/packaging/aur
makepkg -si
```

An AUR package (`yay -S evenband-lv2`) will follow once AUR registration reopens.

From source (C compiler, `lv2` headers, Python 3; `libx11`, `cairo` and `gtk3` development files for the
user interface, each interface is skipped when its libraries are missing):

```
make                          # builds build/evenband.lv2
make test                     # 252 DSP checks, 94 interface checks, converter tests
make install                  # to ~/.lv2
sudo make install LV2DIR=/usr/lib/lv2   # system-wide
```

## Use

In Carla: Add Plugin → "Evenband" (category dynamics), then open the plugin's own window.

**The plugin window.** Double-click the plugin in Carla: the window has every setting of the
plugin as a bar slider (drag, mouse wheel for fine steps, right-click or double-click to reset),
the two modes, the band-overlap choice and the on/off switches, a table of the five bands
(offset, mix, attack, release and the live gain meter) and the four crossover frequencies. A
**Bypass** switch at the top right compares the processed sound with the original: it is
time-aligned, fades in 10 ms and the processing keeps adapting underneath, so flipping it is a
fair A/B.

**Presets.** The list on the left shows five rows at a time and scrolls. One click on a name
applies that preset immediately; the highlighted row is the one that matches the current
settings. *New* returns to the defaults, *Save* overwrites the user preset you are working on
(or asks for a name), *Save as...* asks for a name, *Delete* (click twice) removes a user preset,
*Import...* opens a file chooser (`kdialog` or `zenity`) for a `.ttl` preset or a BreakawayOne
`.amx`. Your own presets are marked with an orange dot and live in `~/.lv2/evenband-user.lv2`
(`$EVENBAND_USER_DIR` overrides), in the same LV2 preset format, so other hosts find them after a
rescan. Importing an `.amx` needs the `evenband-amx2lv2` converter (installed by the Arch package;
in a source tree it is `tools/amx2lv2.py`). Bypass is never stored in presets.

Installed presets (including converted BreakawayOne ones) are found in the usual LV2 folders
(`~/.lv2`, `/usr/lib/lv2`, or `LV2_PATH`). Carla shows the GTK3 version of the window (a native
Wayland window on a Wayland desktop); hosts that only speak X11 use the X11 version (through
XWayland on Wayland). LV2 has no Wayland-specific interface type: X11 and toolkit-embedded
interfaces are the only options it defines.

Balance mode (the sliders in the host's own parameter list): **Power** sets how far the balance is corrected (60 % is a good start),
**Range Boost/Cut** cap the correction per band (the ports allow up to 36 / 60 dB for AGC mode;
Balance mode limits itself to 18 dB), **Deep Bass / Warmth / Mids / Presence /
Brilliance** shift the target of each band, **Tilt** tilts the whole reference, **Speed**
scales every time constant, **Keep Level** holds the overall loudness. The five gain
meters show what each band is doing.

## Acknowledgements

Evenband was inspired by the work of **Leif Claesson** and **Claesson Edwards Audio**:
Breakaway and BreakawayOne, a Windows broadcast audio processor whose multiband tonal
leveling we admire. Evenband is an independent implementation; it contains no code from
that product and is not affiliated with or endorsed by Claesson Edwards Audio.
"Breakaway" and "BreakawayOne" are names of their products.

We wanted that kind of tonal compression in a Linux audio workflow and, to our knowledge,
no free LV2 plugin does it this way. Other tools cover related ground: Stereo Tool offers
broadcast processing on Linux but is proprietary and works differently; Calf and LSP
provide general-purpose multiband dynamics. If you know of a closer match, please open an
issue.

The design is described in [SPEC.md](SPEC.md), including what was measured on the original
(by running it as a black box, never by reading its code) and how closely Evenband
matches it.

## BreakawayOne presets

The presets of BreakawayOne are the work of their authors and are **not included**.
If you own BreakawayOne (or use its trial), convert your own copies for your own use:

```
make presets    # AMX_DIR=... to point at your proc_presets folder
```

(With the AUR package: `evenband-amx2lv2 PRESET_DIR ~/.lv2/evenband-presets.lv2`.)
They appear in your host as "<name> (BreakawayOne)", from a separate bundle,
`~/.lv2/evenband-presets.lv2`. Only the multiband AGC and its band mix are translated;
the noise reduction, wide-band stages, clippers, EQs and stereo enhancer of the original
are not. [SPEC.md](SPEC.md) section 6 gives the match quality and the known limits.

## Status

Verified by measurement (see SPEC.md); judged only informally by ear. Bug reports and
listening feedback are welcome.

## License

MIT, see [LICENSE](LICENSE).
