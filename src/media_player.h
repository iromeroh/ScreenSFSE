#pragma once

#include "playback.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Logger;
struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

// Plays one local video file onto the screen texture with its sound.
// - Video thread: Media Foundation Source Reader (video stream only), decoded and scaled by MF's video processor
//   to fit the screen (letterboxed), converted to RGBA, queued with its timestamp (a few frames ahead).
// - Audio thread: a second Source Reader (audio stream only) decoded to 16-bit PCM and fed to an XAudio2 voice.
// - Clock: samples played by the voice (no audio stream: wall clock). copyLatest() returns the newest frame whose
//   time has come, so video follows the sound. Late frames are dropped, never waited for.
// Spatial sound is a pair of channel gains set from outside (setGains).
class MediaPlayer : public Playback
{
public:
    MediaPlayer(Logger& logger, std::uint32_t width, std::uint32_t height);
    ~MediaPlayer() override;

    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    // Opens the file and starts playing at startSeconds (video from the keyframe before it, then caught up).
    bool open(const std::filesystem::path& file, double startSeconds = 0.0);
    // Length of the file, 0 until a stream has opened.
    double durationSeconds() const override { return duration_.load(); }
    void play() override;
    void pause() override;
    bool playing() const override { return playing_.load(); }
    // Both streams have ended (and the last sound has played).
    bool finished() const override;
    double positionSeconds() const override;
    std::string lastError() const override;

    // Output gains for the left and right speakers (0..1), e.g. from distance and direction.
    void setGains(float left, float right) override;

    std::uint64_t copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch) override;

private:
    struct Frame
    {
        std::int64_t pts{ 0 };   // 100 ns units
        std::uint64_t seq{ 0 };
        std::vector<std::uint32_t> pixels;
    };

    void videoLoop(std::filesystem::path file);
    void audioLoop(std::filesystem::path file);
    void seekReader(void* reader, const char* what);
    void noteDuration(void* reader);
    void applyGains();
    std::int64_t clock() const;   // 100 ns units
    void fail(const std::string& message);
    void closeAll();

    Logger& logger_;
    const std::uint32_t width_;
    const std::uint32_t height_;

    std::atomic<bool> quit_{ false };
    std::int64_t start_{ 0 };   // 100 ns
    std::atomic<double> duration_{ 0.0 };
    std::atomic<bool> playing_{ false };
    std::thread videoThread_;
    std::thread audioThread_;

    // decoded frames, oldest first; the front one is the frame on screen once its time has come
    mutable std::mutex framesMutex_;
    std::condition_variable framesCv_;
    std::deque<Frame> frames_;
    std::vector<Frame> pool_;
    std::uint64_t nextSeq_{ 1 };
    std::atomic<bool> videoEnded_{ true };
    std::atomic<std::int64_t> lastFramePts_{ 0 };
    bool hasVideo_{ false };

    // audio
    mutable std::mutex audioMutex_;
    std::condition_variable audioCv_;
    IXAudio2* xaudio_{ nullptr };
    IXAudio2MasteringVoice* master_{ nullptr };
    IXAudio2SourceVoice* voice_{ nullptr };
    std::uint32_t sampleRate_{ 0 };
    std::uint32_t channels_{ 0 };
    std::uint32_t masterChannels_{ 2 };
    std::atomic<std::int64_t> audioBasePts_{ 0 };   // timestamp of the first sound buffer
    std::atomic<bool> audioBaseSet_{ false };
    std::atomic<bool> audioEnded_{ true };
    std::atomic<bool> hasAudio_{ false };
    std::atomic<bool> audioReady_{ false };   // the audio thread has decided whether there is sound
    float gainL_{ 1.0f };
    float gainR_{ 1.0f };

    // wall clock when there is no audio stream
    mutable std::mutex clockMutex_;
    std::chrono::steady_clock::time_point wallStart_{};
    std::int64_t wallPausedAt_{ 0 };

    mutable std::mutex errorMutex_;
    std::string lastError_;
};
