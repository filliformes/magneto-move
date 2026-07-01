# Magnéto — Claude Code context

## What this is
Stereo cassette/tape looper + recorder for Ableton Move, inspired by the Library of
Congress C1 cassette player and the Tascam Portastudio.
Schwung audio FX module. API: `audio_fx_api_v2`. Language: C. Repo: `filliformes/magneto-move`.
Author/attribution: **Filliformes** (never the real name).

Records `audio_inout` (the chain signal at this slot). To loop **external** audio, place
the stock **Line In (LI)** sound generator in the synth slot upstream (Input Mode: Stereo).

## Repo structure
- `design-spec.md` — design intent + signal flow
- `src/dsp/magneto.c` — Move API wrapper + params + DSP (API typedefs embedded, no headers)
- `src/module.json` — metadata + ui_hierarchy + chain_params (desktop installer fetches this)
- `module.json` — root copy, kept in sync
- `scripts/build.sh` — Windows/MSYS-safe Docker ARM64 build (docker create + docker cp)
- `scripts/install.sh` — flat SCP to `ableton@move.local`
- `scripts/Dockerfile` — aarch64 toolchain
- `.github/workflows/release.yml` — CI: version check, build, release, update release.json

## Parameters
**Menu order (6 pages):** Magneto · Perform · Channel · Tape Model · Deck · Recordings
(`_level` → current_page 0/1/2/3/4/5).

Page 0 — **Magneto** (knobs 1-8): **play**, **rec**, varspeed(±2 oct, 0.25x..4.0x),
speed(1 7/8·15/16), side(A·B), tone(post-tape C-1 tilt), model(9 tape models), volume.
`feedback` menu-only.

Page 1 — **Perform** (knobs 1-8): scrub, **jump**, **scan**, reverse, stutter, failure,
**tape_stop**(On/Off), **stop_speed**(60..3000 ms). jump = random playhead jumps, segment
1/2..1/1024 of loop. scan = second playhead (full-loop offset → micro-loop at 100%, summed
in). tape_stop ramps `ts_mult` 1→0 over stop_speed (pitch drops + volume fades to a halt).

Page 2 — **Channel** (knobs 1-8): Tascam-style input strip. trim(±12 dB), high(shelf ±12 dB),
high_freq(5k..12k), mid_freq(250..5k), mid(peak ±12 dB, Q≈0.8), low(shelf 100 Hz ±12 dB),
pan(dup), chan_vol. Menu-only **eq_in** (Input/Tape/Off) routes the EQ. RBJ biquads via
`chan_eq()`. Real dB/Hz units.

Page 3 — **Tape Model** (knobs 1-8, signal order): model, wow, flutter, saturation,
rolloff(HF 1k..18k), lowcut(HP 20..800Hz), hiss, generations(wear). Selecting `model`
overwrites the 7 character params. 9 models in `MODELS[]`, each with a **baked, non-exposed**
voice (mid peaking biquad + tilt bias + noise colour). GL-MK2 + classic cassette/reel EQ.

Page 4 — **Deck** (knobs 1-8): play, **fwd**, **bwd** (fast-wind On/Off, ±8× via rate_smooth
accel/decel), rec, clear, input_pan, **pan_mode**(default **Stereo**), **mix**(default 100%).

Page 5 — **Recordings** (menu-only): **rec_mode**(Monitor/Loop Only), rec_length(1..30s),
load_a/load_b (filepath browser), blank_a/blank_b, save_recs, recover.

