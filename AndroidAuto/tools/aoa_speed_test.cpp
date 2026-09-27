#include "aoa_speed_protocol.h"

#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <libusb-1.0/libusb.h>

namespace {

using aoa_speed::DecodeReport;
using aoa_speed::Direction;
using aoa_speed::EncodeHeader;
using aoa_speed::FrameParser;
using aoa_speed::Get32;
using aoa_speed::Header;
using aoa_speed::kFrameSize;
using aoa_speed::kHeaderSize;
using aoa_speed::kPayloadSize;
using aoa_speed::MakeFrame;
using aoa_speed::Put32;
using aoa_speed::Report;
using aoa_speed::Type;
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t interrupted = 0;
void OnSignal(int) { interrupted = 1; }

void CheckUsb(int result, const char* operation) {
  if (result < 0) throw std::runtime_error(std::string(operation) + ": " +
                                         libusb_error_name(result));
}
void CheckInterrupt() {
  if (interrupted) throw std::runtime_error("test interrupted");
}

using Device = std::shared_ptr<libusb_device>;
using Handle = std::unique_ptr<libusb_device_handle, decltype(&libusb_close)>;

struct DeviceList {
  libusb_device** devices = nullptr;
  ~DeviceList() { if (devices) libusb_free_device_list(devices, 1); }
};

struct Options {
  std::string controller = "31260000.dwc3";
  std::string port;
  int seconds = 30;
  int warmup = 3;
  int rounds = 3;
  int queue = 32;
  double minimum = 50;
};

int ParseInt(const std::string& text, int low, int high) {
  std::size_t end = 0;
  const auto value = std::stol(text, &end);
  if (end != text.size() || value < low || value > high) {
    throw std::runtime_error("invalid numeric option: " + text);
  }
  return static_cast<int>(value);
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string name = argv[i];
    if (name == "--help") {
      std::printf("AOA payload throughput test (requires AOA Speed Test APK).\n"
                  "Options: --controller NAME --port BUS-PORT[.PORT] "
                  "--seconds 30 --warmup 3 --rounds 3 --queue 32 --min-mb-s 50\n"
                  "Stop AndroidAuto first; this tool switches the phone to AOA.\n"
                  "MB/s means 1,000,000 payload bytes/s; primary criterion is "
                  "phone->board in every measured round.\n");
      std::exit(0);
    }
    if (++i >= argc) throw std::runtime_error("missing value for " + name);
    const std::string value = argv[i];
    if (name == "--controller") options.controller = value;
    else if (name == "--port") options.port = value;
    else if (name == "--seconds") options.seconds = ParseInt(value, 1, 3600);
    else if (name == "--warmup") options.warmup = ParseInt(value, 0, 60);
    else if (name == "--rounds") options.rounds = ParseInt(value, 1, 20);
    else if (name == "--queue") options.queue = ParseInt(value, 1, 128);
    else if (name == "--min-mb-s") {
      std::size_t end = 0;
      options.minimum = std::stod(value, &end);
      if (end != value.size() || !std::isfinite(options.minimum) ||
          options.minimum <= 0) throw std::runtime_error("invalid minimum rate");
    } else throw std::runtime_error("unknown option " + name);
  }
  return options;
}

bool MatchesController(libusb_device* device, const std::string& controller) {
  std::error_code error;
  const auto path = std::filesystem::canonical(
      "/sys/bus/usb/devices/usb" + std::to_string(libusb_get_bus_number(device)), error);
  if (error) return false;
  for (const auto& component : path) if (component == controller) return true;
  return false;
}

std::string PortPath(libusb_device* device) {
  std::array<std::uint8_t, 8> ports{};
  const int count = libusb_get_port_numbers(device, ports.data(), ports.size());
  CheckUsb(count, "port path");
  std::string path = std::to_string(libusb_get_bus_number(device)) + "-";
  for (int i = 0; i < count; ++i) {
    if (i) path += ".";
    path += std::to_string(ports[i]);
  }
  return path;
}

bool IsAccessory(const libusb_device_descriptor& descriptor) {
  return descriptor.idVendor == 0x18d1 &&
         (descriptor.idProduct == 0x2d00 || descriptor.idProduct == 0x2d01 ||
          descriptor.idProduct == 0x2d04 || descriptor.idProduct == 0x2d05);
}

