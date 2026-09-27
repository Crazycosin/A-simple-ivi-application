#include "wayland/WlClient.h"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace aa {

namespace {

std::uint32_t minVersion(std::uint32_t a, std::uint32_t b) {
    return a < b ? a : b;
}

} // namespace

// ---- 监听器表（成员顺序必须与 wayland-client-protocol.h 定义一致）----

const wl_registry_listener WlClient::kRegistryListener = {
    &WlClient::registryGlobalThunk,   // global
    &WlClient::registryRemoveThunk,   // global_remove
};

const wl_seat_listener WlClient::kSeatListener = {
    &WlClient::seatCapsThunk,         // capabilities
    &WlClient::seatNameThunk,         // name
};

const wl_pointer_listener WlClient::kPointerListener = {
    &WlClient::pointerEnterThunk,     // enter
    &WlClient::pointerLeaveThunk,     // leave
    &WlClient::pointerMotionThunk,    // motion
    &WlClient::pointerButtonThunk,    // button
    &WlClient::pointerAxisThunk,      // axis
    &WlClient::pointerFrameThunk,     // frame
    &WlClient::pointerAxisSourceThunk,   // axis_source
    &WlClient::pointerAxisStopThunk,     // axis_stop
    &WlClient::pointerAxisDiscreteThunk, // axis_discrete
    &WlClient::pointerAxisValue120Thunk, // axis_value120
    &WlClient::pointerAxisRelDirThunk,   // axis_relative_direction
};

const wl_touch_listener WlClient::kTouchListener = {
    &WlClient::touchDownThunk,        // down
    &WlClient::touchUpThunk,          // up
    &WlClient::touchMotionThunk,      // motion
    &WlClient::touchFrameThunk,       // frame
    &WlClient::touchCancelThunk,      // cancel
    &WlClient::touchShapeThunk,       // shape
    &WlClient::touchOrientationThunk, // orientation
};

const ivi_surface_listener WlClient::kIviSurfaceListener = {
    &WlClient::iviConfigureThunk,    // configure
};

const ivi_hmi_controller_listener WlClient::kHmiCtrlListener = {
    &WlClient::hmiWorkspaceEndControlThunk,  // workspace_end_control
};

WlClient::~WlClient() {
    disconnect();
}

bool WlClient::connect() {
    /*
     * 连接回退链（覆盖 launcher fork+exec 场景）：
     *  1) 原样连接（标准环境）
     *  2) launcher 由 weston spawn：WAYLAND_SOCKET 指向的 fd 已随 execve 关闭
     *     （CLOEXEC），libwayland 对无效 fd 直接失败且不回退 → 清掉该变量
     *  3) logind 会话的 XDG_RUNTIME_DIR=/run/user/1000 下无 socket → 回退 /run
     *  4) WAYLAND_DISPLAY 可能是父进程视角的编号（wayland-1），
     *     socket-activated weston 实际监听 /run/wayland-0 → 显式修正
     */
    display_ = wl_display_connect(nullptr);
    if (display_ == nullptr) {
        ::unsetenv("WAYLAND_SOCKET");
        display_ = wl_display_connect(nullptr);
    }
    if (display_ == nullptr) {
        ::setenv("XDG_RUNTIME_DIR", "/run", 1);
        display_ = wl_display_connect(nullptr);
    }
    if (display_ == nullptr) {
        ::setenv("XDG_RUNTIME_DIR", "/run", 1);
        ::setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        display_ = wl_display_connect(nullptr);
    }
    if (display_ == nullptr) {
        lastError_ = "wl_display_connect failed (XDG_RUNTIME_DIR/WAYLAND_DISPLAY?)";
        return false;
    }
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &kRegistryListener, this);
    if (wl_display_roundtrip(display_) < 0) {
        lastError_ = "wl_display_roundtrip failed";
        disconnect();
        return false;
    }
    return true;
}

