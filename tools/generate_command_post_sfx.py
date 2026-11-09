"""Gera o tema de UI "command post" (WWII): sons secos e mecânicos.

Sem dependências externas e sem licença de terceiros: tudo é sintetizado aqui.
Uso:
    <python> tools/generate_command_post_sfx.py
Saída:
    assets/audio/interface_command_post/<evento>_00N.wav
"""

import math
import os
import random
import struct
import wave

SR = 44100
VARIANTS = 3
OUT = os.path.join("assets", "audio", "interface_command_post")

# (nome do arquivo, construtor) — o nome bate com o enum do ui_sfx.gd
EVENTS = [
    ("click", "click"),
    ("select", "select"),
    ("toggle", "toggle"),
    ("back", "back"),
    ("open", "open"),
    ("close", "close"),
    ("confirmation", "confirm"),
    ("error", "error"),
    ("scroll", "scroll"),
    ("tick", "tick"),
    ("switch", "switch"),
    ("question", "question"),
    ("maximize", "maximize"),
    ("minimize", "minimize"),
]


def _noise(n, rng):
    return [rng.uniform(-1.0, 1.0) for _ in range(n)]


def _lowpass(x, cutoff):
    dt = 1.0 / SR
    rc = 1.0 / (2.0 * math.pi * cutoff)
    a = dt / (rc + dt)
    y = 0.0
    out = []
    for v in x:
        y += a * (v - y)
        out.append(y)
    return out


def _highpass(x, cutoff):
    lp = _lowpass(x, cutoff)
    return [v - l for v, l in zip(x, lp)]


def _env(t, tau):
    return math.exp(-t / max(tau, 1e-6))


def _tone(freq, dur, tau, amp=1.0, harmonics=(1.0,)):
    n = int(SR * dur)
    out = []
    for i in range(n):
        t = i / SR
        e = _env(t, tau)
        v = 0.0
        for k, h in enumerate(harmonics, 1):
            v += h * math.sin(2.0 * math.pi * freq * k * t)
        out.append(amp * e * v)
    return out


def _burst(dur, tau, amp, cutoff, rng):
    n = int(SR * dur)
    x = [amp * _env(i / SR, tau) * rng.uniform(-1.0, 1.0) for i in range(n)]
    return _lowpass(x, cutoff)


def _mix(*parts):
    n = max(len(p) for p in parts)
    out = [0.0] * n
    for p in parts:
        for i, v in enumerate(p):
            out[i] += v
    return out


def _delay(part, seconds):
    return [0.0] * int(SR * seconds) + part


def _paper(dur, rng):
    n = int(SR * dur)
    raw = _noise(n, rng)
    bp = _lowpass(_highpass(raw, 800.0), 4200.0)
    rate = 16.0 + rng.random() * 10.0
    out = []
    for i, v in enumerate(bp):
        t = i / SR
        e = min(1.0, t / 0.015) * math.exp(-t / (dur * 0.55))
        flutter = 0.65 + 0.35 * math.sin(2.0 * math.pi * rate * t)
        out.append(v * e * flutter * 0.55)
    return out


# ---------------------------------------------------------------- eventos

def click(rng):
    return _mix(
        _burst(0.07, 0.007, 1.0, 5200, rng),
        _tone(170 + rng.uniform(-15, 15), 0.10, 0.022, 0.7, (1.0, 0.35)),
    )


def select(rng):
    return _mix(
        _burst(0.03, 0.004, 0.8, 4000, rng),
        _tone(300 + rng.uniform(-20, 20), 0.10, 0.028, 0.6, (1.0, 0.5, 0.2)),
    )


def toggle(rng):
    a = _mix(
        _burst(0.03, 0.004, 0.9, 5000, rng),
        _tone(1150 + rng.uniform(-60, 60), 0.06, 0.012, 0.35, (1.0, 0.6, 0.3)),
    )
    b = _mix(
        _burst(0.03, 0.004, 0.8, 5000, rng),
        _tone(950 + rng.uniform(-60, 60), 0.06, 0.012, 0.3, (1.0, 0.6, 0.3)),
    )
    return _mix(a, _delay(b, 0.045))


def back(rng):
    return _mix(
        _burst(0.06, 0.006, 0.8, 3800, rng),
        _tone(140 + rng.uniform(-10, 10), 0.08, 0.02, 0.5, (1.0, 0.3)),
    )