Device SelectPhone(libusb_context* context, const Options& options) {
  DeviceList list;
  const auto count = libusb_get_device_list(context, &list.devices);
  CheckUsb(count, "enumerate");
  std::vector<Device> candidates;
  for (ssize_t i = 0; i < count; ++i) {
    auto* device = list.devices[i];
    if (libusb_get_port_number(device) == 0 ||
        !MatchesController(device, options.controller)) continue;
    const auto port = PortPath(device);
    if (!options.port.empty() && port != options.port) continue;
    libusb_device_descriptor descriptor{};
    CheckUsb(libusb_get_device_descriptor(device, &descriptor), "descriptor");
    std::printf("Device port=%s address=%u VID=%04x PID=%04x\n", port.c_str(),
                libusb_get_device_address(device), descriptor.idVendor,
                descriptor.idProduct);
    // Descriptor-only filtering; never send vendor requests to pure peripherals.
    libusb_config_descriptor* raw = nullptr;
    const int result = libusb_get_config_descriptor(device, 0, &raw);
    std::unique_ptr<libusb_config_descriptor, decltype(&libusb_free_config_descriptor)>
        config(raw, libusb_free_config_descriptor);
    bool phone_interface = false;
    if (result == 0) {
      for (int j = 0; j < config->bNumInterfaces; ++j) {
        for (int k = 0; k < config->interface[j].num_altsetting; ++k) {
          const auto& alt = config->interface[j].altsetting[k];
          phone_interface |= (alt.bInterfaceClass == 0xff && alt.bInterfaceSubClass == 0x42) ||
                             alt.bInterfaceClass == 0x06;
        }
      }
    }
    if (IsAccessory(descriptor) || phone_interface || !options.port.empty()) {
      candidates.emplace_back(libusb_ref_device(device), libusb_unref_device);
    }
  }
  if (candidates.size() != 1) {
    throw std::runtime_error("need exactly one phone; use --port BUS-PORT to select "
                             "the intended phone explicitly");
  }
  return candidates.front();
}

Handle OpenDevice(libusb_device* device) {
  libusb_device_handle* raw = nullptr;
  CheckUsb(libusb_open(device, &raw), "open device (check udev permissions)");
  return Handle(raw, libusb_close);
}

