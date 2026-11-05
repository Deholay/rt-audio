"""Frozen v14 causal transient detection and forward AR replacement.

No annotations/clean reference are available to this processor. push/flush
expose the actual fixed 128-sample output FIFO. Timing is a sample schedule,
not an end-to-end audio measurement. Counters count attempted replacement
windows, except warmup_samples and projected_roots which are explicitly named.
"""
from collections import deque

import numpy as np

SHORT = 32
LONG = 320
T_DB = 8.0
D = 16
W = 128
N = 640
ORDER = 16
C = 16
REFRACTORY = 128


class TransientAR:
    latency_samples = 128
    lookahead_samples = 0

    def __init__(self, fs):
        if fs != 16000:
            raise ValueError("frozen v14 requires 16000 Hz")
        i = np.arange(33)
        low = 2 * (1000 / 16000) * np.sinc(2 * (1000 / 16000) * (i - 16))
        low *= 0.54 - 0.46 * np.cos(2 * np.pi * i / 32)
        low /= low.sum()
        self.fir = -low
        self.fir[16] += 1.0
        self.fir /= np.sum(self.fir * (-1.0) ** i)
        self._raw = deque(maxlen=N)
        self._fir_input = deque([0.0] * 33, maxlen=33)
        self._z = deque(maxlen=SHORT + LONG)
        self._fifo = deque([0.0] * self.latency_samples)
        self._clock = 0
        self._closed = False
        self._above = 0
        self._next_trigger = 0
        self._event_start = -W
        self._replacement = None
        self._weights = np.ones(W)
        self._weights[:C] = np.linspace(0.0, 1.0, C)
        self._weights[-C:] = np.linspace(1.0, 0.0, C)
        self.counters = dict(triggers=0, low_energy=0, pole_projection=0,
                             projected_roots=0, solve_failure=0,
                             nonfinite_prediction=0, warmup_samples=0)
        self.events = []
        self.timing = []

    def _fit(self):
        training = np.asarray(self._raw, dtype=np.float64)
        if np.mean(training ** 2) <= 1e-12:
            self.counters['low_energy'] += 1
            return None
        mean = np.mean(training)
        centered = training - mean
        target = centered[ORDER:]
        design = np.column_stack([
            centered[ORDER-k-1:N-k-1] for k in range(ORDER)])
        gram = design.T @ design
        ridge = 1e-6 * np.trace(gram) / ORDER
        try:
            coeff = np.linalg.solve(gram + ridge * np.eye(ORDER), design.T @ target)
            if not np.all(np.isfinite(coeff)):
                raise np.linalg.LinAlgError('nonfinite coefficients')
            roots = np.roots(np.r_[1.0, -coeff])
            if not np.all(np.isfinite(roots)):
                raise np.linalg.LinAlgError('nonfinite poles')
            project = np.abs(roots) > 0.995
            if np.any(project):
                self.counters['pole_projection'] += 1
                self.counters['projected_roots'] += int(np.sum(project))
                roots[project] *= 0.995 / np.abs(roots[project])
                coeff = -np.poly(roots)[1:].real
            if not np.all(np.isfinite(coeff)):
                raise np.linalg.LinAlgError('nonfinite projected coefficients')
        except np.linalg.LinAlgError:
            self.counters['solve_failure'] += 1
            return None
        history = list(centered[-ORDER:])
        bound = 3.0 * np.std(training)
        predicted = []
        for _ in range(W):
            value = np.dot(coeff, np.asarray(history[-ORDER:][::-1]))
            if not np.isfinite(value):
                self.counters['nonfinite_prediction'] += 1
                return None
            value = float(np.clip(value, -bound, bound))
            history.append(value)
            predicted.append(value + mean)
        return np.asarray(predicted)

    def _live_tick(self, value):
        t = self._clock
        value = float(value)
        self._fir_input.append(value)
        self._z.append(float(np.dot(self.fir, np.asarray(self._fir_input)[::-1])))
        eligible = len(self._raw) == N and len(self._z) == SHORT + LONG
        if not eligible:
            self.counters['warmup_samples'] += 1
            self._above = 0
        elif t < self._next_trigger:
            self._above = 0
        else:
            z = np.asarray(self._z)
            short = np.mean(z[-SHORT:] ** 2)
            long = np.mean(z[:-SHORT] ** 2)
            ratio = 10 * np.log10(max(short, 1e-12) / max(long, 1e-12))
            self._above = self._above + 1 if ratio > T_DB else 0
            if self._above >= D:
                self.counters['triggers'] += 1
                before = self.counters.copy()
                self._replacement = self._fit()
                self._event_start = t
                self._next_trigger = t + W + REFRACTORY
                self._above = 0
                self.events.append({
                    'trigger_sample': t, 'window_end_exclusive': t + W,
                    'refractory_end_exclusive': self._next_trigger,
                    'bypassed': self._replacement is None,
                    'counter_delta': {k: self.counters[k] - before[k]
                                      for k in before}})
        u = value
        j = t - self._event_start
        if self._replacement is not None and 0 <= j < W:
            weight = self._weights[j]
            u = (1.0 - weight) * value + weight * self._replacement[j]
        # Training includes only original input strictly before the next tick.
        self._raw.append(value)
        return self._emit(u, False)

    def _emit(self, value, eos):
        t = self._clock
        out = self._fifo.popleft()
        self._fifo.append(value)
        source = t - self.latency_samples
        if source >= 0:
            self.timing.append({
                'source_sample': source, 'input_available_at': source,
                'finalized_at': source, 'emitted_at': t, 'eos': bool(eos)})
        self._clock += 1
        return out

    def push(self, x):
        if self._closed:
            raise RuntimeError('stream already flushed')
        x = np.asarray(x, dtype=np.float64)
        if x.ndim != 1:
            raise ValueError('mono one-dimensional input required')
        return np.array([self._live_tick(v) for v in x], dtype=np.float64)

    def flush(self):
        if self._closed:
            raise RuntimeError('stream already flushed')
        self._closed = True
        # EOS drains existing FIFO only: it cannot cause additional detections.
        return np.array([self._emit(0.0, True)
                         for _ in range(self.latency_samples)], dtype=np.float64)

    def process(self, x):
        if self._clock or self._closed:
            raise RuntimeError('process requires a fresh instance')
        return np.concatenate((self.push(x), self.flush()))
