// aoa_probe.cpp — 独立的 AOA (Android Open Accessory) v1.0 协议探测工具 (C++17)
//
// 用途：在没有 aasdk/autoapp 的板子上，手动验证本机 USB Host 能否
//       和插入的 Android 手机完成 AOA v1.0 握手。
//
// 依赖：仅依赖 libusb-1.0（不依赖 aasdk / openauto）
//
// 编译（在开发主机上交叉编译，示例为 ARM 32 位硬浮点）：
//   arm-linux-gnueabihf-g++ -std=c++17 aoa_probe.cpp -o aoa_probe -lusb-1.0 -static
//
// 如果板子架构是 aarch64：
//   aarch64-linux-gnu-g++ -std=c++17 aoa_probe.cpp -o aoa_probe -lusb-1.0 -static
//
// 用法：
//   ./aoa_probe            # 自动遍历所有已插入设备，找第一个支持 AOA 的
//   ./aoa_probe 0x04e8      # 指定手机的 vendor id（十六进制或十进制均可，用 lsusb 先查出来）

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <libusb-1.0/libusb.h>

namespace {

constexpr uint8_t AOA_GET_PROTOCOL    = 51;
constexpr uint8_t AOA_SEND_STRING     = 52;
constexpr uint8_t AOA_START_ACCESSORY = 53;
constexpr uint16_t GOOGLE_VID         = 0x18D1;

// RAII 包装 libusb_context，析构时自动 libusb_exit
class UsbContext {
public:
    UsbContext() {
        if (libusb_init(&ctx_) < 0) {
            throw std::runtime_error("libusb 初始化失败");
        }
    }
    ~UsbContext() {
        if (ctx_) libusb_exit(ctx_);
    }
    UsbContext(const UsbContext&) = delete;
    UsbContext& operator=(const UsbContext&) = delete;

    libusb_context* get() const { return ctx_; }

private:
    libusb_context* ctx_ = nullptr;
};

// RAII 包装 libusb_device_handle，析构时自动 libusb_close
class UsbHandle {
public:
    explicit UsbHandle(libusb_device_handle* h) : handle_(h) {}
    ~UsbHandle() {
        if (handle_) libusb_close(handle_);
    }
    UsbHandle(const UsbHandle&) = delete;
    UsbHandle& operator=(const UsbHandle&) = delete;

    libusb_device_handle* get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    libusb_device_handle* handle_ = nullptr;
};

// RAII 包装 libusb_device** 列表，析构时自动 libusb_free_device_list
class UsbDeviceList {
public:
    UsbDeviceList(libusb_context* ctx) {
        count_ = libusb_get_device_list(ctx, &list_);
        if (count_ < 0) {
            throw std::runtime_error("获取 USB 设备列表失败");
        }
    }
    ~UsbDeviceList() {
        if (list_) libusb_free_device_list(list_, 1);
    }
    UsbDeviceList(const UsbDeviceList&) = delete;
    UsbDeviceList& operator=(const UsbDeviceList&) = delete;

    ssize_t size() const { return count_; }
    libusb_device* operator[](ssize_t i) const { return list_[i]; }

private:
    libusb_device** list_ = nullptr;
    ssize_t count_ = 0;
};

std::optional<int> getAoaProtocolVersion(libusb_device_handle* handle) {
    std::array<unsigned char, 2> buf{0, 0};
    int r = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_GET_PROTOCOL, 0, 0,
        buf.data(), static_cast<uint16_t>(buf.size()), 1000);

    if (r < 0) {
        std::printf("  -> 发送 GetProtocol 失败: %s (这个设备大概率不支持 AOA)\n",
                    libusb_error_name(r));
        return std::nullopt;
    }

    int version = (buf[1] << 8) | buf[0];
    return version;
}

