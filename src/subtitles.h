#pragma once

#include "frame_source.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Playback;

// Subtitles drawn over a video's frames: SRT/WebVTT files next to a local video (Show.srt, Show.es.srt) or fetched for
// a YouTube video by VideoFetcher (Cache\subs\<id>.<lang>.vtt). ScreenSFSE.ini subtitle_languages picks the language.
namespace Subtitles
{
struct Cue
{
    double start{ 0 };
    double end{ 0 };
    std::wstring text;   // lines separated by '\n'
};
using Cues = std::vector<Cue>;

// Cues of an SRT or WebVTT file (UTF-8, else the Windows ANSI code page; BOM and CRLF allowed). Tags (<i>, <c>,
// <00:00:01.000>, {\an8}) and entities are removed. Cues under 50 ms are dropped: YouTube's automatic captions repeat
// each finished line in a 10 ms cue. Sorted by start; identical neighbours merged; gaps under 0.25 s closed (no blink).
Cues parse(const std::string& text);
Cues load(const std::filesystem::path& file);

struct Track
{
    std::string language;   // "" = untagged (Show.srt), else the tag (Show.es.srt -> "es"; YouTube "en-orig")
    std::filesystem::path file;
};
// <stem>.srt/.vtt and <stem>.<language>.srt/.vtt in dir, sorted by language (untagged first).
std::vector<Track> find(const std::filesystem::path& dir, const std::wstring& stem);
// Language tags compare by their first part, ignoring case: "es" matches "es-419", "ES" and "es-orig".
bool sameLanguage(const std::string& tag, const std::string& wanted);
// The track to show. choice: "off" = none; a language = that track if the video has it; "" (or a language it does not
// have) = the first of `preferred` it has, else the untagged track when allowUntagged, else none.
const Track* pick(const std::vector<Track>& tracks, const std::vector<std::string>& preferred, const std::string& choice,
                  bool allowUntagged);
// For notices and the menu title: "ES", "EN (AUTO)" for YouTube's automatic captions, "ON" for an untagged file.
std::string label(const std::string& language);

// One caption, white text with a dark outline: coverage masks (0-255), width x height.
struct Image
{
    std::uint32_t width{ 0 };
    std::uint32_t height{ 0 };
    std::vector<std::uint8_t> fill;
    std::vector<std::uint8_t> edge;
};
// Word-wrapped, centred lines no wider than maxWidth, letters pixelHeight tall (Segoe UI, Windows GDI).
Image render(const std::wstring& text, std::uint32_t maxWidth, int pixelHeight);
// Over an RGBA frame (DXGI R8G8B8A8 byte order), centred, its bottom `bottomMargin` pixels above the frame's.
void blend(std::uint8_t* frame, std::uint32_t pitch, std::uint32_t width, std::uint32_t height, const Image& image,
           std::uint32_t bottomMargin);
}

// A video's frame source with the current subtitle drawn over its frames. Captions are rendered on this source's own
// thread when the cue changes (the next one ahead), so copyLatest only blends a ready image; the thread sleeps
// otherwise and starts with the first cues.
class SubtitledSource : public FrameSource
{
public:
    // scale: ScreenSFSE.ini subtitle_size (1 = letters 6% of the frame height).
    SubtitledSource(std::shared_ptr<Playback> video, std::uint32_t width, std::uint32_t height, float scale);
    ~SubtitledSource() override;

    // null or empty = no subtitles.
    void setCues(std::shared_ptr<const Subtitles::Cues> cues);

    std::uint64_t copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch) override;
    void onSurfaceLive(bool live) override;

private:
    struct Rendered
    {
        std::uint64_t version{ 0 };
        int index{ -1 };
        std::shared_ptr<const Subtitles::Image> image;
    };

    int cueAtLocked(double seconds) const;
    void loop();

    std::shared_ptr<Playback> video_;
    std::uint32_t width_;
    std::uint32_t height_;
    int pixelHeight_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool quit_{ false };
    std::shared_ptr<const Subtitles::Cues> cues_;
    std::uint64_t version_{ 0 };   // bumps with every setCues: older renders are discarded
    int wanted_{ -1 };             // the cue to show now (render thread target)
    std::vector<Rendered> rendered_;   // the wanted cue and the one after it

    // render-thread side of copyLatest (one caller at a time: the surface's upload)
    std::uint64_t innerSeq_{ 0 };
    std::uint64_t outSeq_{ 0 };
    std::uint64_t drawnVersion_{ 0 };
    int drawnIndex_{ -1 };
};
