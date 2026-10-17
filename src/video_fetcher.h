#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class Logger;

// Downloads YouTube videos as local MP4 files with the external yt-dlp.exe (hidden child process, own thread, job
// object so it dies with the game). The radio streams YouTube audio by URL, but some YouTube MP4/M4A streams are
// fragmented (DASH) and Media Foundation cannot stream them; yt-dlp + ffmpeg merge them into a plain MP4 here.
// Format: H.264 (Windows decodes it without extensions) up to `maxHeight`, plus AAC audio.
class VideoFetcher
{
public:
    struct Settings
    {
        std::filesystem::path ytdlp;
        std::filesystem::path deno;
        std::filesystem::path ffmpeg;
        std::filesystem::path cacheDir;
        int maxHeight{ 720 };
        std::size_t cacheFiles{ 20 };   // newest kept
        std::chrono::seconds timeout{ 900 };
    };

    using Done = std::function<void(bool ok, const std::filesystem::path& file, const std::string& error)>;

    struct Track
    {
        std::string id;
        std::string title;
        std::string status;   // yt-dlp live_status: is_live, is_upcoming, was_live, not_live, post_live, NA
    };
    using ListDone = std::function<void(bool ok, std::vector<Track> tracks, const std::string& error)>;
    // Live HLS URLs: video, and sound when separate (empty when the video URL carries both).
    using StreamDone = std::function<void(bool ok, const std::string& videoUrl, const std::string& audioUrl,
                                          const std::string& error)>;
    // request()'s error when the video is a live stream (never downloaded; play it with requestStream()).
    static constexpr const char* kLiveError = "live stream";

    explicit VideoFetcher(Logger& logger);
    ~VideoFetcher();

    void configure(const Settings& settings);
    bool available() const;
    const Settings& settings() const { return settings_; }

    // The 11-character video ID of a YouTube URL (watch?v=, youtu.be/, shorts/, embed/) or of a bare ID.
    static std::optional<std::string> youTubeId(const std::string& text);

    std::optional<std::filesystem::path> cachedFile(const std::string& videoId) const;
    // Downloads (or finds in the cache) and calls done on the fetcher thread, with no lock held.
    // A newer request replaces a queued one; a running download finishes first.
    void request(const std::string& videoId, Done done);
    // Downloads in the background if not cached (next track), after any queued request; no callback.
    void prefetch(const std::string& videoId);
    // Video IDs and titles of a YouTube playlist (or the single video of a watch URL), in order:
    // yt-dlp --flat-playlist --print "%(id)s<TAB>%(title)s". Runs before queued downloads.
    void requestList(const std::string& url, ListDone done);
    // The HLS URLs of a live stream (yt-dlp -g), to play it while it runs. Runs before queued downloads.
    void requestStream(const std::string& videoId, StreamDone done);
    void shutdown();

private:
    enum class Kind
    {
        Download,
        Prefetch,
        List,
        Stream
    };

    struct Job
    {
        Kind kind{ Kind::Download };
        std::string arg;   // video ID, or playlist URL
        Done done;
        ListDone listDone;
        StreamDone streamDone;
    };

    void loop();
    void run(Job& job);
    void runList(Job& job);
    void runStream(Job& job);
    std::vector<std::wstring> baseArgs(const Settings& s) const;
    void trimCache();
    bool runProcess(const std::wstring& commandLine, std::string& out, std::string& err);

    Logger& logger_;
    Settings settings_{};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::thread thread_;
    bool stop_{ false };
    std::atomic<bool> stopping_{ false };
    void* job_{ nullptr };
};
