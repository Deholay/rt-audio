#include "pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace rt_audio::kcs::v23_ce6237c12 {

namespace {

constexpr int order = Core16k::order;
using Matrix = std::array<std::array<double, order>, order>;

// np.sinc: sin(pi*y)/(pi*y), y = 1e-20 at 0.
double sinc(double x) {
    const double y = std::numbers::pi * (x == 0.0 ? 1.0e-20 : x);
    return std::sin(y) / y;
}

// Modified Bessel I0 by power series (np.i0 uses Cephes Chebyshev; differs in last bits).
double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 200; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1.0e-18 * sum) break;
    }
    return sum;
}

// np.linalg.solve (LAPACK gesv): LU with partial pivoting. False if singular.
bool solve(Matrix a, std::array<double, order>& x) {
    for (int col = 0; col < order; ++col) {
        int pivot = col;
        for (int r = col + 1; r < order; ++r) {
            if (std::abs(a[r][col]) > std::abs(a[pivot][col])) pivot = r;
        }
        if (a[pivot][col] == 0.0) return false;
        std::swap(a[pivot], a[col]);
        std::swap(x[pivot], x[col]);
        for (int r = col + 1; r < order; ++r) {
            const double f = a[r][col] / a[col][col];
            for (int k = col; k < order; ++k) a[r][k] -= f * a[col][k];
            x[r] -= f * x[col];
        }
    }
    for (int r = order - 1; r >= 0; --r) {
        double s = x[r];
        for (int k = r + 1; k < order; ++k) s -= a[r][k] * x[k];
        x[r] = s / a[r][r];
    }
    return true;
}

// Eigenvalues of an upper Hessenberg matrix: balancing + Francis double-shift
// QR (Numerical Recipes 3rd ed. Unsymmeig::balance/hqr). np.roots builds the
// same companion matrix and calls LAPACK geev. False = no convergence, which
// NumPy reports as LinAlgError.
bool hessenberg_eigenvalues(Matrix& a, std::array<std::complex<double>, order>& w) {
    constexpr int n = order;
    constexpr double radix = std::numeric_limits<double>::radix;
    for (bool done = false; !done;) {
        done = true;
        for (int i = 0; i < n; ++i) {
            double r = 0.0, c = 0.0;
            for (int j = 0; j < n; ++j) {
                if (j != i) { c += std::abs(a[j][i]); r += std::abs(a[i][j]); }
            }
            if (c == 0.0 || r == 0.0) continue;
            double g = r / radix, f = 1.0;
            const double s = c + r;
            while (c < g) { f *= radix; c *= radix * radix; }
            g = r * radix;
            while (c > g) { f /= radix; c /= radix * radix; }
            if ((c + r) / f < 0.95 * s) {
                done = false;
                for (int j = 0; j < n; ++j) a[i][j] /= f;
                for (int j = 0; j < n; ++j) a[j][i] *= f;
            }
        }
    }

    constexpr double eps = std::numeric_limits<double>::epsilon();
    double anorm = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int j = std::max(i - 1, 0); j < n; ++j) anorm += std::abs(a[i][j]);
    }
    int nn = n - 1, l = 0;
    double t = 0.0, p = 0.0, q = 0.0, r = 0.0, s = 0.0, x = 0.0, y = 0.0, z = 0.0, u = 0.0, v = 0.0, ww = 0.0;
    while (nn >= 0) {
        int its = 0;
        do {
            for (l = nn; l > 0; --l) {
                s = std::abs(a[l - 1][l - 1]) + std::abs(a[l][l]);
                if (s == 0.0) s = anorm;
                if (std::abs(a[l][l - 1]) <= eps * s) { a[l][l - 1] = 0.0; break; }
            }
            x = a[nn][nn];
            if (l == nn) {
                w[nn--] = x + t;
            } else {
                y = a[nn - 1][nn - 1];
                ww = a[nn][nn - 1] * a[nn - 1][nn];
                if (l == nn - 1) {
                    p = 0.5 * (y - x);
                    q = p * p + ww;
                    z = std::sqrt(std::abs(q));
                    x += t;
                    if (q >= 0.0) {
                        z = p + std::copysign(z, p);
                        w[nn - 1] = w[nn] = x + z;
                        if (z != 0.0) w[nn] = x - ww / z;
                    } else {
                        w[nn] = {x + p, -z};
                        w[nn - 1] = std::conj(w[nn]);
                    }
                    nn -= 2;
                } else {
                    if (its == 30) return false;
                    if (its == 10 || its == 20) {
                        t += x;
                        for (int i = 0; i < nn + 1; ++i) a[i][i] -= x;
                        s = std::abs(a[nn][nn - 1]) + std::abs(a[nn - 1][nn - 2]);
                        y = x = 0.75 * s;
                        ww = -0.4375 * s * s;
                    }
                    ++its;
                    int m = nn - 2;
                    for (; m >= l; --m) {
                        z = a[m][m];
                        r = x - z;
                        s = y - z;
                        p = (r * s - ww) / a[m + 1][m] + a[m][m + 1];
                        q = a[m + 1][m + 1] - z - r - s;
                        r = a[m + 2][m + 1];
                        s = std::abs(p) + std::abs(q) + std::abs(r);
                        p /= s; q /= s; r /= s;
                        if (m == l) break;
                        u = std::abs(a[m][m - 1]) * (std::abs(q) + std::abs(r));
                        v = std::abs(p) * (std::abs(a[m - 1][m - 1]) + std::abs(z) + std::abs(a[m + 1][m + 1]));
                        if (u <= eps * v) break;
                    }
                    for (int i = m; i < nn - 1; ++i) {
                        a[i + 2][i] = 0.0;
                        if (i != m) a[i + 2][i - 1] = 0.0;
                    }
                    for (int k = m; k < nn; ++k) {
                        if (k != m) {
                            p = a[k][k - 1];
                            q = a[k + 1][k - 1];
                            r = 0.0;
                            if (k + 1 != nn) r = a[k + 2][k - 1];
                            if ((x = std::abs(p) + std::abs(q) + std::abs(r)) != 0.0) {
                                p /= x; q /= x; r /= x;
                            }
                        }
                        if ((s = std::copysign(std::sqrt(p * p + q * q + r * r), p)) != 0.0) {
                            if (k == m) {
                                if (l != m) a[k][k - 1] = -a[k][k - 1];
                            } else {
                                a[k][k - 1] = -s * x;
                            }
                            p += s;
                            x = p / s; y = q / s; z = r / s;
                            q /= p; r /= p;
                            for (int j = k; j < nn + 1; ++j) {
                                p = a[k][j] + q * a[k + 1][j];
                                if (k + 1 != nn) {
                                    p += r * a[k + 2][j];
                                    a[k + 2][j] -= p * z;
                                }
                                a[k + 1][j] -= p * y;
                                a[k][j] -= p * x;
                            }
                            const int mmin = nn < k + 3 ? nn : k + 3;
                            for (int i = l; i < mmin + 1; ++i) {
                                p = x * a[i][k] + y * a[i][k + 1];
                                if (k + 1 != nn) {
                                    p += z * a[i][k + 2];
                                    a[i][k + 2] -= p * r;
                                }
                                a[i][k + 1] -= p * q;
                                a[i][k] -= p;
                            }
                        }
                    }
                }
            }
        } while (l + 1 < nn);
    }
    return true;
}

