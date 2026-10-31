#import <Foundation/Foundation.h>

#import "sdk/objc/base/RTCMacros.h"

NS_ASSUME_NONNULL_BEGIN

@class RTC_OBJC_TYPE(RTCRtpSender);
@class RTC_OBJC_TYPE(RTCRtpReceiver);

RTC_OBJC_EXPORT
@interface RTC_OBJC_TYPE (RTCFmFrameEncryptor) : NSObject

- (instancetype)init;
- (BOOL)setKey:(NSData *)key keyId:(uint8_t)keyId;
- (void)attachToSender:(RTC_OBJC_TYPE(RTCRtpSender) *)sender;

@end

RTC_OBJC_EXPORT
@interface RTC_OBJC_TYPE (RTCFmFrameDecryptor) : NSObject

- (instancetype)init;
- (BOOL)setKey:(NSData *)key keyId:(uint8_t)keyId;
- (void)clearKeys;
- (void)attachToReceiver:(RTC_OBJC_TYPE(RTCRtpReceiver) *)receiver;

@end

NS_ASSUME_NONNULL_END
