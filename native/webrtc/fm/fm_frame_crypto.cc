#include "fm/fm_frame_crypto.h"

#include <openssl/evp.h>

#include <algorithm>
#include <memory>

namespace fm {
namespace {

using Nonce = std::array<uint8_t, 12>;
using Tag = std::array<uint8_t, kFrameTagBytes>;

struct CipherContextFree {
  void operator()(EVP_CIPHER_CTX* context) const { EVP_CIPHER_CTX_free(context); }
};

using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, CipherContextFree>;

void Copy(std::span<const uint8_t> from, std::span<uint8_t> to) {
  std::ranges::copy(from, to.begin());
}

Nonce CounterNonce(uint64_t counter) {
  Nonce nonce{};
  for (size_t i = 0; i < kFrameCounterBytes; ++i) {
    nonce[nonce.size() - 1 - i] = static_cast<uint8_t>(counter >> (8 * i));
  }
  return nonce;
}

bool Seal(const FrameKey& key,
          const Nonce& nonce,
          std::span<const uint8_t> aad,
          std::span<const uint8_t> plain,
          std::span<uint8_t> out,
          Tag& tag) {
  CipherContext context(EVP_CIPHER_CTX_new());
  int length = 0;
  std::array<uint8_t, 16> tail{};
  if (!context || out.size() < plain.size() ||
      EVP_EncryptInit_ex(context.get(), EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1 ||
      EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
    return false;
  }
  if (!aad.empty() &&
      EVP_EncryptUpdate(context.get(), nullptr, &length, aad.data(), static_cast<int>(aad.size())) != 1) {
    return false;
  }
  if (!plain.empty() &&
      EVP_EncryptUpdate(context.get(), out.data(), &length, plain.data(), static_cast<int>(plain.size())) != 1) {
    return false;
  }
  return EVP_EncryptFinal_ex(context.get(), tail.data(), &length) == 1 &&
         EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()), tag.data()) == 1;
}

bool Open(const FrameKey& key,
          const Nonce& nonce,
          std::span<const uint8_t> aad,
          std::span<const uint8_t> sealed,
          const Tag& tag,
          std::span<uint8_t> out) {
  CipherContext context(EVP_CIPHER_CTX_new());
  int length = 0;
  Tag expected = tag;
  std::array<uint8_t, 16> tail{};
  if (!context || out.size() < sealed.size() ||
      EVP_DecryptInit_ex(context.get(), EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()), nullptr) != 1 ||
      EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1) {
    return false;
  }
  if (!aad.empty() &&
      EVP_DecryptUpdate(context.get(), nullptr, &length, aad.data(), static_cast<int>(aad.size())) != 1) {
    return false;
  }
  if (!sealed.empty() &&
      EVP_DecryptUpdate(context.get(), out.data(), &length, sealed.data(), static_cast<int>(sealed.size())) != 1) {
    return false;
  }
  return EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(expected.size()),
                             expected.data()) == 1 &&
         EVP_DecryptFinal_ex(context.get(), tail.data(), &length) == 1;
}

}  // namespace

size_t FrameClearBytes(bool audio, std::span<const uint8_t> data) {
  if (data.empty()) {
    return 0;
  }
  if (audio) {
    return 1;
  }
  bool keyframe = (data.front() & 0x01) == 0;
  return std::min<size_t>(keyframe ? 10 : 3, data.size());
}

bool EncryptFrame(bool audio,
                  const FrameKey& key,
                  uint8_t key_id,
                  uint64_t counter,
                  std::span<const uint8_t> frame,
                  std::span<uint8_t> out,
                  size_t* written) {
  if (out.size() < frame.size() + kFrameOverhead) {
    return false;
  }
  size_t clear = FrameClearBytes(audio, frame);
  Nonce nonce = CounterNonce(counter);
  std::span<const uint8_t> header = frame.first(clear);
  std::span<const uint8_t> body = frame.subspan(clear);
  Copy(header, out);
  Tag tag{};
  if (!Seal(key, nonce, header, body, out.subspan(clear, body.size()), tag)) {
    return false;
  }
  std::span<uint8_t> trailer = out.subspan(frame.size(), kFrameOverhead);
  Copy(tag, trailer);
  Copy(std::span<const uint8_t>(nonce).last(kFrameCounterBytes), trailer.subspan(kFrameTagBytes));
  trailer[kFrameOverhead - 1] = key_id;
  *written = frame.size() + kFrameOverhead;
  return true;
}

