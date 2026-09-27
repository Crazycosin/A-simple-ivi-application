#include "wayland/ShmBuffer.h"

#include <sys/mman.h>
#include <unistd.h>

namespace aa {

namespace {

/// wl_buffer release 事件 thunk（合成器用完该 buffer 时解除 busy）
constexpr wl_buffer_listener kBufferListener = {
    [](void* data, wl_buffer* /*buffer*/) {
        static_cast<ShmBuffer*>(data)->handleRelease();
    },
};

} // namespace

ShmBuffer::~ShmBuffer() {
    destroy();
}

ShmBuffer::ShmBuffer(ShmBuffer&& other) noexcept
    : fd_(other.fd_),
      data_(other.data_),
      size_(other.size_),
      pool_(other.pool_),
      buffer_(other.buffer_),
      cairoSurf_(other.cairoSurf_),
      w_(other.w_),
      h_(other.h_),
      stride_(other.stride_),
      busy_(other.busy_) {
    other.fd_ = -1;
    other.data_ = nullptr;
    other.size_ = 0;
    other.pool_ = nullptr;
    other.buffer_ = nullptr;
    other.cairoSurf_ = nullptr;
    other.w_ = other.h_ = other.stride_ = 0;
    other.busy_ = false;
}

ShmBuffer& ShmBuffer::operator=(ShmBuffer&& other) noexcept {
    if (this != &other) {
        destroy();
        fd_ = other.fd_;
        data_ = other.data_;
        size_ = other.size_;
        pool_ = other.pool_;
        buffer_ = other.buffer_;
        cairoSurf_ = other.cairoSurf_;
        w_ = other.w_;
        h_ = other.h_;
        stride_ = other.stride_;
        busy_ = other.busy_;
        other.fd_ = -1;
        other.data_ = nullptr;
        other.size_ = 0;
        other.pool_ = nullptr;
        other.buffer_ = nullptr;
        other.cairoSurf_ = nullptr;
        other.w_ = other.h_ = other.stride_ = 0;
        other.busy_ = false;
    }
    return *this;
}

bool ShmBuffer::create(wl_shm* shm, std::int32_t w, std::int32_t h) {
    destroy();
    if (shm == nullptr || w <= 0 || h <= 0) {
        return false;
    }
    stride_ = w * 4;
    size_ = static_cast<std::size_t>(stride_) * static_cast<std::size_t>(h);

    fd_ = ::memfd_create("androidauto", MFD_CLOEXEC);
    if (fd_ < 0) {
        return false;
    }
    if (::ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
        destroy();
        return false;
    }
    data_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        destroy();
        return false;
    }

    pool_ = wl_shm_create_pool(shm, fd_, static_cast<std::int32_t>(size_));
    if (pool_ == nullptr) {
        destroy();
        return false;
    }
    buffer_ = wl_shm_pool_create_buffer(pool_, 0, w, h, stride_,
                                        WL_SHM_FORMAT_ARGB8888);
    if (buffer_ == nullptr) {
        destroy();
        return false;
    }

    cairoSurf_ = cairo_image_surface_create_for_data(
        static_cast<unsigned char*>(data_), CAIRO_FORMAT_ARGB32, w, h, stride_);
    if (cairo_surface_status(cairoSurf_) != CAIRO_STATUS_SUCCESS) {
        destroy();
        return false;
    }

    wl_buffer_add_listener(buffer_, &kBufferListener, this);
    w_ = w;
    h_ = h;
    return true;
}

void ShmBuffer::destroy() {
    // cairo surface 引用 mmap 数据，必须最先销毁
    if (cairoSurf_ != nullptr) {
        cairo_surface_destroy(cairoSurf_);
        cairoSurf_ = nullptr;
    }
    if (buffer_ != nullptr) {
        wl_buffer_destroy(buffer_);
        buffer_ = nullptr;
    }
    if (pool_ != nullptr) {
        wl_shm_pool_destroy(pool_);
        pool_ = nullptr;
    }
    if (data_ != nullptr) {
        ::munmap(data_, size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    w_ = 0;
    h_ = 0;
    stride_ = 0;
    size_ = 0;
    busy_ = false;
}

} // namespace aa
