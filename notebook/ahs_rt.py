"""Glue for ahs_rt.ipynb. All processing = real C++ (rt-audio engine + AHS releases)
via ahs_bridge. Python only: build, file I/O, path generation, resampling for input."""

import json
import shutil
import subprocess
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

ROOT = Path(__file__).resolve().parents[1]           # rt-audio/
BUILD = ROOT / "build" / "notebook"
OUT = ROOT / "notebook" / "out"
BRIDGE = BUILD / "ahs_bridge"
UPSTREAM = ROOT / "release/ahs/ahs-v7-c0291dd1e/upstream"
FS = 48000


def build():
    """make libs, then compile ahs_bridge against them."""
    cxx = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        raise RuntimeError("need a C++20 compiler")
    subprocess.run(["make", "-C", str(ROOT), "component", "-j4"], check=True,
                   capture_output=True, text=True)
    BUILD.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    cmd = [cxx, "-std=c++20", "-O2", "-pthread",
           "-I", str(ROOT / "components/project2/include"), "-I", str(ROOT / "release/ahs/include"),
           "-isystem", str(UPSTREAM / "src"), "-isystem", str(UPSTREAM / "apps"),
           "-isystem", str(UPSTREAM / "third_party/speexdsp"),
           "-DOUTSIDE_SPEEX", "-DRANDOM_PREFIX=rt_audio_ahs_v7_c0291dd1e_speex", "-DFLOATING_POINT",
           str(ROOT / "notebook/ahs_bridge.cpp"),
           str(ROOT / "build/librt_audio_ahs.a"), str(ROOT / "build/librt_audio_project2.a"),
           "-o", str(BRIDGE)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return BRIDGE


def _run(*args, timeout=600):
    r = subprocess.run([str(BRIDGE), *map(str, args)], capture_output=True, text=True, timeout=timeout)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return json.loads(r.stdout.strip().splitlines()[-1])


def releases():
    """Registered AHS releases (order = release/ahs/registry.cpp)."""
    return ["v7-c0291dd1e", "v7-lowlat-c0291dd1e", "v7-lowlat-fir-c0291dd1e", "v7-split-c0291dd1e"]


def load_example(path=None, seconds=12.0):
    """Example speech -> mono float32 @48 kHz. Default: AHS testdata/speech_female.wav."""
    path = Path(path) if path else ROOT.parent / "AHS/testdata/speech_female.wav"
    if path.exists():
        sr, x = wavfile.read(path)
        x = x.astype(np.float32) / (32768.0 if x.dtype == np.int16 else 1.0)
        if x.ndim > 1:
            x = x.mean(axis=1)
        g = np.gcd(FS, sr)
        x = resample_poly(x, FS // g, sr // g).astype(np.float32)
        name = path.name
    else:  # fallback: synthetic voiced-ish signal
        t = np.arange(int(seconds * FS)) / FS
        x = (0.1 * np.sin(2 * np.pi * 220 * t) * (np.sin(2 * np.pi * 2 * t) > 0)).astype(np.float32)
        name = "synthetic"
    return x[: int(seconds * FS)], name


SOURCES = {name: ROOT.parent / f"AHS/testdata/{name}.wav"
           for name in ("speech_female", "speech_male", "music_chords")}


def make_path(length_ms=50, direct_ms=2.0, t60_s=0.15, seed=1):
    """Synthetic speaker->mic path @48 kHz: direct delay + exp-decay noise tail.
    Normalised so max |H(f)| = 1 -> loop gain in dB is relative to the path peak."""
    rng = np.random.default_rng(seed)
    n = int(FS * length_ms / 1000)
    h = np.zeros(n)
    d = int(FS * direct_ms / 1000)
    h[d] = 1.0
    t = np.arange(n - d - 1) / FS
    h[d + 1:] = 0.5 * rng.standard_normal(t.size) * np.exp(-6.91 * t / t60_s)
    h /= np.abs(np.fft.rfft(h, 1 << 16)).max()
    return h.astype(np.float32)


def run_loop(release, src, path, g1_db, g2_db, step_s, frames=128, speaker="clip"):
    """Closed loop: mic = src + g*(path * speaker). Returns dict(stats, mic, spk, cb_us)."""
    prefix = OUT / f"loop_{release}"
    src_f, path_f = OUT / "src.f32", OUT / "path.f32"
    np.asarray(src, np.float32).tofile(src_f)
    np.asarray(path, np.float32).tofile(path_f)
    stats = _run("loop", release, src_f, path_f, g1_db, g2_db, step_s, frames, prefix, speaker)
    rd = lambda s: np.fromfile(f"{prefix}_{s}.f32", dtype=np.float32)
    return {"stats": stats, "mic": rd("mic"), "spk": rd("spk"), "cb_us": rd("cb_us")}


def latency(release, frames):
    return _run("latency", release, frames)


def resampler_lag():
    return _run("resampler")


def stages(seconds=30):
    return _run("stages", seconds)


def comm(seconds=10):
    return _run("comm", seconds)


def save_wav(name, x):
    OUT.mkdir(parents=True, exist_ok=True)
    # 16-bit PCM: plays everywhere (float32 WAV does not)
    wavfile.write(OUT / f"{name}.wav", FS, (np.clip(x, -1, 1) * 32767).astype(np.int16))
    return OUT / f"{name}.wav"


def rms_db(x, win_ms=100):
    w = int(FS * win_ms / 1000)
    n = len(x) // w
    r = np.sqrt(np.mean(np.asarray(x[: n * w], np.float64).reshape(n, w) ** 2, axis=1))
    return np.arange(n) * win_ms / 1000, 20 * np.log10(r + 1e-9)


# --- stability ("max stable gain") -------------------------------------------------
# strict  : gain -20 dB, step to g at 4 s, hard-clip speaker; fail if any 250 ms
#           after the step > -12 dBFS (catches bursts).
# ahs_sim : constant gain g, tanh(g*out) speaker, fail if last 1 s RMS outside
#           [0.25, 4] x source RMS (ahs_sim's own criterion; bursts ignored).
MODES = ("strict", "ahs_sim")


def stable(release, src, path, gain_db, mode):
    if mode == "strict":
        y = run_loop(release, src, path, -20.0, gain_db, 4.0)["spk"]
        t, d = rms_db(y, 250)
        return d[t >= 4.0].max() <= -12.0
    y = run_loop(release, src, path, gain_db, gain_db, 0.0, speaker="tanh")["spk"]
    r = np.sqrt(np.mean(y[-FS:].astype(np.float64) ** 2))
    s = np.sqrt(np.mean(np.asarray(src, np.float64) ** 2))
    return 0.25 * s <= r <= 4 * s


def max_stable_gain(release, src, path, mode, lo=-6, hi=30):
    """Highest gain (1 dB grid) with every gain from lo up to it stable; None if lo fails.
    Linear scan on purpose: loop stability is not monotonic in gain (e.g. +8 stable,
    +7 not), so a coarse scan would skip failures and over-report."""
    best = None
    for g in range(lo, hi + 1):
        if not stable(release, src, path, g, mode):
            return best
        best = g
    return best


# --- ahs_sim reference (AHS's own closed-loop simulator, 16 kHz) ---------------------
AHS_REPO = ROOT.parent / "AHS"
AHS_COMMIT = "c0291dd1e2051d7934563bbc31daa36bbc044fb4"
AHS_SIM = BUILD / "ahs_sim"


def build_ahs_sim():
    """Compile apps/ahs_sim.cpp from the AHS repo at the vendored commit (git archive,
    no checkout change). Returns path, or None if the repo/commit is unavailable."""
    src = BUILD / "ahs_sim_src"
    if AHS_SIM.exists():
        return AHS_SIM
    if not (AHS_REPO / ".git").exists():
        return None
    src.mkdir(parents=True, exist_ok=True)
    arch = subprocess.run(["git", "-C", str(AHS_REPO), "archive", AHS_COMMIT, "src", "apps"],
                          capture_output=True)
    if arch.returncode:
        return None
    subprocess.run(["tar", "-x", "-C", str(src)], input=arch.stdout, check=True)
    cxx = shutil.which("c++") or shutil.which("clang++")
    r = subprocess.run([cxx, "-std=c++17", "-O3", "-DNDEBUG", "-I", str(src / "src"),
                        "-I", str(src / "apps"), str(src / "apps/ahs_sim.cpp"), "-o", str(AHS_SIM)],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return AHS_SIM


def ahs_sim_msg(wav, *args, configs="afc+notch+guard"):
    """ahs_sim --mode msg -> {config: MSG dB}. bypass is always included."""
    r = subprocess.run([str(AHS_SIM), "--mode", "msg", "--in", str(wav), "--msg-configs", configs,
                        *map(str, args)], capture_output=True, text=True, timeout=600)
    if r.returncode:
        raise RuntimeError(r.stderr)
    out = {}
    for line in r.stdout.splitlines():
        f = line.split()
        if len(f) >= 2 and f[0] != "configuration":
            out[f[0]] = float(f[1])
    return out


def ahs_sim_path48(wav):
    """ahs_sim's own synthetic path (200 taps @16 kHz, 3 modes Q=300) -> 48 kHz, peak |H| = 0 dB."""
    r = subprocess.run([str(AHS_SIM), "--mode", "path", "--in", str(wav)],
                       capture_output=True, text=True, check=True)
    h16 = np.array([float(l.split(",")[1]) for l in r.stdout.splitlines()[1:]])[:200]
    h = resample_poly(h16, 3, 1)
    return (h / np.abs(np.fft.rfft(h, 1 << 16)).max()).astype(np.float32)


def path_wav16(path48, name):
    """Our 48 kHz path -> 16 kHz WAV for ahs_sim --rir."""
    h = resample_poly(np.asarray(path48, np.float64), 1, 3)
    f = OUT / f"{name}.wav"
    wavfile.write(f, 16000, (h / np.max(np.abs(h)) * 0.9 * 32767).astype(np.int16))
    return f
