#include "slideshow_player.h"

#include "logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace
{
using Microsoft::WRL::ComPtr;

std::string narrowPath(const std::filesystem::path& p)
{
    const std::wstring w = p.wstring();
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// out = a * (1 - t) + b * t per byte, t in 0..256.
void blend(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b, std::vector<std::uint32_t>& out, std::uint32_t t)
{
    const std::uint32_t s = 256 - t;
    const std::size_t n = std::min(a.size(), b.size());
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t x = a[i], y = b[i];
        const std::uint32_t rb = (((x & 0x00FF00FFu) * s + (y & 0x00FF00FFu) * t) >> 8) & 0x00FF00FFu;
        const std::uint32_t ga = (((x >> 8) & 0x00FF00FFu) * s + ((y >> 8) & 0x00FF00FFu) * t) & 0xFF00FF00u;
        out[i] = rb | ga;
    }
}
}

bool SlideshowPlayer::pictureFile(const std::filesystem::path& file)
{
    const std::wstring ext = file.extension().wstring();
    for (const wchar_t* e : { L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".tif", L".tiff", L".jxr", L".wdp", L".webp",
                              L".heic", L".heif" }) {
        if (_wcsicmp(ext.c_str(), e) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<std::uint32_t> SlideshowPlayer::decode(const std::filesystem::path& file, std::uint32_t width, std::uint32_t height,
                                                   std::string& error)
{
    auto fail = [&](const char* what, HRESULT hr) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s (0x%08lX)", what, static_cast<unsigned long>(hr));
        error = buf;
        return std::vector<std::uint32_t>{};
    };
    if (width == 0 || height == 0) return fail("no screen size", E_INVALIDARG);
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return fail("Windows Imaging Component unavailable", hr);
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return fail("not a picture Windows can read", hr);
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return fail("no picture in the file", hr);
    UINT sw = 0, sh = 0;
    hr = frame->GetSize(&sw, &sh);
    if (FAILED(hr) || sw == 0 || sh == 0) return fail("empty picture", hr);

    // EXIF orientation (photos from phones and cameras are often stored sideways).
    WICBitmapTransformOptions turn = WICBitmapTransformRotate0;
    ComPtr<IWICMetadataQueryReader> meta;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&meta))) {
        for (const wchar_t* query : { L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}" }) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(meta->GetMetadataByName(query, &v))) {
                const int orientation = v.vt == VT_UI2 ? v.uiVal : 1;
                PropVariantClear(&v);
                switch (orientation) {
                case 2: turn = WICBitmapTransformFlipHorizontal; break;
                case 3: turn = WICBitmapTransformRotate180; break;
                case 4: turn = WICBitmapTransformFlipVertical; break;
                case 5: turn = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal); break;
                case 6: turn = WICBitmapTransformRotate90; break;
                case 7: turn = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal); break;
                case 8: turn = WICBitmapTransformRotate270; break;
                default: break;
                }
                break;
            }
        }
    }
    const bool sideways = (turn & 1) != 0;   // Rotate90 / Rotate270
    const double ow = sideways ? sh : sw, oh = sideways ? sw : sh;
    const double scale = std::min(width / ow, height / oh);
    const UINT fw = std::clamp<UINT>(static_cast<UINT>(std::lround(ow * scale)), 1, width);
    const UINT fh = std::clamp<UINT>(static_cast<UINT>(std::lround(oh * scale)), 1, height);

    ComPtr<IWICBitmapScaler> scaler;
    hr = factory->CreateBitmapScaler(&scaler);
    if (FAILED(hr)) return fail("no scaler", hr);
    hr = scaler->Initialize(frame.Get(), sideways ? fh : fw, sideways ? fw : fh, WICBitmapInterpolationModeHighQualityCubic);
    if (FAILED(hr)) hr = scaler->Initialize(frame.Get(), sideways ? fh : fw, sideways ? fw : fh, WICBitmapInterpolationModeFant);
    if (FAILED(hr)) return fail("cannot scale the picture", hr);
    ComPtr<IWICBitmapSource> source = scaler;
    if (turn != WICBitmapTransformRotate0) {
        ComPtr<IWICBitmapFlipRotator> rotator;
        if (SUCCEEDED(factory->CreateBitmapFlipRotator(&rotator)) && SUCCEEDED(rotator->Initialize(source.Get(), turn))) {
            source = rotator;
        }
    }
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        // premultiplied: transparent parts come out black, as over the black bars
        hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) return fail("cannot convert the picture", hr);
    std::vector<std::uint32_t> fitted(std::size_t{ fw } * fh);
    hr = converter->CopyPixels(nullptr, fw * 4, static_cast<UINT>(fitted.size() * 4), reinterpret_cast<BYTE*>(fitted.data()));
    if (FAILED(hr)) return fail("cannot decode the picture", hr);

    std::vector<std::uint32_t> out(std::size_t{ width } * height, 0xFF000000u);
    const UINT x0 = (width - fw) / 2, y0 = (height - fh) / 2;
    for (UINT y = 0; y < fh; ++y) {
        const std::uint32_t* src = fitted.data() + std::size_t{ y } * fw;
        std::uint32_t* dst = out.data() + std::size_t{ y0 + y } * width + x0;
        for (UINT x = 0; x < fw; ++x) dst[x] = src[x] | 0xFF000000u;
    }
    return out;
}

