#ifndef ANDROID_AUTO_TOOLS_AOA_SPEED_PROTOCOL_H_
#define ANDROID_AUTO_TOOLS_AOA_SPEED_PROTOCOL_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <vector>

namespace aoa_speed {

constexpr std::uint32_t kMagic = 0x53504f41;
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kFrameSize = 16384;
constexpr std::size_t kPayloadSize = kFrameSize - kHeaderSize;
enum class Type : std::uint16_t {
  kHello = 1, kCommand = 2, kReady = 3, kData = 4, kReport = 5, kEnd = 6,
  kStart = 7, kFinish = 8
};
enum class Direction : std::uint32_t { kPhoneToBoard = 1, kBoardToPhone = 2 };

inline void Put32(unsigned char* bytes, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) bytes[i] = (value >> (8 * i)) & 0xff;
}
inline std::uint32_t Get32(const unsigned char* bytes) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= std::uint32_t(bytes[i]) << (8 * i);
  return value;
}
inline void Put64(unsigned char* bytes, std::uint64_t value) {
  Put32(bytes, value & 0xffffffff);
  Put32(bytes + 4, value >> 32);
}
inline std::uint64_t Get64(const unsigned char* bytes) {
  return Get32(bytes) | (std::uint64_t(Get32(bytes + 4)) << 32);
}

struct Header {
  Type type;
  std::uint32_t length;
  std::uint32_t run;
  std::uint64_t sequence;
};

inline void EncodeHeader(unsigned char* bytes, Type type, std::uint32_t length,
                         std::uint32_t run, std::uint64_t sequence = 0) {
  std::memset(bytes, 0, kHeaderSize);
  Put32(bytes, kMagic);
  Put32(bytes + 4, kVersion | (std::uint32_t(type) << 16));
  Put32(bytes + 8, length);
  Put32(bytes + 12, run);
  Put64(bytes + 16, sequence);
}

inline Header DecodeHeader(const unsigned char* bytes) {
  const auto version_type = Get32(bytes + 4);
  if (Get32(bytes) != kMagic || (version_type & 0xffff) != kVersion ||
      Get32(bytes + 8) > kPayloadSize || Get64(bytes + 24) != 0 ||
      (version_type >> 16) < 1 || (version_type >> 16) > 8) {
    throw std::runtime_error("invalid AOA speed protocol header");
  }
  return {static_cast<Type>(version_type >> 16), Get32(bytes + 8),
          Get32(bytes + 12), Get64(bytes + 16)};
}

inline std::vector<unsigned char> MakeFrame(Type type, std::uint32_t run,
                                          const std::vector<unsigned char>& payload) {
  if (payload.size() > kPayloadSize) throw std::runtime_error("payload too large");
  std::vector<unsigned char> bytes(kHeaderSize + payload.size());
  EncodeHeader(bytes.data(), type, payload.size(), run);
  std::copy(payload.begin(), payload.end(), bytes.begin() + kHeaderSize);
  return bytes;
}

// Parses a byte stream; USB transfer boundaries are not protocol boundaries.
class FrameParser {
 public:
  using Handler = std::function<void(const Header&, const unsigned char*)>;
  void Feed(const unsigned char* bytes, std::size_t length, const Handler& handler) {
    while (length > 0) {
      const auto needed = expected_ - pending_.size();
      const auto count = std::min(length, needed);
      pending_.insert(pending_.end(), bytes, bytes + count);
      bytes += count;
      length -= count;
      if (pending_.size() != expected_) continue;
      if (expected_ == kHeaderSize) {
        const auto header = DecodeHeader(pending_.data());
        expected_ = kHeaderSize + header.length;
        if (header.length != 0) continue;
      }
      const auto header = DecodeHeader(pending_.data());
      handler(header, pending_.data() + kHeaderSize);
      pending_.clear();
      expected_ = kHeaderSize;
    }
  }
  bool Empty() const { return pending_.empty(); }

 private:
  std::vector<unsigned char> pending_;
  std::size_t expected_ = kHeaderSize;
};

struct Report {
  std::uint64_t bytes = 0;
  std::uint64_t duration_ns = 0;
  std::uint64_t frames = 0;
  std::uint64_t errors = 0;
  double MegabytesPerSecond() const {
    return duration_ns == 0 ? 0 : double(bytes) * 1000.0 / double(duration_ns);
  }
};

inline Report DecodeReport(const unsigned char* bytes, std::size_t length) {
  if (length != 32) throw std::runtime_error("invalid report length");
  return {Get64(bytes), Get64(bytes + 8), Get64(bytes + 16), Get64(bytes + 24)};
}

}  // namespace aoa_speed

#endif  // ANDROID_AUTO_TOOLS_AOA_SPEED_PROTOCOL_H_
