#include "util/UsbDeviceMonitor.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace {
void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}
}  // namespace

int main() {
  auto& monitor = aa::UsbDeviceMonitor::Instance();
  Require(&monitor == &aa::UsbDeviceMonitor::Instance(), "not a singleton");
  Require(monitor.GetStatus().state == aa::UsbStatus::State::kStopped,
          "initial state is not stopped");
  for (int iteration = 0; iteration < 5; ++iteration) {
    monitor.Start("usb-monitor-test-no-controller");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (monitor.GetStatus().state == aa::UsbStatus::State::kStarting &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto status = monitor.GetStatus();
    Require(status.state == aa::UsbStatus::State::kMonitoring,
            status.message.c_str());
    Require(status.devices.empty(), "unexpected device on nonexistent controller");
    status.message = "modified local copy";
    Require(monitor.GetStatus().message != status.message,
            "query did not return an independent copy");
    auto reader = std::async(std::launch::async, [&monitor] {
      for (int i = 0; i < 1000; ++i) {
        if (monitor.GetStatus().state != aa::UsbStatus::State::kMonitoring) return false;
      }
      return true;
    });
    Require(reader.get(), "concurrent query failed");
    const auto start = std::chrono::steady_clock::now();
    monitor.Stop();
    Require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
            "Stop did not interrupt blocking wait");
    Require(monitor.GetStatus().state == aa::UsbStatus::State::kStopped &&
                monitor.GetStatus().devices.empty(), "Stop left stale state");
  }
  for (int i = 0; i < 20; ++i) {
    monitor.Start("usb-monitor-test-no-controller");
    monitor.Stop();
  }
  std::cout << "USB latest-status query and lifecycle tests passed\n";
}
