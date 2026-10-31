#ifndef FM_FRAME_CRYPTOR_H_
#define FM_FRAME_CRYPTOR_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "api/crypto/frame_decryptor_interface.h"
#include "api/crypto/frame_encryptor_interface.h"
#include "api/media_types.h"
#include "fm/fm_frame_crypto.h"

namespace fm {

class FrameEncryptor : public webrtc::FrameEncryptorInterface {
 public:
  void SetKey(const FrameKey& key, uint8_t key_id);
  int Encrypt(webrtc::MediaType media_type,
              uint32_t ssrc,
              std::span<const uint8_t> additional_data,
              std::span<const uint8_t> frame,
              std::span<uint8_t> encrypted_frame,
              size_t* bytes_written) override;
  size_t GetMaxCiphertextByteSize(webrtc::MediaType media_type, size_t frame_size) override;

 private:
  FrameSendKey key_;
};

class FrameDecryptor : public webrtc::FrameDecryptorInterface {
 public:
  void SetKey(uint8_t key_id, const FrameKey& key);
  void ClearKeys();
  Result Decrypt(webrtc::MediaType media_type,
                 const std::vector<uint32_t>& csrcs,
                 std::span<const uint8_t> additional_data,
                 std::span<const uint8_t> encrypted_frame,
                 std::span<uint8_t> frame) override;
  size_t GetMaxPlaintextByteSize(webrtc::MediaType media_type, size_t encrypted_frame_size) override;

 private:
  FrameKeyring keys_;
};

}  // namespace fm

#endif  // FM_FRAME_CRYPTOR_H_
