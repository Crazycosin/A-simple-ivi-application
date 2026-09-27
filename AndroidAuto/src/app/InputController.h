#pragma once

#include <cstdint>

#include "model/AppConfig.h"
#include "model/AppModel.h"
#include "util/Log.h"
#include "wayland/WlClient.h"

namespace aa {

/// Controller：原始输入事件 → 语义动作。
/// 关闭按钮采用 armed 机制：down 命中热区置位，up 仍命中才退出（防误触/防滑出）。
class InputController {
public:
    InputController(const Config& cfg, AppModel& model, Log& log);

    void onPointerButton(std::int32_t x, std::int32_t y, bool pressed);
    void onTouch(std::int32_t x, std::int32_t y, TouchPhase phase);
    void onConfigure(std::int32_t w, std::int32_t h);

private:
    bool insideCloseRegion(std::int32_t x, std::int32_t y) const;
    void logTouch(const char* kind, std::int32_t x, std::int32_t y) const;

    const Config& cfg_;
    AppModel& model_;
    Log& log_;
    bool closeArmed_ = false;
};

} // namespace aa
