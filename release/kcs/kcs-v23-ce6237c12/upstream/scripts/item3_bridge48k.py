"""技術驗證，非合約交付。Batch19 fixed 48k bridge; no hardware claim."""
import numpy as np
from item3_rt_host import CausalFIR, rate_taps
from item3_transient_v17 import TransientARShifted
from item3_transient_v23 import TransientAR32


class Bridge48k:
    """Float32 mono transport with a fresh frozen 16k core and terminal failures."""
    device_fs = 48000
    core_fs = 16000
    max_core_input_frames = 176000

    def __init__(self, core_factory, taps=None):
        canonical = rate_taps()
        if taps is not None and not np.array_equal(np.asarray(taps), canonical):
            raise ValueError('only canonical rate_taps allowed')
        self.core = core_factory()
        f = self.core.latency_samples
        if isinstance(f, bool) or not isinstance(f, (int, np.integer)) or f < 0:
            raise ValueError('latency_samples must be a nonnegative integer')
        self.latency_samples = int(f)
        self.down = CausalFIR(canonical.copy())
        self.up = CausalFIR(3 * canonical)
        self.state = 'ACTIVE'
        self.failure = None
        self.p = 0
        self.device_frames = 0
        self.core_input_frames = 0
        self.core_tail_frames = 0
        self.core_flush_frames = 0
        self.up_tail_frames = 0
        self.padding_frames = 0

    def counters(self):
        return {name: getattr(self, name) for name in (
            'device_frames', 'core_input_frames', 'core_tail_frames',
            'core_flush_frames', 'up_tail_frames', 'padding_frames')}

    def _fail(self, exc, size):
        if self.failure is None:
            self.failure = dict(stage=self.state, exception=type(exc).__name__,
                                message=str(exc), position=self.p,
                                counters=self.counters())
        self.state = 'FAILED'
        return np.zeros(size, dtype=np.float32)

    @staticmethod
    def _checked(x, size):
        a = np.asarray(x)
        if a.shape != (size,) or a.dtype != np.float64 or not np.isfinite(a).all():
            raise ValueError('invalid float64 stage output')
        return a

    @staticmethod
    def _fir(fir, x):
        # Fixed one-sample calls avoid np.convolve changing summation paths
        # with caller chunk sizes. Reuse the original filter, not its formula.
        y = np.empty(len(x), dtype=np.float64)
        for i in range(len(x)):
            y[i] = Bridge48k._checked(fir.push(x[i:i+1]), 1)[0]
        return y

    def _expand(self, core_output, size):
        expanded = np.zeros(size, dtype=np.float64)
        expanded[(-self.p) % 3::3] = core_output
        y = self._fir(self.up, expanded)
        self.p += size
        return y

    def _through(self, x, tail=False):
        filtered = self._fir(self.down, x)
        selected = filtered[(-self.p) % 3::3]
        n = len(selected)
        if self.core_input_frames + self.core_tail_frames + n > 176020:
            raise ValueError('total core input quota exceeded')
        # Count actual delivered input even if the core subsequently fails.
        if tail:
            if self.core_tail_frames + n > 20:
                raise ValueError('tail quota exceeded')
            self.core_tail_frames += n
        else:
            self.core_input_frames += n
        core_output = self._checked(self.core.push(selected), n)
        return self._expand(core_output, len(x))

    @staticmethod
    def _cast(y):
        with np.errstate(over='ignore', invalid='ignore'):
            result = y.astype(np.float32)
        if not np.isfinite(result).all():
            raise ValueError('nonfinite float32 output')
        return result

    def process_block(self, x):
        size = 0
        try:
            a = np.asarray(x)
            size = a.shape[0] if a.ndim else 0
            if self.state != 'ACTIVE':
                raise RuntimeError('input after terminal state')
            if a.ndim != 1 or not size or a.dtype != np.float32:
                raise ValueError('nonempty mono float32 input required')
            if not np.isfinite(a).all():
                raise ValueError('nonfinite input')
            if (self.device_frames + size + 2) // 3 > self.max_core_input_frames:
                raise ValueError('effective input exceeds 11 seconds')
            y = self._cast(self._through(a.astype(np.float64)))
            self.device_frames += size
            return y
        except Exception as exc:
            return self._fail(exc, size)

    def finish(self):
        if self.state in ('FINISHED', 'FAILED'):
            return np.empty(0, dtype=np.float32)
        f = self.latency_samples
        pad = (-(self.device_frames + 120 + 3*f)) % 128
        size = 120 + 3*f + pad
        try:
            self.state = 'DOWN_DRAIN'
            down = self._through(np.zeros(60, dtype=np.float64), tail=True)
            self.state = 'CORE_FLUSH'
            flushed = self._checked(self.core.flush(), f)
            self.core_flush_frames = f
            core = self._expand(flushed, 3*f)
            self.state = 'UP_DRAIN'
            up = self._fir(self.up, np.zeros(60, dtype=np.float64))
            self.up_tail_frames = 60
            self.state = 'PAD'
            self.padding_frames = pad
            y = self._cast(np.concatenate((down, core, up, np.zeros(pad))))
            if len(y) != size:
                raise ValueError('termination length mismatch')
            self.state = 'FINISHED'
            return y
        except Exception as exc:
            return self._fail(exc, size)


def create_bridge(name, fs=48000):
    """Configuration errors are rejected before entering a callback."""
    if fs != 48000:
        raise ValueError('device fs must be 48000')
    factories = {'transient_ar_v17_bridged': lambda: TransientARShifted(16000),
                 'transient_ar_v23_bridged': lambda: TransientAR32(16000)}
    if name not in factories:
        raise ValueError('unknown bridged processor')
    return Bridge48k(factories[name])
