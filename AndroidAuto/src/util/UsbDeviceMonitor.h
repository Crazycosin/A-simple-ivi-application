#ifndef ANDROID_AUTO_SRC_UTIL_USB_DEVICE_MONITOR_H_
#define ANDROID_AUTO_SRC_UTIL_USB_DEVICE_MONITOR_H_

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util/UsbDeviceRecognizer.h"

struct libusb_context;

namespace aa {

struct UsbDeviceState {
  UsbDeviceInfo device;
  UsbRecognition recognition;
};

struct UsbStatus {
  enum class State { kStopped, kStarting, kMonitoring, kError };
  State state = State::kStopped;
  // Empty while monitoring means no devices on the selected controller.
  std::vector<UsbDeviceState> devices;
  std::string message;
};

// Hotplug updates the latest status in the background. No consumer event queues.
class UsbDeviceMonitor {
 public:
  static UsbDeviceMonitor& Instance();
  UsbDeviceMonitor(const UsbDeviceMonitor&) = delete;
  UsbDeviceMonitor& operator=(const UsbDeviceMonitor&) = delete;

  // Lifecycle calls belong to the owning thread.
  void Start(const std::string& controller = "31260000.dwc3");
  void Stop();
  // Any thread may query a consistent copy; no USB I/O or event consumption.
  UsbStatus GetStatus() const;

 private:
  UsbDeviceMonitor() = default;
  ~UsbDeviceMonitor();
  void Run();
  void SetState(UsbStatus::State state, const std::string& message);
  void UpdateDevice(const UsbDeviceInfo& device);
  void RemoveDevice(int bus, int address);

  std::string controller_;
  std::thread worker_;
  mutable std::mutex mutex_;
  libusb_context* context_ = nullptr;
  std::atomic<bool> stopping_{false};
  UsbStatus status_;
};

}  // namespace aa

#endif  // ANDROID_AUTO_SRC_UTIL_USB_DEVICE_MONITOR_H_
