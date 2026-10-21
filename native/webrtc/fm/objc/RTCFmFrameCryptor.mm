#import "RTCFmFrameCryptor.h"

#import "RTCRtpReceiver+Private.h"
#import "RTCRtpSender+Private.h"

#include "api/make_ref_counted.h"
#include "fm/fm_frame_cryptor.h"

namespace {

bool CopyKey(NSData *data, fm::FrameKey *key) {
  if (data.length != fm::kFrameKeyBytes) {
    return false;
  }
  [data getBytes:key->data() length:fm::kFrameKeyBytes];
  return true;
}

}  // namespace

@implementation RTC_OBJC_TYPE (RTCFmFrameEncryptor) {
  webrtc::scoped_refptr<fm::FrameEncryptor> _native;
}

- (instancetype)init {
  self = [super init];
  if (self) {
    _native = webrtc::make_ref_counted<fm::FrameEncryptor>();
  }
  return self;
}

- (BOOL)setKey:(NSData *)key keyId:(uint8_t)keyId {
  fm::FrameKey raw;
  if (!CopyKey(key, &raw)) {
    return NO;
  }
  _native->SetKey(raw, keyId);
  return YES;
}

- (void)attachToSender:(RTC_OBJC_TYPE(RTCRtpSender) *)sender {
  sender.nativeRtpSender->SetFrameEncryptor(_native);
}

@end

@implementation RTC_OBJC_TYPE (RTCFmFrameDecryptor) {
  webrtc::scoped_refptr<fm::FrameDecryptor> _native;
}

- (instancetype)init {
  self = [super init];
  if (self) {
    _native = webrtc::make_ref_counted<fm::FrameDecryptor>();
  }
  return self;
}

- (BOOL)setKey:(NSData *)key keyId:(uint8_t)keyId {
  fm::FrameKey raw;
  if (!CopyKey(key, &raw)) {
    return NO;
  }
  _native->SetKey(keyId, raw);
  return YES;
}

- (void)clearKeys {
  _native->ClearKeys();
}

- (void)attachToReceiver:(RTC_OBJC_TYPE(RTCRtpReceiver) *)receiver {
  receiver.nativeRtpReceiver->SetFrameDecryptor(_native);
}

@end
