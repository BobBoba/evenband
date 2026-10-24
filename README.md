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

From source (C compiler, `lv2` headers, Python 3):

```
make                          # builds build/evenband.lv2
make test                     # 246 measurement checks + converter tests
make install                  # to ~/.lv2
sudo make install LV2DIR=/usr/lib/lv2   # system-wide
```

## Use

In Carla: Add Plugin → "Evenband" (category dynamics).

Balance mode: **Power** sets how far the balance is corrected (60 % is a good start),
**Range Boost/Cut** cap the correction per band, **Deep Bass / Warmth / Mids / Presence /
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
are not. [SPEC.md](SPEC.md) section 5 gives the match quality and the known limits.

## Status

Verified by measurement (see SPEC.md); judged only informally by ear. Bug reports and
listening feedback are welcome.

## License

MIT, see [LICENSE](LICENSE).
