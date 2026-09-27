#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

#include <wayland-client.h>

#include "ivi-application-client-protocol.h"
#include "ivi-hmi-controller-client-protocol.h"

namespace aa {

enum class TouchPhase : std::uint8_t {
    Down = 0,
    Motion,
    Up,
};

/// ivi_hmi_controller 布局模式（协议枚举同值）
enum class HmiLayoutMode : std::uint32_t {
    Tiling = 0,
    SideBySide = 1,
    FullScreen = 2,
    Random = 3,
};

/// wayland 连接 / registry 全局接口 / seat 输入（基础设施层，RAII）。
/// C 监听器经静态 thunk 转发到 std::function，由上层 Controller 安装。
/// 注意：thunk 持有 this，本类禁止移动/拷贝。
class WlClient {
public:
    using ConfigureFn =
        std::function<void(std::int32_t w, std::int32_t h)>;
    using PointerButtonFn =
        std::function<void(std::int32_t x, std::int32_t y, bool pressed)>;
    using TouchFn =
        std::function<void(std::int32_t x, std::int32_t y, TouchPhase phase)>;

    ConfigureFn onConfigure;
    PointerButtonFn onPointerButton;
    TouchFn onTouch;

    WlClient() = default;
    ~WlClient();
    WlClient(const WlClient&) = delete;
    WlClient& operator=(const WlClient&) = delete;
    WlClient(WlClient&&) = delete;
    WlClient& operator=(WlClient&&) = delete;

    bool connect();
    void disconnect();
    bool connected() const { return display_ != nullptr; }

    /// 必需全局接口是否齐备（wl_compositor / wl_shm / ivi_application）
    bool globalsReady() const;

    /// 创建 wl_surface 并绑定 ivi_surface（自声明 ivi_id）
    bool createIviSurface(std::uint32_t surfaceId);

    /// 切换 application layer 布局模式（需 hmi-controller 全局存在）。
    /// 全局缺失返回 false（非致命，应用按 tiling 自适应）。
    bool switchLayout(HmiLayoutMode mode);

    /// 是否绑定 ivi_hmi_controller 全局。必须在 connect() 前调用。
    /// 默认不绑：当前设备定制版 hmi-controller 的 bind 有权限校验
    /// （只认 weston-ivi-shell-user-interface），其余客户端 bind 即
    /// 触发 fatal 协议错误。仅在配套 compositor 侧允许时开启。
    void enableHmiController(bool enable) { bindHmiCtrl_ = enable; }

    wl_surface* surface() const { return surface_; }
    wl_shm* shm() const { return shm_; }

    int dispatch();
    int flush();
    /// wayland socket fd（供 glib 主循环挂 watch；连接后有效）
    int displayFd() const;

    const std::string& lastError() const { return lastError_; }

private:
    // ---- C 回调 thunk ----
    static void registryGlobalThunk(void* data, wl_registry* r,
                                    std::uint32_t name, const char* iface,
                                    std::uint32_t ver);
    static void registryRemoveThunk(void* data, wl_registry* r,
                                    std::uint32_t name);
    static void seatCapsThunk(void* data, wl_seat* s, std::uint32_t caps);
    static void seatNameThunk(void* data, wl_seat* s, const char* name);
    static void pointerEnterThunk(void* data, wl_pointer* p,
                                  std::uint32_t serial, wl_surface* surf,
                                  wl_fixed_t sx, wl_fixed_t sy);
    static void pointerLeaveThunk(void* data, wl_pointer* p,
                                  std::uint32_t serial, wl_surface* surf);
    static void pointerMotionThunk(void* data, wl_pointer* p,
                                  std::uint32_t time, wl_fixed_t sx,
                                  wl_fixed_t sy);
    static void pointerButtonThunk(void* data, wl_pointer* p,
                                   std::uint32_t serial, std::uint32_t time,
                                   std::uint32_t button, std::uint32_t state);
    static void touchDownThunk(void* data, wl_touch* t, std::uint32_t serial,
                               std::uint32_t time, wl_surface* surf,
                               std::int32_t id, wl_fixed_t x, wl_fixed_t y);
    static void touchUpThunk(void* data, wl_touch* t, std::uint32_t serial,
                             std::uint32_t time, std::int32_t id);
    static void touchMotionThunk(void* data, wl_touch* t, std::uint32_t time,
                                 std::int32_t id, wl_fixed_t x, wl_fixed_t y);
    static void touchFrameThunk(void* data, wl_touch* t);
    static void touchCancelThunk(void* data, wl_touch* t);
    // ---- no-op（libwayland 对 NULL listener 直接 abort，事件必须全接住）----
    static void pointerAxisThunk(void* data, wl_pointer* p, std::uint32_t time,
                                 std::uint32_t axis, wl_fixed_t value);
    static void pointerFrameThunk(void* data, wl_pointer* p);
    static void pointerAxisSourceThunk(void* data, wl_pointer* p,
                                       std::uint32_t axis_source);
    static void pointerAxisStopThunk(void* data, wl_pointer* p,
                                     std::uint32_t time, std::uint32_t axis);
    static void pointerAxisDiscreteThunk(void* data, wl_pointer* p,
                                         std::uint32_t axis,
                                         std::int32_t discrete);
    static void pointerAxisValue120Thunk(void* data, wl_pointer* p,
                                         std::uint32_t axis,
                                         std::int32_t value120);
    static void pointerAxisRelDirThunk(void* data, wl_pointer* p,
                                       std::uint32_t axis,
                                       std::uint32_t direction);
    static void touchShapeThunk(void* data, wl_touch* t, std::int32_t id,
                                wl_fixed_t major, wl_fixed_t minor);
    static void touchOrientationThunk(void* data, wl_touch* t,
                                      std::int32_t id,
                                      wl_fixed_t orientation);
    static void iviConfigureThunk(void* data, ivi_surface* s,
                                  std::int32_t w, std::int32_t h);
    // hmi-controller workspace_end_control 事件（本应用不发起 workspace_control，
    // no-op 接住防 NULL listener abort）
    static void hmiWorkspaceEndControlThunk(void* data, ivi_hmi_controller* c,
                                            std::int32_t controlled);

    void emitPointer(std::int32_t x, std::int32_t y, bool pressed);
    void emitTouch(std::int32_t x, std::int32_t y, TouchPhase phase);

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_shm* shm_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wl_touch* touch_ = nullptr;
    ivi_application* iviApp_ = nullptr;
    ivi_hmi_controller* hmiCtrl_ = nullptr;  // 仅 bindHmiCtrl_ 时绑定
    wl_surface* surface_ = nullptr;
    ivi_surface* iviSurface_ = nullptr;

    // pointer button 事件不带坐标：由 enter/motion 跟踪
    std::int32_t pointerX_ = 0;
    std::int32_t pointerY_ = 0;

    // touch up 事件不带坐标：按 id 记录最后位置
    struct TouchTrack {
        std::int32_t x = 0;
        std::int32_t y = 0;
        bool used = false;
    };
    static constexpr std::size_t kMaxTouchPoints = 32;
    TouchTrack touchTracks_[kMaxTouchPoints];

    std::string lastError_;

    bool bindHmiCtrl_ = false;

    static const wl_registry_listener kRegistryListener;
    static const wl_seat_listener kSeatListener;
    static const wl_pointer_listener kPointerListener;
    static const wl_touch_listener kTouchListener;
    static const ivi_surface_listener kIviSurfaceListener;
    static const ivi_hmi_controller_listener kHmiCtrlListener;
};

} // namespace aa
