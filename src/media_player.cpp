#include "media_player.h"

#include "logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>

namespace
{
constexpr std::size_t kFrameQueue = 6;       // decoded frames kept ahead (about 0.2 s at 30 fps)
constexpr UINT32 kAudioBuffersQueued = 6;    // XAudio2 buffers kept queued (one decoded sample each, ~20-40 ms)
constexpr std::int64_t kTicksPerSecond = 10'000'000;

template <class T>
void release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string hr(HRESULT value)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(value));
    return buf;
}

std::string narrow(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>((std::max)(size, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

struct ComScope
{
    HRESULT result;
    ComScope() :
        result(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {}
    ~ComScope()
    {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};
}

MediaPlayer::MediaPlayer(Logger& logger, std::uint32_t width, std::uint32_t height) :
    logger_(logger), width_(width), height_(height)
{
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
}

MediaPlayer::~MediaPlayer()
{
    closeAll();
    MFShutdown();
}

void MediaPlayer::closeAll()
{
    quit_.store(true);
    framesCv_.notify_all();
    audioCv_.notify_all();
    if (videoThread_.joinable()) {
        videoThread_.join();
    }
    if (audioThread_.joinable()) {
        audioThread_.join();
    }
}

bool MediaPlayer::open(const std::filesystem::path& file, double startSeconds)
{
    start_ = static_cast<std::int64_t>(std::max(0.0, startSeconds) * kTicksPerSecond);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        fail("file not found: " + narrow(file.wstring()));
        return false;
    }
    logger_.info("Opening video " + narrow(file.wstring()));
    videoEnded_.store(false);
    audioEnded_.store(false);
    playing_.store(true);
    {
        std::lock_guard lock(clockMutex_);
        wallStart_ = std::chrono::steady_clock::now();
        wallPausedAt_ = start_;
    }
    videoThread_ = std::thread([this, file] { videoLoop(file); });
    audioThread_ = std::thread([this, file] { audioLoop(file); });
    return true;
}

void MediaPlayer::play()
{
    if (playing_.exchange(true)) {
        return;
    }
    {
        std::lock_guard lock(clockMutex_);
        wallStart_ = std::chrono::steady_clock::now();
    }
    std::lock_guard lock(audioMutex_);
    if (voice_) {
        voice_->Start(0);
    }
}

void MediaPlayer::pause()
{
    if (!playing_.load()) {
        return;
    }
    {
        std::lock_guard lock(clockMutex_);
        wallPausedAt_ += std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, kTicksPerSecond>>>(
                             std::chrono::steady_clock::now() - wallStart_).count();
    }
    playing_.store(false);
    std::lock_guard lock(audioMutex_);
    if (voice_) {
        voice_->Stop(0);
    }
}

bool MediaPlayer::finished() const
{
    if (!videoEnded_.load() || !audioEnded_.load()) {
        return false;
    }
    // with sound: its end is the end (all of it has played). Without: the clock has passed the last frame.
    // Never "the frame queue is empty": only the renderer drains it, and nothing renders an unseen screen.
    return hasAudio_.load() || clock() >= lastFramePts_.load();
}

double MediaPlayer::positionSeconds() const
{
    return static_cast<double>(clock()) / kTicksPerSecond;
}

std::string MediaPlayer::lastError() const
{
    std::lock_guard lock(errorMutex_);
    return lastError_;
}

void MediaPlayer::fail(const std::string& message)
{
    logger_.warn("Video: " + message);
    std::lock_guard lock(errorMutex_);
    lastError_ = message;
}

void MediaPlayer::setGains(float left, float right)
{
    std::lock_guard lock(audioMutex_);
    gainL_ = std::clamp(left, 0.0f, 1.0f);
    gainR_ = std::clamp(right, 0.0f, 1.0f);
    applyGains();
}

// Caller holds audioMutex_. Source channels -> the first two speaker channels (front left/right).
void MediaPlayer::applyGains()
{
    if (!voice_ || channels_ == 0) {
        return;
    }
    std::vector<float> m(static_cast<std::size_t>(channels_) * masterChannels_, 0.0f);
    auto set = [&](std::uint32_t src, std::uint32_t dst, float g) {
        if (dst < masterChannels_) {
            m[static_cast<std::size_t>(dst) * channels_ + src] = g;
        }
    };
    if (channels_ == 1) {
        set(0, 0, gainL_);
        set(0, 1, gainR_);
    } else {
        for (std::uint32_t c = 0; c < channels_; ++c) {
            // stereo L/R as they are; 5.1 and up: centre to both, LFE dropped, the rest alternate L/R, all quieter
            if (c == 0) {
                set(c, 0, gainL_);
            } else if (c == 1) {
                set(c, 1, gainR_);
            } else if (c == 2) {
                set(c, 0, gainL_ * 0.7f);
                set(c, 1, gainR_ * 0.7f);
            } else if (c != 3) {
                set(c, c % 2, (c % 2 ? gainR_ : gainL_) * 0.6f);
            }
        }
    }
    voice_->SetOutputMatrix(nullptr, channels_, masterChannels_, m.data());
}

std::int64_t MediaPlayer::clock() const
{
    if (hasAudio_.load()) {
        std::lock_guard lock(audioMutex_);
        const std::int64_t base = audioBasePts_.load();
        if (!voice_ || !audioBaseSet_.load() || sampleRate_ == 0) {
            return 0;
        }
        XAUDIO2_VOICE_STATE state{};
        voice_->GetState(&state, 0);
        return std::max<std::int64_t>(0, base + static_cast<std::int64_t>(state.SamplesPlayed * kTicksPerSecond / sampleRate_));
    }
    std::lock_guard lock(clockMutex_);
    if (!playing_.load()) {
        return wallPausedAt_;
    }
    return wallPausedAt_ + std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, kTicksPerSecond>>>(
                               std::chrono::steady_clock::now() - wallStart_).count();
}

std::uint64_t MediaPlayer::copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch)
{
    if (!audioReady_.load()) {
        return 0;
    }
    const std::int64_t now = clock();
    std::unique_lock lock(framesMutex_);
    bool dropped = false;
    while (frames_.size() >= 2 && frames_[1].pts <= now) {
        pool_.push_back(std::move(frames_.front()));
        frames_.pop_front();
        dropped = true;
    }
    if (frames_.empty()) {
        return 0;
    }
    const Frame& f = frames_.front();
    // the first frame shows at once (it is the picture before playback has really started)
    if (f.seq == lastSeq || (lastSeq != 0 && f.pts > now + kTicksPerSecond / 50)) {
        if (dropped) {
            framesCv_.notify_all();
        }
        return 0;
    }
    const std::size_t rowBytes = std::size_t{ width_ } * 4;
    for (std::uint32_t y = 0; y < height_; ++y) {
        std::memcpy(dst + std::size_t{ y } * dstPitch, f.pixels.data() + std::size_t{ y } * width_, rowBytes);
    }
    const std::uint64_t seq = f.seq;
    lock.unlock();
    if (dropped) {
        framesCv_.notify_all();
    }
    return seq;
}

void MediaPlayer::seekReader(void* r, const char* what)
{
    if (start_ <= 0) {
        return;
    }
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = start_;
    const HRESULT h = static_cast<IMFSourceReader*>(r)->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);
    if (FAILED(h)) {
        fail(std::string("cannot seek the ") + what + " (" + hr(h) + "); starting from the beginning");
    }
}

void MediaPlayer::noteDuration(void* r)
{
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(static_cast<IMFSourceReader*>(r)->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                                              MF_PD_DURATION, &value)) && value.vt == VT_UI8) {
        duration_.store(static_cast<double>(value.uhVal.QuadPart) / kTicksPerSecond);
    }
    PropVariantClear(&value);
}