void sendAccessoryStrings(libusb_device_handle* handle) {
    static constexpr std::array<std::string_view, 6> strs{
        "TestAccessory",    // 0: manufacturer
        "AOAProbe",         // 1: model
        "AOA v1.0 test",    // 2: description
        "1.0",              // 3: version
        "",                 // 4: uri (可留空)
        "0000000012345678"  // 5: serial
    };

    for (size_t i = 0; i < strs.size(); ++i) {
        // control transfer 需要非 const buffer，这里拷贝一份带 \0 的字符串
        std::string payload(strs[i]);
        int r = libusb_control_transfer(
            handle,
            LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
            AOA_SEND_STRING, 0, static_cast<uint16_t>(i),
            reinterpret_cast<unsigned char*>(payload.data()),
            static_cast<uint16_t>(payload.size() + 1), 1000);

        if (r < 0) {
            std::printf("  -> 发送字符串 index=%zu 失败: %s\n", i, libusb_error_name(r));
        }
    }
}

bool startAccessoryMode(libusb_device_handle* handle) {
    int r = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_START_ACCESSORY, 0, 0, nullptr, 0, 1000);

    if (r < 0) {
        std::printf("  -> 发送 StartAccessory 失败: %s\n", libusb_error_name(r));
        return false;
    }
    std::printf("  -> 已发送 StartAccessory，设备应该马上会掉线重新枚举\n");
    std::printf("     请在几秒内执行 lsusb，确认是否出现 18d1:2d00 或 18d1:2d01\n");
    return true;
}

// 返回 true 表示这个设备成功走完了 AOA 探测流程（找到并处理了）
bool tryDevice(libusb_device* dev, uint16_t filterVid, bool doSwitch) {
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(dev, &desc) < 0) {
        return false;
    }

    if (desc.idVendor == GOOGLE_VID) {
        std::printf("[跳过] VID=0x%04x PID=0x%04x 已经是 Google accessory VID，可能已在 AOA 模式\n",
                    desc.idVendor, desc.idProduct);
        return false;
    }

    if (filterVid != 0 && desc.idVendor != filterVid) {
        return false;
    }

    libusb_device_handle* rawHandle = nullptr;
    int openResult = libusb_open(dev, &rawHandle);
    if (openResult < 0) {
        std::printf("[跳过] VID=0x%04x PID=0x%04x 打开失败: %s\n",
                    desc.idVendor, desc.idProduct, libusb_error_name(openResult));
        return false;
    }
    UsbHandle handle(rawHandle);

    std::printf("[尝试] VID=0x%04x PID=0x%04x ...\n", desc.idVendor, desc.idProduct);

    auto version = getAoaProtocolVersion(handle.get());
    if (!version) {
        return false;
    }

    std::printf("  -> GetProtocol 返回协议版本号: %d\n", *version);

    if (*version <= 0) {
        std::printf("  -> 版本号为 0，设备不支持 AOA\n");
        return false;
    }

    std::printf("  => 该设备支持 AOA，协议版本 %d (v1.0 对应返回值通常为 1)\n", *version);

    if (!doSwitch) {
        return true;
    }

    sendAccessoryStrings(handle.get());
    startAccessoryMode(handle.get());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t filterVid = 0;
    if (argc > 1) {
        filterVid = static_cast<uint16_t>(std::strtol(argv[1], nullptr, 0));
        std::printf("只探测 VID=0x%04x 的设备\n", filterVid);
    }

    try {
        UsbContext ctx;
        UsbDeviceList devices(ctx.get());

        std::printf("共发现 %zd 个 USB 设备，逐个探测中...\n\n", devices.size());
        std::printf("HOTPLUG: %s\n",
              libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)
                  ? "supported"
                  : "unsupported");
        bool found = false;
        for (ssize_t i = 0; i < devices.size(); ++i) {
            if (tryDevice(devices[i], filterVid, /*doSwitch=*/true)) {
                found = true;
                break;  // 找到一个支持的就够了，如需测试所有设备可去掉这行
            }
        }

        if (!found) {
            std::printf("\n结论: 没有找到任何支持 AOA 协议的设备。\n");
            std::printf("排查方向: 1) 手机本身是否支持 AOA(几乎不会是问题)\n");
            std::printf("          2) 该 USB 口是否处于 Host 模式(用之前查过的 dr_mode/role 确认)\n");
            std::printf("          3) 是否插错了口(比如插到了 OTG/Device 口上)\n");
        }

        return found ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "错误: %s\n", e.what());
        return 1;
    }
}
