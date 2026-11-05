"""技術驗證，非合約交付。Experimental full-duplex host; no hardware deadline claim.

Requires NumPy. Hardware mode additionally requires sounddevice/PortAudio.
The 16 kHz cores are untouched. At 48 kHz, causal stateful FIR rate conversion
runs before/after the core; its group delay is reported separately.
"""
import argparse
import json
import time
from pathlib import Path
import numpy as np
from item3_rt import FastCausalHPSS, FastTransientAR
from item3_transient_v17 import TransientARShifted

PROCESSORS = ('bypass', 'hpss_causal', 'transient_ar', 'transient_ar_v17')
V17_MAX_FRAMES = 11 * 16000
FLAGS = ('input_underflow', 'input_overflow', 'output_underflow', 'output_overflow')


class StreamingBypass:
    latency_samples = 0
    lookahead_samples = 0

    def __init__(self):
        self.closed = False

    def push(self, block):
        if self.closed:
            raise RuntimeError('stream already flushed')
        return np.asarray(block, dtype=np.float64).copy()

    def flush(self):
        if self.closed:
            raise RuntimeError('stream already flushed')
        self.closed = True
        return np.empty(0, dtype=np.float64)


class CausalFIR:
    """History is carried across callbacks; no per-block zero-phase resampling."""
    def __init__(self, taps):
        self.taps = np.asarray(taps, dtype=np.float64)
        self.history = np.zeros(len(self.taps)-1, dtype=np.float64)

    def push(self, x):
        if not len(x):
            return np.empty(0, dtype=np.float64)
        extended = np.concatenate((self.history, x))
        y = np.convolve(extended, self.taps, mode='valid')
        self.history[:] = extended[-len(self.history):]
        return y


def rate_taps():
    # Windowed-sinc linear-phase low-pass: 61 taps, 7 kHz cutoff at 48 kHz.
    # Infrastructure choice, not a frozen quality prediction or v15 experiment.
    n = np.arange(61, dtype=np.float64)-30
    cutoff = 7000/48000
    taps = 2*cutoff*np.sinc(2*cutoff*n)*np.kaiser(61, 5.0)
    return taps/np.sum(taps)


class Pipeline:
    """Device-rate push -> same-length output, one core push per callback.

    48k decimation phase is global (not restarted at block boundaries).
    Interpolation places core samples on the same phase, with gain three.
    """
    def __init__(self, fs, processor):
        if fs not in (16000, 48000):
            raise ValueError('fs must be 16000 or 48000')
        if processor not in PROCESSORS:
            raise ValueError('unknown processor')
        if processor == 'transient_ar_v17' and fs != 16000:
            raise ValueError('v17 host requires 16000 Hz; no implicit resampling')
        self.fs, self.name, self.position = fs, processor, 0
        self.processor = (StreamingBypass() if processor == 'bypass' else
                          FastCausalHPSS(16000) if processor == 'hpss_causal' else
                          TransientARShifted(16000) if processor == 'transient_ar_v17' else
                          FastTransientAR(16000))
        if fs == 48000:
            taps = rate_taps()
            self.down = CausalFIR(taps)
            self.up = CausalFIR(3*taps)

    def push(self, block):
        x = np.asarray(block, dtype=np.float64)
        if x.ndim != 1:
            raise ValueError('mono one-dimensional input required')
        if not np.isfinite(x).all():
            raise ValueError('non-finite audio input')
        if self.fs == 16000:
            if self.name == 'transient_ar_v17' and self.position + len(x) > V17_MAX_FRAMES:
                raise ValueError('v17 take exceeds 11 seconds')
            y = self.processor.push(x)
            self.position += len(x)
            return y
        first = (-self.position) % 3
        filtered = self.down.push(x)
        core = self.processor.push(filtered[first::3])
        expanded = np.zeros(len(x), dtype=np.float64)
        expanded[first::3] = core  # interpolation gain is in up.taps
        self.position += len(x)
        return self.up.push(expanded)

    def metadata(self):
        core = int(self.processor.latency_samples)
        conversion = 60 if self.fs == 48000 else 0
        return {'scope': '技術驗證，非合約交付', 'processor': self.name,
                'device_fs': self.fs, 'internal_fs': 16000,
                'algorithm_latency_samples_16k': core,
                'algorithm_latency_ms': core/16,
                'lookahead_samples_16k': int(self.processor.lookahead_samples),
                'resampling_group_delay_device_samples': conversion,
                'resampling_group_delay_ms': 1000*conversion/self.fs,
                'resampling': '61-tap causal FIR each way; 7 kHz cutoff; Kaiser beta 5; global phase' if conversion else 'none',
                'end_to_end_hardware_ms': None,
                'hardware_status': '待錄音日實測'}


