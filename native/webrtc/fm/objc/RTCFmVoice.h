#import <Foundation/Foundation.h>

#import "sdk/objc/base/RTCMacros.h"

NS_ASSUME_NONNULL_BEGIN

@class RTC_OBJC_TYPE(RTCPeerConnectionFactory);

typedef void (^RTCFmCaptureBlock)(float *samples, NSInteger frames, NSInteger sampleRate);

RTC_OBJC_EXPORT
@interface RTC_OBJC_TYPE (RTCFmCaptureProcessor) : NSObject

@property(atomic, copy, nullable) RTCFmCaptureBlock block;

@end

RTC_OBJC_EXPORT
@interface RTC_OBJC_TYPE (RTCFmFactory) : NSObject

+ (RTC_OBJC_TYPE(RTCPeerConnectionFactory) *)factoryWithCaptureProcessor:
    (RTC_OBJC_TYPE(RTCFmCaptureProcessor) *)processor;

@end

NS_ASSUME_NONNULL_END
