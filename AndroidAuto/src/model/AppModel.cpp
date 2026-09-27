#include "model/AppModel.h"

namespace aa {

void AppModel::setWindowSize(std::int32_t w, std::int32_t h) {
    if (w <= 0 || h <= 0) {
        return;
    }
    if (w != width_ || h != height_) {
        width_ = w;
        height_ = h;
        dirty_ = true;
    }
}

} // namespace aa
