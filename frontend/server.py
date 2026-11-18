"""Quencher console: one local web UI for P2 (AHS howling) and P3 (KCS key-click).

All audio processing is the real C++: offline via notebook bridges (ahs_bridge loop,
kcs_bridge run), live via project2_live / project3_live. This file only validates
requests, spawns those binaries, does file I/O and serves the page.

    python3 frontend/server.py        # from rt-audio/, then open http://127.0.0.1:8765
"""

import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

ROOT = Path(__file__).resolve().parents[1]  # rt-audio/
HERE = ROOT / "frontend"
OUT = HERE / "out"
sys.dont_write_bytecode = True  # no __pycache__ in notebook/
sys.path.insert(0, str(ROOT / "notebook"))
import ahs_rt  # noqa: E402  (bridge glue shared with the notebook)

ahs_rt.OUT = OUT  # keep UI runs out of notebook/out
FS = 48000
BUILD = ROOT / "build"
KCS_BRIDGE = BUILD / "notebook" / "kcs_bridge"
LIVE = {"p2": BUILD / "project2_live", "p3": BUILD / "project3_live"}
AHS_SOURCES = ROOT.parent / "testdata"
KCS_SOURCES = ROOT.parent / "KCS_demo_20261102_48k"
P2_SCENARIO = {"g1_db": -20.0, "g2_db": 6.0, "step_s": 4.0, "seconds": 12.0}


def releases(system):
    """Release names from release/<ahs|kcs>/ directories, as the CLIs/bridges accept them."""
    if system == "p2":
        return ["passthrough"] + sorted(p.name[4:] for p in (ROOT / "release/ahs").glob("ahs-*") if p.is_dir())
    return ["passthrough"] + sorted(p.name for p in (ROOT / "release/kcs").glob("kcs-*") if p.is_dir())


def sources(system):
    if system == "p2":
        return sorted(p.stem for p in AHS_SOURCES.glob("*.wav"))
    return sorted(p.name for p in KCS_SOURCES.iterdir() if (p / "01_input_mix_48k.wav").exists())


# ---------- offline ----------

