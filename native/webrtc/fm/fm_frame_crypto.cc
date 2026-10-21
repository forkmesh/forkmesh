#include "fm/fm_frame_crypto.h"

#include <openssl/evp.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace fm {
namespace {

struct CipherContextFree {
  void operator()(EVP_CIPHER_CTX* context) const { EVP_CIPHER_CTX_free(context); }
};

using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, CipherContextFree>;

void CounterNonce(uint64_t counter, uint8_t nonce[12]) {
  std::memset(nonce, 0, 12);
  for (int i = 0; i < 8; ++i) {
    nonce[11 - i] = static_cast<uint8_t>(counter >> (8 * i));
  }
}

bool Seal(const FrameKey& key,
          const uint8_t nonce[12],
          const uint8_t* aad,
          size_t aad_size,
          const uint8_t* plain,
          size_t plain_size,
          uint8_t* out,
          uint8_t tag[kFrameTagBytes]) {
  CipherContext context(EVP_CIPHER_CTX_new());
  int length = 0;
  if (!context ||
      EVP_EncryptInit_ex(context.get(), EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
      EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce) != 1) {
    return false;
  }
  if (aad_size > 0 &&
      EVP_EncryptUpdate(context.get(), nullptr, &length, aad, static_cast<int>(aad_size)) != 1) {
    return false;
  }
  if (plain_size > 0 &&
      EVP_EncryptUpdate(context.get(), out, &length, plain, static_cast<int>(plain_size)) != 1) {
    return false;
  }
  return EVP_EncryptFinal_ex(context.get(), out + plain_size, &length) == 1 &&
         EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, kFrameTagBytes, tag) == 1;
}

bool Open(const FrameKey& key,
          const uint8_t nonce[12],
          const uint8_t* aad,
          size_t aad_size,
          const uint8_t* sealed,
          size_t sealed_size,
          const uint8_t tag[kFrameTagBytes],
          uint8_t* out) {
  CipherContext context(EVP_CIPHER_CTX_new());
  int length = 0;
  uint8_t expected[kFrameTagBytes];
  std::memcpy(expected, tag, kFrameTagBytes);
  if (!context ||
      EVP_DecryptInit_ex(context.get(), EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
      EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce) != 1) {
    return false;
  }
  if (aad_size > 0 &&
      EVP_DecryptUpdate(context.get(), nullptr, &length, aad, static_cast<int>(aad_size)) != 1) {
    return false;
  }
  if (sealed_size > 0 &&
      EVP_DecryptUpdate(context.get(), out, &length, sealed, static_cast<int>(sealed_size)) != 1) {
    return false;
  }
  return EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, kFrameTagBytes, expected) == 1 &&
         EVP_DecryptFinal_ex(context.get(), out + sealed_size, &length) == 1;
}

}  // namespace

size_t FrameClearBytes(bool audio, const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  if (audio) {
    return 1;
  }
  bool keyframe = (data[0] & 0x01) == 0;
  return std::min<size_t>(keyframe ? 10 : 3, size);
}

bool EncryptFrame(bool audio,
                  const FrameKey& key,
                  uint8_t key_id,
                  uint64_t counter,
                  const uint8_t* frame,
                  size_t frame_size,
                  uint8_t* out,
                  size_t out_capacity,
                  size_t* written) {
  if (out_capacity < frame_size + kFrameOverhead) {
    return false;
  }
  size_t clear = FrameClearBytes(audio, frame, frame_size);
  uint8_t nonce[12];
  CounterNonce(counter, nonce);
  std::memmove(out, frame, clear);
  uint8_t tag[kFrameTagBytes];
  if (!Seal(key, nonce, frame, clear, frame + clear, frame_size - clear, out + clear, tag)) {
    return false;
  }
  size_t at = frame_size;
  std::memcpy(out + at, tag, kFrameTagBytes);
  at += kFrameTagBytes;
  std::memcpy(out + at, nonce + 4, 8);
  at += 8;
  out[at++] = key_id;
  *written = at;
  return true;
}

bool FrameKeyId(const uint8_t* encrypted, size_t encrypted_size, uint8_t* key_id) {
  if (encrypted_size < kFrameOverhead + 1) {
    return false;
  }
  *key_id = encrypted[encrypted_size - 1];
  return true;
}

bool DecryptFrame(bool audio,
                  const FrameKey& key,
                  const uint8_t* encrypted,
                  size_t encrypted_size,
                  uint8_t* out,
                  size_t out_capacity,
                  size_t* written) {
  if (encrypted_size < kFrameOverhead + 1) {
    return false;
  }
  size_t body = encrypted_size - kFrameOverhead;
  if (out_capacity < body) {
    return false;
  }
  size_t clear = FrameClearBytes(audio, encrypted, body);
  uint8_t nonce[12] = {0};
  std::memcpy(nonce + 4, encrypted + encrypted_size - kFrameTrailerBytes, 8);
  const uint8_t* tag = encrypted + body;
  std::vector<uint8_t> plain(body);
  std::memcpy(plain.data(), encrypted, clear);
  if (!Open(key, nonce, encrypted, clear, encrypted + clear, body - clear, tag, plain.data() + clear)) {
    return false;
  }
  std::memcpy(out, plain.data(), body);
  *written = body;
  return true;
}

void FrameSendKey::Set(const FrameKey& key, uint8_t key_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  key_ = key;
  key_id_ = key_id;
  counter_ = 0;
  ready_ = true;
}

bool FrameSendKey::Encrypt(bool audio,
                           const uint8_t* frame,
                           size_t frame_size,
                           uint8_t* out,
                           size_t out_capacity,
                           size_t* written) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ready_) {
    return false;
  }
  ++counter_;
  return EncryptFrame(audio, key_, key_id_, counter_, frame, frame_size, out, out_capacity, written);
}

void FrameKeyring::Set(uint8_t key_id, const FrameKey& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (keys_.count(key_id) == 0) {
    order_.push_back(key_id);
  }
  keys_[key_id] = key;
  while (order_.size() > kFrameKeysKept) {
    keys_.erase(order_.front());
    order_.erase(order_.begin());
  }
}

void FrameKeyring::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  keys_.clear();
  order_.clear();
}

bool FrameKeyring::Decrypt(bool audio,
                           const uint8_t* encrypted,
                           size_t encrypted_size,
                           uint8_t* out,
                           size_t out_capacity,
                           size_t* written) {
  uint8_t key_id = 0;
  if (!FrameKeyId(encrypted, encrypted_size, &key_id)) {
    return false;
  }
  FrameKey key;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = keys_.find(key_id);
    if (found == keys_.end()) {
      return false;
    }
    key = found->second;
  }
  return DecryptFrame(audio, key, encrypted, encrypted_size, out, out_capacity, written);
}

}  // namespace fm