void WlClient::disconnect() {
    if (touch_ != nullptr) {
        wl_touch_destroy(touch_);
        touch_ = nullptr;
    }
    if (pointer_ != nullptr) {
        wl_pointer_destroy(pointer_);
        pointer_ = nullptr;
    }
    if (iviSurface_ != nullptr) {
        ivi_surface_destroy(iviSurface_);
        iviSurface_ = nullptr;
    }
    if (hmiCtrl_ != nullptr) {
        ivi_hmi_controller_destroy(hmiCtrl_);
        hmiCtrl_ = nullptr;
    }
    if (surface_ != nullptr) {
        wl_surface_destroy(surface_);
        surface_ = nullptr;
    }
    if (seat_ != nullptr) {
        wl_seat_destroy(seat_);
        seat_ = nullptr;
    }
    if (shm_ != nullptr) {
        wl_shm_destroy(shm_);
        shm_ = nullptr;
    }
    if (compositor_ != nullptr) {
        wl_compositor_destroy(compositor_);
        compositor_ = nullptr;
    }
    if (registry_ != nullptr) {
        wl_registry_destroy(registry_);
        registry_ = nullptr;
    }
    if (display_ != nullptr) {
        wl_display_disconnect(display_);
        display_ = nullptr;
    }
}

bool WlClient::globalsReady() const {
    return compositor_ != nullptr && shm_ != nullptr && iviApp_ != nullptr;
}

bool WlClient::createIviSurface(std::uint32_t surfaceId) {
    if (compositor_ == nullptr || iviApp_ == nullptr) {
        lastError_ = "missing globals (compositor/shm/ivi_application)";
        return false;
    }
    surface_ = wl_compositor_create_surface(compositor_);
    if (surface_ == nullptr) {
        lastError_ = "wl_compositor_create_surface failed";
        return false;
    }
    iviSurface_ = ivi_application_surface_create(iviApp_, surfaceId, surface_);
    if (iviSurface_ == nullptr) {
        lastError_ = "ivi_application_surface_create failed";
        return false;
    }
    ivi_surface_add_listener(iviSurface_, &kIviSurfaceListener, this);
    return true;
}

int WlClient::dispatch() {
    return wl_display_dispatch(display_);
}

bool WlClient::switchLayout(HmiLayoutMode mode) {
    if (hmiCtrl_ == nullptr) {
        lastError_ = "ivi_hmi_controller global not advertised";
        return false;
    }
    ivi_hmi_controller_switch_mode(
        hmiCtrl_, static_cast<std::uint32_t>(mode));
    return true;
}

int WlClient::flush() {
    return wl_display_flush(display_);
}

int WlClient::displayFd() const {
    return display_ != nullptr ? wl_display_get_fd(display_) : -1;
}

// ---- registry ----

void WlClient::registryGlobalThunk(void* data, wl_registry* r,
                                    std::uint32_t name, const char* iface,
                                    std::uint32_t ver) {
    auto* self = static_cast<WlClient*>(data);
    if (std::strcmp(iface, "wl_compositor") == 0) {
        self->compositor_ = static_cast<wl_compositor*>(wl_registry_bind(
            r, name, &wl_compositor_interface, minVersion(ver, 4)));
    } else if (std::strcmp(iface, "wl_shm") == 0) {
        self->shm_ = static_cast<wl_shm*>(
            wl_registry_bind(r, name, &wl_shm_interface, 1));
    } else if (std::strcmp(iface, "wl_seat") == 0) {
        self->seat_ = static_cast<wl_seat*>(wl_registry_bind(
            r, name, &wl_seat_interface, minVersion(ver, 5)));
        if (self->seat_ != nullptr) {
            wl_seat_add_listener(self->seat_, &kSeatListener, self);
        }
    } else if (std::strcmp(iface, "ivi_application") == 0) {
        self->iviApp_ = static_cast<ivi_application*>(wl_registry_bind(
            r, name, &ivi_application_interface, 1));
    } else if (self->bindHmiCtrl_ &&
               std::strcmp(iface, "ivi_hmi_controller") == 0) {
        self->hmiCtrl_ = static_cast<ivi_hmi_controller*>(wl_registry_bind(
            r, name, &ivi_hmi_controller_interface, 1));
        if (self->hmiCtrl_ != nullptr) {
            ivi_hmi_controller_add_listener(self->hmiCtrl_,
                                            &kHmiCtrlListener, self);
        }
    }
}

