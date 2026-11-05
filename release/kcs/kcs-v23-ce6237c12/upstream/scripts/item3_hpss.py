"""Frozen v14 causal HPSS; technical validation, not measured real-time audio.

push() returns one clocked output per input sample. process() is the scorer's
one-shot adapter, including explicit EOS flush. Trace times are sample indices,
not CPU timestamps; host/interface latency must be measured separately.
"""
from collections import deque

import numpy as np

FRAME = 240
HOP = 120
K = 17
M = 4
FLOOR_DB = -12.0


class CausalHPSS:
    latency_samples = 240
    lookahead_samples = 0

    def __init__(self, fs):
        if fs != 16000:
            raise ValueError("frozen v14 requires 16000 Hz")
        self.window = np.hanning(241)[:-1]
        self._input = deque([0.0] * FRAME, maxlen=FRAME)
        self._history = deque(maxlen=K)
        self._ola = np.zeros(FRAME)
        self._norm = np.zeros(FRAME)
        self._ready = {}
        self._clock = 0
        self._real_count = 0
        self._closed = False
        self.timing = []
        # Completed all-zero left-padding frame [-240, 0).
        self._frame(0)

    def _frame(self, count):
        spec = np.fft.rfft(np.asarray(self._input) * self.window, n=FRAME)
        mag = np.abs(spec)
        self._history.append(mag)
        harmonic = np.median(np.asarray(self._history), axis=0)
        percussive = np.array([
            np.median(mag[max(0, b - M):min(mag.size, b + M + 1)])
            for b in range(mag.size)])
        denominator = harmonic ** 2 + percussive ** 2
        gain = np.ones_like(mag)
        keep = denominator > 1e-30
        gain[keep] = harmonic[keep] ** 2 / denominator[keep]
        gain = np.maximum(gain, 10.0 ** (FLOOR_DB / 20.0))
        self._ola += np.fft.irfft(spec * gain, n=FRAME) * self.window
        self._norm += self.window ** 2
        start = count - FRAME
        for j in range(HOP):
            source = start + j
            if source >= 0:
                # Left-padding coverage makes the live normalization positive.
                self._ready[source] = (self._ola[j] / self._norm[j], count - 1)
        self._ola[:HOP] = self._ola[HOP:]
        self._ola[HOP:] = 0.0
        self._norm[:HOP] = self._norm[HOP:]
        self._norm[HOP:] = 0.0

    def _tick(self, value, eos):
        clock = self._clock
        self._input.append(float(value))
        count = clock + 1
        if count % HOP == 0:
            self._frame(count)
        source = clock - self.latency_samples
        out = 0.0
        if source >= 0:
            out, finalized = self._ready.pop(source)
            self.timing.append({
                "source_sample": source, "input_available_at": source,
                "finalized_at": finalized, "emitted_at": clock,
                "eos": bool(eos)})
        self._clock = count
        return out

    def push(self, x):
        if self._closed:
            raise RuntimeError("stream already flushed")
        x = np.asarray(x, dtype=np.float64)
        if x.ndim != 1:
            raise ValueError("mono one-dimensional input required")
        self._real_count += x.size
        return np.array([self._tick(v, False) for v in x], dtype=np.float64)

    def flush(self):
        if self._closed:
            raise RuntimeError("stream already flushed")
        self._closed = True
        # Right padding is legal only here; no live safe prefix uses this tail.
        return np.array([self._tick(0.0, True)
                         for _ in range(self.latency_samples)], dtype=np.float64)

    def process(self, x):
        if self._clock or self._closed:
            raise RuntimeError("process requires a fresh instance")
        return np.concatenate((self.push(x), self.flush()))
