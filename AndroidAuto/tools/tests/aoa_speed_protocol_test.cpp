#include "../aoa_speed_protocol.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void Require(bool value, const char* message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
}  // namespace

int main() {
  using namespace aoa_speed;
  std::vector<unsigned char> payload(kPayloadSize);
  for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = i & 0xff;
  auto data = MakeFrame(Type::kData, 7, payload);
  EncodeHeader(data.data(), Type::kData, payload.size(), 7, 0x123456789abcdef0);
  const auto decoded = DecodeHeader(data.data());
  Require(decoded.run == 7 && decoded.sequence == 0x123456789abcdef0,
          "64-bit little-endian header round trip");
  std::vector<unsigned char> report_bytes(32);
  Put64(report_bytes.data(), 5000000000ULL);
  Put64(report_bytes.data() + 8, 100000000000ULL);
  const auto report = DecodeReport(report_bytes.data(), report_bytes.size());
  Require(report.MegabytesPerSecond() == 50.0, "MB/s units or 64-bit counters");
  const auto terminal = MakeFrame(Type::kReport, 7, report_bytes);
  auto combined = data;
  combined.insert(combined.end(), terminal.begin(), terminal.end());
  for (const std::size_t fragment : {1, 7, 31, 32, 511, 16384, 32768}) {
    FrameParser parser;
    int frames = 0;
    for (std::size_t offset = 0; offset < combined.size(); offset += fragment) {
      parser.Feed(combined.data() + offset, std::min(fragment, combined.size() - offset),
                  [&](const Header& header, const unsigned char* bytes) {
        if (frames == 0) {
          Require(header.type == Type::kData && header.length == payload.size(), "data header");
          Require(std::equal(payload.begin(), payload.end(), bytes), "payload changed");
        } else {
          Require(header.type == Type::kReport && DecodeReport(bytes, header.length).bytes ==
                      5000000000ULL, "terminal report");
        }
        ++frames;
      });
    }
    Require(frames == 2 && parser.Empty(), "fragmented/coalesced stream parsing");
  }
  FrameParser parser;
  int frames = 0;
  const auto empty = MakeFrame(Type::kStart, 1, {});
  parser.Feed(empty.data(), empty.size(), [&](const Header& header, const unsigned char*) {
    Require(header.type == Type::kStart && header.length == 0, "empty control frame");
    ++frames;
  });
  Require(frames == 1 && parser.Empty(), "empty frame parser stalled");
  for (const int field : {0, 4, 8, 24}) {
    auto corrupt = data;
    corrupt[field] = 0xff;
    bool rejected = false;
    try { DecodeHeader(corrupt.data()); } catch (const std::runtime_error&) { rejected = true; }
    if (field == 8) {
      Put32(corrupt.data() + 8, kPayloadSize + 1);
      try { DecodeHeader(corrupt.data()); } catch (const std::runtime_error&) { rejected = true; }
    }
    Require(rejected, "malformed header accepted");
  }
  std::cout << "AOA speed protocol tests passed\n";
}
