#pragma once

#include <cstdint>
#include <cstddef>

#include <wayland-client.h>
#include <cairo.h>

namespace aa {

/// RAII wl_shm 共享内存缓冲 + 关联 cairo 图像 surface。
/// move-only：所有权可转移，禁止拷贝。
/// busy 状态由 wl_buffer release 事件维护，配合双缓冲消除撕裂。
class ShmBuffer {
public:
    ShmBuffer() = default;
    ~ShmBuffer();
    ShmBuffer(const ShmBuffer&) = delete;
    ShmBuffer& operator=(const ShmBuffer&) = delete;
    ShmBuffer(ShmBuffer&& other) noexcept;
    ShmBuffer& operator=(ShmBuffer&& other) noexcept;

    bool create(wl_shm* shm, std::int32_t w, std::int32_t h);
    void destroy();

    wl_buffer* wlBuffer() const { return buffer_; }
    cairo_surface_t* cairoSurface() const { return cairoSurf_; }
    std::int32_t width() const { return w_; }
    std::int32_t height() const { return h_; }
    bool busy() const { return busy_; }
    void setBusy(bool b) { busy_ = b; }
    void handleRelease() { busy_ = false; }

private:
    int fd_ = -1;
    void* data_ = nullptr;
    std::size_t size_ = 0;
    wl_shm_pool* pool_ = nullptr;
    wl_buffer* buffer_ = nullptr;
    cairo_surface_t* cairoSurf_ = nullptr;
    std::int32_t w_ = 0;
    std::int32_t h_ = 0;
    std::int32_t stride_ = 0;
    bool busy_ = false;
};

} // namespace aa
