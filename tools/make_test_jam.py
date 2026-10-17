#!/usr/bin/env python3
"""Writes a synthetic jam for testing the listener: a chord loop with a bass line and a drum-like pulse at a known
tempo, so `autojam --analyze` has a ground truth. No dependencies beyond the standard library.
    python3 tools/make_test_jam.py out.wav [--bpm 100] [--key C] [--minor] [--seconds 24]
Default: C major, 100 BPM, C | F | G | Am, two bars each (then repeats), 24 s. The script prints what to expect."""
import argparse, math, random, struct, wave

NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
RATE = 48000


def midi_hz(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--bpm", type=float, default=100)
    ap.add_argument("--key", default="C")
    ap.add_argument("--minor", action="store_true")
    ap.add_argument("--seconds", type=float, default=24)
    a = ap.parse_args()
    key = NOTES.index(a.key)
    # I IV V vi in major; i VI VII iv in minor
    if a.minor:
        prog = [(0, "m"), (8, ""), (10, ""), (5, "m")]
    else:
        prog = [(0, ""), (5, ""), (7, ""), (9, "m")]
    beat = 60.0 / a.bpm
    n = int(a.seconds * RATE)
    out = [0.0] * n
    random.seed(1)
    bar = 4 * beat
    t_bar = 0.0
    bar_i = 0
    while t_bar < a.seconds:
        root, q = prog[(bar_i // 2) % 4]
        root_pc = (key + root) % 12
        chord = [60 + root_pc, 60 + root_pc + (3 if q == "m" else 4), 60 + root_pc + 7]
        bass = 36 + root_pc
        # keys: chord on beats 1 and 3, decaying
        for b in (0, 2):
            t0 = t_bar + b * beat
            for m in chord:
                hz = midi_hz(m)
                for i in range(int(t0 * RATE), min(n, int((t0 + 2 * beat) * RATE))):
                    t = i / RATE - t0
                    out[i] += 0.12 * math.exp(-t * 1.2) * (math.sin(2 * math.pi * hz * t) + 0.3 * math.sin(4 * math.pi * hz * t))
        # bass: root on 1, fifth on 3, 8ths
        for b in range(8):
            t0 = t_bar + b * beat / 2
            m = bass if b % 4 != 2 else bass + 7
            hz = midi_hz(m)
            for i in range(int(t0 * RATE), min(n, int((t0 + beat * 0.45) * RATE))):
                t = i / RATE - t0
                out[i] += 0.25 * math.exp(-t * 4) * math.tanh(2 * math.sin(2 * math.pi * hz * t))
        # drums: kick on 1 and 3, snare-ish noise on 2 and 4, hats on 8ths
        for b in range(4):
            t0 = t_bar + b * beat
            if b in (0, 2):
                for i in range(int(t0 * RATE), min(n, int((t0 + 0.15) * RATE))):
                    t = i / RATE - t0
                    out[i] += 0.6 * math.exp(-t * 25) * math.sin(2 * math.pi * (50 + 120 * math.exp(-t * 30)) * t)
            else:
                for i in range(int(t0 * RATE), min(n, int((t0 + 0.12) * RATE))):
                    t = i / RATE - t0
                    out[i] += 0.3 * math.exp(-t * 30) * (random.random() * 2 - 1)
            for h in (0, 1):
                th = t0 + h * beat / 2
                for i in range(int(th * RATE), min(n, int((th + 0.03) * RATE))):
                    t = i / RATE - th
                    out[i] += 0.08 * math.exp(-t * 120) * (random.random() * 2 - 1)
        t_bar += bar
        bar_i += 1
    peak = max(abs(v) for v in out) or 1.0
    with wave.open(a.out, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, v / peak * 0.9)) * 32767)) for v in out))
    names = [NOTES[(key + r) % 12] + q for r, q in prog]
    print(f"wrote {a.out}: {a.seconds:.0f} s, {a.bpm:g} BPM, key {a.key} {'minor' if a.minor else 'major'}, chords {' | '.join(names)} (two bars each)")


if __name__ == "__main__":
    main()