bool all_finite(const std::array<double, order>& v) {
    return std::all_of(v.begin(), v.end(), [](double e) { return std::isfinite(e); });
}

} // namespace

Core16k::Core16k() noexcept {
    // TransientAR32.__init__: 33-tap Hamming high-pass, unit gain at Nyquist.
    std::array<double, 33> low{};
    double low_sum = 0.0;
    for (int i = 0; i < 33; ++i) {
        low[i] = 2.0 * (1000.0 / 16000.0) * sinc(2.0 * (1000.0 / 16000.0) * (i - 16));
        low[i] *= 0.54 - 0.46 * std::cos(2.0 * std::numbers::pi * i / 32.0);
        low_sum += low[i];
    }
    double nyquist = 0.0;
    for (int i = 0; i < 33; ++i) {
        fir_[i] = -(low[i] / low_sum);
        if (i == 16) fir_[i] += 1.0;
        nyquist += fir_[i] * (i % 2 ? -1.0 : 1.0);
    }
    for (double& tap : fir_) tap /= nyquist;
    reset();
}

void Core16k::reset() noexcept {
    fir_input_.fill(0.0);
    z_.fill(0.0);
    raw_.fill(0.0);
    fifo_.fill(0.0);
    fir_head_ = z_head_ = z_count_ = raw_head_ = raw_count_ = fifo_head_ = 0;
    clock_ = above_ = next_trigger_ = 0;
    event_start_ = previous_end_ = -(b + w_short);
    repair_length_ = b + w_short;
    have_replacement_ = false;
    counters_ = {};
    last_event_ = {};
}

// np.linspace ramps of length C at both ends of a window of repair_length_.
double Core16k::weight(int j) const noexcept {
    if (j < c) return j == c - 1 ? 1.0 : j * (1.0 / (c - 1));
    const int k = j - (repair_length_ - c);
    if (k >= 0) return k == c - 1 ? 0.0 : k * (-1.0 / (c - 1)) + 1.0;
    return 1.0;
}