SlideshowPlayer::SlideshowPlayer(Logger& logger, std::uint32_t width, std::uint32_t height, std::vector<std::filesystem::path> files,
                                 std::size_t index, double seconds) :
    logger_(logger), width_(width), height_(height), files_(std::move(files)), seconds_(std::max(1.0, seconds))
{
    bad_.assign(files_.size(), false);
    index_ = files_.empty() ? 0 : index % files_.size();
}

SlideshowPlayer::~SlideshowPlayer()
{
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void SlideshowPlayer::start(double at)
{
    std::lock_guard lock(mutex_);
    if (worker_.joinable()) return;
    if (files_.empty()) {
        error_ = "no pictures";
        finished_ = true;
        return;
    }
    startAt_ = std::clamp(at, 0.0, std::max(0.0, seconds_ - 0.5));
    worker_ = std::thread([this] { run(); });
}

void SlideshowPlayer::show(std::size_t index)
{
    {
        std::lock_guard lock(mutex_);
        if (files_.empty()) return;
        index_ = index % files_.size();
        jump_ = index_;
    }
    wake_.notify_all();
}

std::size_t SlideshowPlayer::index() const
{
    std::lock_guard lock(mutex_);
    return index_;
}

void SlideshowPlayer::play()
{
    {
        std::lock_guard lock(mutex_);
        if (!paused_) return;
        clockStart_ += Clock::now() - pausedAt_;
        paused_ = false;
    }
    wake_.notify_all();
}

void SlideshowPlayer::pause()
{
    {
        std::lock_guard lock(mutex_);
        if (paused_) return;
        paused_ = true;
        pausedAt_ = Clock::now();
    }
    wake_.notify_all();
}

bool SlideshowPlayer::playing() const
{
    std::lock_guard lock(mutex_);
    return !paused_ && !finished_;
}

bool SlideshowPlayer::finished() const
{
    std::lock_guard lock(mutex_);
    return finished_;
}

double SlideshowPlayer::positionSeconds() const
{
    std::lock_guard lock(mutex_);
    if (clockStart_ == Clock::time_point{}) return startAt_;   // the first picture is not up yet
    const double t = std::chrono::duration<double>((paused_ ? pausedAt_ : Clock::now()) - clockStart_).count();
    return std::clamp(t, 0.0, seconds_);
}

std::string SlideshowPlayer::lastError() const
{
    std::lock_guard lock(mutex_);
    return error_;
}

std::uint64_t SlideshowPlayer::copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch)
{
    std::lock_guard lock(frameMutex_);
    if (frontSeq_ == 0 || frontSeq_ == lastSeq || front_.size() != std::size_t{ width_ } * height_) {
        return 0;
    }
    const std::size_t rowBytes = std::size_t{ width_ } * 4;
    for (std::uint32_t y = 0; y < height_; ++y) {
        std::memcpy(dst + std::size_t{ y } * dstPitch, front_.data() + std::size_t{ y } * width_, rowBytes);
    }
    return frontSeq_;
}

