#import "RTCFmVoice.h"

#import "RTCPeerConnectionFactoryBuilder+DefaultComponents.h"
#import "RTCPeerConnectionFactoryBuilder.h"

#include <memory>
#include <string>

#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "modules/audio_processing/audio_buffer.h"

@implementation RTC_OBJC_TYPE (RTCFmCaptureProcessor)
@synthesize block = _block;
@end

namespace {

class CaptureBlockProcessing : public webrtc::CustomProcessing {
 public:
  explicit CaptureBlockProcessing(RTC_OBJC_TYPE(RTCFmCaptureProcessor) * processor) : processor_(processor) {}

  void Initialize(int sample_rate_hz, int num_channels) override { sample_rate_ = sample_rate_hz; }

  void Process(webrtc::AudioBuffer* audio) override {
    RTCFmCaptureBlock block = processor_.block;
    if (block == nil || audio == nullptr || audio->num_channels() == 0) {
      return;
    }
    auto mono = audio->view()[0];
    block(mono.data(), static_cast<NSInteger>(mono.size()), sample_rate_);
  }

  std::string ToString() const override { return "ForkMeshCaptureProcessing"; }

 private:
  RTC_OBJC_TYPE(RTCFmCaptureProcessor) * processor_;
  int sample_rate_ = 48000;
};

}  // namespace

@implementation RTC_OBJC_TYPE (RTCFmFactory)

+ (RTC_OBJC_TYPE(RTCPeerConnectionFactory) *)factoryWithCaptureProcessor:
    (RTC_OBJC_TYPE(RTCFmCaptureProcessor) *)processor {
  RTCPeerConnectionFactoryBuilder *builder = [RTCPeerConnectionFactoryBuilder defaultBuilder];
  auto processing = std::make_unique<webrtc::BuiltinAudioProcessingBuilder>();
  processing->SetCapturePostProcessing(std::make_unique<CaptureBlockProcessing>(processor));
  [builder setAudioProcessingBuilder:std::move(processing)];
  return [builder createPeerConnectionFactory];
}

@end
