#include "util/UsbDeviceRecognizer.h"

#include <cstdlib>
#include <iostream>

namespace {

void Check(const aa::UsbDeviceInfo& device, bool phone, bool candidate,
           const char* scenario) {
  const auto result = aa::RecognizeUsbDevice(device);
  if (result.is_phone != phone || result.aa_candidate != candidate) {
    std::cerr << "Failed: " << scenario << " reason=" << result.reason << '\n';
    std::exit(EXIT_FAILURE);
  }
}

}  // namespace

int main() {
  aa::UsbDeviceInfo device;
  Check(device, false, false, "unavailable descriptors");
  device.interfaces = {{0x03, 0}};
  device.product = "mobile keyboard";
  Check(device, false, false, "HID excludes misleading name");
  device.interfaces = {{0x08, 0}};
  device.product = "Samsung storage";
  Check(device, false, false, "pure storage excludes name fallback");
  device.aoa_version = 1;
  Check(device, true, true, "storage phone responding to AOA 1");
  device.aoa_version = -1;
  device.product.clear();
  device.interfaces = {{0x06, 0x01}};
  Check(device, true, false, "MTP with failed AOA query");
  device.interfaces = {{0x06, 0x02}};
  Check(device, true, false, "PTP heuristic");
  device.interfaces = {{0xff, 0x42}, {0x03, 0}};
  device.aoa_version = 2;
  Check(device, true, true, "ADB composite AOA 2");
  device.vendor_id = 0x12d1;
  Check(device, true, false, "Java Huawei exclusion");
  device.vendor_id = 0x18d1;
  device.product_id = 0x2d00;
  device.interfaces = {{0xff, 0}};
  device.aoa_version = -1;
  Check(device, true, false, "AOA VID/PID without protocol response");
  device.vendor_id = 0x1234;
  device.product_id = 0;
  device.interfaces = {{0x08, 0}, {0xff, 0}};
  Check(device, true, false, "storage composite fallback");
  device.interfaces = {{0xff, 0}};
  device.product = "ANDROID device";
  Check(device, true, false, "case insensitive name fallback");
  device.product = "unknown device";
  Check(device, false, false, "unknown vendor interface");
  device.vendor_id = 0x05ac;
  Check(device, false, false, "Apple VID alone disabled in Java");
  std::cout << "USB recognition tests passed (13 scenarios)\n";
}
