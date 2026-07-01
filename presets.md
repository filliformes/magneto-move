# Magnéto — Tape Model Presets

Reference for the 9 tape models in `src/dsp/magneto.c` (`MODELS[]`). Selecting a model on
the **Main** (knob 6) or **Tape** (knob 1) page seeds the 8 exposed Tape knobs, and applies
the model's **baked voice** (midrange EQ + tilt + noise colour, not user-editable).

Researched from the Generation Loss MKII model set (CPR-3300 VCR gens, MS-WALKER,
Model 12 reel, CAM-8 camcorder, DICTATRON, FISHY 60 toy) and classic cassette/reel EQ.

## Exposed knob seeds

Shown as they read on the Move (Wow/Flutter/Saturation/Hiss/Generations as %, Rolloff &
Low Cut converted to Hz). Raw 0–1 value in parentheses where the display differs.

Rolloff Hz = `1000 + rolloff·17000` · Low Cut Hz = `20 + lowcut·780`.

| Model | Wow | Flutter | Saturation | HF Rolloff | Low Cut | Hiss | Generations |
|---|---|---|---|---|---|---|---|
| Type I     | 18% | 20% | 30% | 11.5 kHz (.62) | 59 Hz (.05)  | 20% | 12% |
| Type II    | 10% | 14% | 20% | 14.9 kHz (.82) | 43 Hz (.03)  | 10% | 6%  |
| Type IV    | 6%  | 9%  | 12% | 16.8 kHz (.93) | 36 Hz (.02)  | 5%  | 3%  |
| Worn       | 55% | 58% | 45% | 6.4 kHz (.32)  | 238 Hz (.28) | 30% | 60% |
| Radio      | 25% | 30% | 55% | 3.7 kHz (.16)  | 410 Hz (.50) | 35% | 45% |
| VCR        | 33% | 26% | 40% | 8.1 kHz (.42)  | 215 Hz (.25) | 25% | 45% |
| Dictaphone | 38% | 55% | 55% | 4.4 kHz (.20)  | 449 Hz (.55) | 45% | 58% |
| Microcass  | 28% | 75% | 45% | 3.4 kHz (.14)  | 488 Hz (.60) | 50% | 62% |
| Studio     | 5%  | 7%  | 35% | 17.2 kHz (.95) | 36 Hz (.02)  | 6%  | 2%  |

## Baked voice (not user-editable)

Midrange peaking EQ (RBJ biquad) + tilt bias (added on top of the user Tone) + hiss colour
(0 = dark/rumbly one-pole-LP'd noise … 1 = bright/white noise).

| Model | Mid freq | Mid gain | Mid Q | Tilt bias | Noise colour |
|---|---|---|---|---|---|
| Type I     | 1.2 kHz | +2.5 dB | 0.8 | −0.10 | 0.55 |
| Type II    | 3.0 kHz | +1.5 dB | 0.7 | +0.20 | 0.70 |
| Type IV    | 5.0 kHz | +0.5 dB | 0.6 | +0.05 | 0.80 |
| Worn       | 900 Hz  | +4.0 dB | 1.4 | −0.45 | 0.25 |
| Radio      | 1.6 kHz | +6.0 dB | 1.8 | −0.20 | 0.60 |
| VCR        | 2.5 kHz | +3.0 dB | 1.0 | −0.05 | 0.65 |
| Dictaphone | 1.5 kHz | +7.0 dB | 2.0 | −0.15 | 0.75 |
| Microcass  | 2.2 kHz | +5.0 dB | 1.6 | +0.10 | 0.85 |
| Studio     | 200 Hz  | +2.0 dB | 0.7 | 0.00  | 0.40 |

## Notes
- The Rolloff/Low Cut Hz values are the **seeded** knob positions. At playback the
  **Generations** knob degrades further: narrows Rolloff (`×(1 − gen·0.7)`, floored at
  400 Hz), raises Low Cut (`lowcut + gen·0.4`), adds noise (`hiss + gen·0.05`), and
  increases wow/flutter depth (`×(1 + gen·0.6)`).
- Model selection seeds knobs 2–8 on the Tape page; tweak freely afterward. The baked
  voice always tracks the currently-selected model index.
- Character at a glance: **Type I/II/IV** = clean cassette ladder (ferric→metal);
  **Studio** = pristine reel with a low head-bump; **Worn** = dark, honky, falling apart;
  **Radio** = mid-forward AM band-limit; **VCR** = video presence; **Dictaphone/Microcass**
  = nasal, narrow, hissy voice recorders.