void MediaPlayer::videoLoop(std::filesystem::path file)
{
    ComScope com;
    IMFAttributes* attrs = nullptr;
    IMFSourceReader* reader = nullptr;
    IMFMediaType* native = nullptr;
    IMFMediaType* out = nullptr;
    IMFMediaType* current = nullptr;
    auto cleanup = [&] {
        release(current);
        release(out);
        release(native);
        release(reader);
        release(attrs);
        videoEnded_.store(true);
        framesCv_.notify_all();
    };

    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    HRESULT h = MFCreateSourceReaderFromURL(file.c_str(), attrs, &reader);
    if (FAILED(h)) {
        fail("cannot open the file for video (" + hr(h) + ")");
        cleanup();
        return;
    }
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE)) ||
        FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native))) {
        logger_.info("Video: the file has no video stream.");
        cleanup();
        return;
    }
    UINT32 vw = 0, vh = 0, parN = 1, parD = 1;
    MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &vw, &vh);
    if (FAILED(MFGetAttributeRatio(native, MF_MT_PIXEL_ASPECT_RATIO, &parN, &parD)) || parN == 0 || parD == 0) {
        parN = parD = 1;
    }
    GUID codec{};
    native->GetGUID(MF_MT_SUBTYPE, &codec);
    if (vw == 0 || vh == 0) {
        fail("video stream without a frame size");
        cleanup();
        return;
    }
    // letterbox: the largest even-sized rectangle of the video's display aspect inside the screen
    const double aspect = (static_cast<double>(vw) * parN) / (static_cast<double>(vh) * parD);
    std::uint32_t fitW = width_, fitH = height_;
    if (aspect > static_cast<double>(width_) / height_) {
        fitH = static_cast<std::uint32_t>(std::lround(width_ / aspect));
    } else {
        fitW = static_cast<std::uint32_t>(std::lround(height_ * aspect));
    }
    fitW = std::clamp<std::uint32_t>(fitW & ~1u, 2, width_);
    fitH = std::clamp<std::uint32_t>(fitH & ~1u, 2, height_);

    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(out, MF_MT_FRAME_SIZE, fitW, fitH);
    MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    bool scaledByMf = SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, out));
    if (!scaledByMf) {
        out->DeleteItem(MF_MT_FRAME_SIZE);
        out->DeleteItem(MF_MT_PIXEL_ASPECT_RATIO);
        h = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, out);
        if (FAILED(h)) {
            fail("no RGB32 conversion for this video codec (" + hr(h) + ")");
            cleanup();
            return;
        }
    }
    UINT32 srcW = 0, srcH = 0;
    LONG stride = 0;
    auto readCurrent = [&] {
        release(current);
        reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current);
        MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &srcW, &srcH);
        stride = static_cast<LONG>(MFGetAttributeUINT32(current, MF_MT_DEFAULT_STRIDE, srcW * 4));
    };
    readCurrent();
    noteDuration(reader);
    seekReader(reader, "video");
    const bool cpuScale = srcW != fitW || srcH != fitH;
    char codecText[8] = {};
    std::memcpy(codecText, &codec.Data1, 4);
    logger_.info("Video stream " + std::string(codecText) + " " + std::to_string(vw) + "x" + std::to_string(vh) +
                 " -> " + std::to_string(fitW) + "x" + std::to_string(fitH) + " on the screen (" +
                 (cpuScale ? "CPU scaling from " + std::to_string(srcW) + "x" + std::to_string(srcH) : std::string("scaled by MF")) + ")");
    hasVideo_ = true;
    const std::uint32_t offX = (width_ - fitW) / 2;
    const std::uint32_t offY = (height_ - fitH) / 2;
    std::vector<std::uint32_t> xmap(fitW);
    for (std::uint32_t x = 0; x < fitW; ++x) {
        xmap[x] = cpuScale ? static_cast<std::uint32_t>(std::uint64_t{ x } * srcW / fitW) : x;
    }

    std::uint64_t decoded = 0;
    while (!quit_.load()) {
        {
            std::unique_lock lock(framesMutex_);
            // Frames are normally consumed by copyLatest (the renderer). When nothing renders the screen, drop the
            // frames whose time has passed here, so decoding (and the end of the video) keeps up with the clock.
            while (!quit_.load() && frames_.size() >= kFrameQueue) {
                lock.unlock();
                const std::int64_t now = clock();
                lock.lock();
                while (frames_.size() >= 2 && frames_[1].pts <= now) {
                    pool_.push_back(std::move(frames_.front()));
                    frames_.pop_front();
                }
                if (frames_.size() >= kFrameQueue) {
                    framesCv_.wait_for(lock, std::chrono::milliseconds(50));
                }
            }
            if (quit_.load()) {
                break;
            }
        }
        DWORD index = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
        h = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &index, &flags, &ts, &sample);
        if (FAILED(h)) {
            fail("video decoding failed (" + hr(h) + ")");
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            release(sample);
            break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            readCurrent();
        }
        if (!sample) {
            continue;
        }
        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
            release(sample);
            continue;
        }
        Frame frame;
        {
            std::lock_guard lock(framesMutex_);
            if (!pool_.empty()) {
                frame = std::move(pool_.back());
                pool_.pop_back();
            }
        }
        if (frame.pixels.size() != std::size_t{ width_ } * height_) {
            frame.pixels.assign(std::size_t{ width_ } * height_, 0xFF000000u);   // black bars stay black
        }
        BYTE* data = nullptr;
        LONG pitch = 0;
        IMF2DBuffer* buffer2d = nullptr;
        bool locked2d = SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))) && SUCCEEDED(buffer2d->Lock2D(&data, &pitch));
        DWORD length = 0;
        if (!locked2d && SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
            pitch = stride;
            if (pitch < 0) {   // bottom-up: the first row is the last in memory
                data += static_cast<std::size_t>(srcH - 1) * static_cast<std::size_t>(-pitch);
            }
        }
        if (data && srcW && srcH) {
            for (std::uint32_t y = 0; y < fitH; ++y) {
                const std::uint32_t sy = cpuScale ? static_cast<std::uint32_t>(std::uint64_t{ y } * srcH / fitH) : y;
                const auto* src = reinterpret_cast<const std::uint32_t*>(data + static_cast<std::ptrdiff_t>(sy) * pitch);
                std::uint32_t* row = frame.pixels.data() + std::size_t{ offY + y } * width_ + offX;
                for (std::uint32_t x = 0; x < fitW; ++x) {
                    const std::uint32_t v = src[xmap[x]];   // B G R X in memory
                    row[x] = ((v >> 16) & 0xFFu) | (v & 0xFF00u) | ((v & 0xFFu) << 16) | 0xFF000000u;
                }
            }
        }
        if (locked2d) {
            buffer2d->Unlock2D();
        } else if (data) {
            buffer->Unlock();
        }
        release(buffer2d);
        release(buffer);
        release(sample);
        if (ts + kTicksPerSecond / 10 < start_) {   // decoded from the keyframe before a seek target: skip
            std::lock_guard lock(framesMutex_);
            pool_.push_back(std::move(frame));
            continue;
        }
        frame.pts = ts;
        lastFramePts_.store(ts);
        {
            std::lock_guard lock(framesMutex_);
            frame.seq = nextSeq_++;
            frames_.push_back(std::move(frame));
        }
        ++decoded;
    }
    logger_.info("Video stream ended after " + std::to_string(decoded) + " frames.");
    cleanup();
}

