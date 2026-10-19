# autojam

Gemma sits in with your jam. A native C++ app for the **Retroid Pocket Flip 2** running **ROCKNIX** (and the
ETK/ROCKNIX-GTK kernel): it listens to the room through the handheld's microphone, works out the key, the chord
being played, the tempo and the energy, and a synth band (drums, bass, keys, lead, and a sung voice) improvises
along in time. **Gemma 4 E4B**, running locally on the Snapdragon through [llamatalk](https://github.com/mercurious/llamatalk),
steers that band at phrase level: style, feel, how busy each part is, a motif for the lead, chords for when the
room goes quiet, lyrics that **Pocket TTS** sings through a pitch corrector in the key of the jam, and a spoken
comment now and then. The screen shows what Gemma hears and what it contributes, in real time, so the humans can
play with it, and the gamepad and touchscreen steer it in other directions.

Everything runs on the device: no cloud, no account, no network once the models are downloaded.

> **AI disclosure.** This project was written with Claude Code (Anthropic's Claude Fable 5.1). Most of the
> code and this README are model output, produced under the author's direction. The author chose what to build
> and runs it on the hardware. Treat it as a working prototype, not a finished product: the listener and the
> band have a ground-truth test (`tools/test_analysis.sh`), the whole has not yet been played with a live band
> on the device when this README was written.

```
microphone (PipeWire: USB, Bluetooth, 3.5 mm headset, or the Flip 2's built-in mic on the ETK path)
  -> listener: onset/tempo/beat phase, chroma -> key + chord, YIN pitch, level          (capture thread)
  -> band: clock locked to the room's beat, drums/bass/keys/lead on the chord heard,      (audio thread, core 7, RT)
           Gemma's lines sung through the tuner, mix -> speaker / headphones / USB out
  -> Gemma 4 E4B: llama-server, schema-constrained JSON plans, streamed                   (big cores 4-6, batch prio)
  -> Pocket TTS: tts-server, streamed s16 24 kHz                                          (big cores 4-6, nice 5)
  -> USB MIDI out: notes on channels 1-4 and 10, clock, start/stop; MIDI in: chord hints, mod wheel = energy
```

## What the screen shows

Three columns on the 1920x1080 panel:

- **What Gemma hears** (left): the key with its confidence and the Roman numeral of the current chord; the chord,
  large, with the last chords and how many beats each lasted; the tempo with four beat lights (the bar's first beat
  in orange), locked or still listening; the twelve pitch classes with the chord tones lit; the input level and the
  melody note being played.
- **What Gemma plays** (middle): the plan (title, Gemma's one-line direction, what it believes it hears, style /
  feel / energy chips, the motif), the band (16-step light, a level meter per part, the chord the band is on and
  its BPM), and the lyrics: the line being sung with a progress bar and the note the tuner is forcing, the next
  line, the melodies as dots, Gemma's last spoken comment.
- **Steer Gemma** (right): twelve directions (Calmer, Busier, Darker, Brighter, Sparser, Denser, Lay back,
  Solo!, New lyrics, Change feel, Own chords, Surprise me), toggles (Band on/off, Follow room, Auto plans,
  Gemma sings, Tune my mic, Gemma's ears), the autotune amount and retune speed, band volume, the voice,
  tap tempo, Ask Gemma now, the mic, the output and the USB MIDI port, the voice transposition.

The top bar has the mic in use and its level, the model and voice servers' state (and Gemma's progress while it
writes), the MIDI port and the battery.

## Controls

| Pad | Function |
|---|---|
| D-pad | Move the focus between controls (left/right on a slider changes it) |
| A | Press the focused control |
| B | Tap tempo (four taps set the tempo and the bar) |
| X | Ask Gemma for a full plan now |
| Y | Mute / unmute Gemma's voice |
| L1 / R1 | Previous / next Pocket TTS voice |
| L2 / R2 | Energy down / up (takes effect at once; Gemma is told) |
| L3 | Put your own mic through the tuner (headphones only: speakers feed back) |
| R3 | Gemma's ears on/off (8 s of audio with each full plan) |
| Left stick | Band volume |
| Right stick | Voice volume (vertical), autotune amount (horizontal) |
| Start | Band play / stop |
| Select | Re-pick the microphone (after plugging something in) |
| Select + Start, Guide | Quit |

Touch: tap a control, drag a slider. Desktop (`--window`): arrows, Enter, Space play/stop, T tap, G ask Gemma,
V voice, M mic tuner, -/= energy, Esc quit.

Some directions act at once (Calmer/Busier change the energy, Lay back rests the lead, Solo! sets it to busy,
Change feel steps the drums) and every direction is sent to Gemma, which answers within a short plan.

## How Gemma takes part

Gemma 4 E4B generates about 6 tokens a second on the Flip 2's CPU, so it cannot play notes. The band does that:
it follows the beat and the chord the listener hears, and never waits for the model. Gemma writes **plans**:

- A **full plan** (about 300-600 tokens, 40-90 s on E4B; half that with `AJ_MODEL=e2b`) every two minutes while
  the band plays (`AJ_AUTO`), or when you press X / Ask Gemma / New lyrics / Surprise me: style, feel, energy,
  the four parts' density, a motif, up to four chords for when the room is silent (or to take over with
  `follow=false`), swing, a direction, what it believes it hears (genre, mood, instruments), a verse of 4 lyric lines
  each with a melody as scale degrees (or none, to keep the current verse), the voice, a spoken sentence and a title.
- A **quick plan** (about 150 tokens) after any steering press, honouring the request in the numbers.

Plans are JSON constrained by a schema, sent as a compact GBNF grammar (no whitespace, so plans stay short
and never run into the token limit), so they always parse. They are applied on
the next bar. Lyrics go to Pocket TTS line by line; each line starts on a bar and is sung through the tuner,
forced onto its melody in the key of the jam, so spoken TTS comes out sung. The verse repeats, with a two-bar breath
between rounds, until Gemma writes a new one, so a song keeps going between plans.

**Gemma's ears.** The Gemma 4 E4B audio projector (`gemma-4-E4B-it-mmproj.gguf`, in the shared models directory
when Retro Voice Chat installed it) lets the model hear the last eight seconds of the room with each full plan,
on top of the measurements. The launcher turns this on when the projector is there and the device has 10 GB or
more (`AJ_EARS=1|0` overrides); it costs memory and some seconds per plan. Without it Gemma works from the
listener's numbers alone, which is what the band plays from anyway.

