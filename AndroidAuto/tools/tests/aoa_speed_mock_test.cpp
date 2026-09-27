// Exercise the real async pipeline with a synthetic peer and reordered callbacks.
#define main AoaSpeedToolMain
#include "../aoa_speed_test.cpp"
#undef main

#include <iostream>

using aoa_speed::Put64;

namespace {
struct FakePeer {
  std::vector<libusb_transfer*> active;
  std::deque<unsigned char> control;
  std::vector<unsigned char> incoming;
  std::size_t position = 0;
  FrameParser outgoing;
  Direction direction = Direction::kPhoneToBoard;
  std::uint32_t run = 0;
  Report received;
  bool corrupt = false;
  bool wrong_sequence = false;
  bool wrong_count = false;
  bool fail_transfer = false;
  int fragments = 0;

  void QueueControl(Type type, const std::vector<unsigned char>& payload = {}) {
    const auto frame = MakeFrame(type, run, payload);
    control.insert(control.end(), frame.begin(), frame.end());
  }
  void QueueIncoming() {
    incoming.clear();
    position = 0;
    constexpr int kFrames = 17;
    std::vector<unsigned char> payload(kPayloadSize);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = i & 0xff;
    for (int i = 0; i < kFrames; ++i) {
      auto frame = MakeFrame(Type::kData, run, payload);
      EncodeHeader(frame.data(), Type::kData, kPayloadSize, run,
                    wrong_sequence && i == 1 ? 9 : i);
      if (corrupt && i == 0) frame[kHeaderSize + 3] ^= 1;
      incoming.insert(incoming.end(), frame.begin(), frame.end());
    }
    std::vector<unsigned char> report(32);
    Put64(report.data(), kFrames * kPayloadSize + (wrong_count ? 1 : 0));
    Put64(report.data() + 8, 1000000000ULL);
    Put64(report.data() + 16, kFrames);
    auto frame = MakeFrame(Type::kReport, run, report);
    incoming.insert(incoming.end(), frame.begin(), frame.end());
  }
  void Consume(const Header& header, const unsigned char* payload) {
    if (header.type == Type::kCommand) {
      run = header.run;
      direction = static_cast<Direction>(Get32(payload));
      received = {};
      QueueControl(Type::kReady);
    } else if (header.type == Type::kStart) {
      if (direction == Direction::kPhoneToBoard) QueueIncoming();
    } else if (header.type == Type::kData) {
      if (header.run != run || header.sequence != received.frames || header.length != kPayloadSize) {
        throw std::runtime_error("mock receiver: stream reordered");
      }
      for (std::size_t i = 0; i < header.length; ++i) {
        if (payload[i] != (i & 0xff)) ++received.errors;
      }
      received.bytes += header.length;
      ++received.frames;
    } else if (header.type == Type::kEnd) {
      std::vector<unsigned char> report(32);
      Put64(report.data(), received.bytes + (wrong_count ? 1 : 0));
      Put64(report.data() + 8, 1000000000ULL);
      Put64(report.data() + 16, received.frames);
      Put64(report.data() + 24, received.errors);
      QueueControl(Type::kReport, report);
    } else throw std::runtime_error("unexpected host frame");
  }
} fake;

void Require(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

Report Phase(Direction direction, bool verify = true) {
  const Endpoints endpoints{0, 0, 0x81, 0x01};
  UsbStream stream(nullptr, endpoints);
  Options options;
  options.queue = 4;
  return RunPhase(nullptr, nullptr, endpoints, &stream, options, direction, 1, 1, verify);
}

void ExpectFailure(Direction direction, const char* message) {
  bool failed = false;
  try { Phase(direction); } catch (const std::runtime_error&) { failed = true; }
  Require(failed && fake.active.empty(), message);
}
}  // namespace

extern "C" int LIBUSB_CALL libusb_submit_transfer(libusb_transfer* transfer) {
  fake.active.push_back(transfer);
  return 0;
}

extern "C" int LIBUSB_CALL libusb_cancel_transfer(libusb_transfer* transfer) {
  const auto it = std::find(fake.active.begin(), fake.active.end(), transfer);
  if (it == fake.active.end()) return LIBUSB_ERROR_NOT_FOUND;
  fake.active.erase(it);
  transfer->actual_length = 0;
  transfer->status = LIBUSB_TRANSFER_CANCELLED;
  transfer->callback(transfer);
  return 0;
}

extern "C" int LIBUSB_CALL libusb_handle_events_timeout_completed(
    libusb_context*, timeval*, int*) {
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::vector<libusb_transfer*> completed;
  for (auto it = fake.active.begin(); it != fake.active.end();) {
    auto* transfer = *it;
    if (fake.fail_transfer) {
      fake.fail_transfer = false;
      transfer->status = LIBUSB_TRANSFER_TIMED_OUT;
      transfer->actual_length = 0;
    } else if (transfer->endpoint & LIBUSB_ENDPOINT_IN) {
      if (fake.position == fake.incoming.size()) { ++it; continue; }
      const int size = (++fake.fragments % 3 == 0) ? 513 : transfer->length;
      const auto count = std::min<std::size_t>(size, fake.incoming.size() - fake.position);
      std::copy_n(fake.incoming.data() + fake.position, count, transfer->buffer);
      fake.position += count;
      transfer->actual_length = count;
      transfer->status = LIBUSB_TRANSFER_COMPLETED;
    } else {
      fake.outgoing.Feed(transfer->buffer, transfer->length,
                         [](const Header& header, const unsigned char* bytes) {
        fake.Consume(header, bytes);
      });
      transfer->actual_length = transfer->length;
      transfer->status = LIBUSB_TRANSFER_COMPLETED;
    }
    completed.push_back(transfer);
    it = fake.active.erase(it);
  }
  for (auto it = completed.rbegin(); it != completed.rend(); ++it) (*it)->callback(*it);
  return 0;
}

extern "C" int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle*, unsigned char endpoint,
    unsigned char* bytes, int length, int* transferred, unsigned int) {
  if (endpoint & LIBUSB_ENDPOINT_IN) {
    const int count = std::min<int>(7, std::min<int>(length, fake.control.size()));
    for (int i = 0; i < count; ++i) { bytes[i] = fake.control.front(); fake.control.pop_front(); }
    *transferred = count;
    return count ? 0 : LIBUSB_ERROR_TIMEOUT;
  }
  fake.outgoing.Feed(bytes, length, [](const Header& header, const unsigned char* payload) {
    fake.Consume(header, payload);
  });
  *transferred = length;
  return 0;
}

