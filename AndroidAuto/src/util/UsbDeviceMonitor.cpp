#include "util/UsbDeviceMonitor.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>

#include <utility>

#include <libusb-1.0/libusb.h>

namespace aa {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kRetryInterval = std::chrono::seconds(5);
constexpr unsigned int kAoaTimeoutMs = 1000;

struct HotplugNotice {
  std::shared_ptr<libusb_device> device;
  libusb_hotplug_event event;
};

struct HotplugQueue {
  std::mutex& mutex;
  explicit HotplugQueue(std::mutex& shared_mutex) : mutex(shared_mutex) {}
  std::vector<HotplugNotice> notices;
  std::exception_ptr failure;
};

int LIBUSB_CALL OnHotplug(libusb_context*, libusb_device* device,
                         libusb_hotplug_event event, void* data) {
  auto* queue = static_cast<HotplugQueue*>(data);
  std::lock_guard<std::mutex> lock(queue->mutex);
  try {
    // No synchronous USB I/O inside libusb's event callback.
    queue->notices.push_back({std::shared_ptr<libusb_device>(
        libusb_ref_device(device), libusb_unref_device), event});
  } catch (...) {
    queue->failure = std::current_exception();
  }
  return 0;
}

struct HotplugRegistration {
  libusb_context* context;
  libusb_hotplug_callback_handle handle;
  ~HotplugRegistration() {
    libusb_hotplug_deregister_callback(context, handle);
  }
};

bool IsControllerBus(int bus, const std::string& controller) {
  std::error_code error;
  const auto path = std::filesystem::canonical(
      "/sys/bus/usb/devices/usb" + std::to_string(bus), error);
  if (error) return false;
  for (const auto& component : path) {
    if (component == controller) return true;
  }
  return false;
}

void AppendError(const std::string& operation, int status, UsbDeviceInfo* info) {
  if (!info->query_error.empty()) info->query_error += "; ";
  info->query_error += operation + ": " + libusb_error_name(status);
}

std::string ReadString(libusb_device_handle* handle, std::uint8_t index,
                       UsbDeviceInfo* info) {
  if (index == 0) return {};
  unsigned char buffer[256] = {};
  const int length = libusb_get_string_descriptor_ascii(
      handle, index, buffer, sizeof(buffer));
  if (length < 0) {
    AppendError("string descriptor", length, info);
    return {};
  }
  return std::string(reinterpret_cast<const char*>(buffer), length);
}

UsbDeviceInfo InspectDevice(libusb_device* device) {
  UsbDeviceInfo info;
  info.bus = libusb_get_bus_number(device);
  info.address = libusb_get_device_address(device);
  switch (libusb_get_device_speed(device)) {
    case LIBUSB_SPEED_LOW: info.speed = UsbDeviceInfo::Speed::kLow; break;
    case LIBUSB_SPEED_FULL: info.speed = UsbDeviceInfo::Speed::kFull; break;
    case LIBUSB_SPEED_HIGH: info.speed = UsbDeviceInfo::Speed::kHigh; break;
    case LIBUSB_SPEED_SUPER: info.speed = UsbDeviceInfo::Speed::kSuper; break;
    case LIBUSB_SPEED_SUPER_PLUS: info.speed = UsbDeviceInfo::Speed::kSuperPlus; break;
    default: break;
  }
  libusb_device_descriptor descriptor{};
  int status = libusb_get_device_descriptor(device, &descriptor);
  if (status < 0) {
    AppendError("device descriptor", status, &info);
    return info;
  }
  info.vendor_id = descriptor.idVendor;
  info.product_id = descriptor.idProduct;
  libusb_config_descriptor* raw_config = nullptr;
  status = libusb_get_active_config_descriptor(device, &raw_config);
  if (status == LIBUSB_ERROR_NOT_FOUND && descriptor.bNumConfigurations > 0) {
    status = libusb_get_config_descriptor(device, 0, &raw_config);
  }
  const std::unique_ptr<libusb_config_descriptor,
                        decltype(&libusb_free_config_descriptor)>
      config(raw_config, libusb_free_config_descriptor);
  if (status < 0) {
    AppendError("configuration descriptor", status, &info);
  } else {
    for (int i = 0; i < config->bNumInterfaces; ++i) {
      const auto& iface = config->interface[i];
      // Count each interface once, using its default alternate setting.
      for (int j = 0; j < iface.num_altsetting; ++j) {
        const auto& alt = iface.altsetting[j];
        if (alt.bAlternateSetting == 0) {
          info.interfaces.push_back({alt.bInterfaceClass, alt.bInterfaceSubClass});
          break;
        }
      }
    }
  }
  // Preserve Java's pure-peripheral exclusion without vendor control requests.
  if (RecognizeUsbDevice(info).reason == "pure peripheral") return info;

  libusb_device_handle* raw_handle = nullptr;
  status = libusb_open(device, &raw_handle);
  if (status < 0) {
    AppendError("open", status, &info);
    return info;
  }
  const std::unique_ptr<libusb_device_handle, decltype(&libusb_close)>
      handle(raw_handle, libusb_close);
  info.manufacturer = ReadString(handle.get(), descriptor.iManufacturer, &info);
  info.product = ReadString(handle.get(), descriptor.iProduct, &info);
  unsigned char version[2] = {};
  status = libusb_control_transfer(
      handle.get(), LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
                        LIBUSB_RECIPIENT_DEVICE,
      51, 0, 0, version, sizeof(version), kAoaTimeoutMs);
  if (status == 2) {
    info.aoa_version = version[0] | (static_cast<int>(version[1]) << 8);
  } else if (status == LIBUSB_ERROR_PIPE) {
    info.aoa_version = 0;  // Device stalled an unsupported vendor request.
  } else if (status < 0) {
    AppendError("AOA GET_PROTOCOL", status, &info);
  } else {
    info.query_error += "AOA GET_PROTOCOL: expected two bytes";
  }
  return info;
}

struct DeviceState {
  std::shared_ptr<libusb_device> reference;
  UsbDeviceInfo info;
  Clock::time_point retry_at;
};

}  // namespace