def read_wav48(path, seconds=None):
    sr, x = wavfile.read(path)
    x = x.astype(np.float32) / (32768.0 if x.dtype == np.int16 else 1.0)
    if x.ndim > 1:
        x = x.mean(axis=1)
    if sr != FS:
        g = np.gcd(FS, sr)
        x = resample_poly(x, FS // g, sr // g).astype(np.float32)
    return x[: int(seconds * FS)] if seconds else x


def envelope_db(x, win_ms=20):
    w = int(FS * win_ms / 1000)
    n = len(x) // w
    r = np.sqrt(np.mean(np.asarray(x[: n * w], np.float64).reshape(n, w) ** 2, axis=1))
    return np.round(20 * np.log10(r + 1e-9), 1).clip(-100, 0).tolist()


def track(key, label, x, stats=None):
    name = f"{key}.wav"
    wavfile.write(OUT / name, FS, (np.clip(np.nan_to_num(x), -1, 1) * 32767).astype(np.int16))
    return {"label": label, "wav": f"/out/{name}?t={time.time_ns()}", "env": envelope_db(x),
            "seconds": len(x) / FS, "stats": stats or {}}


def offline_p2(source, names):
    src = read_wav48(AHS_SOURCES / f"{source}.wav", P2_SCENARIO["seconds"])
    path = ahs_rt.make_path()
    tracks = [track("p2_source", "來源（無回授）", src)]
    for name in names:
        r = ahs_rt.run_loop(name, src, path, P2_SCENARIO["g1_db"], P2_SCENARIO["g2_db"], P2_SCENARIO["step_s"])
        spk = r["spk"]
        tail = spk[-FS:]
        s = r["stats"]
        stats = {"後 1 s RMS dBFS": round(float(20 * np.log10(np.sqrt(np.mean(tail.astype(np.float64) ** 2)) + 1e-9)), 1),
                 "callback p99 µs": (s.get("callback_us") or {}).get("p99")}
        tracks.append(track(f"p2_{name}", name, spk, stats))
    return {"tracks": tracks, "marker_s": P2_SCENARIO["step_s"],
            "note": f"閉迴路：迴路增益 {P2_SCENARIO['g1_db']:+.0f} → {P2_SCENARIO['g2_db']:+.0f} dB @ {P2_SCENARIO['step_s']:.0f} s，播放喇叭輸出"}


def offline_p3(case, names):
    folder = KCS_SOURCES / case
    mix = read_wav48(folder / "01_input_mix_48k.wav")
    tracks = [track("p3_input", "輸入（含按鍵噪音）", mix)]
    in_f = OUT / "p3_in.f32"
    mix.tofile(in_f)
    for name in names:
        prefix = OUT / f"p3_{name}"
        r = subprocess.run([str(KCS_BRIDGE), "run", name, str(in_f), str(prefix), "128", "sync"],
                           capture_output=True, text=True, timeout=600)
        if r.returncode:
            raise RuntimeError(r.stderr.strip() or f"kcs_bridge failed ({r.returncode})")
        s = json.loads(r.stdout.strip().splitlines()[-1])
        y = np.fromfile(f"{prefix}_out.f32", dtype=np.float32)[s["delay_frames"]:][: len(mix)]  # align to input
        stats = {"triggers": s["triggers"], "fallback_blocks": s["stats"]["fallback_blocks"],
                 "delay ms": round(1000 * s["delay_frames"] / FS, 2)}
        tracks.append(track(f"p3_{name}", name, y, stats))
    clean = folder / "03_clean_reference_48k.wav"
    if clean.exists():
        tracks.append(track("p3_clean", "乾淨參考（無按鍵）", read_wav48(clean)))
    return {"tracks": tracks, "marker_s": None, "note": "輸出已依固定延遲對齊輸入"}


offline_lock = threading.Lock()  # bridges share scratch files in OUT


def run_offline(body):
    system = body.get("system")
    if system not in ("p2", "p3"):
        raise ValueError("system must be p2 or p3")
    allowed = releases(system)
    names = body.get("releases") or []
    if not names or any(n not in allowed for n in names):
        raise ValueError(f"releases must be a non-empty subset of {allowed}")
    source = body.get("source")
    if source not in sources(system):
        raise ValueError("unknown source")
    if system == "p2" and not ahs_rt.BRIDGE.exists():
        ahs_rt.build()
    if system == "p3" and not KCS_BRIDGE.exists():
        raise RuntimeError(f"missing {KCS_BRIDGE}; build it from notebook/kcs_rt.ipynb")
    OUT.mkdir(parents=True, exist_ok=True)
    with offline_lock:
        return (offline_p2 if system == "p2" else offline_p3)(source, names)


# ---------- live ----------

DEVICE_RE = re.compile(r"^(\d+): (.*) \[(.*)\] input=(\d+) output=(\d+) default_rate=(\S+)(.*)$")
STATS_RE = re.compile(r"(\w+)=(\d+)")


def devices():
    r = subprocess.run([str(LIVE["p2"]), "--list-devices"], capture_output=True, text=True, timeout=20)
    if r.returncode:
        raise RuntimeError(r.stderr.strip())
    out = []
    for line in r.stdout.splitlines():
        m = DEVICE_RE.match(line)
        if m:
            out.append({"index": int(m[1]), "name": m[2], "inputs": int(m[4]), "outputs": int(m[5]),
                        "rate": float(m[6]), "default_input": "default input" in m[7],
                        "default_output": "default output" in m[7]})
    return out


class Live:
    """At most one live engine process; stdout tail + latest stats line."""

    def __init__(self):
        self.lock = threading.Lock()
        self.proc = None
        self.reset({})

    def reset(self, meta):
        self.meta, self.lines, self.stats, self.started = meta, deque(maxlen=60), None, time.time()

    def start(self, body):
        system = body.get("system")
        if system not in LIVE:
            raise ValueError("system must be p2 or p3")
        name = body.get("release")
        if name not in releases(system):
            raise ValueError("unknown release")
        args = [str(LIVE[system]), "--stats-interval-ms", "250",
                "--processor", name]
        for key, flag, lo, hi, kind in (("input", "--input", 0, 255, int), ("output", "--output", 0, 255, int),
                                        ("seconds", "--seconds", 1, 3600, float),
                                        ("frames", "--frames", 1, 2048, int),
                                        ("gain", "--gain", 0, 1, float), ("latency", "--latency", 0.1, 1000, float)):
            v = body.get(key)
            if v in (None, ""):
                continue
            if key in ("gain", "latency") and system != "p2":
                continue
            v = kind(v)
            if not lo <= v <= hi:
                raise ValueError(f"{key} must be in [{lo}, {hi}]")
            args += [flag, str(v)]
        with self.lock:
            if self.proc and self.proc.poll() is None:
                raise RuntimeError("已有即時執行中，請先停止")
            self.reset({"system": system, "release": name, "args": args[1:]})
            self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
            threading.Thread(target=self.pump, args=(self.proc,), daemon=True).start()
        return self.status()

    def pump(self, proc):
        for line in proc.stdout:
            line = line.rstrip("\n")
            with self.lock:
                if proc is not self.proc:
                    return
                if line.startswith("callbacks="):
                    self.stats = {k: int(v) for k, v in STATS_RE.findall(line)}
                    self.stats["t"] = round(time.time() - self.started, 2)
                else:
                    self.lines.append(line)

    def stop(self):
        with self.lock:
            if self.proc and self.proc.poll() is None:
                self.proc.send_signal(signal.SIGINT)  # app joins worker and prints final stats
        return self.status()

    def status(self):
        with self.lock:
            code = self.proc.poll() if self.proc else None
            return {"running": bool(self.proc) and code is None, "exit_code": code, "meta": self.meta,
                    "lines": list(self.lines), "stats": self.stats, "elapsed": round(time.time() - self.started, 1)}


live = Live()


# ---------- http ----------

class Handler(BaseHTTPRequestHandler):
    def send(self, code, body, ctype="application/json; charset=utf-8"):
        data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def guard(self, fn):
        try:
            self.send(200, fn())
        except (ValueError, TypeError) as e:
            self.send(400, {"error": str(e)})
        except Exception as e:  # surface engine/bridge errors to the UI
            self.send(500, {"error": str(e)})

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/":
            return self.send(200, (HERE / "index.html").read_bytes(), "text/html; charset=utf-8")
        if path == "/api/config":
            return self.guard(lambda: {s: {"releases": releases(s), "sources": sources(s),
                                           "live_ready": LIVE[s].exists()} for s in ("p2", "p3")}
                              | {"p2_scenario": P2_SCENARIO})
        if path == "/api/devices":
            return self.guard(devices)
        if path == "/api/live":
            return self.guard(live.status)
        m = re.fullmatch(r"/out/([\w.-]+\.wav)", path)
        if m and (OUT / m[1]).is_file():
            return self.send(200, (OUT / m[1]).read_bytes(), "audio/wav")
        self.send(404, {"error": "not found"})

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
        except json.JSONDecodeError:
            return self.send(400, {"error": "invalid JSON"})
        routes = {"/api/offline": lambda: run_offline(body), "/api/live/start": lambda: live.start(body),
                  "/api/live/stop": live.stop}
        fn = routes.get(self.path)
        return self.guard(fn) if fn else self.send(404, {"error": "not found"})

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1] if len(sys.argv) > 1 else os.environ.get("PORT", 8765))
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)  # localhost only: it spawns processes
    print(f"Quencher console: http://127.0.0.1:{port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        live.stop()
