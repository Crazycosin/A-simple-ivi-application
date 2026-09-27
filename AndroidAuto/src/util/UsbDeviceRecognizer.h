#ifndef ANDROID_AUTO_SRC_UTIL_USB_DEVICE_RECOGNIZER_H_
#define ANDROID_AUTO_SRC_UTIL_USB_DEVICE_RECOGNIZER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace aa {

struct UsbInterfaceInfo {
  std::uint8_t device_class = 0;
  std::uint8_t subclass = 0;
};

struct UsbDeviceInfo {
  enum class Speed { kUnknown, kLow, kFull, kHigh, kSuper, kSuperPlus };
  int bus = 0;
  int address = 0;
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;
  Speed speed = Speed::kUnknown;  // Negotiated link speed, not device capability.
  std::string manufacturer;
  std::string product;
  std::vector<UsbInterfaceInfo> interfaces;
  // -1: query unavailable/failed; 0: unsupported; positive: AOA version.
  int aoa_version = -1;
  std::string query_error;
};

struct UsbRecognition {
  bool is_phone = false;
  bool aa_candidate = false;
  std::string reason;
};

// Mirrors UsbDevices.java heuristics. AOA support cannot confirm AA support.
UsbRecognition RecognizeUsbDevice(const UsbDeviceInfo& device);
const char* UsbSpeedName(UsbDeviceInfo::Speed speed);

}  // namespace aa

#endif  // ANDROID_AUTO_SRC_UTIL_USB_DEVICE_RECOGNIZER_H_
