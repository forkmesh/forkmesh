#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "fm/fm_frame_crypto.h"

namespace {

std::vector<uint8_t> FromHex(const std::string& text) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < text.size(); i += 2) {
    out.push_back(static_cast<uint8_t>(std::strtoul(text.substr(i, 2).c_str(), nullptr, 16)));
  }
  return out;
}

std::string ToHex(const uint8_t* data, size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < size; ++i) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 15]);
  }
  return out;
}

fm::FrameKey KeyFrom(const std::string& hex) {
  fm::FrameKey key{};
  std::vector<uint8_t> raw = FromHex(hex);
  std::memcpy(key.data(), raw.data(), std::min(raw.size(), key.size()));
  return key;
}

struct Step {
  std::string kind;
  std::string first;
  std::string second;
};

Step Parse(const std::string& token) {
  Step step;
  size_t one = token.find(':');
  step.kind = token.substr(0, one);
  if (one == std::string::npos) {
    return step;
  }
  size_t two = token.find(':', one + 1);
  step.first = token.substr(one + 1, two == std::string::npos ? std::string::npos : two - one - 1);
  if (two != std::string::npos) {
    step.second = token.substr(two + 1);
  }
  return step;
}

int Send(int argc, char** argv) {
  fm::FrameSendKey sender;
  for (int i = 2; i < argc; ++i) {
    Step step = Parse(argv[i]);
    if (step.kind == "key") {
      sender.Set(KeyFrom(step.first), static_cast<uint8_t>(std::atoi(step.second.c_str())));
      continue;
    }
    std::vector<uint8_t> frame = FromHex(step.first);
    std::vector<uint8_t> out(frame.size() + fm::kFrameOverhead);
    size_t written = 0;
    if (!sender.Encrypt(step.kind == "audio", frame, out, &written)) {
      std::puts("fail");
      continue;
    }
    std::puts(ToHex(out.data(), written).c_str());
  }
  return 0;
}

int Receive(int argc, char** argv) {
  fm::FrameKeyring ring;
  for (int i = 2; i < argc; ++i) {
    Step step = Parse(argv[i]);
    if (step.kind == "key") {
      ring.Set(static_cast<uint8_t>(std::atoi(step.second.c_str())), KeyFrom(step.first));
      continue;
    }
    std::vector<uint8_t> encrypted = FromHex(step.first);
    std::vector<uint8_t> out(encrypted.size());
    size_t written = 0;
    if (!ring.Decrypt(step.kind == "audio", encrypted, out, &written)) {
      std::puts("fail");
      continue;
    }
    std::puts(ToHex(out.data(), written).c_str());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "send") {
    return Send(argc, argv);
  }
  if (argc >= 2 && std::string(argv[1]) == "receive") {
    return Receive(argc, argv);
  }
  if (argc < 5) {
    std::fprintf(stderr,
                 "usage: encrypt audio|video KEY KEYID COUNTER FRAME | decrypt audio|video KEY FRAME | "
                 "send key:KEY:KEYID|audio:FRAME|video:FRAME... | receive key:KEY:KEYID|audio:FRAME|video:FRAME...\n");
    return 2;
  }
  std::string mode = argv[1];
  bool audio = std::string(argv[2]) == "audio";
  fm::FrameKey key = KeyFrom(argv[3]);
  if (mode == "encrypt" && argc == 7) {
    std::vector<uint8_t> frame = FromHex(argv[6]);
    std::vector<uint8_t> out(frame.size() + fm::kFrameOverhead);
    size_t written = 0;
    if (!fm::EncryptFrame(audio, key, static_cast<uint8_t>(std::atoi(argv[4])),
                          std::strtoull(argv[5], nullptr, 10), frame, out, &written)) {
      std::puts("fail");
      return 1;
    }
    std::puts(ToHex(out.data(), written).c_str());
    return 0;
  }
  if (mode == "decrypt") {
    std::vector<uint8_t> encrypted = FromHex(argv[4]);
    fm::FrameKeyring ring;
    uint8_t key_id = 0;
    if (!fm::FrameKeyId(encrypted, &key_id)) {
      std::puts("fail");
      return 1;
    }
    ring.Set(key_id, key);
    std::vector<uint8_t> out(encrypted.size());
    size_t written = 0;
    if (!ring.Decrypt(audio, encrypted, out, &written)) {
      std::puts("fail");
      return 1;
    }
    std::puts(ToHex(out.data(), written).c_str());
    return 0;
  }
  return 2;
}
