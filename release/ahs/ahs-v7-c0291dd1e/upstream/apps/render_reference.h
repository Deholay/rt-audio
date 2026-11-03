#pragma once
#include "fixed_resampler.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ahs_live {
// No silent eviction: losing a timeline sample invalidates the entire epoch.
class MonoQueue {
 public:
  explicit MonoQueue(std::size_t capacity): data_(capacity) {
    if (!capacity) throw std::invalid_argument("zero mono queue");
  }
  bool push(float x) noexcept {
    if (size_==data_.size()) return false;
    data_[(read_+size_)%data_.size()]=x; ++size_; return true;
  }
  bool pop(float& x) noexcept {
    if (!size_) { x=0; return false; }
    x=data_[read_]; read_=(read_+1)%data_.size(); --size_; return true;
  }
  std::size_t size() const noexcept { return size_; }
  void clear() noexcept { read_=size_=0; }
 private:
  std::vector<float> data_; std::size_t read_=0,size_=0;
};
class DownsampleQueue {
 public:
  explicit DownsampleQueue(std::size_t capacity): queue_(capacity) {}
  bool feed(const float* input,std::size_t count) noexcept {
    while(count) {
      spx_uint32_t ni=static_cast<spx_uint32_t>(std::min<std::size_t>(192,count)),no=64;
      std::array<float,64> output{};
      if (filter_.process(input,ni,output.data(),no)!=RESAMPLER_ERR_SUCCESS || (!ni&&!no)) return false;
      for(spx_uint32_t i=0;i<no;++i) if(!queue_.push(output[i])) return false;
      input+=ni;count-=ni;
    }
    return true;
  }
  bool pop(float& x) noexcept {return queue_.pop(x);}
  std::size_t size() const noexcept {return queue_.size();}
  bool reset() noexcept {queue_.clear();return filter_.reset()==RESAMPLER_ERR_SUCCESS;}
 private:
  FixedResampler filter_{48000,16000}; MonoQueue queue_;
};
// Call ONLY after successful ReleaseBuffer, with exactly the submitted data.
// Dropped/unreleased output is never fed. SILENT overrides even a nonzero buffer.
class RenderReference {
 public:
  explicit RenderReference(std::size_t capacity): down_(capacity) {}
  bool committed(const float* submitted,std::size_t frames,unsigned channels,bool silent) noexcept {
    if ((channels!=1&&channels!=2)||(!silent&&!submitted)) return false;
    std::array<float,192> mono{};
    for(std::size_t start=0;start<frames;start+=mono.size()) {
      const auto count=std::min(mono.size(),frames-start);
      for(std::size_t i=0;i<count;++i) {
        float x=0;
        if(!silent) {
          x=submitted[(start+i)*channels];
          if(channels==2) x=.5f*x+.5f*submitted[(start+i)*channels+1];
          if(!std::isfinite(x)) return false; // final render must already be finite
        }
        mono[i]=x;
      }
      if(!down_.feed(mono.data(),count)) return false;
    }
    committed_frames_+=frames;return true;
  }
  bool pop(float& x) noexcept {return down_.pop(x);}
  std::size_t size() const noexcept {return down_.size();}
  std::size_t committed_frames() const noexcept {return committed_frames_;}
  bool reopen() noexcept {committed_frames_=0;return down_.reset();}
 private:
  DownsampleQueue down_;std::size_t committed_frames_=0;
};
}
