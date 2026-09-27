#include "util/UsbDeviceRecognizer.h"

#include <algorithm>
#include <cctype>

namespace aa {
namespace {

bool IsPeripheralClass(std::uint8_t device_class) {
  switch (device_class) {
    case 0x01:  // Audio.
    case 0x02:  // Communications.
    case 0x03:  // HID.
    case 0x07:  // Printer.
    case 0x09:  // Hub.
    case 0x0a:  // CDC data.
    case 0x0e:  // Video.
      return true;
    default:
      return false;
  }
}

}  // namespace

UsbRecognition RecognizeUsbDevice(const UsbDeviceInfo& device) {
  UsbRecognition result;
  // Empty/failed descriptors must not be classified as pure peripherals.
  if (!device.interfaces.empty() &&
      std::all_of(device.interfaces.begin(), device.interfaces.end(),
                  [](const UsbInterfaceInfo& iface) {
                    return IsPeripheralClass(iface.device_class);
                  })) {
    result.reason = "pure peripheral";
    return result;
  }
  if (device.vendor_id == 0x18d1 && device.product_id >= 0x2d00 &&
      device.product_id <= 0x2d05) {
    result.reason = "Google AOA VID/PID";
  }
  for (const auto& iface : device.interfaces) {
    if (!result.reason.empty()) break;
    if (iface.device_class == 0x06 &&
        (iface.subclass == 0x01 || iface.subclass == 0x02)) {
      result.reason = "MTP/PTP interface";
    } else if (iface.device_class == 0xff && iface.subclass == 0x42) {
      result.reason = "ADB interface";
    }
  }
  if (result.reason.empty() && device.aoa_version >= 1) {
    result.reason = "AOA protocol";
  }
  if (result.reason.empty()) {
    bool has_storage = false;
    bool has_non_peripheral = false;
    for (const auto& iface : device.interfaces) {
      has_storage |= iface.device_class == 0x08;
      has_non_peripheral |= iface.device_class != 0x08 &&
                            !IsPeripheralClass(iface.device_class);
    }
    if (has_storage && !has_non_peripheral &&
        std::all_of(device.interfaces.begin(), device.interfaces.end(),
                    [](const UsbInterfaceInfo& iface) {
                      return iface.device_class == 0x08;
                    })) {
      result.reason = "pure mass storage";
      return result;
    }
    if (has_storage && has_non_peripheral && device.interfaces.size() >= 2) {
      result.reason = "storage composite heuristic";
    } else {
      std::string name = device.manufacturer + " " + device.product;
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char ch) { return std::tolower(ch); });
      constexpr const char* kKeywords[] = {
          "phone", "mobile", "android", "smartphone", "mi", "huawei",
          "oppo", "vivo", "samsung", "oneplus", "realme", "meizu"};
      for (const char* keyword : kKeywords) {
        if (name.find(keyword) != std::string::npos) {
          result.reason = "manufacturer/product name heuristic";
          break;
        }
      }
    }
  }
  result.is_phone = !result.reason.empty();
  result.aa_candidate = result.is_phone && device.vendor_id != 0x12d1 &&
                        device.aoa_version >= 1;
  if (result.reason.empty()) result.reason = "no phone feature";
  return result;
}

const char* UsbSpeedName(UsbDeviceInfo::Speed speed) {
  switch (speed) {
    case UsbDeviceInfo::Speed::kLow: return "Low-Speed (1.5 Mb/s)";
    case UsbDeviceInfo::Speed::kFull: return "Full-Speed (12 Mb/s)";
    case UsbDeviceInfo::Speed::kHigh: return "USB 2.0 High-Speed (480 Mb/s)";
    case UsbDeviceInfo::Speed::kSuper: return "USB 3.x SuperSpeed (5 Gb/s)";
    case UsbDeviceInfo::Speed::kSuperPlus: return "USB 3.x SuperSpeedPlus (>=10 Gb/s)";
    case UsbDeviceInfo::Speed::kUnknown: return "unknown";
  }
  return "unknown";
}

}  // namespace aa
