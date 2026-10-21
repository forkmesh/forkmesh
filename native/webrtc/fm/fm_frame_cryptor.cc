#include "fm/fm_frame_cryptor.h"

namespace fm {

void FrameEncryptor::SetKey(const FrameKey& key, uint8_t key_id) {
  key_.Set(key, key_id);
}

int FrameEncryptor::Encrypt(webrtc::MediaType media_type,
                            uint32_t ssrc,
                            std::span<const uint8_t> additional_data,
                            std::span<const uint8_t> frame,
                            std::span<uint8_t> encrypted_frame,
                            size_t* bytes_written) {
  bool audio = media_type == webrtc::MediaType::AUDIO;
  size_t written = 0;
  if (!key_.Encrypt(audio, frame.data(), frame.size(), encrypted_frame.data(), encrypted_frame.size(),
                    &written)) {
    return 1;
  }
  *bytes_written = written;
  return 0;
}

size_t FrameEncryptor::GetMaxCiphertextByteSize(webrtc::MediaType media_type, size_t frame_size) {
  return frame_size + kFrameOverhead;
}

void FrameDecryptor::SetKey(uint8_t key_id, const FrameKey& key) {
  keys_.Set(key_id, key);
}

void FrameDecryptor::ClearKeys() {
  keys_.Clear();
}

webrtc::FrameDecryptorInterface::Result FrameDecryptor::Decrypt(webrtc::MediaType media_type,
                                                                const std::vector<uint32_t>& csrcs,
                                                                std::span<const uint8_t> additional_data,
                                                                std::span<const uint8_t> encrypted_frame,
                                                                std::span<uint8_t> frame) {
  bool audio = media_type == webrtc::MediaType::AUDIO;
  size_t written = 0;
  if (!keys_.Decrypt(audio, encrypted_frame.data(), encrypted_frame.size(), frame.data(), frame.size(),
                     &written)) {
    return Result(Status::kFailedToDecrypt, 0);
  }
  return Result(Status::kOk, written);
}

size_t FrameDecryptor::GetMaxPlaintextByteSize(webrtc::MediaType media_type, size_t encrypted_frame_size) {
  return encrypted_frame_size;
}

}  // namespace fm
