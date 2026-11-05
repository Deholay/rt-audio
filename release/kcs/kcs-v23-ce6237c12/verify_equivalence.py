"""Equivalence: KCS v23 Bridge48k (Python reference, upstream/) vs the C++ port.

Technical validation, not contract delivery. Synthetic signals only (not KCS
material; no quality claim). Run from rt-audio root after `make all`:

    python -I release/kcs/kcs-v23-ce6237c12/verify_equivalence.py [--kcs ../KCS]

Checks: upstream/ files are byte-identical to the pinned KCS commit; per signal,
float32 outputs (same length, process_block in 128-frame blocks), trigger
events (sample, W, bypass) and counters. Writes equivalence_results.json.
"""
import argparse
import hashlib
import json
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
COMMIT = 'ce6237c124a62c79b28667aced2ee2ff6df6b1b0'
UPSTREAM = HERE / 'upstream' / 'scripts'
sys.path.insert(0, str(UPSTREAM))
from item3_bridge48k import create_bridge  # noqa: E402

FS = 48000


def check_upstream(kcs):
    rows = {}
    for path in sorted(UPSTREAM.glob('*.py')):
        ours = hashlib.sha256(path.read_bytes()).hexdigest()
        row = {'sha256': ours}
        if kcs:
            ref = subprocess.run(['git', '-C', str(kcs), 'show', f'{COMMIT}:scripts/{path.name}'],
                                 capture_output=True, check=True).stdout
            row['matches_commit'] = hashlib.sha256(ref).hexdigest() == ours
            if not row['matches_commit']:
                raise SystemExit(f'upstream mismatch: {path.name}')
        rows[path.name] = row
    return rows


def tone(n, rng, f0=233.0):
    t = np.arange(n) / FS
    vib = 1 + 0.004 * np.sin(2 * np.pi * 5.5 * t)
    phase = 2 * np.pi * np.cumsum(f0 * vib) / FS
    x = sum(0.25 / h * np.sin(h * phase + rng.uniform(0, 6.28)) for h in range(1, 7))
    return x * (0.6 + 0.4 * np.sin(2 * np.pi * 0.3 * t) ** 2)


def clicks(n, rng, every, amp):
    x = np.zeros(n)
    for start in range(FS, n - 2000, every):
        length = int(rng.integers(120, 600))
        burst = rng.standard_normal(length) * np.exp(-np.arange(length) / rng.uniform(30, 120))
        x[start:start + length] += amp * burst
    return x


def signals():
    n = 6 * FS
    out = {}
    for seed in range(4):
        rng = np.random.default_rng(seed)
        out[f'tone_clicks_seed{seed}'] = tone(n, rng, 180 + 40 * seed) + clicks(n, rng, 14000 + 1000 * seed, 0.3 + 0.2 * seed)
    rng = np.random.default_rng(10)
    out['tone_strong_clicks'] = tone(n, rng) * 0.5 + clicks(n, rng, 20000, 1.5)
    rng = np.random.default_rng(14)  # R >= 20 dB and S >= -5 dBFS: W = 256 window
    out['tone_very_strong_clicks'] = tone(n, rng) * 0.2 + clicks(n, rng, 20000, 5.0)
    rng = np.random.default_rng(11)
    out['silence_clicks'] = clicks(n, rng, 24000, 0.5)
    rng = np.random.default_rng(12)
    out['noise'] = 0.05 * rng.standard_normal(n)
    rng = np.random.default_rng(13)
    out['loud_clipped'] = np.clip(2.0 * tone(n, rng) + clicks(n, rng, 16000, 2.0), -1, 1)
    return {k: v.astype(np.float32) for k, v in out.items()}


def run_python(x):
    bridge = create_bridge('transient_ar_v23_bridged')
    y = np.concatenate([bridge.process_block(x[i:i + 128]) for i in range(0, len(x), 128)])
    if bridge.state != 'ACTIVE':
        raise RuntimeError(f'reference bridge failed: {bridge.failure}')
    events = [(e['trigger_sample'], e['W'], bool(e['bypassed']), e['R_db'], e['S_dbfs'])
              for e in bridge.core.events]
    return y, events, dict(bridge.core.counters)


def run_cpp(binary, x):
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = Path(tmp, 'in.f32'), Path(tmp, 'out.f32')
        x.tofile(src)
        proc = subprocess.run([str(binary), str(src), str(dst)], capture_output=True, text=True, check=True)
        y = np.fromfile(dst, dtype=np.float32)
    events = []
    for line in proc.stdout.split('\n'):
        if line:
            t, w, r, s, bypassed = line.split()
            events.append((int(t), int(w), bypassed == '1', float(r), float(s)))
    counters = {k: int(v) for k, v in (kv.split('=') for kv in proc.stderr.split())}
    return y, events, counters


def compare(name, x, binary):
    yp, ep, cp = run_python(x)
    yc, ec, cc = run_cpp(binary, x)
    assert len(yp) == len(yc) == len(x)
    diff = np.abs(yp.astype(np.float64) - yc.astype(np.float64))
    i = int(np.argmax(diff))
    discrete_p = [(t, w, b) for t, w, b, _, _ in ep]
    discrete_c = [(t, w, b) for t, w, b, _, _ in ec]
    same_events = discrete_p == discrete_c
    r_err = max((abs(a[3] - b[3]) for a, b in zip(ep, ec)), default=0.0) if same_events else None
    s_err = max((abs(a[4] - b[4]) for a, b in zip(ep, ec)), default=0.0) if same_events else None
    return {
        'signal': name, 'frames': len(x),
        'max_abs_error': float(diff[i]), 'max_error_sample_48k': i,
        'max_error_core_tick': i // 3,
        'nonidentical_samples': int(np.count_nonzero(diff)),
        'peak_abs_output': float(np.max(np.abs(yp))),
        'events_python': len(ep), 'events_cpp': len(ec), 'events_identical': same_events,
        'max_r_db_error': r_err, 'max_s_dbfs_error': s_err,
        'counters_identical': cp == cc, 'counters_python': cp, 'counters_cpp': cc,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kcs', type=Path, default=HERE.parents[3] / 'KCS')
    ap.add_argument('--binary', type=Path, default=HERE.parents[2] / 'build' / 'kcs_v23_equiv')
    args = ap.parse_args()
    kcs = args.kcs if (args.kcs / '.git').exists() else None
    report = {
        'kcs_commit': COMMIT, 'upstream': check_upstream(kcs),
        'environment': {'platform': platform.platform(), 'machine': platform.machine(),
                        'python': sys.version.split()[0], 'numpy': np.__version__},
        'note': 'synthetic signals only; not KCS material; equivalence, not quality',
        'results': [compare(name, x, args.binary) for name, x in signals().items()],
    }
    (HERE / 'equivalence_results.json').write_text(json.dumps(report, indent=2) + '\n')
    for r in report['results']:
        print(f"{r["signal"]:24s} max|err|={r['max_abs_error']:.3e} @{r['max_error_sample_48k']:7d} "
              f"nonidentical={r['nonidentical_samples']:6d}/{r['frames']} events {r['events_python']}/{r['events_cpp']} "
              f"identical={r['events_identical']} counters={r['counters_identical']}")
    ok = all(r['events_identical'] and r['counters_identical'] for r in report['results'])
    print('PASS' if ok else 'FAIL: discrete divergence (events/counters)')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
