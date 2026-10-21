#ifndef FM_FRAME_CRYPTO_H_
#define FM_FRAME_CRYPTO_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace fm {

constexpr size_t kFrameKeyBytes = 16;
constexpr size_t kFrameTagBytes = 16;
constexpr size_t kFrameTrailerBytes = 9;
constexpr size_t kFrameOverhead = kFrameTagBytes + kFrameTrailerBytes;
constexpr size_t kFrameKeysKept = 3;

using FrameKey = std::array<uint8_t, kFrameKeyBytes>;

size_t FrameClearBytes(bool audio, const uint8_t* data, size_t size);

bool EncryptFrame(bool audio,
                  const FrameKey& key,
                  uint8_t key_id,
                  uint64_t counter,
                  const uint8_t* frame,
                  size_t frame_size,
                  uint8_t* out,
                  size_t out_capacity,
                  size_t* written);

bool DecryptFrame(bool audio,
                  const FrameKey& key,
                  const uint8_t* encrypted,
                  size_t encrypted_size,
                  uint8_t* out,
                  size_t out_capacity,
                  size_t* written);

bool FrameKeyId(const uint8_t* encrypted, size_t encrypted_size, uint8_t* key_id);

class FrameSendKey {
 public:
  void Set(const FrameKey& key, uint8_t key_id);
  bool Encrypt(bool audio,
               const uint8_t* frame,
               size_t frame_size,
               uint8_t* out,
               size_t out_capacity,
               size_t* written);

 private:
  std::mutex mutex_;
  bool ready_ = false;
  FrameKey key_{};
  uint8_t key_id_ = 0;
  uint64_t counter_ = 0;
};

class FrameKeyring {
 public:
  void Set(uint8_t key_id, const FrameKey& key);
  void Clear();
  bool Decrypt(bool audio,
               const uint8_t* encrypted,
               size_t encrypted_size,
               uint8_t* out,
               size_t out_capacity,
               size_t* written);

 private:
  std::mutex mutex_;
  std::map<uint8_t, FrameKey> keys_;
  std::vector<uint8_t> order_;
};

}  // namespace fm

#endif  // FM_FRAME_CRYPTO_H_
