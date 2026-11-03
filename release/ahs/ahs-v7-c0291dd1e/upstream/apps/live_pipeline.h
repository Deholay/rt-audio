#pragma once
#include "render_reference.h"
#include "ahs/engine.h"
#include <array>
namespace ahs_live {
// Owns one epoch. Recreate outside transfer loop on discontinuity/reopen.
class LivePipeline {
 public:
  explicit LivePipeline(std::size_t capacity): mic_(capacity),reference_(capacity),output_(capacity*3),engine_(config()) {}
  static ahs::EngineConfig config() {
    ahs::EngineConfig c;c.sample_rate=16000;c.block_size=64;c.enable_shift=false;
    c.detector.growth_papr_threshold_db=15;return c;
  }
  bool capture(const float* input,std::size_t count,unsigned channels,bool silent) noexcept {
    std::array<float,192> mono{};
    for(std::size_t start=0;start<count;start+=mono.size()) {
      const auto n=std::min(mono.size(),count-start);
      for(std::size_t i=0;i<n;++i){
        float l=silent?0:input[(start+i)*channels];
        float r=silent?0:channels==2?input[(start+i)*channels+1]:l;
        if(!std::isfinite(l))l=0;if(!std::isfinite(r))r=0;
        mono[i]=.5f*l+.5f*r;
      }
      if(!mic_.feed(mono.data(),n)||!pump())return false;
    }
    return true;
  }
  bool committed(const float* final,std::size_t frames,unsigned channels,bool silent) noexcept {
    return reference_.committed(final,frames,channels,silent)&&pump();
  }
  std::size_t render(float* final,std::size_t frames,unsigned channels,float gain) noexcept {
    std::size_t silence=0;
    for(std::size_t i=0;i<frames;++i){float x;if(!output_.pop(x))++silence;
      x=std::isfinite(x)?std::clamp(x*gain,-1.0f,1.0f):0;
      for(unsigned ch=0;ch<channels;++ch)final[i*channels+ch]=x;
    }
    return silence;
  }
  std::size_t queued_device_frames() const noexcept{return output_.size();}
  std::size_t processed_blocks() const noexcept{return blocks_;}
 private:
  bool pump() noexcept {
    while(mic_.size()>=64&&reference_.size()>=64){
      std::array<float,64> mic{},ref{},processed{};
      for(std::size_t i=0;i<64;++i){mic_.pop(mic[i]);reference_.pop(ref[i]);}
      engine_.processBlock(mic.data(),ref.data(),processed.data(),64);++blocks_;
      std::size_t offset=0;
      while(offset<64){
        std::array<float,192> up{};spx_uint32_t ni=static_cast<spx_uint32_t>(64-offset),no=192;
        if(up_.process(processed.data()+offset,ni,up.data(),no)!=RESAMPLER_ERR_SUCCESS||(!ni&&!no))return false;
        offset+=ni;for(spx_uint32_t j=0;j<no;++j)if(!output_.push(up[j]))return false;
      }
    }
    return true;
  }
  DownsampleQueue mic_;RenderReference reference_;MonoQueue output_;
  ahs::AhsEngine engine_;FixedResampler up_{16000,48000};std::size_t blocks_=0;
};
}