## Monitor / signal flow (chosen design)
Input → **channel strip** (trim → [EQ if `eq_in`=Input] → chan_vol → pan) = `cond` (the
**recorded** signal + the monitor source). Two separate tape-color paths: `tape_main` (loop)
and `tape_mon` (input monitor), each its own `tape_state_t` (so both can colour at once).
`apply_tape_color` = sat→model EQ→rolloff→lowcut→**hiss(×0.01)**; then `apply_tone` (post-tape
C-1 tilt around 1 kHz, so **hiss brightness follows Tone**).
**Rec Mode** (`rec_mode`, default **Monitor**): while playing, input is always monitored
(`mon`) through tape+EQ and the loop plays on top at **Mix = loop level** — external audio is
always audible. **Loop Only** = equal-power Mix(raw dry, wet); Mix 0 = raw clean, Mix 100 =
loop only.
**Channel Mode** (`eq_in` key, "Channel Mode" label): routes the WHOLE strip (Trim+EQ+Vol+Pan
via `channel_strip()`) — Input (on `cond`, recorded) · Tape (on the played loop) · Off.
**Scrub** = turntable jog: knob move sets `jog_target` + a ~0.3 s `jog_hold`; the playhead then
*chases* the target (`JOG_CHASE`, capped `JOG_VEL_CAP`=16×) — smooth, continuous, pitch follows
turn speed, plays even when stopped.
**Fwd/Bwd** wind up over ~3-4 s (`rate_coeff` 0.00002 while winding, else 0.0025).
**Scan** micro-loop floored at `SCAN_MIN_WIN`=4000 (≫ crossfade → no single-cycle buzz).
**Jump/Stutter** position jumps get a ~7 ms declick crossfade (`xj_pos`/`xj_count`).
**Loop seam:** ~20 ms equal-power crossfade-loop (end faded into start) near the wrap so the
loop point doesn't click (forward playback; standard overlap = loop effectively −20 ms).
**Feedback = classic sound-on-sound**: overdub writes `raw_old_loop·feedback + input` (raw
loop read at the write head — NOT the processed wet). Feeding the processed wet back re-warped
it with wow/flutter + tape colour every pass → wobbly compounding garbage; reverted. (A proper
"resample the performance" mode would need a separate output-capture path.)
**Master limiter:** Airwindows **ClipOnly2** (MIT) soft ceiling at −0.2 dBFS on the final
output (`clip_l`/`clip_r`), before the int16 clamp — tape-style, not hard clipping.
**20 ms smoothing** (`sm_*`, per-block one-pole) on all Perform + Channel continuous knobs.
Click-free: Side A↔B crossfade + Scan micro-loop crossfade; Reverse/Var-Speed/Fwd/Bwd via
`rate_smooth`. Channel EQ/Stop Speed/Freqs carry real units (dB/Hz/ms/sec) via `meta.unit`.
Buffers = 30 s/side (**MAX_LOOP_SEC=30**, ~10.6 MB/instance); `rec_length` caps live takes.
GL-MK2-derived: lowcut, generations (Tape); stutter, failure (Deck) — see design-spec.

## Transport (menu-driven — chosen design)
Transport is **menu triggers only** (Deck page) — guaranteed to work regardless of CC
routing, since Move's sequencer may consume CC85/86. `move_audio_fx_on_midi` is still
wired as a *bonus*: CC85 Play · CC86 Record · CC3 jog-click=reverse · CC14 jog=scrub —
but the module is fully operable from the Deck menu without any MIDI.

## Critical Schwung rules followed (from /move + /move-schwung)
- `"audio_in": true` in capabilities — **required** or the module may silently fail to
  load (confirmed Deforme). `"midi_in": true` for transport CCs.
- NO non-standard root fields (license/source_url) — host JSON parser may reject.
- ui_hierarchy in module.json ONLY; DSP returns chain_params + knob_N_* + handles `_level`.
- chain_params lists ALL params from ALL pages; clean `name`s (page gives context).
- Init symbol MUST be `move_audio_fx_init_v2`; on_midi also exported (`move_audio_fx_on_midi`).
- get_param returns **-1** for unknown keys (not 0); enums return name strings.
- Page-aware knob overlay: `_level` updates current_page; KNOB_MAP (Main) / KNOB_MAP_P2
  (Tape) / KNOB_MAP_DECK (Deck — all-NULL, menu-only page).
- Transport triggers are `type:"enum"` (NOT float/int) per Bobines/Signal lessons; the
  host refreshes enum display from get_param so play/rec/reverse stay in sync.
- No malloc/printf/locks in process_block; buffers calloc'd in create_instance.
- `-ffast-math` + explicit denormal guards (1e-25f) on one-pole states.
- Equal-power dry/wet (cos/sin), hoisted per block.
- Overdub feedback capped ×0.95 + tape soft-clip in the loop (no runaway).
- Power cycle (or remove/re-add FX) required after module.json changes — host caches it.
- No systemctl/schwung service; modules dlopen'd by MoveOriginal.

## Memory
Buffers now sized for **MAX_LOOP_SEC=30** → 2 sides × 30s × stereo int16 = **~10.6 MB/
instance** (accepted to allow up to 30 s recordings; the `rec_length` param, default 15 s,
caps live takes below the buffer ceiling). This is well over the old 5 MB note — watch RAM
with multiple instances; consider lowering MAX_LOOP_SEC if it bites.

