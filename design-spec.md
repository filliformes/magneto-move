# Magnéto — Design Spec

## Identity
- **Module ID:** `magneto`
- **Name:** Magneto (display without accent for OLED font safety; "Magnéto" in prose)
- **Abbrev:** `MAGNTO`
- **Type:** `audio_fx` (Schwung, `audio_fx_api_v2`)
- **Language:** C
- **Repo:** `filliformes/magneto-move`
- **Attribution:** Filliformes
- **License:** MIT (original DSP, nothing ported)

## Concept
A stereo cassette/tape looper + recorder inspired by the Library of Congress C1
cassette player (variable speed, half/full speed, two sides, reverse, tone) and the
Tascam Portastudio (record + overdub). It records the audio flowing through its chain
slot and plays it back as tape, with variable speed, two sides, and tape colour.

## Signal source / chain placement
- Records `audio_inout` (the chain signal at this FX slot).
- To loop **external** audio, place the stock **Line In (LI)** sound generator in the
  synth slot upstream (Input Mode: Stereo). Its output flows into `audio_inout`.
- `audio_in: true` is set in capabilities (required by the host or the module may fail
  to load — confirmed from Deforme). Direct `mapped_memory + audio_in_offset` reads are
  available if a direct-line-in source is ever wanted, but are not used: the Line In
  module path keeps the looper a clean, composable FX.

## Controls
### Page 1 — Main (8 knobs)
| # | Key | Type | Range / options | Maps to |
|---|-----|------|-----------------|---------|
| 1 | rec | enum | Stopped · Rec | record/overdub (duplicated from Deck) |
| 2 | varspeed | float | 0..1 (0.5 centre) | playback ratio 0.25x..4.0x (±2 oct, exp around 1.0x) |
| 3 | speed | enum | 1 7/8 · 15/16 | base ips; 15/16 = half = octave down, ×2 length |
| 4 | side | enum | A 1-2 · B 3-4 | active loop buffer |
| 5 | tone | float | 0..1 (0.5 flat) | tilt EQ, dark↔bright |
| 6 | model | enum | 9 tape models | seeds Tape-page character |
| 7 | mix | float | 0..1 | dry(live)↔wet(loop), equal-power |
| 8 | volume | float | 0..1 | playback level |

`feedback` (float, overdub retention ×0.95, soft-clipped) is **menu-only** on the Main page.

### Page 2 — Tape (8 knobs, ordered to follow the signal path)
| # | Key | Maps to |
|---|-----|---------|
| 1 | model | Tape/Model selector — the source (duplicated from Main) |
| 2 | wow | motion (read-position mod) |
| 3 | flutter | motion |
| 4 | saturation | tape drive |
| 5 | rolloff | HF Rolloff (LP 1k..18k) — high-frequency loss |
| 6 | lowcut | Low Cut / HP 20..800 Hz — low-frequency loss (GL-MK2 band-limiting) |
| 7 | hiss | noise floor (added after the filters) |
| 8 | generations | "Wear" macro: narrows HF + raises low-cut + adds noise + instability |

Selecting `model` overwrites the 7 character params (wow..hiss + lowcut + generations)
with that model's preset. **Models (9):** Type I, Type II, Type IV, Worn, Radio, VCR,
Dictaphone, Microcass, Studio.

**Baked per-model voice (not user-editable).** Each model in the `MODELS[]` table also
carries a baked **midrange peaking EQ** (`mid_hz`/`mid_gain_db`/`mid_q`), a **tilt bias**
(on top of the user Tone), and a **noise colour** (dark↔bright hiss). These give each
machine a distinct EQ fingerprint, researched from the Generation Loss MKII model set
(CPR-3300 VCR, MS-WALKER, Model 12 reel, CAM-8 camcorder, DICTATRON, FISHY 60 toy) and
classic cassette/reel EQ. e.g. Dictaphone = narrow 1.5 kHz nasal honk + bright hiss;
Worn = dark 900 Hz honk + rumbly noise; Studio = warm 200 Hz head-bump, low smooth noise.