void WlClient::registryRemoveThunk(void* /*data*/, wl_registry* /*r*/,
                                   std::uint32_t /*name*/) {
    // 本应用不持有按 name 绑定的动态对象，无需处理
}

// ---- seat ----

void WlClient::seatCapsThunk(void* data, wl_seat* /*s*/, std::uint32_t caps) {
    auto* self = static_cast<WlClient*>(data);

    if ((caps & WL_SEAT_CAPABILITY_POINTER) != 0 && self->pointer_ == nullptr) {
        self->pointer_ = wl_seat_get_pointer(self->seat_);
        if (self->pointer_ != nullptr) {
            wl_pointer_add_listener(self->pointer_, &kPointerListener, self);
        }
    } else if ((caps & WL_SEAT_CAPABILITY_POINTER) == 0 &&
               self->pointer_ != nullptr) {
        wl_pointer_destroy(self->pointer_);
        self->pointer_ = nullptr;
    }

    if ((caps & WL_SEAT_CAPABILITY_TOUCH) != 0 && self->touch_ == nullptr) {
        self->touch_ = wl_seat_get_touch(self->seat_);
        if (self->touch_ != nullptr) {
            wl_touch_add_listener(self->touch_, &kTouchListener, self);
        }
    } else if ((caps & WL_SEAT_CAPABILITY_TOUCH) == 0 &&
               self->touch_ != nullptr) {
        wl_touch_destroy(self->touch_);
        self->touch_ = nullptr;
    }
}

void WlClient::seatNameThunk(void* /*data*/, wl_seat* /*s*/,
                             const char* /*name*/) {}

// ---- pointer ----

void WlClient::pointerEnterThunk(void* data, wl_pointer* /*p*/,
                                 std::uint32_t /*serial*/,
                                 wl_surface* /*surf*/, wl_fixed_t sx,
                                 wl_fixed_t sy) {
    auto* self = static_cast<WlClient*>(data);
    self->pointerX_ = wl_fixed_to_int(sx);
    self->pointerY_ = wl_fixed_to_int(sy);
}

void WlClient::pointerLeaveThunk(void* /*data*/, wl_pointer* /*p*/,
                                 std::uint32_t /*serial*/,
                                 wl_surface* /*surf*/) {}

void WlClient::pointerMotionThunk(void* data, wl_pointer* /*p*/,
                                  std::uint32_t /*time*/, wl_fixed_t sx,
                                  wl_fixed_t sy) {
    auto* self = static_cast<WlClient*>(data);
    self->pointerX_ = wl_fixed_to_int(sx);
    self->pointerY_ = wl_fixed_to_int(sy);
}

void WlClient::pointerButtonThunk(void* data, wl_pointer* /*p*/,
                                  std::uint32_t /*serial*/,
                                  std::uint32_t /*time*/,
                                  std::uint32_t /*button*/,
                                  std::uint32_t state) {
    auto* self = static_cast<WlClient*>(data);
    self->emitPointer(self->pointerX_, self->pointerY_,
                      state == WL_POINTER_BUTTON_STATE_PRESSED);
}

// ---- touch ----

void WlClient::touchDownThunk(void* data, wl_touch* /*t*/,
                              std::uint32_t /*serial*/,
                              std::uint32_t /*time*/, wl_surface* /*surf*/,
                              std::int32_t id, wl_fixed_t x, wl_fixed_t y) {
    auto* self = static_cast<WlClient*>(data);
    const auto xi = wl_fixed_to_int(x);
    const auto yi = wl_fixed_to_int(y);
    if (id >= 0 && static_cast<std::size_t>(id) < kMaxTouchPoints) {
        self->touchTracks_[id] = {xi, yi, true};
    }
    self->emitTouch(xi, yi, TouchPhase::Down);
}