void SlideshowPlayer::publish(const std::vector<std::uint32_t>& pixels)
{
    std::lock_guard lock(frameMutex_);
    front_ = pixels;
    ++frontSeq_;
}

std::size_t SlideshowPlayer::nextReadable(std::size_t from) const
{
    const std::size_t n = files_.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t k = (from + i) % n;
        if (!bad_[k]) return k;
    }
    return from % n;
}

void SlideshowPlayer::run()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::size_t n = files_.size();
    std::vector<std::uint32_t> shown(std::size_t{ width_ } * height_, 0xFF000000u), from, to, mixed;
    std::vector<std::uint32_t> pre;
    std::optional<std::size_t> preIndex;
    publish(shown);   // black; the first picture fades in from it

    std::unique_lock lock(mutex_);
    std::optional<std::size_t> want = index_;
    bool first = true;
    bool fading = false;
    Clock::time_point fadeStart{};
    while (!quit_) {
        if (jump_) {
            want = *jump_;
            jump_.reset();
        }
        if (want) {
            std::size_t got = *want;
            want.reset();
            std::vector<std::uint32_t> img;
            for (std::size_t tries = 0; tries < n && img.empty() && !quit_ && !jump_; ++tries) {
                if (bad_[got]) {
                    got = (got + 1) % n;
                    continue;
                }
                if (preIndex == got) {
                    img = std::move(pre);
                    preIndex.reset();
                    break;
                }
                lock.unlock();
                std::string why;
                auto px = decode(files_[got], width_, height_, why);
                lock.lock();
                if (px.empty()) {
                    bad_[got] = true;
                    logger_.warn("Slideshow: skipping " + narrowPath(files_[got]) + ": " + why);
                    got = (got + 1) % n;
                } else {
                    img = std::move(px);
                }
            }
            if (quit_) break;
            if (jump_) continue;   // a newer choice came in while decoding
            if (img.empty()) {
                error_ = "no readable pictures";
                finished_ = true;
                logger_.warn("Slideshow: none of the " + std::to_string(n) + " pictures can be read.");
                wake_.wait(lock, [this] { return quit_; });
                break;
            }
            const auto now = Clock::now();
            index_ = got;
            clockStart_ = now - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(first ? startAt_ : 0.0));
            if (paused_) pausedAt_ = now;
            first = false;
            from = shown;
            to = std::move(img);
            fading = true;
            fadeStart = now;
        }
        if (fading) {
            const double t = std::chrono::duration<double>(Clock::now() - fadeStart).count() / kFadeSeconds;
            lock.unlock();
            if (t >= 1.0) {
                shown = to;
                fading = false;
            } else {
                const double eased = t * t * (3.0 - 2.0 * t);
                blend(from, to, mixed, static_cast<std::uint32_t>(eased * 256.0));
                shown.swap(mixed);
            }
            publish(shown);
            lock.lock();
            if (fading) {
                wake_.wait_for(lock, std::chrono::milliseconds(33), [this] { return quit_ || jump_.has_value(); });
            }
            continue;
        }
        const std::size_t next = nextReadable((index_ + 1) % n);
        if (next == index_) {   // one picture (or only one readable): it stays
            wake_.wait(lock, [this] { return quit_ || jump_.has_value(); });
            continue;
        }
        if (preIndex != next) {   // decode the next picture ahead of its turn
            const auto file = files_[next];
            lock.unlock();
            std::string why;
            auto px = decode(file, width_, height_, why);
            lock.lock();
            if (px.empty()) {
                bad_[next] = true;
                logger_.warn("Slideshow: skipping " + narrowPath(file) + ": " + why);
            } else {
                pre = std::move(px);
                preIndex = next;
            }
            continue;
        }
        if (paused_) {
            wake_.wait(lock, [this] { return quit_ || jump_.has_value() || !paused_; });
            continue;
        }
        const auto due = clockStart_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds_));
        if (Clock::now() >= due) {
            index_ = next;
            want = next;
            continue;
        }
        wake_.wait_until(lock, due, [this] { return quit_ || jump_.has_value() || paused_; });
    }
    lock.unlock();
    if (SUCCEEDED(co)) CoUninitialize();
}
