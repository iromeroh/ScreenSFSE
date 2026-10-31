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

// Downloads online videos (YouTube, or any site yt-dlp supports) as local MP4 files with the external yt-dlp.exe
// (hidden child process, own thread, job object so it dies with the game). The radio streams YouTube audio by URL, but
// some YouTube MP4/M4A streams are fragmented (DASH) and Media Foundation cannot stream them; yt-dlp + ffmpeg merge
// them into a plain MP4 here. Format: H.264 (Windows decodes it without extensions) up to `maxHeight`, plus AAC audio.
// A video is named by its key: the 11-character ID for YouTube, else "<extractor>_<id>" (trackKey). Each call takes
// the video's own URL too; empty = https://www.youtube.com/watch?v=<key>.
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
        std::string id;       // the key: YouTube video ID, or trackKey(extractor, id)
        std::string title;
        std::string status;   // yt-dlp live_status: is_live, is_upcoming, was_live, not_live, post_live, NA
        std::string url;      // where yt-dlp gets this video; empty for YouTube
    };
    using ListDone = std::function<void(bool ok, std::vector<Track> tracks, const std::string& error)>;
    // Stream URLs: video, and sound when separate (empty when the video URL carries both); the length in seconds
    // when known (0 for a live stream); the HTTP headers yt-dlp would send ("Name: value" lines ending in CRLF; many
    // CDNs refuse requests without their Referer / Origin / User-Agent).
    using StreamDone = std::function<void(bool ok, const std::string& videoUrl, const std::string& audioUrl,
                                          double duration, const std::string& headers, const std::string& error)>;
    // request()'s error when the video is a live stream (never downloaded; play it with requestStream()).
    static constexpr const char* kLiveError = "live stream";
    // request()'s error when the site offers no H.264 MP4 to download: stream it with requestStream() instead.
    static constexpr const char* kStreamOnlyError = "stream only";

    explicit VideoFetcher(Logger& logger);
    ~VideoFetcher();

    void configure(const Settings& settings);
    bool available() const;
    const Settings& settings() const { return settings_; }

    // The 11-character video ID of a YouTube URL (watch?v=, youtu.be/, shorts/, embed/) or of a bare ID.
    static std::optional<std::string> youTubeId(const std::string& text);
    // The cache name of a video from another site: "<extractor>_<id>", letters, digits, '-' and '_' only, at most 80
    // characters (longer ones end in a hash). Never 11 characters, so it is never taken for a YouTube ID.
    static std::string trackKey(const std::string& extractor, const std::string& id);
    // A URL yt-dlp can read: an http(s) address (any site), or a YouTube video ID.
    static bool onlineSource(const std::string& source);

    std::optional<std::filesystem::path> cachedFile(const std::string& videoId) const;
    // Downloads (or finds in the cache), prepares an MF-compatible copy, then calls done on the fetcher thread.
    // Original downloads are preserved; only the prepared path is handed to the player.
    // A newer request replaces a queued one; a running download finishes first.
    void request(const std::string& videoId, Done done, const std::string& url = {});
    // Downloads/prepares in the background if not ready (next track), after any queued request; no callback.
    void prefetch(const std::string& videoId, const std::string& url = {});
    // The videos of a playlist or page, in order (a single video gives one): yt-dlp --flat-playlist --print
    // "<extractor> <id> <live_status> <entry url> <page url> <title>". items: yt-dlp --playlist-items ("" = all).
    // Runs before queued downloads.
    void requestList(const std::string& url, ListDone done, const std::string& items = {});
    // The stream URLs of a video (yt-dlp -g): a live stream while it runs, or a video that is not downloaded.
    // Runs before queued downloads.
    void requestStream(const std::string& videoId, StreamDone done, const std::string& url = {});
    // Subtitles of a video in the given languages, into <cacheDir>\subs\<id>.<lang>.vtt (Subtitles::find reads them).
    // First the uploader's subtitles (yt-dlp --write-subs); only if there are none, YouTube's automatic captions in the
    // video's spoken language (<lang>-orig). Machine translations are never requested: YouTube refuses them with
    // HTTP 429 at once. Runs after queued downloads; done (may be empty) gets whether a file was written.
    using SubtitlesDone = std::function<void(bool found)>;
    void requestSubtitles(const std::string& videoId, const std::vector<std::string>& languages, SubtitlesDone done,
                          const std::string& url = {});
    std::filesystem::path subtitlesDir() const;
    void shutdown();

private:
    enum class Kind
    {
        Download,
        Prefetch,
        List,
        Stream,
        Subtitles
    };

    struct Job
    {
        Kind kind{ Kind::Download };
        std::string arg;   // video key, or playlist URL
        std::string url;   // the video's URL (empty = YouTube by key); a listing's --playlist-items
        Done done;
        ListDone listDone;
        StreamDone streamDone;
        std::vector<std::string> languages;
        SubtitlesDone subtitlesDone;
    };

    void loop();
    void run(Job& job);
    void runList(Job& job);
    void runStream(Job& job);
    void runSubtitles(Job& job);
    std::vector<std::wstring> baseArgs(const Settings& s) const;
    static std::wstring videoUrl(const Job& job);
    std::optional<std::filesystem::path> prepareVideo(const std::filesystem::path& original, std::string& error);
    void trimCache(const std::filesystem::path& requestedFile);
    bool runProcess(const std::wstring& commandLine, std::string& out, std::string& err,
                    const std::filesystem::path& executable = {});

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