Device EnterAccessory(libusb_context* context, Device phone, const Options& options) {
  const auto port = PortPath(phone.get());
  auto handle = OpenDevice(phone.get());
  unsigned char version[2]{};
  const int result = libusb_control_transfer(handle.get(), 0xc0, 51, 0, 0,
                                            version, 2, 1000);
  CheckUsb(result, "AOA GET_PROTOCOL");
  if (result != 2 || (version[0] | (int(version[1]) << 8)) == 0) {
    throw std::runtime_error("phone did not return a supported AOA version");
  }
  std::printf("AOA version=%d\n", version[0] | (int(version[1]) << 8));
  libusb_device_descriptor descriptor{};
  CheckUsb(libusb_get_device_descriptor(phone.get(), &descriptor), "descriptor");
  if (IsAccessory(descriptor)) {
    unsigned char model[256]{};
    const int length = libusb_get_string_descriptor_ascii(
        handle.get(), descriptor.iProduct, model, sizeof(model));
    // Android's USB product string need not equal the AOA model string; the
    // actual test identity is established exclusively by the HELLO handshake.
    if (length > 0) std::printf("Already in AOA mode, product=%.*s\n", length, model);
    std::printf("If this mode was started by aoa_probe/AndroidAuto, unplug and "
                "reconnect before using the test APK.\n");
    return phone;
  }
  const char* strings[] = {"X9Test", "AOASpeedTest", "USB payload throughput test",
                           "1.0", "", "x9-aoa-speed"};
  for (int i = 0; i < 6; ++i) {
    std::string value = strings[i];
    const auto length = value.size() + 1;
    const int written = libusb_control_transfer(
        handle.get(), 0x40, 52, 0, i,
        reinterpret_cast<unsigned char*>(value.data()), length, 1000);
    CheckUsb(written, "AOA identity");
    if (written != int(length)) throw std::runtime_error("short AOA identity write");
  }
  CheckUsb(libusb_control_transfer(handle.get(), 0x40, 53, 0, 0, nullptr, 0, 1000),
           "AOA START");
  handle.reset();
  phone.reset();
  std::printf("Waiting for AOA re-enumeration at %s...\n", port.c_str());
  const auto deadline = Clock::now() + std::chrono::seconds(20);
  while (Clock::now() < deadline) {
    CheckInterrupt();
    DeviceList list;
    const auto count = libusb_get_device_list(context, &list.devices);
    CheckUsb(count, "re-enumerate");
    for (ssize_t i = 0; i < count; ++i) {
      auto* device = list.devices[i];
      if (!MatchesController(device, options.controller) || PortPath(device) != port) continue;
      CheckUsb(libusb_get_device_descriptor(device, &descriptor), "descriptor");
      if (IsAccessory(descriptor)) return Device(libusb_ref_device(device), libusb_unref_device);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  throw std::runtime_error("AOA re-enumeration timed out");
}

struct Endpoints {
  int interface = -1;
  int alternate = 0;
  unsigned char in = 0;
  unsigned char out = 0;
};

Endpoints FindEndpoints(libusb_device* device) {
  libusb_config_descriptor* raw = nullptr;
  CheckUsb(libusb_get_active_config_descriptor(device, &raw), "active configuration");
  std::unique_ptr<libusb_config_descriptor, decltype(&libusb_free_config_descriptor)>
      config(raw, libusb_free_config_descriptor);
  for (int i = 0; i < config->bNumInterfaces; ++i) {
    for (int j = 0; j < config->interface[i].num_altsetting; ++j) {
      const auto& alt = config->interface[i].altsetting[j];
      // AOA interface is vendor class, subclass/protocol zero. Exclude ADB.
      if (alt.bInterfaceClass != 0xff || alt.bInterfaceSubClass != 0 ||
          alt.bInterfaceProtocol != 0) continue;
      Endpoints result;
      result.interface = alt.bInterfaceNumber;
      result.alternate = alt.bAlternateSetting;
      for (int k = 0; k < alt.bNumEndpoints; ++k) {
        const auto& ep = alt.endpoint[k];
        if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
        if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) result.in = ep.bEndpointAddress;
        else result.out = ep.bEndpointAddress;
      }
      if (result.in && result.out) return result;
    }
  }
  throw std::runtime_error("AOA data interface not found; refusing to use ADB endpoints");
}

struct Frame {
  Header header;
  std::vector<unsigned char> payload;
};

class UsbStream {
 public:
  UsbStream(libusb_device_handle* handle, Endpoints endpoints)
      : handle_(handle), endpoints_(endpoints) {}
  void Send(Type type, std::uint32_t run, const std::vector<unsigned char>& payload = {}) {
    auto bytes = MakeFrame(type, run, payload);
    int sent = 0;
    CheckUsb(libusb_bulk_transfer(handle_, endpoints_.out, bytes.data(), bytes.size(),
                                  &sent, 5000), "send control frame");
    if (sent != int(bytes.size())) throw std::runtime_error("short control frame write");
  }
  Frame Receive(int timeout_seconds = 10) {
    const auto deadline = Clock::now() + std::chrono::seconds(timeout_seconds);
    while (frames_.empty() && Clock::now() < deadline) {
      CheckInterrupt();
      std::array<unsigned char, kFrameSize> buffer{};
      int received = 0;
      const int result = libusb_bulk_transfer(handle_, endpoints_.in, buffer.data(),
                                               buffer.size(), &received, 1000);
      if (result != LIBUSB_ERROR_TIMEOUT) CheckUsb(result, "receive control frame");
      parser_.Feed(buffer.data(), received, [this](const Header& header, const unsigned char* data) {
        frames_.push_back({header, {data, data + header.length}});
      });
    }
    if (frames_.empty()) throw std::runtime_error("peer timed out; open/authorize AOA Speed Test APK");
    Frame frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
  }
  void Expect(Type type, std::uint32_t run) {
    const auto frame = Receive();
    if (frame.header.type != type || frame.header.run != run || !frame.payload.empty() ||
        !frames_.empty() || !parser_.Empty()) throw std::runtime_error("unexpected peer reply");
  }

 private:
  libusb_device_handle* handle_;
  Endpoints endpoints_;
  FrameParser parser_;
  std::deque<Frame> frames_;
};

class BulkPipeline {
 public:
  BulkPipeline(libusb_context* context, libusb_device_handle* handle, Endpoints endpoints,
               int depth, std::uint32_t run, bool verify, bool receive)
      : context_(context), handle_(handle), endpoints_(endpoints), run_(run),
        verify_(verify), receive_(receive) {
    for (int i = 0; i < depth; ++i) {
      auto slot = std::make_unique<Slot>();
      slot->owner = this;
      slot->transfer = libusb_alloc_transfer(0);
      if (!slot->transfer) throw std::runtime_error("cannot allocate USB transfer");
      for (std::size_t j = 0; j < kPayloadSize; ++j) slot->bytes[kHeaderSize + j] = j & 0xff;
      slots_.push_back(std::move(slot));
    }
  }
  ~BulkPipeline() { CancelAndDrain(); }

  Report Run(int seconds) {
    start_ = last_sample_ = Clock::now();
    const auto deadline = start_ + std::chrono::seconds(seconds);
    for (auto& slot : slots_) Submit(slot.get());
    while (pending_ > 0) {
      CheckInterrupt();
      timeval timeout{0, 100000};
      const int result = libusb_handle_events_timeout_completed(context_, &timeout, nullptr);
      if (result != LIBUSB_ERROR_INTERRUPTED) CheckUsb(result, "handle USB transfers");
      // Process completions in submission order, even if callback order differs.
      for (;;) {
        Slot* next = nullptr;
        for (auto& slot : slots_) {
          if (slot->done && slot->ticket == next_completion_) { next = slot.get(); break; }
        }
        if (!next) break;
        next->done = false;
        ++next_completion_;
        if (next->status != LIBUSB_TRANSFER_COMPLETED) {
          throw std::runtime_error("bulk transfer failed, status=" + std::to_string(next->status));
        }
        if (receive_) {
          parser_.Feed(next->bytes.data(), next->actual, [this](const Header& header,
                                                               const unsigned char* payload) {
            if (header.run != run_ || finished_) throw std::runtime_error("unexpected run/frame");
            if (header.type == Type::kReport) {
              peer_ = DecodeReport(payload, header.length);
              finished_ = true;
            } else if (header.type == Type::kData && header.length == kPayloadSize &&
                       header.sequence == stats_.frames) {
              if (verify_) {
                for (std::size_t j = 0; j < kPayloadSize; ++j) {
                  if (payload[j] != (j & 0xff)) ++stats_.errors;
                }
              }
              stats_.bytes += header.length;
              ++stats_.frames;
              last_data_ = Clock::now();
            } else throw std::runtime_error("invalid data frame or sequence");
          });
        } else {
          if (next->actual != int(kFrameSize)) throw std::runtime_error("short bulk write");
          stats_.bytes += kPayloadSize;
          ++stats_.frames;
          last_data_ = Clock::now();
        }
        if (finished_) break;
        if (receive_ || Clock::now() < deadline) Submit(next);
      }
      PrintProgress();
      if (finished_) break;
    }
    if (receive_) {
      CancelAndDrain();
      if (!finished_ || !parser_.Empty() || peer_.bytes != stats_.bytes ||
          peer_.frames != stats_.frames || peer_.errors != 0) {
        throw std::runtime_error("peer/receiver count mismatch or missing report");
      }
    }
    if (stats_.frames == 0) throw std::runtime_error("no payload received/transmitted");
    stats_.duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(last_data_ - start_).count();
    return stats_;
  }

 private:
  struct Slot {
    BulkPipeline* owner = nullptr;
    libusb_transfer* transfer = nullptr;
    std::array<unsigned char, kFrameSize> bytes{};
    std::uint64_t ticket = 0;
    bool active = false;
    bool done = false;
    libusb_transfer_status status = LIBUSB_TRANSFER_ERROR;
    int actual = 0;
    ~Slot() { if (transfer) libusb_free_transfer(transfer); }
  };
  static void LIBUSB_CALL OnComplete(libusb_transfer* transfer) {
    auto* slot = static_cast<Slot*>(transfer->user_data);
    slot->active = false;
    slot->done = true;
    slot->status = transfer->status;
    slot->actual = transfer->actual_length;
    --slot->owner->pending_;
  }
  void Submit(Slot* slot) {
    slot->ticket = next_submission_++;
    if (!receive_) EncodeHeader(slot->bytes.data(), Type::kData, kPayloadSize, run_, slot->ticket);
    libusb_fill_bulk_transfer(slot->transfer, handle_, receive_ ? endpoints_.in : endpoints_.out,
                              slot->bytes.data(), kFrameSize, OnComplete, slot, 5000);
    CheckUsb(libusb_submit_transfer(slot->transfer), "submit bulk transfer");
    slot->active = true;
    ++pending_;
  }
  void CancelAndDrain() {
    for (auto& slot : slots_) if (slot->active) libusb_cancel_transfer(slot->transfer);
    while (pending_ > 0) {
      timeval timeout{0, 100000};
      // Callbacks must finish before buffers or the handle are destroyed.
      libusb_handle_events_timeout_completed(context_, &timeout, nullptr);
    }
  }
  void PrintProgress() {
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - last_sample_).count();
    if (elapsed < 1) return;
    std::printf("  %.1fs %s %.2f MB/s payload=%llu bytes\n",
                std::chrono::duration<double>(now - start_).count(),
                receive_ ? "receiver" : "USB sender", (stats_.bytes - sample_bytes_) / elapsed / 1e6,
                static_cast<unsigned long long>(stats_.bytes));
    last_sample_ = now;
    sample_bytes_ = stats_.bytes;
  }
  libusb_context* context_;
  libusb_device_handle* handle_;
  Endpoints endpoints_;
  std::uint32_t run_;
  bool verify_;
  bool receive_;
  std::vector<std::unique_ptr<Slot>> slots_;
  int pending_ = 0;
  std::uint64_t next_submission_ = 0;
  std::uint64_t next_completion_ = 0;
  std::uint64_t sample_bytes_ = 0;
  bool finished_ = false;
  FrameParser parser_;
  Report stats_, peer_;
  Clock::time_point start_, last_data_, last_sample_;
};