class CallbackHost:
    def __init__(self, fs, processor):
        self.pipeline = Pipeline(fs, processor)
        self.counts = {name: 0 for name in FLAGS}
        self.callbacks = 0
        self.frames = 0
        self.error = None

    def callback(self, indata, outdata, frames, callback_time=None, status=None):
        outdata.fill(0)
        for name in FLAGS:
            if status is not None and bool(getattr(status, name, False)):
                self.counts[name] += 1
        if indata.shape != (frames, 1) or outdata.shape != (frames, 1):
            raise ValueError('mono callback shape mismatch')
        y = self.pipeline.push(indata[:, 0])
        if y.shape != (frames,) or not np.isfinite(y).all():
            raise RuntimeError('invalid processor output')
        outdata[:, 0] = y
        self.callbacks += 1
        self.frames += frames

    def summary(self):
        result = self.pipeline.metadata()
        result.update(callbacks=self.callbacks, frames=self.frames,
                      callback_flags=self.counts.copy(), callback_error=self.error)
        if hasattr(self.pipeline.processor, 'counters'):
            result['ar_counters'] = self.pipeline.processor.counters.copy()
        return result


class DelayLine:
    """Explicit software-only model of a return cable/interface delay."""
    def __init__(self, samples):
        if samples < 0:
            raise ValueError('negative delay')
        self.buffer = np.zeros(samples, dtype=np.float32)

    def push(self, x):
        if len(self.buffer) == 0:
            return x.copy()
        q = np.concatenate((self.buffer, x))
        y = q[:len(x)].copy()
        self.buffer = q[len(x):].copy()
        return y