## The microphone

| microphone | notes |
|---|---|
| USB-C or Bluetooth | used first when present (snd-usb-audio is built into the ROCKNIX kernel) |
| 3.5 mm headset (TRRS) | needs the capture pinned to S16: `flip2/install.sh --jack-mic` on stock ROCKNIX (ETK installs the same pin) |
| built-in mic | on the ETK path (the mic device tree + UCM "Internal Microphone"): picked when no headset is in |

The source is chosen the way Retro Voice Chat does it (USB/BT, then the headset if the jack switch says one is
in, then the built-in `[In] Mic` port); Select re-picks it. The band's output goes to the default sink; **Output ›**
cycles through the sinks (speaker, headphones, a USB interface). A microphone in the same room as the speaker
hears the band too: the listener is built for that (the band's chord is the room's chord), but keep the band's
volume under the humans' or it will lead itself. Headphones for the band and the room mic for the humans is the
clean setup.

## USB

- **Host side** (a hub or OTG adapter on the USB-C port): USB audio interfaces and microphones appear as
  PipeWire sources and sinks and are preferred as the mic; USB MIDI keyboards and synths appear as raw MIDI ports
  (`/dev/snd/midiC*D*`). autojam sends the band on MIDI (bass channel 1, keys 2, lead 3, drums 10, General MIDI
  drum notes), with MIDI clock, start and stop; what you play on a MIDI keyboard counts as heard chord tones and
  the mod wheel steps the energy. **USB MIDI ›** cycles the ports; `AJ_MIDI=none` turns it off.