int main() {
  const auto rx = Phase(Direction::kPhoneToBoard);
  Require(rx.frames == 17 && rx.bytes == 17 * kPayloadSize && rx.errors == 0 &&
              fake.active.empty(), "RX fragmented stream/cancel tail failed");
  fake = {};
  const auto tx = Phase(Direction::kBoardToPhone);
  Require(tx.frames > 4 && tx.errors == 0 && tx.bytes == fake.received.bytes &&
              tx.duration_ns == 1000000000ULL, "TX receiver report/order failed");
  fake = {};
  fake.corrupt = true;
  Require(Phase(Direction::kPhoneToBoard).errors == 1, "integrity check missed corrupt byte");
  fake = {};
  fake.wrong_sequence = true;
  ExpectFailure(Direction::kPhoneToBoard, "wrong sequence was accepted or transfers leaked");
  fake = {};
  fake.wrong_count = true;
  ExpectFailure(Direction::kPhoneToBoard, "wrong peer byte count accepted");
  fake = {};
  fake.fail_transfer = true;
  ExpectFailure(Direction::kPhoneToBoard, "transfer timeout accepted or buffers freed early");
  fake = {};
  fake.wrong_count = true;
  ExpectFailure(Direction::kBoardToPhone, "TX sender count substituted for receiver count");
  std::cout << "AOA async pipeline integration tests passed\n";
}
