#include <rt_audio/project2/processor.hpp>

#include <algorithm>

namespace rt_audio {

void PassthroughProcessor::process(const ProcessBlock& block) noexcept {
    if (block.input != block.output) {
        std::copy_n(block.input,
                    static_cast<std::size_t>(block.frames) * block.channels,
                    block.output);
    }
}

} // namespace rt_audio