- **Device side** (the Flip 2 as a USB sound card or MIDI port for a computer): the ROCKNIX 20261001 kernel
  builds only the CDC Ethernet and MTP gadget functions, not UAC2 or MIDI, so this is not possible on stock
  ROCKNIX. `tools/usb-gadget-audio.sh status` checks the running kernel; `docs/USB.md` has the kernel config
  delta for the ROCKNIX-GTK lane, after which `tools/usb-gadget-audio.sh on` configures the gadget.

## Install

Build a release (below), then from a computer with ssh access to the handheld:

```sh
flip2/install.sh flip2-12g             # rsync to /storage/apps/autojam, the Ports entry, the models (e4b + tts)
flip2/install.sh flip2-12g --jack-mic  # also the 3.5 mm headset mic pin (stock ROCKNIX; reboot afterwards)
```

The models go to llamatalk's shared directory, so a device that has Retro Voice Chat, autogamer or the rap
machine downloads nothing new. Then Ports > **Autojam** in EmulationStation. Settings live in
`/storage/apps/autojam/data`, logs in `logs/`. `AJ_MODEL=e2b` in the Ports entry picks the small Gemma.

## Build

```sh
build/build.sh                 # podman (ENGINE=docker), Ubuntu 24.04 container -> dist/autojam and a tarball
build/remote.sh etk-cloud      # the same on a remote arm64 docker host; dist/ comes back
```

The llamatalk kit comes from `LLAMATALK_DIST` (an unpacked `dist/llamatalk` from the llamatalk repo's build) or
the pinned release tarball. The listener's test runs anywhere the binary runs:

```sh
tools/test_analysis.sh                 # a synthetic jam (C major, 100 BPM, C F G Am): the listener, then the band's
                                       # own output rendered along to it and analyzed back
autojam --analyze some-recording.wav   # what the listener hears in a file, second by second
autojam --wav jam.wav --render band.wav --seconds 30   # the band plays along to a file, offline, to a WAV
autojam --wav jam.wav --screenshot panel.bmp --no-llm  # the panel after hearing the file
autojam --headless 180 --wav jam.wav   # no screen: band + Gemma + voice for 3 minutes, a log on stdout
autojam --window --wav jam.wav --no-llm   # desktop: a WAV as the mic, no servers
```

The build container can run all of these (`--analyze`, `--render`, `--screenshot` with SDL's offscreen driver,
`--headless` with `--network host` to reach servers on the host).

## Layout of the source

- `src/analysis.*` the listener: spectral-flux onsets, autocorrelation tempo with a phase lock, bar from the kick
  band, peak-picked chroma, Krumhansl-Schmuckler key, chord templates with hysteresis, YIN pitch.
- `src/player.*` the band: the clock that locks onto the room, style and feel patterns, chord voicings, the
  voice bus. `src/synth.h` oscillators, filter, envelopes, drums, delay, reverb. `src/autotune.*` the tuner.
- `src/brain.*` Gemma: the plan schema and prompts, streamed completions, the TTS speaker. `src/http.h`,
  `src/json.h` the plumbing. `src/plan.h` what a plan holds.
- `src/mic.*` PipeWire capture and WAV stand-ins; `src/sysaudio.*` sources, sinks, jack switch, USB cards, raw MIDI,
  battery; `src/midi.*` MIDI in and out.
- `src/ui.*`, `src/gfx.*` the panel; `src/main.cpp` wiring and the offline modes.
- `flip2/` the launcher and the installer; `build/` the container build; `extras/` the capture pin;
  `tools/` the test jam, the listener test and the USB gadget script; `docs/` notes.

Fonts: Liberation Sans (SIL Open Font License). Code: MIT. Models and servers: see `THIRD_PARTY.md`.
