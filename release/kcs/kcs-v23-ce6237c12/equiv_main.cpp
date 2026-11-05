// Equivalence driver for verify_equivalence.py: raw float32 mono 48 kHz in,
// raw float32 out (same length, Bridge48k.process_block semantics), one line
// per trigger on stdout, final counters on stderr.
// Usage: kcs_v23_equiv <in.f32> <out.f32>

#include "pipeline.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: kcs_v23_equiv <in.f32> <out.f32>\n";
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<float> x;
    for (float v; in.read(reinterpret_cast<char*>(&v), sizeof v);) x.push_back(v);
    using rt_audio::kcs::v23_ce6237c12::Pipeline48k;
    Pipeline48k pipeline;
    std::vector<float> y(x.size());
    std::uint64_t triggers = 0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        y[n] = pipeline.tick(x[n]);
        const auto& core = pipeline.core();
        if (core.counters().triggers != triggers) {
            triggers = core.counters().triggers;
            const auto& e = core.last_event();
            std::printf("%llu %u %.17g %.17g %d\n", static_cast<unsigned long long>(e.trigger_sample), e.window,
                        e.r_db, e.s_dbfs, e.bypassed ? 1 : 0);
        }
    }
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(y.data()), static_cast<std::streamsize>(y.size() * sizeof(float)));
    const auto& c = pipeline.core().counters();
    std::cerr << "triggers=" << c.triggers << " low_energy=" << c.low_energy << " pole_projection=" << c.pole_projection
              << " projected_roots=" << c.projected_roots << " solve_failure=" << c.solve_failure
              << " nonfinite_prediction=" << c.nonfinite_prediction << " warmup_samples=" << c.warmup_samples
              << " bypass=" << c.bypass << '\n';
    return out ? 0 : 1;
}
