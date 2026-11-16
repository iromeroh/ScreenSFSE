#pragma once

#include "playback.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class Logger;

// A picture slideshow (playlist option |slideshow=<seconds>): each picture shows for `seconds`, then cross-fades into
// the next; the list loops. Pictures are decoded with WIC (JPEG, PNG, BMP, GIF, TIFF, JPEG XR, and WebP/HEIC when
// Windows has their codecs) on the player's own thread, turned by their EXIF orientation, and fitted to the screen
// with black bars. Silent. Position = seconds into the current picture, duration = `seconds`. Unreadable pictures are
// skipped; finished() only turns true when none can be read.
class SlideshowPlayer : public Playback
{
public:
    static constexpr double kFadeSeconds = 1.5;

    SlideshowPlayer(Logger& logger, std::uint32_t width, std::uint32_t height, std::vector<std::filesystem::path> files,
                    std::size_t index, double seconds);
    ~SlideshowPlayer() override;

    // Starts on the picture given to the constructor, `at` seconds into it (fading in from black).
    void start(double at);
    // Fades to picture `index` now; its time starts over (paused stays paused).
    void show(std::size_t index);
    // The picture shown (or being faded in).
    std::size_t index() const;

    std::uint64_t copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch) override;

    void play() override;
    void pause() override;
    bool playing() const override;
    bool finished() const override;
    double positionSeconds() const override;
    double durationSeconds() const override { return seconds_; }
    std::string lastError() const override;
    void setGains(float, float) override {}

    // Decodes a picture fitted (letterboxed) into width x height RGBA pixels (DXGI R8G8B8A8 order, opaque).
    // Needs COM on the calling thread. Empty with `error` set when it cannot be read.
    static std::vector<std::uint32_t> decode(const std::filesystem::path& file, std::uint32_t width, std::uint32_t height,
                                             std::string& error);
    static bool pictureFile(const std::filesystem::path& file);

private:
    using Clock = std::chrono::steady_clock;

    void run();
    void publish(const std::vector<std::uint32_t>& pixels);
    std::size_t nextReadable(std::size_t from) const;   // caller holds mutex_

    Logger& logger_;
    const std::uint32_t width_;
    const std::uint32_t height_;
    const std::vector<std::filesystem::path> files_;
    const double seconds_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::size_t index_{ 0 };
    std::optional<std::size_t> jump_;   // show() request for the thread
    std::vector<bool> bad_;             // pictures that failed to decode
    Clock::time_point clockStart_{};    // when the current picture started (moved forward over pauses)
    Clock::time_point pausedAt_{};
    double startAt_{ 0.0 };
    bool paused_{ false };
    bool finished_{ false };
    bool quit_{ false };
    std::string error_;

    std::mutex frameMutex_;
    std::vector<std::uint32_t> front_;
    std::uint64_t frontSeq_{ 0 };

    std::thread worker_;
};
