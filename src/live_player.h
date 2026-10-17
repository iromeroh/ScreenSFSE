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

// Plays a live stream (a YouTube live's HLS URLs from yt-dlp: video and sound are separate there) through the external
// ffmpeg.exe, in two stages:
//   1. fetch:  ffmpeg -i <video> [-i <sound>] -c copy -f mpegts  -> our in-memory buffer, as fast as the stream comes
//   2. decode: ffmpeg -re -f mpegts -i <buffer, after bufferSeconds> -> video: scaled/letterboxed to the screen texture,
//              30 fps, raw RGBA on stdout; sound: 48 kHz stereo s16 PCM to a localhost TCP socket we listen on
//              (Windows pipes cannot carry a second output).
// The buffer keeps ~bufferSeconds of compressed stream ahead (MBs, not decoded frames), so network or HLS segment
// stalls up to that long are not seen or heard (in-game 2026-10-14 with -re straight on the HLS: a ~1.4 s freeze). Frames are shown in arrival order at 30 fps from a small buffer (live delivery is bursty and a
// little under 30 fps: scheduling by timestamp dropped a third of them); a growing buffer is trimmed. No seek, no length.
// pause() stops ffmpeg; play() starts again at the live edge. The stream ending or failing (ffmpeg exits) =
// finished(), with ffmpeg's last error line in lastError(). Tested by hand 2026-10-14 on a YouTube live: 30 fps, sound.
class LivePlayer : public Playback
{
public:
    LivePlayer(Logger& logger, std::uint32_t width, std::uint32_t height, std::filesystem::path ffmpeg,
               double bufferSeconds = 5.0);
    ~LivePlayer() override;

    LivePlayer(const LivePlayer&) = delete;
    LivePlayer& operator=(const LivePlayer&) = delete;

    // audioUrl empty: the video URL carries the sound too.
    bool open(const std::string& videoUrl, const std::string& audioUrl);

    void play() override;
    void pause() override;
    bool playing() const override { return playing_.load(); }
    bool finished() const override;
    double positionSeconds() const override;
    double durationSeconds() const override { return 0.0; }
    std::string lastError() const override;
    void setGains(float left, float right) override;
    bool live() const override { return true; }

    std::uint64_t copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch) override;

private:
    struct Frame
    {
        double pts{ 0 };
        std::uint64_t seq{ 0 };
        std::vector<std::uint32_t> pixels;
    };

    bool start();
    void stop();
    void* spawn(const std::vector<std::wstring>& args, void** stdinWrite, void** stdoutRead, void** stderrRead);
    void fetchLoop(void* stdoutPipe);
    void feedLoop(void* stdinPipe);
    void videoLoop(void* stdoutPipe);
    void audioLoop(std::uintptr_t listenSocket);
    void errorLoop(void* stderrPipe);
    double clock() const;
    void applyGains();
    void fail(const std::string& message);

    Logger& logger_;
    const std::uint32_t width_;
    const std::uint32_t height_;
    const std::filesystem::path ffmpeg_;
    const double bufferSeconds_;
    std::string url_;
    std::string audioUrl_;

    std::atomic<bool> quit_{ false };
    std::atomic<bool> playing_{ false };
    std::atomic<bool> ended_{ false };      // ffmpeg exited (stream over or failed)
    std::atomic<bool> userStopped_{ false };
    void* process_{ nullptr };        // decoder
    void* fetchProcess_{ nullptr };
    void* job_{ nullptr };
    std::thread videoThread_;
    std::thread audioThread_;
    std::thread errorThread_;
    std::thread fetchThread_;
    std::thread feedThread_;
    std::thread fetchErrorThread_;

    // compressed stream between the stages
    std::mutex ringMutex_;
    std::condition_variable ringCv_;
    std::deque<std::vector<char>> ring_;
    std::size_t ringBytes_{ 0 };
    bool fetchEnded_{ false };
    std::chrono::steady_clock::time_point firstByte_{};

    mutable std::mutex framesMutex_;
    std::deque<Frame> frames_;          // decoded, not shown yet, in arrival order
    Frame current_;                     // the frame on screen (seq 0: none yet)
    std::chrono::steady_clock::time_point nextShow_{};
    std::vector<Frame> pool_;
    std::uint64_t nextSeq_{ 1 };
    std::uint64_t framesRead_{ 0 };
    std::uint64_t droppedFull_{ 0 };   // queue overflow (frames came in a burst)
    std::uint64_t skippedLate_{ 0 };   // superseded before shown

    mutable std::mutex audioMutex_;
    IXAudio2* xaudio_{ nullptr };
    IXAudio2MasteringVoice* master_{ nullptr };
    IXAudio2SourceVoice* voice_{ nullptr };
    std::uint32_t masterChannels_{ 2 };
    float gainL_{ 1.0f };
    float gainR_{ 1.0f };

    mutable std::mutex clockMutex_;
    std::chrono::steady_clock::time_point started_{};
    double offset_{ 0 };                    // seconds already played before the current run (pause/play)

    mutable std::mutex errorMutex_;
    std::string lastError_;
};