Report RunPhase(libusb_context* context, libusb_device_handle* handle, Endpoints endpoints,
                UsbStream* stream, const Options& options, Direction direction,
                std::uint32_t run, int seconds, bool verify) {
  std::vector<unsigned char> command(16);
  Put32(command.data(), std::uint32_t(direction));
  Put32(command.data() + 4, seconds * 1000);
  Put32(command.data() + 8, verify ? 1 : 0);
  stream->Send(Type::kCommand, run, command);
  stream->Expect(Type::kReady, run);
  const bool receive = direction == Direction::kPhoneToBoard;
  BulkPipeline pipeline(context, handle, endpoints, options.queue, run, verify, receive);
  stream->Send(Type::kStart, run);
  auto local = pipeline.Run(seconds);
  if (receive) return local;
  stream->Send(Type::kEnd, run);
  const auto frame = stream->Receive();
  if (frame.header.type != Type::kReport || frame.header.run != run) {
    throw std::runtime_error("missing receiver report");
  }
  const auto peer = DecodeReport(frame.payload.data(), frame.payload.size());
  if (peer.bytes != local.bytes || peer.frames != local.frames || peer.errors != 0 ||
      peer.duration_ns == 0) {
    throw std::runtime_error("phone receiver count mismatch or data errors");
  }
  return peer;  // The phone's received bytes/time are authoritative for this direction.
}