def simulate(source, fs, block, processor, delay_samples):
    if block <= 0:
        raise ValueError('block must be positive')
    if processor == 'transient_ar_v17':
        if fs != 16000:
            raise ValueError('v17 host requires 16000 Hz')
        from item3_v17_latency_probe import Capture
        source = np.asarray(source, dtype=np.float32)
        capture = Capture(processor, block, len(source), source)
        for _ in range(capture.total // block):
            capture.step(np.zeros((block, 1), np.float32),
                         np.empty((block, 1), np.float32), block)
        returned = DelayLine(delay_samples).push(capture.played)
        result = capture.summary()
        result.update(mode='loopback-sim', simulated_return_delay_samples=delay_samples,
                      simulated_return_delay_ms=1000*delay_samples/fs,
                      measurement_note='Unpaced fixture; return arrays trimmed to input length; full tail in dedicated probe; no hardware claim')
        return capture.played[:len(source)], returned[:len(source)], result
    host = CallbackHost(fs, processor)
    line = DelayLine(delay_samples)
    length = len(source)
    padded = np.pad(np.asarray(source, dtype=np.float32), (0, (-length) % block))
    played = np.empty_like(padded)
    recorded = np.empty_like(padded)
    durations = []
    start = time.perf_counter()
    for offset in range(0, len(padded), block):
        input_block = padded[offset:offset+block, None]
        output_block = played[offset:offset+block, None]
        t0 = time.perf_counter()
        host.callback(input_block, output_block, block)
        durations.append(time.perf_counter()-t0)
        recorded[offset:offset+block] = line.push(output_block[:, 0])
    result = host.summary()
    elapsed = time.perf_counter()-start
    result.update(mode='loopback-sim', simulated_return_delay_samples=delay_samples,
                  simulated_return_delay_ms=1000*delay_samples/fs,
                  block=block, wall_seconds=elapsed,
                  rtf=elapsed/(len(padded)/fs) if len(padded) else None,
                  simulated_callbacks_over_block_budget=sum(t > block/fs for t in durations),
                  measurement_note='Unpaced software wall-clock including instrumentation; no hardware deadline or actual underrun inference')
    return played[:length], recorded[:length], result


def parse_device(value):
    if value is None:
        return None
    try:
        return int(value)
    except ValueError:
        return value


def hardware_run(args):
    if args.processor == 'transient_ar_v17':
        from item3_v17_latency_probe import live_capture
        return live_capture(args)
    try:
        import sounddevice as sd
    except ImportError as exc:
        raise RuntimeError('Hardware mode requires sounddevice; --loopback-sim needs only NumPy') from exc
    host = CallbackHost(args.fs, args.processor)

    def callback(indata, outdata, frames, callback_time, status):
        try:
            host.callback(indata, outdata, frames, callback_time, status)
        except Exception as exc:
            outdata.fill(0)
            host.error = str(exc)
            raise sd.CallbackAbort

    print(json.dumps(host.pipeline.metadata(), ensure_ascii=False), flush=True)
    with sd.Stream(samplerate=args.fs, blocksize=args.block,
                   device=parse_device(args.device), channels=1, dtype='float32',
                   latency='low', callback=callback) as stream:
        stop = time.monotonic()+args.seconds
        previous = host.counts.copy()
        while stream.active and time.monotonic() < stop:
            time.sleep(0.05)
            if host.counts != previous:
                print(json.dumps({'callback_flags': host.counts.copy()}), flush=True)
                previous = host.counts.copy()
        metadata = {'reported_interface_latency_seconds': list(stream.latency),
                    'device': list(stream.device), 'sounddevice': sd.__version__,
                    'portaudio': list(sd.get_portaudio_version())}
    result = host.summary()
    result.update(mode='hardware', block=args.block, **metadata)
    if host.error:
        raise RuntimeError(host.error)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--device', help='sounddevice device index or query string')
    ap.add_argument('--fs', type=int, choices=(16000, 48000), default=16000)
    ap.add_argument('--block', type=int, default=128)
    ap.add_argument('--processor', choices=PROCESSORS, default='bypass')
    ap.add_argument('--seconds', type=float, default=3)
    ap.add_argument('--loopback-sim', action='store_true')
    ap.add_argument('--sim-delay-blocks', type=int, default=2)
    ap.add_argument('--output', type=Path)
    ap.add_argument('--capture-dir', type=Path, help='new directory required for v17 hardware capture')
    ap.add_argument('--setup', type=Path, help='hardware setup JSON required for v17')
    args = ap.parse_args()
    if args.block <= 0 or not np.isfinite(args.seconds) or args.seconds <= 0 or args.sim_delay_blocks < 0:
        ap.error('block/seconds must be positive; simulated delay cannot be negative')
    if args.loopback_sim:
        t = np.arange(round(args.seconds*args.fs))/args.fs
        source = .05*np.sin(2*np.pi*440*t)
        if len(source):
            source[min(len(source)-1, args.fs//2)] += .25
        _, _, result = simulate(source, args.fs, args.block, args.processor, args.sim_delay_blocks*args.block)
    else:
        result = hardware_run(args)
    text = json.dumps(result, indent=2, ensure_ascii=False)+'\n'
    print(text)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)


if __name__ == '__main__':
    main()