bool FrameKeyId(std::span<const uint8_t> encrypted, uint8_t* key_id) {
  if (encrypted.size() < kFrameOverhead + 1) {
    return false;
  }
  *key_id = encrypted.back();
  return true;
}

bool FrameCounter(std::span<const uint8_t> encrypted, uint64_t* counter) {
  if (encrypted.size() < kFrameOverhead + 1) {
    return false;
  }
  uint64_t value = 0;
  for (uint8_t byte : encrypted.last(kFrameTrailerBytes).first(kFrameCounterBytes)) {
    value = (value << 8) | byte;
  }
  *counter = value;
  return true;
}

bool FrameReplayWindow::Fresh(uint64_t counter) const {
  if (!started_ || counter > top_) {
    return true;
  }
  uint64_t age = top_ - counter;
  return age < kFrameReplayWindow && !seen_.test(static_cast<size_t>(age));
}

void FrameReplayWindow::Mark(uint64_t counter) {
  if (!started_) {
    started_ = true;
    top_ = counter;
    seen_.reset();
    seen_.set(0);
  } else if (counter > top_) {
    uint64_t shift = counter - top_;
    if (shift >= kFrameReplayWindow) {
      seen_.reset();
    } else {
      seen_ <<= static_cast<size_t>(shift);
    }
    seen_.set(0);
    top_ = counter;
  } else if (top_ - counter < kFrameReplayWindow) {
    seen_.set(static_cast<size_t>(top_ - counter));
  }
}

bool DecryptFrame(bool audio,
                  const FrameKey& key,
                  std::span<const uint8_t> encrypted,
                  std::span<uint8_t> out,
                  size_t* written) {
  if (encrypted.size() < kFrameOverhead + 1) {
    return false;
  }
  size_t body_size = encrypted.size() - kFrameOverhead;
  if (out.size() < body_size) {
    return false;
  }
  std::span<const uint8_t> body = encrypted.first(body_size);
  std::span<const uint8_t> trailer = encrypted.subspan(body_size);
  size_t clear = FrameClearBytes(audio, body);
  Nonce nonce{};
  Copy(trailer.subspan(kFrameTagBytes, kFrameCounterBytes), std::span<uint8_t>(nonce).last(kFrameCounterBytes));
  Tag tag{};
  Copy(trailer.first(kFrameTagBytes), tag);
  std::vector<uint8_t> plain(body_size);
  std::span<uint8_t> opened(plain);
  Copy(body.first(clear), opened);
  if (!Open(key, nonce, body.first(clear), body.subspan(clear), tag, opened.subspan(clear))) {
    return false;
  }
  Copy(opened, out);
  *written = body_size;
  return true;
}

void FrameSendKey::Set(const FrameKey& key, uint8_t key_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  key_id_ = key_id;
  if (ready_ && key_ == key) {
    return;
  }
  key_ = key;
  counter_ = 0;
  ready_ = true;
}

bool FrameSendKey::Encrypt(bool audio, std::span<const uint8_t> frame, std::span<uint8_t> out, size_t* written) {
  FrameKey key;
  uint8_t key_id = 0;
  uint64_t counter = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {
      return false;
    }
    ++counter_;
    key = key_;
    key_id = key_id_;
    counter = counter_;
  }
  return EncryptFrame(audio, key, key_id, counter, frame, out, written);
}

void FrameKeyring::Set(uint8_t key_id, const FrameKey& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = keys_.find(key_id);
  if (found != keys_.end() && found->second.key == key) {
    return;
  }
  std::erase(order_, key_id);
  order_.push_back(key_id);
  keys_[key_id] = Entry{key, FrameReplayWindow()};
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

bool FrameKeyring::Decrypt(bool audio, std::span<const uint8_t> encrypted, std::span<uint8_t> out, size_t* written) {
  uint8_t key_id = 0;
  uint64_t counter = 0;
  if (!FrameKeyId(encrypted, &key_id) || !FrameCounter(encrypted, &counter)) {
    return false;
  }
  FrameKey key;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = keys_.find(key_id);
    if (found == keys_.end() || !found->second.window.Fresh(counter)) {
      return false;
    }
    key = found->second.key;
  }
  size_t opened = 0;
  if (!DecryptFrame(audio, key, encrypted, out, &opened)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = keys_.find(key_id);
  if (found == keys_.end() || found->second.key != key || !found->second.window.Fresh(counter)) {
    return false;
  }
  found->second.window.Mark(counter);
  *written = opened;
  return true;
}

}  // namespace fm