int Test(const Options& options) {
  libusb_context* raw_context = nullptr;
  CheckUsb(libusb_init(&raw_context), "libusb_init");
  std::unique_ptr<libusb_context, decltype(&libusb_exit)> context(raw_context, libusb_exit);
  auto phone = EnterAccessory(context.get(), SelectPhone(context.get(), options), options);
  auto handle = OpenDevice(phone.get());
  const int speed = libusb_get_device_speed(phone.get());
  const char* speed_name = speed == LIBUSB_SPEED_HIGH ? "High-Speed (480 Mb/s)" :
                          speed >= LIBUSB_SPEED_SUPER ? "SuperSpeed (>=5 Gb/s)" :
                          speed == LIBUSB_SPEED_FULL ? "Full-Speed (12 Mb/s)" : "low/unknown";
  std::printf("Link=%s port=%s address=%u\n", speed_name, PortPath(phone.get()).c_str(),
              libusb_get_device_address(phone.get()));
  if (speed == LIBUSB_SPEED_HIGH) {
    std::printf("Note: raw line rate is 60 MB/s before overhead; 50 MB/s is not guaranteed.\n");
  }
  const auto endpoints = FindEndpoints(phone.get());
  CheckUsb(libusb_claim_interface(handle.get(), endpoints.interface),
           "claim AOA interface (stop AndroidAuto/other accessory apps first)");
  auto release = [&endpoints](libusb_device_handle* h) { libusb_release_interface(h, endpoints.interface); };
  std::unique_ptr<libusb_device_handle, decltype(release)> claimed(handle.get(), release);
  if (endpoints.alternate != 0) {
    CheckUsb(libusb_set_interface_alt_setting(handle.get(), endpoints.interface, endpoints.alternate),
             "select AOA alternate setting");
  }
  std::printf("AOA interface=%d IN=0x%02x OUT=0x%02x; authorize/open test APK now.\n",
              endpoints.interface, endpoints.in, endpoints.out);
  UsbStream stream(handle.get(), endpoints);
  const auto hello = stream.Receive(90);
  const std::string identity(hello.payload.begin(), hello.payload.end());
  if (hello.header.type != Type::kHello || hello.header.run != 0 || identity != "AOASPEED") {
    throw std::runtime_error("peer is not AOA Speed Test; refusing to send test payload");
  }
  bool passed = true;
  std::uint32_t run = 1;
  std::vector<Report> measured;
  for (const auto direction : {Direction::kPhoneToBoard, Direction::kBoardToPhone}) {
    const char* name = direction == Direction::kPhoneToBoard ? "phone->board" : "board->phone";
    std::printf("\n%s integrity check (2s)...\n", name);
    const auto verified = RunPhase(context.get(), handle.get(), endpoints, &stream,
                                   options, direction, run++, 2, true);
    if (verified.errors) throw std::runtime_error("integrity check failed");
    for (int round = 1; round <= options.rounds; ++round) {
      if (options.warmup) {
        std::printf("%s round %d warmup (%ds)...\n", name, round, options.warmup);
        RunPhase(context.get(), handle.get(), endpoints, &stream, options, direction,
                 run++, options.warmup, false);
      }
      std::printf("%s round %d measured (%ds)...\n", name, round, options.seconds);
      const auto report = RunPhase(context.get(), handle.get(), endpoints, &stream, options,
                                    direction, run++, options.seconds, false);
      const bool okay = report.errors == 0 && report.MegabytesPerSecond() >= options.minimum;
      std::printf("RESULT %s round=%d receiver=%.2f MB/s bytes=%llu duration=%.3fs "
                  "frames=%llu errors=%llu %s\n", name, round, report.MegabytesPerSecond(),
                  static_cast<unsigned long long>(report.bytes), report.duration_ns / 1e9,
                  static_cast<unsigned long long>(report.frames),
                  static_cast<unsigned long long>(report.errors), okay ? "PASS" : "BELOW_TARGET");
      if (direction == Direction::kPhoneToBoard) {
        measured.push_back(report);
        passed &= okay;
      }
    }
  }
  stream.Send(Type::kFinish, 0);
  std::printf("\nPRIMARY phone->board: %s (each of %zu rounds >= %.2f MB/s).\n",
              passed ? "PASS" : "FAIL", measured.size(), options.minimum);
  return passed ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  try {
    return Test(ParseOptions(argc, argv));
  } catch (const std::exception& error) {
    std::fprintf(stderr, "ERROR: %s\n", error.what());
    return 1;
  }
}
