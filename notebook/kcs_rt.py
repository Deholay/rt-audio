"""Glue for kcs_rt.ipynb. All processing = real C++ (rt-audio Project 3 engine/worker +
release/kcs) via kcs_bridge. Python only: build, file I/O, plots. Quality numbers use the
KCS frozen scorer functions (../KCS/scripts/item3_score.py), imported read-only."""

import json
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[1]           # rt-audio/
BUILD = ROOT / "build" / "notebook"
OUT = ROOT / "notebook" / "out"
BRIDGE = BUILD / "kcs_bridge"
DEMO = ROOT.parent / "KCS_demo_20261102_48k"
KCS_REPO = ROOT.parent / "KCS"
FS = 48000

# Frozen 16 kHz KSR from the demo README (results/item3/v23/v13/v23/item3_transient_ar32.json).
FROZEN_KSR = {"snr0_sparse_in_gap_t1": 97.5, "snr5_dense_over_tone_t1": 87.6,
              "snr10_sparse_over_tone_t1": 94.5, "snr15_dense_in_gap_t3": 86.9,
              "snr20_sparse_in_gap_t2": 87.8, "snr20_dense_over_tone_t1": -24.2}


def build():
    """make libs, then compile kcs_bridge against them."""
    cxx = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        raise RuntimeError("need a C++20 compiler")
    subprocess.run(["make", "-C", str(ROOT), "component", "-j4"], check=True, capture_output=True, text=True)
    BUILD.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    cmd = [cxx, "-std=c++20", "-O2", "-pthread",
           "-I", str(ROOT / "components/project3/include"), "-I", str(ROOT / "release/kcs/include"),
           str(ROOT / "notebook/kcs_bridge.cpp"),
           str(ROOT / "build/librt_audio_kcs.a"), str(ROOT / "build/librt_audio_project3.a"),
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


# --- demo set -------------------------------------------------------------------
def _wav(path, gain):
    sr, x = wavfile.read(path)
    assert sr == FS and x.dtype == np.int16 and x.ndim == 1, path
    return (x.astype(np.float64) / 32767.0 / gain).astype(np.float32)   # undo the demo's write gain


def load_demo():
    """{case: dict(mix, clean, key, frozen_out, gain, triggers)}; original level (÷ manifest gain).
    mix = what the processor sees; clean/key are references only (never fed to the processor)."""
    man = json.loads((DEMO / "manifest_48k.json").read_text())
    cases = {}
    for c in man["cases"]:
        d, g = DEMO / c["case"], c["gain"]
        cases[c["case"]] = dict(
            mix=_wav(d / "01_input_mix_48k.wav", g), frozen_out=_wav(d / "02_output_v23_bridged_48k.wav", g),
            clean=_wav(d / "03_clean_reference_48k.wav", g), key=_wav(d / "04_key_noise_only_48k.wav", g),
            gain=g, frozen_triggers=c["core_triggers"])
    return cases


# --- rt system ---------------------------------------------------------------------
def run(processor, x, frames=32, mode="sync"):
    """Real Project 3 Engine (+ worker) with `processor` (passthrough | kcs-latest);
    frames = hop = callback size, delay = kcs::minimum_delay_frames(frames, frames).
    Returns dict(stats, out (raw engine output, len >= len(x)+delay), y (aligned to x), cb_us)."""
    tag = f"{processor}_{mode}_{frames}"
    src = OUT / "kcs_in.f32"
    np.asarray(x, np.float32).tofile(src)
    res = _run("run", processor, src, OUT / f"kcs_{tag}", frames, mode)
    out = np.fromfile(OUT / f"kcs_{tag}_out.f32", dtype=np.float32)
    d = res["delay_frames"]
    return {"stats": res, "out": out, "y": out[d:d + len(x)],
            "cb_us": np.fromfile(OUT / f"kcs_{tag}_cb_us.f32", dtype=np.float32)}


def latency(frames):
    return _run("latency", frames)


def stages(x):
    src = OUT / "kcs_in.f32"
    np.asarray(x, np.float32).tofile(src)
    return _run("stages", src)


def comm(x, hop=128):
    src = OUT / "kcs_in.f32"
    np.asarray(x, np.float32).tofile(src)
    return _run("comm", src, hop, timeout=900)


# --- KCS frozen scorer (read-only import) ----------------------------------------
def scorer():
    """KCS scripts/item3_score.py at the repo's checked-out commit."""
    sys.path.insert(0, str(KCS_REPO / "scripts"))
    import item3_score
    head = subprocess.run(["git", "-C", str(KCS_REPO), "rev-parse", "--short", "HEAD"],
                          capture_output=True, text=True).stdout.strip()
    return item3_score, head


def metrics(score, y_mix, y_clean, clean, key):
    """D1 KSR (residual = y_mix - y_clean), clean-run LSD / seg SNR — frozen formulas,
    applied to the 48 kHz demo (reference, not the frozen 16 kHz scoring)."""
    y_mix, y_clean, clean, key = (np.asarray(v, np.float64) for v in (y_mix, y_clean, clean, key))
    return {"ksr": score.energy_ksr(y_mix - y_clean, key),
            "lsd_db": score.lsd_db(y_clean, clean, FS),
            "seg_snr_db": score.seg_snr_db(y_clean, clean, FS)}


# --- helpers -------------------------------------------------------------------------
def save_wav(name, x):
    OUT.mkdir(parents=True, exist_ok=True)
    peak = float(np.max(np.abs(x))) or 1.0
    wavfile.write(OUT / f"{name}.wav", FS, (np.clip(np.asarray(x) * (0.9 / peak), -1, 1) * 32767).astype(np.int16))
    return OUT / f"{name}.wav"


def rms_db(x, win_ms=20):
    w = int(FS * win_ms / 1000)
    n = len(x) // w
    r = np.sqrt(np.mean(np.asarray(x[: n * w], np.float64).reshape(n, w) ** 2, axis=1))
    return np.arange(n) * win_ms / 1000, 20 * np.log10(r + 1e-9)


def key_events(key, thresh_db=-30, min_gap_s=0.1):
    """Onsets of the key layer (reference only, for plot markers)."""
    t, d = rms_db(key, 5)
    on = d > d.max() + thresh_db
    starts = t[1:][on[1:] & ~on[:-1]]
    keep = [s for i, s in enumerate(starts) if i == 0 or s - starts[i - 1] > min_gap_s]
    return np.array(keep)