double Core16k::tick(double value) {
    const std::int64_t t = clock_;
    fir_input_[fir_head_] = value;
    fir_head_ = (fir_head_ + 1) % 33;
    double filtered = 0.0;
    for (int k = 0; k < 33; ++k) filtered += fir_[k] * fir_input_[(fir_head_ + 32 - k) % 33];
    z_[(z_head_ + z_count_) % z_len] = filtered;
    if (z_count_ < z_len) ++z_count_;
    else z_head_ = (z_head_ + 1) % z_len;

    const bool eligible = raw_count_ == raw_len && z_count_ == z_len;
    if (!eligible) {
        ++counters_.warmup_samples;
        above_ = 0;
    } else if (t < next_trigger_) {
        above_ = 0;
    } else {
        double long_energy = 0.0, short_energy = 0.0;
        for (int i = 0; i < long_len; ++i) {
            const double e = z_[(z_head_ + i) % z_len];
            long_energy += e * e;
        }
        for (int i = long_len; i < z_len; ++i) {
            const double e = z_[(z_head_ + i) % z_len];
            short_energy += e * e;
        }
        short_energy /= short_len;
        long_energy /= long_len;
        const double ratio = 10.0 * std::log10(std::max(short_energy, 1e-12) / std::max(long_energy, 1e-12));
        above_ = ratio > t_db ? above_ + 1 : 0;
        if (above_ >= d) {
            ++counters_.triggers;
            start_repair(t, ratio, short_energy);
            next_trigger_ = previous_end_ + refractory;
            above_ = 0;
        }
    }
    double u = value;
    const std::int64_t j = t - event_start_;
    if (have_replacement_ && j >= 0 && j < repair_length_) {
        const double w = weight(static_cast<int>(j));
        u = (1.0 - w) * value + w * replacement_[static_cast<std::size_t>(j)];
    }
    // Training includes only original input strictly before the next tick.
    raw_[(raw_head_ + raw_count_) % raw_len] = value;
    if (raw_count_ < raw_len) ++raw_count_;
    else raw_head_ = (raw_head_ + 1) % raw_len;

    const double out = fifo_[fifo_head_];
    fifo_[fifo_head_] = u;
    fifo_head_ = (fifo_head_ + 1) % latency_samples;
    ++clock_;
    return out;
}

void Core16k::start_repair(std::int64_t t, double ratio, double short_energy) {
    const std::int64_t g = t - b;
    // Same integrity checks as KCS (asserts there): never mask a schedule error.
    if (g < previous_end_) throw std::logic_error("KCS v23: overlapping repair windows");
    const std::int64_t first_index = g - (t - latency_samples); // 80
    if (g < 0 || first_index < 0 || first_index + b > latency_samples) {
        throw std::logic_error("KCS v23: repair source already emitted or unavailable");
    }
    const double s_dbfs = 10.0 * std::log10(std::max(short_energy, 1e-12));
    const int window = (ratio >= r_t && s_dbfs >= s_t) ? w_long : w_short;
    repair_length_ = b + window;
    have_replacement_ = fit(repair_length_);
    event_start_ = g;
    previous_end_ = t + window;
    if (!have_replacement_) {
        ++counters_.bypass;
    } else {
        // fifo_head_ is the oldest entry = sample t - latency_samples.
        for (int k = 0; k < b; ++k) {
            double& slot = fifo_[(fifo_head_ + first_index + k) % latency_samples];
            const double w = weight(k);
            slot = (1.0 - w) * slot + w * replacement_[static_cast<std::size_t>(k)];
        }
    }
    last_event_ = {static_cast<std::uint64_t>(t), static_cast<std::uint32_t>(window), ratio, s_dbfs,
                   !have_replacement_};
}

