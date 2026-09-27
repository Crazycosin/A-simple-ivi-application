#include "view/AppView.h"

#include "view/Layout.h"

namespace aa {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// 0xRRGGBBAA → cairo 颜色
void argb(cairo_t* cr, std::uint32_t color) {
    cairo_set_source_rgba(cr,
                          static_cast<double>((color >> 24) & 0xFFu) / 255.0,
                          static_cast<double>((color >> 16) & 0xFFu) / 255.0,
                          static_cast<double>((color >> 8) & 0xFFu) / 255.0,
                          static_cast<double>(color & 0xFFu) / 255.0);
}

void centerText(cairo_t* cr, const Rect& r, const char* text) {
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    const double x = r.x + (r.w - ext.width) / 2.0 - ext.x_bearing;
    const double y = r.y + (r.h - ext.height) / 2.0 - ext.y_bearing;
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
}

void roundedRectPath(cairo_t* cr, const Rect& r, double radius) {
    if (radius > r.w / 2.0) {
        radius = r.w / 2.0;
    }
    if (radius > r.h / 2.0) {
        radius = r.h / 2.0;
    }
    const double x2 = r.x + r.w - radius;
    const double y2 = r.y + r.h - radius;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x2, r.y + radius, radius, -kPi / 2.0, 0.0);
    cairo_arc(cr, x2, y2, radius, 0.0, kPi / 2.0);
    cairo_arc(cr, r.x + radius, y2, radius, kPi / 2.0, kPi);
    cairo_arc(cr, r.x + radius, r.y + radius, radius, kPi, kPi * 1.5);
    cairo_close_path(cr);
}

} // namespace

AppView::AppView(const Config& cfg) : cfg_(cfg) {}

AppView::~AppView() {
    if (logo_ != nullptr) {
        cairo_surface_destroy(logo_);
    }
}

void AppView::paint(cairo_t* cr, const AppModel& m, const VideoFrameView* frame) {
    const auto w = m.windowWidth();
    const auto h = m.windowHeight();
    drawBackground(cr, w, h);
    drawTitleBar(cr, w);
    drawCloseButton(cr, w);
    drawVideoWindow(cr, w, h, frame);
}

void AppView::drawBackground(cairo_t* cr, std::int32_t w, std::int32_t h) const {
    argb(cr, cfg_.ui.colorBackground);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_fill(cr);
}

void AppView::drawTitleBar(cairo_t* cr, std::int32_t winW) const {
    const Rect tb = Layout::titleBar(cfg_, winW);
    argb(cr, cfg_.ui.colorTitleBar);
    cairo_rectangle(cr, tb.x, tb.y, tb.w, tb.h);
    cairo_fill(cr);

    double textX = 24.0;
    if (ensureLogo()) {
        const double lw = cairo_image_surface_get_width(logo_);
        const double lh = cairo_image_surface_get_height(logo_);
        const double pad = 12.0;
        const double scale = (tb.h - 2.0 * pad) / lh;
        cairo_surface_set_device_scale(logo_, scale, scale);
        cairo_set_source_surface(cr, logo_, 20.0, (tb.h - lh * scale) / 2.0);
        cairo_paint(cr);
        textX = 20.0 + lw * scale + 16.0;
    }

    argb(cr, cfg_.ui.colorText);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, cfg_.ui.fontSize * 1.6);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, cfg_.title.c_str(), &ext);
    cairo_move_to(cr, textX, (tb.h - ext.height) / 2.0 - ext.y_bearing);
    cairo_show_text(cr, cfg_.title.c_str());
}

void AppView::drawCloseButton(cairo_t* cr, std::int32_t winW) const {
    const Rect cb = Layout::closeHit(cfg_, winW);
    const double pad = cb.w * 0.25;

    argb(cr, cfg_.ui.colorCloseGlyph);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, cb.w / 8.0);
    cairo_move_to(cr, cb.x + pad, cb.y + pad);
    cairo_line_to(cr, cb.x + cb.w - pad, cb.y + cb.h - pad);
    cairo_move_to(cr, cb.x + cb.w - pad, cb.y + pad);
    cairo_line_to(cr, cb.x + pad, cb.y + cb.h - pad);
    cairo_stroke(cr);
}

void AppView::drawVideoWindow(cairo_t* cr, std::int32_t winW, std::int32_t winH,
                              const VideoFrameView* frame) const {
    const std::int32_t vw = (frame != nullptr) ? frame->width : 0;
    const std::int32_t vh = (frame != nullptr) ? frame->height : 0;
    const Rect vr = Layout::videoWindow(cfg_, winW, winH, vw, vh);
    if (vr.w <= 0 || vr.h <= 0) {
        return;
    }

    // 视频帧内容（零拷贝：GStreamer BGRx 与 cairo RGB24 内存序一致）
    if (frame != nullptr && frame->data != nullptr && vw > 0 && vh > 0) {
        cairo_surface_t* img = cairo_image_surface_create_for_data(
            const_cast<std::uint8_t*>(frame->data), CAIRO_FORMAT_RGB24,
            vw, vh, frame->stride);
        cairo_save(cr);
        roundedRectPath(cr, vr, cfg_.ui.videoBorderRadius);
        cairo_clip(cr);
        cairo_translate(cr, vr.x, vr.y);
        cairo_scale(cr, static_cast<double>(vr.w) / vw,
                    static_cast<double>(vr.h) / vh);
        cairo_set_source_surface(cr, img, 0.0, 0.0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(img);
    } else {
        roundedRectPath(cr, vr, cfg_.ui.videoBorderRadius);
        argb(cr, cfg_.ui.colorVideoFill);
        cairo_fill_preserve(cr);

        argb(cr, cfg_.ui.colorText);
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, cfg_.ui.fontSize);
        centerText(cr, vr, cfg_.ui.placeholderText.c_str());
    }

    // 边框统一后画（叠在内容/占位之上）
    cairo_set_line_width(cr, cfg_.ui.videoBorderWidth);
    argb(cr, cfg_.ui.colorBorder);
    roundedRectPath(cr, vr, cfg_.ui.videoBorderRadius);
    cairo_stroke(cr);
}

bool AppView::ensureLogo() const {
    if (logoFailed_) {
        return false;
    }
    if (logo_ == nullptr) {
        if (cfg_.logoPath.empty()) {
            logoFailed_ = true;
            return false;
        }
        logo_ = cairo_image_surface_create_from_png(cfg_.logoPath.c_str());
        if (cairo_surface_status(logo_) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(logo_);
            logo_ = nullptr;
            logoFailed_ = true;
            return false;
        }
    }
    return true;
}

} // namespace aa