UsbDeviceMonitor& UsbDeviceMonitor::Instance() {
  static UsbDeviceMonitor instance;
  return instance;
}

UsbDeviceMonitor::~UsbDeviceMonitor() {
  Stop();
}

void UsbDeviceMonitor::Start(const std::string& controller) {
  if (worker_.joinable()) return;
  controller_ = controller;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = false;
    status_ = {UsbStatus::State::kStarting, {}, {}};
  }
  worker_ = std::thread(&UsbDeviceMonitor::Run, this);
}

void UsbDeviceMonitor::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    if (context_ != nullptr) libusb_interrupt_event_handler(context_);
  }
  if (worker_.joinable()) worker_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  status_ = {};
}

UsbStatus UsbDeviceMonitor::GetStatus() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

void UsbDeviceMonitor::SetState(UsbStatus::State state,
                                const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_.state = state;
  status_.message = message;
}

void UsbDeviceMonitor::UpdateDevice(const UsbDeviceInfo& device) {
  // Recognition and USB I/O happen outside the shared-state lock.
  UsbDeviceState updated{device, RecognizeUsbDevice(device)};
  std::lock_guard<std::mutex> lock(mutex_);
  auto& devices = status_.devices;
  auto it = std::find_if(devices.begin(), devices.end(),
                        [&device](const UsbDeviceState& state) {
    return state.device.bus == device.bus && state.device.address == device.address;
  });
  if (it == devices.end()) devices.push_back(std::move(updated));
  else *it = std::move(updated);
}

void UsbDeviceMonitor::RemoveDevice(int bus, int address) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& devices = status_.devices;
  devices.erase(std::remove_if(devices.begin(), devices.end(),
                              [bus, address](const UsbDeviceState& state) {
    return state.device.bus == bus && state.device.address == address;
  }), devices.end());
}