## Persistence (loops travel with the Set)
Each instance mints a random 16-hex-char `loop_id` in create_instance. Loop audio is
written to `module_dir/magneto_<id>_{A,B}.raw` + `magneto_<id>.meta` (on record-stop, on
`clear`, and on destroy). The `loop_id` round-trips inside the small `state` string
(`get_param("state")` / `set_param("state")`) — so a saved Set restores its `loop_id` and
reloads exactly its own loops, and concurrent instances never collide on the shared
`module_dir`. A blank instance gets a fresh id and starts empty.
NOTE: orphaned `.raw` files from discarded blank instances are not garbage-collected yet.

**Swap footgun + Recover:** swapping Magneto out of a slot overwrites the slot's saved
`id=`, orphaning the on-disk `.raw` (still saved, just unreachable). The Deck **Recover
Loop** trigger (`recover_loop()`) rescues it: scans `module_dir`, loads the newest
non-empty saved loop, adopts its id.
**Confirmed on-device (2026-06-25):** Per-Set state lives in
`set_state/<set-uuid>/slot_N.json` → `chain.audio_fx[i].params.state` (the full state
string incl. `id=`); the standalone `slot_state/*.json` dir is unused/empty. But
`create_instance` receives `config_json == NULL` and the host never passes the
set-uuid/slot-index to the DSP — so the plugin can't anchor a stable id, and automatic
recovery-on-swap is impossible. Three recovery paths exist instead:
 1. **Set travel** — `id=` in set_state reloads on Set reopen (primary).
 2. **Recover Loop** (Deck) — scans disk for the newest non-empty loop.
 3. **Presets** — `presets/magneto/<name>.json` stores the state string incl. `id=`, so a
    saved preset is a named bookmark to its loop; loading it recalls that loop.
CONSTRAINT for any future orphan-GC: never delete a `magneto_<id>_*.raw` whose `id` is
referenced by a preset or a set_state file — only truly unreferenced ids are safe to prune.

## Sample I/O (Recordings page)
Relies on Schwung's stock **`filepath` browser** (`shadow/shadow_ui.js` + `shared/
filepath_browser.mjs`, already on-device — same mechanism Granny/MrDrums use). No host
porting needed; a `chain_param` of `type:"filepath"` opens the folder browser on select.
- **Load:** `set_param("load_a"/"load_b", <abs path>)` → `load_wav_into_side()` parses RIFF
  (PCM 8/16/24/32 + float32, mono/stereo; parser adapted from Granny), linear-resamples to
  44.1k, truncates to MAX_LOOP_SEC, writes int16 into `loop[side]`, then `save_loops()`.
  Decode is capped to ~tape-length of *source* frames to bound transient RAM.
- **Export:** `save_recs` (active side) → `export_wav()` writes a canonical 44-byte stereo
  16-bit 44.1k WAV to `RECS_DIR` = `/data/UserData/UserLibrary/Magneto Recs` (auto-`mkdir`
  on each create_instance). Filename `M<n><Side>_YYYYMMDD_HHMM.wav`; `<n>` = per-session
  instance number from a process-static `g_next_instance` counter (resets on host restart —
  date/time keeps names unique). Recs live under UserLibrary so the Move's native sampler
  sees them too.
- **Scrub:** `scrub` (Deck knob 6) sets `play_pos = scrub·(len-1)` when the knob moves
  (`scrub_prev` change-detect); lets you start playback anywhere and overdub there.
All file I/O is control-thread only (set_param), never `process_block`.

## Verify on device
1. Transport from the Deck menu (primary). Confirm CC85/86/3/14 bonus routing actually
   reaches `on_midi` (may be eaten by the sequencer) — not required for operation.
2. Loop recall: record in a blank instance, save the Set, reopen — loop should return.
   Confirm `module_dir` is writable and `state` round-trips the `id=` field.
3. Memory headroom with multiple instances at 15 s/side.

## Build & deploy
```bash
./scripts/build.sh          # Docker ARM64 cross-compile
./scripts/install.sh        # flat SCP to move.local; then power-cycle
```

## Pipeline
Use `/move` to advance stages. Release via `/move-schwung-release`. First release also
needs a module-catalog.json PR to charlesvestal/schwung.

## Credits / license
Inspired by the LoC C1 and Tascam Portastudio. Original DSP, nothing ported. MIT.