void MediaPlayer::audioLoop(std::filesystem::path file)
{
    ComScope com;
    IMFSourceReader* reader = nullptr;
    IMFMediaType* pcm = nullptr;
    IMFMediaType* current = nullptr;
    WAVEFORMATEX* wfx = nullptr;
    std::deque<std::vector<BYTE>> inflight;   // XAudio2 reads these until their buffer has played
    auto finishSetup = [&](bool sound) {
        hasAudio_.store(sound);
        if (!sound) {
            audioEnded_.store(true);
        }
        audioReady_.store(true);
    };
    auto cleanup = [&] {
        {
            std::lock_guard lock(audioMutex_);
            if (voice_) {
                voice_->Stop(0);
                voice_->DestroyVoice();
                voice_ = nullptr;
            }
            if (master_) {
                master_->DestroyVoice();
                master_ = nullptr;
            }
            release(xaudio_);
        }
        inflight.clear();
        if (wfx) {
            CoTaskMemFree(wfx);
        }
        release(current);
        release(pcm);
        release(reader);
    };

    HRESULT h = MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader);
    if (FAILED(h)) {
        fail("cannot open the file for sound (" + hr(h) + ")");
        finishSetup(false);
        cleanup();
        return;
    }
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    MFCreateMediaType(&pcm);
    pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    UINT32 size = 0;
    if (FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE)) ||
        FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pcm)) ||
        FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &current)) ||
        FAILED(MFCreateWaveFormatExFromMFMediaType(current, &wfx, &size))) {
        logger_.info("Video: no playable sound stream; playing silent.");
        finishSetup(false);
        cleanup();
        return;
    }
    noteDuration(reader);
    seekReader(reader, "sound");
    {
        std::lock_guard lock(audioMutex_);
        h = XAudio2Create(&xaudio_, 0, XAUDIO2_USE_DEFAULT_PROCESSOR);
        if (SUCCEEDED(h)) {
            h = xaudio_->CreateMasteringVoice(&master_);
        }
        if (SUCCEEDED(h)) {
            XAUDIO2_VOICE_DETAILS details{};
            master_->GetVoiceDetails(&details);
            masterChannels_ = (std::max)(details.InputChannels, 2u);
            h = xaudio_->CreateSourceVoice(&voice_, wfx);
        }
        if (SUCCEEDED(h)) {
            sampleRate_ = wfx->nSamplesPerSec;
            channels_ = wfx->nChannels;
            applyGains();
            if (playing_.load()) {
                voice_->Start(0);
            }
        }
    }
    if (FAILED(h)) {
        fail("XAudio2 output failed (" + hr(h) + "); playing silent");
        finishSetup(false);
        cleanup();
        return;
    }
    logger_.info("Sound stream " + std::to_string(sampleRate_) + " Hz, " + std::to_string(channels_) + " channel(s).");
    finishSetup(true);

    bool ended = false;
    while (!quit_.load()) {
        XAUDIO2_VOICE_STATE state{};
        {
            std::lock_guard lock(audioMutex_);
            voice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        }
        while (inflight.size() > state.BuffersQueued) {
            inflight.pop_front();
        }
        if (ended) {
            if (state.BuffersQueued == 0) {
                audioEnded_.store(true);
            }
            std::unique_lock lock(audioMutex_);
            audioCv_.wait_for(lock, std::chrono::milliseconds(50), [this] { return quit_.load(); });
            continue;
        }
        if (state.BuffersQueued >= kAudioBuffersQueued) {
            std::unique_lock lock(audioMutex_);
            audioCv_.wait_for(lock, std::chrono::milliseconds(10), [this] { return quit_.load(); });
            continue;
        }
        DWORD index = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
        h = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &index, &flags, &ts, &sample);
        if (FAILED(h) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            if (FAILED(h)) {
                fail("sound decoding failed (" + hr(h) + ")");
            }
            release(sample);
            ended = true;   // no Discontinuity(): XAudio2 resets SamplesPlayed after an end-of-stream buffer
            continue;
        }
        if (!sample) {
            continue;
        }
        IMFMediaBuffer* buffer = nullptr;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && SUCCEEDED(buffer->Lock(&data, nullptr, &length)) && length) {
            inflight.emplace_back(data, data + length);
            buffer->Unlock();
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = length;
            xb.pAudioData = inflight.back().data();
            if (!audioBaseSet_.load()) {   // AAC often starts slightly before 0 (encoder priming)
                audioBasePts_.store(ts);
                audioBaseSet_.store(true);   // only this thread writes; the clock reads the pts after the flag
            }
            std::lock_guard lock(audioMutex_);
            if (FAILED(voice_->SubmitSourceBuffer(&xb))) {
                inflight.pop_back();
            }
        }
        release(buffer);
        release(sample);
    }
    cleanup();
}