void UsbDeviceMonitor::Run() {
  try {
    libusb_context* raw_context = nullptr;
    const int status = libusb_init(&raw_context);
    if (status < 0) {
      SetState(UsbStatus::State::kError,
               std::string("libusb_init: ") + libusb_error_name(status));
      return;
    }
    auto release_context = [this](libusb_context* context) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        context_ = nullptr;
      }
      libusb_exit(context);
    };
    const std::unique_ptr<libusb_context, decltype(release_context)>
        context(raw_context, release_context);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      context_ = context.get();
      if (stopping_) return;
    }
    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
      SetState(UsbStatus::State::kError,
               "libusb hotplug unsupported; USB monitoring disabled");
      return;
    }
    HotplugQueue queue(mutex_);
    libusb_hotplug_callback_handle handle;
    const int registration_status = libusb_hotplug_register_callback(
        context.get(), LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                           LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
        LIBUSB_HOTPLUG_ENUMERATE, LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
        OnHotplug, &queue, &handle);
    if (registration_status < 0) {
      SetState(UsbStatus::State::kError,
               std::string("hotplug registration: ") +
                   libusb_error_name(registration_status));
      return;
    }
    const HotplugRegistration registration{context.get(), handle};
    std::map<libusb_device*, DeviceState> known;
    bool initializing = true;
    while (true) {
      if (stopping_) break;
      std::vector<HotplugNotice> notices;
      {
        std::lock_guard<std::mutex> lock(queue.mutex);
        if (queue.failure) std::rethrow_exception(queue.failure);
        notices.swap(queue.notices);
      }
      for (const auto& notice : notices) {
        auto* device = notice.device.get();
        auto it = known.find(device);
        if (notice.event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
          // Use cached identity: sysfs may already be gone on removal.
          if (it != known.end()) {
            RemoveDevice(it->second.info.bus, it->second.info.address);
            known.erase(it);
          }
          continue;
        }
        // ENUMERATE can deliver a duplicate arrival. Keep one record per device.
        if (it != known.end() || libusb_get_port_number(device) == 0 ||
            !IsControllerBus(libusb_get_bus_number(device), controller_)) continue;
        auto info = InspectDevice(device);
        UpdateDevice(info);
        known.emplace(device, DeviceState{notice.device, std::move(info),
                                         Clock::now() + kRetryInterval});
      }
      if (initializing) {
        SetState(UsbStatus::State::kMonitoring,
                 "hotplug monitoring started: " + controller_);
        initializing = false;
      }
      for (auto& entry : known) {
        auto& state = entry.second;
        if (state.info.query_error.empty() || Clock::now() < state.retry_at) continue;
        if (stopping_) return;
        auto info = InspectDevice(entry.first);
        UpdateDevice(info);
        state.info = std::move(info);
        state.retry_at = Clock::now() + kRetryInterval;
      }
      if (stopping_) break;
      // Synchronous inspection can dispatch more hotplug callbacks. Drain them
      // before blocking so a queued removal does not wait for another event.
      {
        std::lock_guard<std::mutex> lock(queue.mutex);
        if (!queue.notices.empty() || queue.failure) continue;
      }
      // Long blocking wait, shortened only for failed-device retry deadlines.
      auto wait = std::chrono::microseconds(std::chrono::hours(24));
      for (const auto& entry : known) {
        if (entry.second.info.query_error.empty()) continue;
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            entry.second.retry_at - Clock::now());
        if (remaining < wait) wait = remaining;
      }
      if (wait.count() < 0) wait = std::chrono::microseconds::zero();
      timeval timeout{};
      timeout.tv_sec = wait.count() / 1000000;
      timeout.tv_usec = wait.count() % 1000000;
      const int event_status = libusb_handle_events_timeout_completed(
          context.get(), &timeout, nullptr);
      if (event_status < 0 && event_status != LIBUSB_ERROR_INTERRUPTED) {
        SetState(UsbStatus::State::kError,
                 std::string("hotplug event handling stopped: ") +
                     libusb_error_name(event_status));
        break;  // Avoid spinning on a persistent backend failure.
      }
    }
  } catch (const std::exception& error) {
    SetState(UsbStatus::State::kError, error.what());
  }
}

}  // namespace aa
