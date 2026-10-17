#pragma once

#include "frame_source.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// Test card frame source, drawn on its own thread. Animated: redrawn at `fps` while a screen texture is live (the
// proof of concept, and the "loading" card while a video downloads). Static: drawn once (the idle card).
class TestPatternSource : public FrameSource
{
public:
    TestPatternSource(std::uint32_t width, std::uint32_t height, double fps, bool animated = true);
    ~TestPatternSource() override;

    void start();
    void stop();

    std::uint64_t copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch) override;
    void onSurfaceLive(bool live) override { live ? start() : stop(); }

private:
    void run();
    void draw(std::vector<std::uint32_t>& px, std::uint64_t frame, double seconds) const;

    const std::uint32_t width_;
    const std::uint32_t height_;
    const double fps_;
    const bool animated_;

    std::mutex frameMutex_;
    std::vector<std::uint32_t> front_;
    std::uint64_t frontSeq_{ 0 };

    std::mutex runMutex_;
    std::condition_variable wake_;
    bool running_{ false };
    bool quit_{ false };
    std::thread worker_;
};

// Idle card without a thread or a frame buffer: drawn straight into the upload memory once per new texture
// (colour bars, dark lower third with a channel marker: `channelNumber` light cells).
class StaticCardSource : public FrameSource
{
public:
    StaticCardSource(std::uint32_t width, std::uint32_t height, int channelNumber) :
        width_(width), height_(height), channelNumber_(channelNumber)
    {}

    std::uint64_t copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch) override;

private:
    const std::uint32_t width_;
    const std::uint32_t height_;
    const int channelNumber_;
};

// A switched-off screen: black, written once per new texture (no thread, no buffer).
class BlackSource : public FrameSource
{
public:
    BlackSource(std::uint32_t width, std::uint32_t height) :
        width_(width), height_(height)
    {}

    std::uint64_t copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch) override;

private:
    const std::uint32_t width_;
    const std::uint32_t height_;
};
