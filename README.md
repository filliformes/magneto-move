# Magnéto

**A stereo cassette/tape looper + recorder for the Ableton Move**, built for the
[Schwung](https://github.com/charlesvestal/schwung) open plugin framework. Inspired by the
**Library of Congress C‑1** variable‑speed cassette player and the **Tascam Portastudio**.

Magnéto records the audio flowing through its chain slot onto two sides of virtual tape, and
plays it back with real tape character — wow, flutter, saturation, hiss, band‑limiting and
per‑machine EQ voices — plus a full performance surface: variable speed, reverse, fast‑wind,
scrub, jump, scan, stutter, dropouts and tape‑stop. It records and loads WAV files, and every
recording travels with your Set.

> Author: **Filliformes** · License: **MIT** · Original DSP, nothing ported (the master
> limiter adapts Airwindows **ClipOnly2**, MIT).

---

## Table of contents
- [What it is](#what-it-is)
- [Requirements](#requirements)
- [Install](#install)
- [Getting started (5 minutes)](#getting-started-5-minutes)
- [How it works — signal flow](#how-it-works--signal-flow)
- [The six menus](#the-six-menus)
  - [1 · Magnéto (main)](#1--magnéto-main)
  - [2 · Perform](#2--perform)
  - [3 · Channel](#3--channel)
  - [4 · Tape Model](#4--tape-model)
  - [5 · Deck](#5--deck)
  - [6 · Recordings](#6--recordings)
- [Behaviour notes & tips](#behaviour-notes--tips)
- [Persistence — where your loops live](#persistence--where-your-loops-live)
- [Build & deploy from source](#build--deploy-from-source)
- [Known limitations](#known-limitations)
- [Credits](#credits)

---

## What it is

Magnéto is an **audio FX** module (Schwung `audio_fx_api_v2`). It processes the stereo signal
at its slot in the chain, so **whatever is upstream is what gets recorded**:

- To loop **external audio** (line‑in, an instrument, a mic), place the stock **Line In** (LI)
  sound generator in the synth slot *before* Magnéto, set its Input Mode to **Stereo**, and
  Magnéto records its output.
- To loop an **internal Move sound**, put Magnéto after that track/instrument in the chain.

It gives you **two sides of tape** (A = tracks 1‑2, B = tracks 3‑4, C‑1 style), each up to
**30 seconds**, that you can record, overdub, load samples into, mangle live, and export.

---

## Requirements

- An **Ableton Move** running **Schwung** (Move Everything). Magnéto uses Schwung's stock
  file‑browser, so no extra host patching is needed.
- To capture external audio: the stock **Line In** module (ships with Schwung).

---

## Install

**From the Schwung Manager / Module Store** (once published): search for *Magneto* and install.

**Manually** (SSH), if you have a built `magneto.so` + `module.json`:

```bash
DEST=/data/UserData/schwung/modules/audio_fx/magneto
ssh ableton@move.local "mkdir -p $DEST"
scp dist/magneto/magneto.so dist/magneto/module.json ableton@move.local:$DEST/
# then POWER‑CYCLE the Move (it caches module.json at startup)
```

After any `module.json` change you must **power‑cycle** the Move (removing/re‑adding the FX
reloads the `.so` but not `module.json`).

---

## Getting started (5 minutes)

1. **Route audio in.** Add **Line In** (Stereo) to the synth slot, then add **Magneto** as an
   audio FX after it. Play something into the Move's input — you should hear it monitored
   through Magnéto's tape colour.
2. **Record side A.** Go to the **Magnéto** page → turn **Rec** to `Rec`. Play your part.
   Turn **Rec** back to `Stopped` — the loop closes and starts looping.
3. **Play along / overdub.** By default (**Rec Mode = Monitor**) you always hear your live
   input on top of the loop, so you can jam over it. Turn **Rec** to `Rec` again to **overdub**
   another layer; **Feedback** (menu on the Magnéto page) sets how much of the old loop is
   kept (0 % = replace, 100 % = full sound‑on‑sound).
4. **Play with it.** On the **Perform** page ride **Scan**, **Jump**, **Stutter**, **Failure**
   and **Tape Stop**; on **Magnéto** sweep **Var Speed** (±2 octaves) and flip **Side**.
5. **Pick a tape.** On the **Tape Model** page choose a machine (Type I…Studio) — each has its
   own EQ voice, wow/flutter and hiss.
6. **Keep it.** On **Recordings** → **Save to Recs** exports the current side as a WAV to
   `UserLibrary/Magneto Recs`. Your loop also travels automatically inside a saved Set.

---

## How it works — signal flow

```
          ┌───────────── Channel strip (Trim → Low → Mid → High → Vol → Pan) ─────────────┐
 INPUT ───┤  (Channel Mode routes this to the Input, the Tape, or Off)                    │
          └──────────────────────────────┬───────────────────────────────────────────────┘
                                          │  = "conditioned input" (recorded + monitored)
                         ┌────────────────┴─────────────────┐
             record ◄────┤  RECORD: raw old loop × Feedback + input  (classic sound‑on‑sound)
                         └────────────────┬─────────────────┘
                                          │
 TWO SIDES OF TAPE (A/B, 30 s each) ──────┤ playback head (Var Speed, Reverse, Fwd/Bwd,
                                          │ Scrub jog, Jump, Scan, Stutter, Tape Stop)
                                          ▼
   Tape colour: saturation → model EQ → HF rolloff → low cut → hiss → post‑tape Tone
                                          ▼
   MIX  (Monitor: input always audible + loop at Mix level · Loop Only: dry/wet crossfade)
                                          ▼
   Master VOLUME  →  ClipOnly2 soft limiter (tape‑style ceiling)  →  OUTPUT
```

Two design choices worth knowing:

- **Tape character is applied on *playback*, not baked into the recording.** Overdub retains
  the *raw* old loop (× Feedback) so wow/flutter, hiss and saturation never compound into a
  wobbly mess — the tape voice is re‑applied fresh every pass, exactly like a real deck.
- **The dry/monitor path is your clean input.** Turning **Mix** down gives you the untouched
  input; the tape only colours the loop.

---

## The six menus

Menu order: **Magnéto · Perform · Channel · Tape Model · Deck · Recordings.**

### 1 · Magnéto (main)

The everyday controls. Knobs 1‑8:

| Knob | Control | What it does |
|---|---|---|
| 1 | **Play** | Start / stop loop playback (fades in/out, no click). |
| 2 | **Rec** | Arm record / overdub. First take records a fresh loop; later takes overdub. |
| 3 | **Var Speed** | Playback speed **0.25×…4× (±2 octaves)**, 1× at centre. Re‑pitches like tape. |
| 4 | **Speed** | Base tape speed: **1 7⁄8 ips** (normal) or **15⁄16** (half = one octave down, ×2 length). |
| 5 | **Side** | Switch tape **A (1‑2)** / **B (3‑4)**, with a click‑free crossfade. |
| 6 | **Tone** | Post‑tape treble tilt (C‑1 style). Bright boosts highs & the hiss; dark rolls them off. |
| 7 | **Tape** | Selects one of 9 tape models (see Tape Model page). |
| 8 | **Volume** | Master output level. |

*Menu‑only:*
- **Feedback** — overdub retention (0 % replace → 100 % sound‑on‑sound).
- **Sync Mode** — **Free** (Rec Length in seconds, the default) or **Sync** (loop length locked to bars/beats).
- **Rec Length** — max length of a fresh take, 1–30 s (used in Free mode; duplicated from Recordings).
- **Sync Div** — the musical loop length in Sync mode: 1 Beat · 2 Beats · 1 Bar · 2 Bars · **4 Bars** (default) · 8 Bars · 16 Bars.
- **Tempo** — 20–999 BPM. Used as the sync tempo when no MIDI clock is present.

**Tempo‑synced loops.** In **Sync** mode a fresh recording auto‑stops at exactly the chosen
**Sync Div**, so the loop is a whole number of bars. When Move's **MIDI clock** is running
(enable *MIDI Clock Out* in Move's settings), the take is **locked to the clock itself** — it
counts clock ticks and closes on the exact beat, immune to tempo jitter or drift. With no
clock, it falls back to the manual **Tempo** knob — set that to your project's BPM. Because a
side holds 30 s, long divisions only fit at higher tempos: **2 Bars** always fits (any tempo
down to 20 BPM), **4 Bars** needs ≥ 32 BPM, **8 Bars** ≥ 64 BPM, **16 Bars** ≥ 128 BPM;
anything longer than 30 s is clamped to 30 s. Sync affects new takes only — overdubs keep the
existing loop length.

### 2 · Perform

Live playhead mangling. Everything here is smoothed (~20 ms) so you can ride it.

| Knob | Control | What it does |
|---|---|---|
| 1 | **Scrub** | Turntable jog — turn it to drag the playhead through the loop; **pitch follows how fast you turn**. Works stopped or playing. |
| 2 | **Jump** | Random playhead jumps. From 1 % (plays ~½ the loop before jumping) to 100 % (tiny 1⁄1024 fragments). Declicked. |
| 3 | **Scan** | A *second* playhead summed in. Low = a full‑length copy offset in time (doubling/phasing); high = a shrinking micro‑loop. |
| 4 | **Reverse** | Play the tape backwards (eases through zero, no click). |
| 5 | **Stutter** | Broken‑tape glitching: the playhead sticks, jumps back, or flips direction. Declicked. |
| 6 | **Failure** | Sporadic short dropouts (mutes), as if the tape were failing. |
| 7 | **Tape Stop** | Engage the tape‑stop ramp (On/Off) — pitch drops and volume fades to a halt. |
| 8 | **Stop Speed** | How long Tape Stop takes: **60 ms … 10 s**. |

### 3 · Channel

A Tascam‑424‑style channel strip. EQ gains are centre‑detent (0 dB in the middle).

| Knob | Control | Range |
|---|---|---|
| 1 | **Trim** | Input gain, ±12 dB |
| 2 | **High** | High‑shelf gain, ±12 dB |
| 3 | **High Freq** | High‑shelf corner, 5 k…12 kHz (≈10 kHz default) |
| 4 | **Mid Freq** | Swept mid centre, 250 Hz…5 kHz |
| 5 | **Mid** | Mid peak gain, ±12 dB (fixed broad Q ≈ 0.8 — the swept band is the "instrument") |
| 6 | **Low** | Low‑shelf gain @ ~100 Hz, ±12 dB |
| 7 | **Pan** | Position in the stereo field |
| 8 | **Volume** | Channel level (separate from the master Volume) |

*Menu‑only:* **Channel Mode** — where the whole strip acts:
- **Input** (default): shape the signal **before** it's recorded.
- **Tape**: shape the **played‑back loop** (EQ / pan / trim a recording *after* the fact).
- **Off**: bypass the strip entirely.

### 4 · Tape Model

The tape voice. Selecting a **model** overwrites the seven character knobs with that machine's
preset; tweak from there. Each model also has a **baked, non‑editable** voice (a midrange EQ,
a spectral tilt and a hiss colour) drawn from the Generation Loss MKII model set and classic
cassette/reel EQ.

| Knob | Control |
|---|---|
| 1 | **Tape** (model selector, same as Magnéto knob 7) |
| 2 | **Wow** — slow pitch drift |
| 3 | **Flutter** — fast pitch flutter |
| 4 | **Saturation** — tape drive |
| 5 | **HF Rolloff** — high‑frequency loss (1 k…18 kHz) |
| 6 | **Low Cut** — high‑pass / low thinning (20…800 Hz) |
| 7 | **Hiss** — noise floor (coloured per model) |
| 8 | **Generations** — a "wear" macro: narrows the band, adds noise and instability |

**Models:** Type I · Type II · Type IV · Worn · Radio · VCR · Dictaphone · Microcass · Studio.

### 5 · Deck

Transport, the tape‑deck way.

| Knob | Control |
|---|---|
| 1 | **Play** (same as Magnéto) |
| 2 | **Fwd** — fast‑wind forward; winds up to ~8× over **3‑4 s** with the accelerating tape sound |
| 3 | **Bwd** — fast‑wind backward |
| 4 | **Rec** |
| 5 | **Clear** — wipe the active side |
| 6 | **Pan** — input pan (same param as the Channel strip) |
| 7 | **Pan Mode** — **Stereo** (pan = L/R balance, default) or **Mono** (sum → pan) |
| 8 | **Mix** — dry (raw input) ↔ wet (tape). Default **100 %** |

### 6 · Recordings

Sample I/O and rescue (menu‑only).

| Item | What it does |
|---|---|
| **Rec Mode** | **Monitor** (default — external audio is always audible so you can play on top) / **Loop Only** (Mix crossfades input↔loop). |
| **Rec Length** | Max length of a fresh take, **1…30 s** (default 15 s). |
| **Load A / Load B** | Open the file browser (`UserLibrary`) and load a `.wav` into side A or B — decoded, resampled to 44.1 kHz, truncated to tape length. |
| **Blank A / Blank B** | Clear a side to empty tape. |
| **Save to Recs** | Export the active side to `UserLibrary/Magneto Recs` as `M<n><Side>_YYYYMMDD_HHMM.wav`. |
| **Recover Loop** | Rescue the newest loop on disk — use it if you accidentally swapped Magnéto out of a slot (see below). |

---

## Behaviour notes & tips

- **Two "Volume" knobs.** Magnéto/Deck **Volume** = master output. Channel **Volume** = channel
  fader before the tape. Both exist on purpose.
- **Monitor mode (default) vs Loop Only.** In Monitor you always hear your live input to jam
  over the loop; **Mix** sets the loop level. In Loop Only, **Mix** is a dry/wet crossfade —
  turn it down for the clean, untouched input.
- **Overdub is clean sound‑on‑sound.** Feedback keeps the raw old loop; the tape colour is
  re‑applied on playback, so layers don't turn to mush. Feedback 100 % = infinite sustain.
- **Scrub** is a jog wheel: the pitch tracks your turn speed, and it plays even when stopped —
  great for finding a spot, scratching, or dropping in an overdub anywhere.
- **Channel Mode = Tape** lets you EQ/pan/trim a loop **after** you recorded it.
- **Everything that jumps is declicked** (Play/Stop, Side switch, Jump, Stutter, Scan, the loop
  seam) and the master output goes through a tape‑style **ClipOnly2** soft ceiling.
- **Tape saturation is unity‑gain** — the drive stage is level‑compensated, so Saturation (and
  the tape models) add colour and harmonics without pumping the volume up.

---

## Persistence — where your loops live

- **With your Set.** Each instance mints a random ID; the loop audio is saved next to the module
  and the ID rides inside the Set. Reopen the Set and the loop returns.
- **As presets.** A saved Magnéto preset stores that ID too, so loading the preset recalls its
  loop — presets double as named loop bookmarks.
- **Recover Loop.** Swapping Magnéto out of a slot orphans the on‑disk audio (still saved, just
  unlinked). **Recordings → Recover Loop** scans the folder and reloads the newest real take.
- **As WAV.** **Save to Recs** writes a normal WAV to `UserLibrary/Magneto Recs`, visible to the
  Move's own sampler.

---

## Build & deploy from source

Requires Docker (for the ARM64 cross‑compile) and SSH access to `ableton@move.local`.

```bash
./scripts/build.sh          # Docker aarch64 cross‑compile → dist/magneto/
./scripts/install.sh        # flat SCP to the Move; then power‑cycle
```

- `src/dsp/magneto.c` — the whole DSP + Move API wrapper (single file, C, API typedefs embedded).
- `src/module.json` — metadata, UI hierarchy and `chain_params` (the root `module.json` is a
  synced copy).
- `.github/workflows/release.yml` — CI: version check, build, GitHub release, `release.json` bump.

---

## Known limitations

- **Memory:** ~10.6 MB per instance (2 sides × 30 s stereo). Fine for a couple of instances;
  watch RAM if you stack several.
- **Overdub at Var Speed ≠ 1×** drifts the write head vs the play head (inherent to the design).
- Perform effects are heard live but are **not baked into the recording** (overdub keeps the
  raw loop by design — a dedicated "resample the performance" mode would be the way to capture
  them).
- Reverse playback isn't crossfaded at the loop seam (forward playback is).

---

## Credits

- Inspired by the **Library of Congress C‑1** cassette player and the **Tascam Portastudio**.
- Tape‑model EQ voices researched from the **Chase Bliss Generation Loss MKII** model set and
  classic cassette/reel response.
- Master limiter adapts **Airwindows ClipOnly2** by Chris Johnson (MIT).
- Built on **Schwung** (Move Everything) by charlesvestal and contributors.
- DSP and design by **Filliformes**. MIT licensed.
