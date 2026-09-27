#include "app/InputController.h"

#include "view/Layout.h"

namespace aa {

InputController::InputController(const Config& cfg, AppModel& model, Log& log)
    : cfg_(cfg), model_(model), log_(log) {}

bool InputController::insideCloseRegion(std::int32_t x, std::int32_t y) const {
    return Layout::closeHit(cfg_, model_.windowWidth()).contains(x, y);
}

void InputController::logTouch(const char* kind, std::int32_t x,
                               std::int32_t y) const {
    log_.write("[touch] %s x=%d y=%d", kind,
               static_cast<int>(x), static_cast<int>(y));
}

void InputController::onPointerButton(std::int32_t x, std::int32_t y,
                                      bool pressed) {
    if (pressed) {
        closeArmed_ = insideCloseRegion(x, y);
        logTouch("pointer-down", x, y);
        return;
    }
    logTouch("pointer-up", x, y);
    if (closeArmed_ && insideCloseRegion(x, y)) {
        log_.write("[input] close button activated (pointer)");
        model_.quit();
    }
    closeArmed_ = false;
}

void InputController::onTouch(std::int32_t x, std::int32_t y,
                              TouchPhase phase) {
    model_.recordTouch(x, y);
    switch (phase) {
    case TouchPhase::Down:
        closeArmed_ = insideCloseRegion(x, y);
        logTouch("down", x, y);
        break;
    case TouchPhase::Motion:
        logTouch("motion", x, y);
        break;
    case TouchPhase::Up:
        logTouch("up", x, y);
        if (closeArmed_ && insideCloseRegion(x, y)) {
            log_.write("[input] close button activated (touch)");
            model_.quit();
        }
        closeArmed_ = false;
        break;
    }
}

void InputController::onConfigure(std::int32_t w, std::int32_t h) {
    model_.setWindowSize(w, h);
    log_.write("[layout] configure %dx%d",
               static_cast<int>(w), static_cast<int>(h));
}

} // namespace aa