bool Core16k::fit(int length) {
    // training = oldest N of the N+B raw samples.
    std::array<double, n_train> centered{};
    double energy = 0.0, mean = 0.0;
    for (int i = 0; i < n_train; ++i) {
        centered[i] = raw_[(raw_head_ + i) % raw_len];
        energy += centered[i] * centered[i];
        mean += centered[i];
    }
    if (energy / n_train <= 1e-12) {
        ++counters_.low_energy;
        return false;
    }
    mean /= n_train;
    double variance = 0.0;
    for (double& e : centered) {
        e -= mean;
        variance += e * e;
    }
    const double bound = 3.0 * std::sqrt(variance / n_train);

    // design[i][k] = centered[order-k-1+i], target[i] = centered[order+i], i < N-ORDER.
    Matrix gram{};
    std::array<double, order> rhs{};
    for (int a = 0; a < order; ++a) {
        for (int bb = a; bb < order; ++bb) {
            double sum = 0.0;
            for (int i = 0; i < n_train - order; ++i) {
                sum += centered[order - a - 1 + i] * centered[order - bb - 1 + i];
            }
            gram[a][bb] = gram[bb][a] = sum;
        }
        double sum = 0.0;
        for (int i = 0; i < n_train - order; ++i) sum += centered[order - a - 1 + i] * centered[order + i];
        rhs[a] = sum;
    }
    double trace = 0.0;
    for (int a = 0; a < order; ++a) trace += gram[a][a];
    const double ridge = 1e-6 * trace / order;
    for (int a = 0; a < order; ++a) gram[a][a] += ridge;

    std::array<double, order> coeff = rhs;
    if (!solve(gram, coeff) || !all_finite(coeff)) {
        ++counters_.solve_failure;
        return false;
    }
    // np.roots(r_[1, -coeff]): companion matrix, first row = coeff, ones below.
    Matrix companion{};
    for (int k = 0; k < order; ++k) companion[0][k] = coeff[k];
    for (int k = 1; k < order; ++k) companion[k][k - 1] = 1.0;
    std::array<std::complex<double>, order> roots{};
    if (!hessenberg_eigenvalues(companion, roots) ||
        !std::all_of(roots.begin(), roots.end(),
                     [](std::complex<double> r) { return std::isfinite(r.real()) && std::isfinite(r.imag()); })) {
        ++counters_.solve_failure;
        return false;
    }
    int projected = 0;
    for (auto& root : roots) {
        if (std::abs(root) > 0.995) {
            root *= 0.995 / std::abs(root);
            ++projected;
        }
    }
    if (projected) {
        ++counters_.pole_projection;
        counters_.projected_roots += static_cast<std::uint64_t>(projected);
        // coeff = -np.poly(roots)[1:].real
        std::array<std::complex<double>, order + 1> poly{};
        poly[0] = 1.0;
        for (int k = 0; k < order; ++k) {
            for (int i = k + 1; i > 0; --i) poly[i] = poly[i] - roots[k] * poly[i - 1];
        }
        for (int k = 0; k < order; ++k) coeff[k] = -poly[k + 1].real();
        if (!all_finite(coeff)) {
            ++counters_.solve_failure;
            return false;
        }
    }
    // Forward prediction from the last ORDER centered samples, clipped to 3 std.
    std::array<double, n_train - order + max_repair> history{};
    for (int i = 0; i < order; ++i) history[i] = centered[n_train - order + i];
    for (int step = 0; step < length; ++step) {
        double value = 0.0;
        for (int k = 0; k < order; ++k) value += coeff[k] * history[order + step - 1 - k];
        if (!std::isfinite(value)) {
            ++counters_.nonfinite_prediction;
            return false;
        }
        value = std::clamp(value, -bound, bound);
        history[order + step] = value;
        replacement_[static_cast<std::size_t>(step)] = value + mean;
    }
    return true;
}

Pipeline48k::Pipeline48k() noexcept {
    // item3_rt_host.rate_taps(): 61 taps, 7 kHz at 48 kHz, Kaiser beta 5, DC-normalised.
    const double cutoff = 7000.0 / 48000.0;
    const double alpha = (fir_taps - 1) / 2.0;
    double sum = 0.0;
    for (std::uint32_t k = 0; k < fir_taps; ++k) {
        const double n = static_cast<double>(k) - 30.0;
        const double ratio = (k - alpha) / alpha;
        const double window = bessel_i0(5.0 * std::sqrt(1.0 - ratio * ratio)) / bessel_i0(5.0);
        down_taps_[k] = 2.0 * cutoff * sinc(2.0 * cutoff * n) * window;
        sum += down_taps_[k];
    }
    for (std::uint32_t k = 0; k < fir_taps; ++k) {
        down_taps_[k] /= sum;
        up_taps_[k] = 3.0 * down_taps_[k];
    }
    reset();
}

void Pipeline48k::reset() noexcept {
    down_.fill(0.0);
    up_.fill(0.0);
    down_head_ = up_head_ = 0;
    position_ = 0;
    core_.reset();
}

double Pipeline48k::filter(const std::array<double, fir_taps>& taps, std::array<double, fir_taps>& history,
                           int& head, double x) noexcept {
    history[static_cast<std::size_t>(head)] = x;
    head = (head + 1) % static_cast<int>(fir_taps);
    double y = 0.0;
    for (std::uint32_t k = 0; k < fir_taps; ++k) {
        y += taps[k] * history[(static_cast<std::uint32_t>(head) + fir_taps - 1 - k) % fir_taps];
    }
    return y;
}

float Pipeline48k::tick(float x) {
    // Bridge48k._through/_expand: global phase, core sees every third filtered sample.
    const double down = filter(down_taps_, down_, down_head_, static_cast<double>(x));
    const double core = position_ % 3 == 0 ? core_.tick(down) : 0.0;
    ++position_;
    return static_cast<float>(filter(up_taps_, up_, up_head_, core));
}

} // namespace rt_audio::kcs::v23_ce6237c12