def open_(rng):
    return _mix(
        _paper(0.16, rng),
        _delay(_mix(
            _burst(0.03, 0.004, 0.8, 5000, rng),
            _tone(190, 0.06, 0.015, 0.4),
        ), 0.13),
    )


def close(rng):
    return _mix(
        _mix(_burst(0.03, 0.004, 0.8, 5000, rng), _tone(160, 0.06, 0.015, 0.4)),
        _delay(_paper(0.14, rng), 0.02),
    )


def confirm(rng):
    return _mix(
        _burst(0.035, 0.004, 1.0, 6000, rng),
        _tone(240 + rng.uniform(-10, 10), 0.09, 0.022, 0.6, (1.0, 0.4)),
        _tone(1400, 0.04, 0.008, 0.25),
    )


def error(rng):
    n = int(SR * 0.22)
    f = 105 + rng.uniform(-5, 5)
    out = []
    for i in range(n):
        t = i / SR
        e = min(1.0, t / 0.01) * math.exp(-t / 0.16)
        sq = 1.0 if math.sin(2.0 * math.pi * f * t) >= 0.0 else -1.0
        trem = 1.0 if (int(t * 30.0) % 2 == 0) else 0.55
        out.append(e * trem * 0.35 * sq)
    return _lowpass(out, 1800.0)


def scroll(rng):
    return _mix(
        _burst(0.02, 0.003, 0.6, 7000, rng),
        _tone(700 + rng.uniform(-40, 40), 0.03, 0.006, 0.25),
    )


def tick(rng):
    return _mix(
        _burst(0.03, 0.003, 0.9, 8000, rng),
        _tone(950 + rng.uniform(-50, 50), 0.045, 0.009, 0.35, (1.0, 0.4)),
    )


def switch(rng):
    a = _mix(
        _burst(0.03, 0.004, 0.9, 5000, rng),
        _tone(820 + rng.uniform(-40, 40), 0.07, 0.014, 0.4, (1.0, 0.5, 0.25)),
    )
    b = _mix(
        _burst(0.03, 0.004, 0.85, 5500, rng),
        _tone(1100 + rng.uniform(-50, 50), 0.07, 0.014, 0.35, (1.0, 0.5, 0.25)),
    )
    return _mix(a, _delay(b, 0.055))


def question(rng):
    return _mix(
        _burst(0.04, 0.01, 0.5, 3000, rng),
        _delay(_tone(620 + rng.uniform(-30, 30), 0.09, 0.02, 0.35), 0.03),
    )


def maximize(rng):
    return _mix(
        _paper(0.18, rng),
        _delay(_mix(
            _burst(0.03, 0.004, 0.8, 5000, rng),
            _tone(220, 0.06, 0.015, 0.4),
        ), 0.15),
    )


def minimize(rng):
    return _mix(
        _mix(_burst(0.03, 0.004, 0.8, 5000, rng), _tone(180, 0.06, 0.015, 0.4)),
        _delay(_paper(0.15, rng), 0.02),
    )


BUILDERS = {
    "click": click, "select": select, "toggle": toggle, "back": back,
    "open": open_, "close": close, "confirm": confirm, "error": error,
    "scroll": scroll, "tick": tick, "switch": switch, "question": question,
    "maximize": maximize, "minimize": minimize,
}


def write_wav(path, samples, peak=0.7):
    n = len(samples)
    fi = int(SR * 0.002)
    fo = int(SR * 0.006)
    for i in range(min(fi, n)):
        samples[i] *= i / fi
    for i in range(min(fo, n)):
        samples[n - 1 - i] *= i / fo
    m = max(1e-9, max(abs(v) for v in samples))
    g = peak / m
    frames = bytearray()
    for v in samples:
        x = int(round(max(-1.0, min(1.0, v * g)) * 32767))
        frames += struct.pack("<h", x)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(bytes(frames))
    return m


def main():
    os.makedirs(OUT, exist_ok=True)
    total = 0
    for event, builder_name in EVENTS:
        builder = BUILDERS[builder_name]
        for v in range(1, VARIANTS + 1):
            rng = random.Random(20261109 + v * 101 + sum(ord(c) for c in event))
            samples = builder(rng)
            path = os.path.join(OUT, f"{event}_{v:03d}.wav")
            peak = write_wav(path, samples)
            total += 1
            print(f"{path}  samples={len(samples)} peak_before_norm={peak:.3f}")
    print(f"OK: {total} arquivos em {OUT}")


if __name__ == "__main__":
    main()
