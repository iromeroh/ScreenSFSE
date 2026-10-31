#include "test_pattern.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <windows.h>

namespace
{
constexpr std::uint32_t rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b)
{
    return r | (g << 8) | (b << 16) | 0xFF000000u;   // DXGI R8G8B8A8 byte order
}

std::uint32_t hue(double h)
{
    h = h - std::floor(h);
    const double r = std::clamp(std::abs(h * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    const double g = std::clamp(2.0 - std::abs(h * 6.0 - 2.0), 0.0, 1.0);
    const double b = std::clamp(2.0 - std::abs(h * 6.0 - 4.0), 0.0, 1.0);
    return rgba(static_cast<std::uint32_t>(r * 255), static_cast<std::uint32_t>(g * 255),
                static_cast<std::uint32_t>(b * 255));
}
}

TestPatternSource::TestPatternSource(std::uint32_t width, std::uint32_t height, double fps, bool animated) :
    width_(width), height_(height), fps_(fps > 0.0 ? fps : 30.0), animated_(animated), front_(std::size_t{ width } * height, 0)
{
    worker_ = std::thread([this] { run(); });
}

TestPatternSource::~TestPatternSource()
{
    {
        std::lock_guard lock(runMutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void TestPatternSource::start()
{
    {
        std::lock_guard lock(runMutex_);
        running_ = true;
    }
    wake_.notify_all();
}

void TestPatternSource::stop()
{
    std::lock_guard lock(runMutex_);
    running_ = false;
}

std::uint64_t TestPatternSource::copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch)
{
    std::lock_guard lock(frameMutex_);
    if (frontSeq_ == 0 || frontSeq_ == lastSeq) {
        return 0;
    }
    const std::size_t rowBytes = std::size_t{ width_ } * 4;
    for (std::uint32_t y = 0; y < height_; ++y) {
        std::memcpy(dst + std::size_t{ y } * dstPitch, front_.data() + std::size_t{ y } * width_, rowBytes);
    }
    return frontSeq_;
}

void TestPatternSource::run()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::vector<std::uint32_t> back(std::size_t{ width_ } * height_, 0);
    const auto period = std::chrono::duration<double>(1.0 / fps_);
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t frame = 0;
    auto next = t0;
    for (;;) {
        {
            std::unique_lock lock(runMutex_);
            wake_.wait(lock, [this] { return quit_ || running_; });
            if (quit_) {
                return;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        draw(back, ++frame, std::chrono::duration<double>(now - t0).count());
        {
            std::lock_guard lock(frameMutex_);
            front_.swap(back);
            frontSeq_ = frame;
        }
        if (!animated_) {   // one frame is all a static card needs
            std::unique_lock lock(runMutex_);
            wake_.wait(lock, [this] { return quit_; });
            return;
        }
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
        if (next < now) {
            next = now;
        }
        std::unique_lock lock(runMutex_);
        wake_.wait_until(lock, next, [this] { return quit_; });
        if (quit_) {
            return;
        }
    }
}

// Test card: a slowly cycling hue gradient, seven colour bars, a sweeping white bar (motion and tearing check),
// a bouncing square, and the frame counter as 24 binary cells along the bottom (proves frames really advance).
void TestPatternSource::draw(std::vector<std::uint32_t>& px, std::uint64_t frame, double seconds) const
{
    const std::uint32_t w = width_;
    const std::uint32_t h = height_;
    const std::uint32_t barsBottom = h / 2;
    static constexpr std::uint32_t bars[7] = { rgba(191, 191, 191), rgba(191, 191, 0), rgba(0, 191, 191),
                                               rgba(0, 191, 0),     rgba(191, 0, 191), rgba(191, 0, 0),
                                               rgba(0, 0, 191) };
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint32_t* row = px.data() + std::size_t{ y } * w;
        if (y < barsBottom) {
            for (std::uint32_t x = 0; x < w; ++x) {
                row[x] = bars[std::min<std::uint32_t>(6, x * 7 / w)];
            }
        } else {
            const std::uint32_t c = hue(seconds * 0.1 + double(y) / h * 0.5);
            std::fill(row, row + w, c);
        }
    }
    const std::uint32_t sweep = static_cast<std::uint32_t>(std::fmod(seconds * 0.25, 1.0) * w);
    for (std::uint32_t y = 0; y < h; ++y) {
        std::uint32_t* row = px.data() + std::size_t{ y } * w;
        for (std::uint32_t x = sweep; x < std::min(w, sweep + 12); ++x) {
            row[x] = rgba(255, 255, 255);
        }
    }
    const std::uint32_t side = h / 6;
    const double bx = std::abs(std::fmod(seconds * 0.37, 2.0) - 1.0);
    const double by = std::abs(std::fmod(seconds * 0.53, 2.0) - 1.0);
    const std::uint32_t sx = static_cast<std::uint32_t>(bx * (w - side));
    const std::uint32_t sy = static_cast<std::uint32_t>(by * (h - side));
    for (std::uint32_t y = sy; y < sy + side; ++y) {
        std::fill(px.data() + std::size_t{ y } * w + sx, px.data() + std::size_t{ y } * w + sx + side,
                  rgba(20, 20, 20));
    }
    const std::uint32_t cell = w / 24;
    const std::uint32_t cellTop = h - cell;
    for (std::uint32_t bit = 0; bit < 24; ++bit) {
        const bool on = (frame >> (23 - bit)) & 1u;
        for (std::uint32_t y = cellTop + 2; y < h - 2; ++y) {
            std::uint32_t* row = px.data() + std::size_t{ y } * w;
            std::fill(row + bit * cell + 2, row + (bit + 1) * cell - 2, on ? rgba(255, 255, 255) : rgba(0, 0, 0));
        }
    }
}

std::uint64_t StaticCardSource::copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch)
{
    if (lastSeq == 1) {
        return 0;
    }
    static constexpr std::uint32_t bars[7] = { rgba(150, 150, 150), rgba(150, 150, 0), rgba(0, 150, 150),
                                               rgba(0, 150, 0),     rgba(150, 0, 150), rgba(150, 0, 0),
                                               rgba(0, 0, 150) };
    const std::uint32_t w = width_;
    const std::uint32_t h = height_;
    const std::uint32_t cell = w / 16;
    for (std::uint32_t y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(dst + std::size_t{ y } * dstPitch);
        if (y < h * 2 / 3) {
            for (std::uint32_t x = 0; x < w; ++x) {
                row[x] = bars[std::min<std::uint32_t>(6, x * 7 / w)];
            }
            continue;
        }
        std::fill(row, row + w, rgba(14, 16, 20));
        const bool markerRow = y > h * 3 / 4 && y < h * 3 / 4 + cell;
        if (markerRow) {
            for (int n = 0; n < channelNumber_; ++n) {
                const std::uint32_t x0 = cell + static_cast<std::uint32_t>(n) * cell * 3 / 2;
                std::fill(row + x0, row + std::min(w, x0 + cell), rgba(220, 220, 220));
            }
        }
    }
    return 1;
}

std::uint64_t BlackSource::copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch)
{
    if (lastSeq == 1) {
        return 0;
    }
    for (std::uint32_t y = 0; y < height_; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(dst + std::size_t{ y } * dstPitch);
        std::fill(row, row + width_, rgba(0, 0, 0));
    }
    return 1;
}

NoiseSource::NoiseSource(std::uint32_t width, std::uint32_t height) :
    width_(width), height_(height), tile_(std::size_t{ kTileRows } * kTileWidth * 2), bright_(tile_.size())
{
    std::uint64_t x = seed_;
    for (std::size_t i = 0; i < std::size_t{ kTileRows } * kTileWidth; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const std::uint32_t v = static_cast<std::uint32_t>(x >> 56);   // 0-255
        const std::uint32_t grey = 20 + v * 150 / 255;
        const std::uint32_t lit = std::min<std::uint32_t>(255, grey + 45);
        const std::size_t row = i / kTileWidth, col = i % kTileWidth;
        for (std::size_t copy : { col, col + kTileWidth }) {
            tile_[row * kTileWidth * 2 + copy] = rgba(grey, grey, grey);
            bright_[row * kTileWidth * 2 + copy] = rgba(lit, lit, lit);
        }
    }
}

std::uint64_t NoiseSource::copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch)
{
    using namespace std::chrono;
    const std::uint64_t frame = static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() / 66);
    const std::uint64_t seq = frame + 1;
    if (seq == lastSeq) {
        return 0;
    }
    const std::uint32_t band = std::max<std::uint32_t>(1, height_ / 10);
    const std::uint32_t bandTop = static_cast<std::uint32_t>(frame * 3 % (height_ + band));   // rolls down in ~5 s
    for (std::uint32_t y = 0; y < height_; ++y) {
        seed_ ^= seed_ << 13;
        seed_ ^= seed_ >> 7;
        seed_ ^= seed_ << 17;
        const std::size_t row = static_cast<std::size_t>(seed_ % kTileRows);
        const std::size_t offset = static_cast<std::size_t>((seed_ >> 20) % kTileWidth);
        const bool lit = y + band >= bandTop && y < bandTop;
        const std::uint32_t* src = (lit ? bright_ : tile_).data() + row * kTileWidth * 2 + offset;
        auto* out = reinterpret_cast<std::uint32_t*>(dst + std::size_t{ y } * dstPitch);
        for (std::uint32_t x = 0; x < width_; x += kTileWidth) {
            std::memcpy(out + x, src, std::min(kTileWidth, width_ - x) * sizeof(std::uint32_t));
        }
    }
    return seq;
}