void WlClient::touchUpThunk(void* data, wl_touch* /*t*/,
                            std::uint32_t /*serial*/, std::uint32_t /*time*/,
                            std::int32_t id) {
    auto* self = static_cast<WlClient*>(data);
    std::int32_t xi = 0;
    std::int32_t yi = 0;
    if (id >= 0 && static_cast<std::size_t>(id) < kMaxTouchPoints &&
        self->touchTracks_[id].used) {
        xi = self->touchTracks_[id].x;
        yi = self->touchTracks_[id].y;
        self->touchTracks_[id].used = false;
    }
    self->emitTouch(xi, yi, TouchPhase::Up);
}

void WlClient::touchMotionThunk(void* data, wl_touch* /*t*/,
                                std::uint32_t /*time*/, std::int32_t id,
                                wl_fixed_t x, wl_fixed_t y) {
    auto* self = static_cast<WlClient*>(data);
    const auto xi = wl_fixed_to_int(x);
    const auto yi = wl_fixed_to_int(y);
    if (id >= 0 && static_cast<std::size_t>(id) < kMaxTouchPoints) {
        self->touchTracks_[id] = {xi, yi, true};
    }
    self->emitTouch(xi, yi, TouchPhase::Motion);
}

void WlClient::touchFrameThunk(void* /*data*/, wl_touch* /*t*/) {}

void WlClient::touchCancelThunk(void* data, wl_touch* /*t*/) {
    auto* self = static_cast<WlClient*>(data);
    for (auto& t : self->touchTracks_) {
        t.used = false;
    }
}

// ---- no-op 事件（签名必须与 wayland-client-protocol.h 一致）----

void WlClient::pointerAxisThunk(void* /*data*/, wl_pointer* /*p*/,
                                std::uint32_t /*time*/,
                                std::uint32_t /*axis*/,
                                wl_fixed_t /*value*/) {}

void WlClient::pointerFrameThunk(void* /*data*/, wl_pointer* /*p*/) {}

void WlClient::pointerAxisSourceThunk(void* /*data*/, wl_pointer* /*p*/,
                                      std::uint32_t /*axis_source*/) {}

void WlClient::pointerAxisStopThunk(void* /*data*/, wl_pointer* /*p*/,
                                    std::uint32_t /*time*/,
                                    std::uint32_t /*axis*/) {}

void WlClient::pointerAxisDiscreteThunk(void* /*data*/, wl_pointer* /*p*/,
                                        std::uint32_t /*axis*/,
                                        std::int32_t /*discrete*/) {}

void WlClient::pointerAxisValue120Thunk(void* /*data*/, wl_pointer* /*p*/,
                                        std::uint32_t /*axis*/,
                                        std::int32_t /*value120*/) {}

void WlClient::pointerAxisRelDirThunk(void* /*data*/, wl_pointer* /*p*/,
                                      std::uint32_t /*axis*/,
                                      std::uint32_t /*direction*/) {}

void WlClient::touchShapeThunk(void* /*data*/, wl_touch* /*t*/,
                               std::int32_t /*id*/, wl_fixed_t /*major*/,
                               wl_fixed_t /*minor*/) {}

void WlClient::touchOrientationThunk(void* /*data*/, wl_touch* /*t*/,
                                     std::int32_t /*id*/,
                                     wl_fixed_t /*orientation*/) {}

// ---- ivi_surface ----

void WlClient::iviConfigureThunk(void* data, ivi_surface* /*s*/,
                                std::int32_t w, std::int32_t h) {
    auto* self = static_cast<WlClient*>(data);
    if (self->onConfigure) {
        self->onConfigure(w, h);
    }
}

// ---- ivi_hmi_controller ----

void WlClient::hmiWorkspaceEndControlThunk(void* /*data*/,
                                           ivi_hmi_controller* /*c*/,
                                           std::int32_t /*controlled*/) {}

void WlClient::emitPointer(std::int32_t x, std::int32_t y, bool pressed) {
    if (onPointerButton) {
        onPointerButton(x, y, pressed);
    }
}

void WlClient::emitTouch(std::int32_t x, std::int32_t y, TouchPhase phase) {
    if (onTouch) {
        onTouch(x, y, phase);
    }
}

} // namespace aa
