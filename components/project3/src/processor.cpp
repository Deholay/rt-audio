#include <rt_audio/project3/processor.hpp>

namespace rt_audio::project3 {

void PassthroughProcessor::on_audio(const AudioBlock& input, OutputSink& output) {
    (void)output.try_submit_output(input);
}

} // namespace rt_audio::project3
