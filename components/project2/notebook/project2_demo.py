"""Notebook-only I/O glue; processing uses the component's real C++ sources."""

import json
import math
import shutil
import subprocess
import tempfile
from html import escape
from pathlib import Path

import numpy as np

COMPONENT = Path(__file__).resolve().parents[1]


def record(seconds=3, sample_rate=48000, device=None):
    """Record mono float32; only called when the user runs the recording cell."""
    import sounddevice as sd

    if not math.isfinite(seconds) or not 0 < seconds <= 30:
        raise ValueError("seconds must be in (0, 30]")
    if not isinstance(sample_rate, int) or not 8000 <= sample_rate <= 384000:
        raise ValueError("sample_rate must be an integer in [8000, 384000]")
    audio = sd.rec(round(seconds * sample_rate), samplerate=sample_rate,
                   channels=1, dtype="float32", device=device, blocking=True)
    status = sd.get_status()
    if status:
        raise RuntimeError(f"錄音裝置發生資料遺失：{status}")
    return audio[:, 0].copy(), sample_rate


def run_project2(audio, sample_rate):
    """Replay a recording through C++ callbacks and the real analysis thread."""
    audio = np.asarray(audio, dtype=np.float32)
    if audio.ndim != 1 or audio.size == 0 or not np.isfinite(audio).all():
        raise ValueError("Expected nonempty, finite mono samples")
    if not isinstance(sample_rate, int) or not 8000 <= sample_rate <= 384000:
        raise ValueError("sample_rate must be an integer in [8000, 384000]")
    if audio.size > sample_rate * 30:
        raise ValueError("Keep this demonstration within 30 seconds")
    compiler = shutil.which("c++")
    if compiler is None:
        raise RuntimeError("需要支援 C++20 的 GCC／Clang（macOS 可安裝 Xcode Command Line Tools）")
    build = COMPONENT / "build" / "notebook"
    build.mkdir(parents=True, exist_ok=True)
    executable = build / "project2_bridge"
    command = [compiler, "-std=c++20", "-O2", "-pthread", "-Wall", "-Wextra",
               "-Wpedantic", "-Werror", "-I", str(COMPONENT / "include"),
               str(Path(__file__).with_name("bridge.cpp")),
               *(str(COMPONENT / "src" / f"{name}.cpp")
                 for name in ("engine", "processor", "worker")),
               "-o", str(executable)]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
    if compiled.returncode:
        raise RuntimeError(compiled.stderr)
    # Recording files are temporary; returned samples stay in notebook memory.
    with tempfile.TemporaryDirectory(prefix="project2-", dir=build) as temporary:
        source, target = Path(temporary) / "in.f32", Path(temporary) / "out.f32"
        audio.tofile(source)
        result = subprocess.run([str(executable), str(source), str(target), str(sample_rate)],
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stderr)
        output = np.fromfile(target, dtype=np.float32)
        report = json.loads(result.stdout)
    if output.shape != audio.shape or not np.array_equal(output, audio):
        raise RuntimeError("C++ passthrough output differs from input")
    return output, report


def show_stages(report):
    """A compact view of observed C++ results, not a simulated Python pipeline."""
    from IPython.display import HTML, display

    head = lambda values: ", ".join(f"{value:.4f}" for value in values)
    parameter = report["parameter"]
    target = (f'{parameter["frequency_hz"]:g} Hz / '
              f'{parameter["bandwidth_hz"]:g} Hz / '
              f'{parameter["depth_db"]:g} dB；transition '
              f'{parameter["transition_ms"]:g} ms') if parameter else "尚未套用"
    rows = [
        ("輸入錄音", f'{report["frames"]:,} frames；前 4 點：{head(report["input_head"])}'),
        ("C++ callback → DSP", f'{report["callbacks"]} blocks；PassthroughProcessor 原音直通'),
        ("分析 SPSC", f'{report["analysis_blocks"]} blocks 送達；drop={report["analysis_dropped"]}'),
        ("AnalysisWorker / Handler", f'RMS={report["analysis_rms"]:.5f}；前 4 點：{head(report["analysis_head"])}'),
        ("參數 SPSC", f'{report["commands_submitted"]} 個固定示範快照；drop={report["commands_dropped"]}'),
        ("callback 套用參數", f'{report["commands_applied"]} 次，revision={report["applied_revision"]}；{target}'),
        ("輸出音訊", f'與輸入逐點相同；前 4 點：{head(report["output_head"])}'),
    ]
    body = "".join(f"<tr><td>{escape(label)}</td><td>{escape(value)}</td></tr>"
                   for label, value in rows)
    display(HTML(f"<table><thead><tr><th>部件</th><th>實際結果</th></tr></thead><tbody>{body}</tbody></table>"))
