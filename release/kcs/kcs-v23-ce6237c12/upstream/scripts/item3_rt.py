"""技術驗證，非合約交付: conservative vectorized v14 streaming adapters.

HPSS batches interior frequency medians. AR batches energy-window reductions,
replacement blending and FIFO movement; FIR dot products and AR recursion keep
frozen operation order. This is partial vectorization, not a real-time claim.
Frozen modules remain authoritative and unmodified. AR omits per-sample timing
trace allocation; event/counter telemetry is retained (unbounded in long runs).
"""
import numpy as np
from numpy.lib.stride_tricks import sliding_window_view
from item3_hpss import CausalHPSS, FRAME, HOP, M, FLOOR_DB
from item3_transient import TransientAR, SHORT, LONG, N, D, W, REFRACTORY, T_DB


class FastCausalHPSS(CausalHPSS):
    def _frame(self, count):
        spec = np.fft.rfft(np.asarray(self._input) * self.window, n=FRAME)
        mag = np.abs(spec)
        self._history.append(mag)
        harmonic = np.median(np.asarray(self._history), axis=0)
        percussive = np.empty_like(mag)
        percussive[M:-M] = np.median(sliding_window_view(mag, 2*M+1), axis=1)
        for b in list(range(M)) + list(range(mag.size-M, mag.size)):
            percussive[b] = np.median(mag[max(0,b-M):min(mag.size,b+M+1)])
        denominator = harmonic**2 + percussive**2
        gain = np.ones_like(mag)
        keep = denominator > 1e-30
        gain[keep] = harmonic[keep]**2 / denominator[keep]
        gain = np.maximum(gain, 10.0**(FLOOR_DB/20.0))
        self._ola += np.fft.irfft(spec*gain, n=FRAME)*self.window
        self._norm += self.window**2
        start = count-FRAME
        for j in range(HOP):
            if start+j >= 0:
                self._ready[start+j] = (self._ola[j]/self._norm[j], count-1)
        self._ola[:HOP] = self._ola[HOP:]
        self._ola[HOP:] = 0.0
        self._norm[:HOP] = self._norm[HOP:]
        self._norm[HOP:] = 0.0


class FastTransientAR(TransientAR):
    def _push_block(self, x):
        # Same 33-element np.dot (including reversed stride), not GEMM/convolve.
        raw = np.concatenate((np.asarray(self._fir_input)[1:], x))
        znew = np.array([float(np.dot(self.fir, row[::-1]))
                        for row in sliding_window_view(raw, 33)])
        zold = np.asarray(self._z)
        z = np.concatenate((zold, znew))
        ratios = np.empty(0)
        if z.size >= SHORT+LONG:
            windows = sliding_window_view(z, SHORT+LONG)
            short = np.mean(windows[:, -SHORT:]**2, axis=1)
            long = np.mean(windows[:, :-SHORT]**2, axis=1)
            ratios = 10*np.log10(np.maximum(short, 1e-12)/np.maximum(long, 1e-12))
        indices, weights, predictions = [], [], []
        for j, value in enumerate(x):
            t = self._clock
            eligible = len(self._raw)==N and zold.size+j+1 >= SHORT+LONG
            if not eligible:
                self.counters['warmup_samples'] += 1
                self._above = 0
            elif t < self._next_trigger:
                self._above = 0
            else:
                ratio = ratios[zold.size+j-(SHORT+LONG)+1]
                self._above = self._above+1 if ratio > T_DB else 0
                if self._above >= D:
                    self.counters['triggers'] += 1
                    before = self.counters.copy()
                    self._replacement = self._fit()
                    self._event_start = t
                    self._next_trigger = t+W+REFRACTORY
                    self._above = 0
                    self.events.append({'trigger_sample':t,'window_end_exclusive':t+W,
                        'refractory_end_exclusive':self._next_trigger,
                        'bypassed':self._replacement is None,
                        'counter_delta':{k:self.counters[k]-before[k] for k in before}})
            offset = t-self._event_start
            if self._replacement is not None and 0 <= offset < W:
                indices.append(j);weights.append(self._weights[offset])
                predictions.append(self._replacement[offset])
            self._raw.append(float(value))
            self._clock += 1
        self._fir_input.extend(float(v) for v in x)
        self._z.extend(float(v) for v in znew)
        u = x.copy()
        if indices:
            idx = np.asarray(indices);w = np.asarray(weights)
            u[idx] = (1.0-w)*x[idx] + w*np.asarray(predictions)
        queued = np.concatenate((np.asarray(self._fifo), u))
        y = queued[:x.size].copy()
        self._fifo.clear();self._fifo.extend(queued[x.size:])
        return y

    def push(self, x):
        if self._closed:
            raise RuntimeError('stream already flushed')
        x = np.asarray(x,dtype=np.float64)
        if x.ndim != 1:
            raise ValueError('mono one-dimensional input required')
        if x.size==0:
            return np.empty(0,dtype=np.float64)
        return np.concatenate([self._push_block(x[i:i+256]) for i in range(0,x.size,256)])

    def flush(self):
        if self._closed:
            raise RuntimeError('stream already flushed')
        self._closed = True
        y = np.asarray(self._fifo,dtype=np.float64).copy()
        self._fifo.clear();self._fifo.extend([0.0]*self.latency_samples)
        self._clock += self.latency_samples
        return y