### Page 3 — Deck (8 knobs)
| # | Key | Type | Maps to |
|---|-----|------|---------|
| 1 | play | enum | Stopped · Playing (stop also disarms record) |
| 2 | rec | enum | Stopped · Rec (toggles record/overdub) |
| 3 | reverse | enum | Fwd · Rev |
| 4 | clear | enum | Clear · Cleared (momentary; wipes active side) |
| 5 | input_pan | float | 0..1, 0.5 = centre; pans the input that is recorded + monitored |
| 6 | scrub | float | 0..1 — playhead start position into the active side (scrub & overdub anywhere) |
| 7 | stutter | float | 0..1 — playhead sticks / jumps back / reverses (broken tape) |
| 8 | failure | float | 0..1 — sporadic short dropouts (mutes), click-free gate |
| (menu) | pan_mode | enum | **Mono** (sum→pan, default) / **Stereo** (pan = L/R balance) |
| (menu) | recover | enum | Recover · Recovered — rescue the newest non-empty loop on disk (orphaned by a module swap) |

### Page 4 — Recordings (menu-only — sample I/O)
| Key | Type | Maps to |
|-----|------|---------|
| load_a | filepath | Browse `/data/UserData/UserLibrary` for a `.wav`, import into side A (resample→44.1k, truncate to tape length) |
| load_b | filepath | Same, into side B |
| blank_a / blank_b | enum | Clear a side to blank tape |
| save_recs | enum | Export the **active** side to `Magneto Recs` as `M<n><Side>_YYYYMMDD_HHMM.wav` |

Uses Schwung's stock `filepath` browser (already on-device). `Magneto Recs` lives at
`/data/UserData/UserLibrary/Magneto Recs` (auto-created), so recordings are visible to the
Move's native sampler too. A loaded sample becomes the side's loop — overdub / scrub / save
it exactly like a live take.

**Transport** also works from the encoder menu (every Deck param is enum/float). `on_midi`
still maps CC85/86 + jog (CC3 reverse, CC14 scrub) as a *bonus*. `save` and `_level`
remain `set_param`-only.

**Note — Pan Mode default is Mono:** by the chosen design, the input is summed to mono
then panned, so loops are mono unless Pan Mode = Stereo. Flip the `pan_mode` default in
`create_instance` if stereo-by-default is preferred.

## Recording model
- Two sides (A/B), each a stereo int16 buffer of MAX_LOOP_SEC (15 s) @ 44.1 kHz.
- Fresh take grows until stopped (auto-closes if the buffer fills).
- Overdub mixes `old*feedback + new`, soft-clipped, wrapped within loop length, aligned
  to the play head.
- Playback: variable-speed fractional read with linear interpolation; wow/flutter
  modulate the read position; chain: saturation → baked midrange EQ → tone tilt (+ model
  tilt bias) → HF rolloff → low cut → coloured hiss → failure gate → volume.
- **Click-free switching:** Side A↔B does a ~12 ms equal-power crossfade between the two
  buffers (`xfade_count`/`read_loop`); Reverse and Var-Speed changes are smoothed via
  `rate_smooth` (the playhead eases through zero instead of jumping direction).

## Persistence (loops travel with the Set)
Each instance mints a random 16-hex `loop_id` in `create_instance`. Loop audio is written
to `module_dir/magneto_<id>_{A,B}.raw` + `magneto_<id>.meta` on record-stop, on `clear`,
and on `destroy`. The `loop_id` is carried inside the small `state` string, which Schwung
persists per-slot and restores via `set_param("state", …)` right after `create_instance`
— at which point the saved id's loops are reloaded. So a saved Set recalls exactly its own
loops; a blank instance gets a fresh id and starts empty; multiple instances sharing
`module_dir` never collide. (Schwung does NOT pass loops through `config_json` — the
`state` round-trip is the mechanism, per the Genera lesson.)

## Known challenges / verify-on-device
1. **Transport** — operate from the Deck menu (guaranteed). The CC/jog bonus path
   (CC 85/86/3/14 → `on_midi`) is unverified and non-essential.
2. **Loop recall** — record in a blank instance, save the Set, reopen → loop returns.
   Verify `module_dir` is writable and the `state` `id=` field round-trips.
3. **Memory** — 15 s/side ≈ 5.05 MB/instance. Linear in MAX_LOOP_SEC; watch RAM with
   multiple instances.
4. **Orphan cleanup** — `.raw` files from discarded blank instances aren't GC'd yet.
