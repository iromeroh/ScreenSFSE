#include "video_fetcher.h"

#include "logger.h"

#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include <windows.h>

// Process handling follows RadioSFSEPlugin/src/ytdlp_client.cpp (tested in game with YouTube audio).
namespace
{
std::wstring widen(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>((std::max)(size, 0)), L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    }
    return out;
}

std::wstring quoteArg(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}

std::wstring childEnvironment()
{
    std::wstring block;
    if (LPWCH env = GetEnvironmentStringsW()) {
        for (LPWCH p = env; *p; p += wcslen(p) + 1) {
            std::wstring entry(p);
            std::wstring name = entry.substr(0, entry.find(L'='));
            std::transform(name.begin(), name.end(), name.begin(), ::towupper);
            if (name == L"PYTHONIOENCODING" || name == L"PYTHONUTF8") {
                continue;
            }
            block += entry;
            block.push_back(L'\0');
        }
        FreeEnvironmentStringsW(env);
    }
    block += L"PYTHONIOENCODING=utf-8";
    block.push_back(L'\0');
    block += L"PYTHONUTF8=1";
    block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

void drain(HANDLE pipe, std::string& out)
{
    DWORD available = 0;
    while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        char buffer[4096];
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, (std::min<DWORD>)(available, sizeof(buffer)), &read, nullptr) || read == 0) {
            break;
        }
        out.append(buffer, read);
    }
}

std::string lastLine(const std::string& text)
{
    std::size_t end = text.find_last_not_of("\r\n \t");
    if (end == std::string::npos) {
        return {};
    }
    const std::size_t start = text.rfind('\n', end);
    std::string line = text.substr(start == std::string::npos ? 0 : start + 1, end + 1 - (start == std::string::npos ? 0 : start + 1));
    return line.size() > 300 ? line.substr(0, 300) : line;
}

bool isIdChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// {"User-Agent": "Mozilla/5.0 ...", "Referer": "https://..."} (yt-dlp's %(http_headers)j) -> "User-Agent: ...\r\n
// Referer: ...\r\n". A flat object of strings; JSON escapes are undone, control characters dropped.
std::string headerLines(const std::string& json)
{
    std::string out;
    std::vector<std::string> strings;
    for (std::size_t i = 0; i < json.size(); ++i) {
        if (json[i] != '"') {
            continue;
        }
        std::string s;
        for (++i; i < json.size() && json[i] != '"'; ++i) {
            char c = json[i];
            if (c == '\\' && i + 1 < json.size()) {
                c = json[++i];
                if (c == 'u' && i + 4 < json.size()) {
                    const unsigned code = static_cast<unsigned>(std::strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16));
                    i += 4;
                    c = code >= 0x20 && code < 0x7F ? static_cast<char>(code) : '?';
                } else if (c != '"' && c != '\\' && c != '/') {
                    continue;   // \n, \t...: not wanted in a header
                }
            }
            if (static_cast<unsigned char>(c) >= 0x20) {
                s.push_back(c);
            }
        }
        strings.push_back(std::move(s));
    }
    for (std::size_t k = 0; k + 1 < strings.size(); k += 2) {
        if (!strings[k].empty() && strings[k].find(':') == std::string::npos) {
            out += strings[k] + ": " + strings[k + 1] + "\r\n";
        }
    }
    return out;
}

// Versioned derivative cache: originals remain usable by the published DLL and can be reprocessed later.
constexpr const wchar_t* kPreparedDir = L"mf-sei-v1";

std::string fileStamp(const std::filesystem::path& file)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size == 0) {
        return {};
    }
    const auto time = std::filesystem::last_write_time(file, ec);
    return ec ? std::string{} : std::to_string(size) + " " + std::to_string(time.time_since_epoch().count());
}
}

VideoFetcher::VideoFetcher(Logger& logger) :
    logger_(logger)
{}

VideoFetcher::~VideoFetcher()
{
    shutdown();
}

void VideoFetcher::configure(const Settings& settings)
{
    std::lock_guard lock(mutex_);
    settings_ = settings;
}

bool VideoFetcher::available() const
{
    std::lock_guard lock(mutex_);
    std::error_code ec;
    return !settings_.ytdlp.empty() && std::filesystem::exists(settings_.ytdlp, ec);
}

std::optional<std::string> VideoFetcher::youTubeId(const std::string& text)
{
    auto take = [&text](std::size_t pos) -> std::optional<std::string> {
        if (pos == std::string::npos || pos + 11 > text.size()) {
            return std::nullopt;
        }
        const std::string id = text.substr(pos, 11);
        if (!std::all_of(id.begin(), id.end(), isIdChar) || (pos + 11 < text.size() && isIdChar(text[pos + 11]))) {
            return std::nullopt;
        }
        return id;
    };
    if (text.size() == 11) {
        return take(0);
    }
    for (const char* marker : { "v=", "youtu.be/", "/shorts/", "/embed/", "/live/" }) {
        const std::size_t at = text.find(marker);
        if (at != std::string::npos) {
            if (auto id = take(at + std::char_traits<char>::length(marker))) {
                return id;
            }
        }
    }
    return std::nullopt;
}

std::string VideoFetcher::trackKey(const std::string& extractor, const std::string& id)
{
    std::string key = (extractor.empty() || extractor == "NA" ? std::string("Web") : extractor) + "_" + id;
    for (char& c : key) {
        if (!isIdChar(c)) {
            c = '_';
        }
    }
    if (key.size() > 80) {
        std::uint64_t hash = 14695981039346656037ull;
        for (unsigned char c : key) hash = (hash ^ c) * 1099511628211ull;
        char suffix[20];
        std::snprintf(suffix, sizeof(suffix), "_%016llx", static_cast<unsigned long long>(hash));
        key = key.substr(0, 63) + suffix;
    }
    if (key.size() == 11) {
        key += "_";   // an 11-character key is a YouTube ID
    }
    return key;
}

bool VideoFetcher::onlineSource(const std::string& source)
{
    std::string lower = source.substr(0, 8);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.starts_with("http://") || lower.starts_with("https://") ||
           source.find("youtube.com") != std::string::npos || source.find("youtu.be") != std::string::npos ||
           (source.size() == 11 && youTubeId(source));
}

std::wstring VideoFetcher::videoUrl(const Job& job)
{
    return job.url.empty() ? L"https://www.youtube.com/watch?v=" + widen(job.arg) : widen(job.url);
}

std::optional<std::filesystem::path> VideoFetcher::cachedFile(const std::string& videoId) const
{
    std::filesystem::path dir;
    {
        std::lock_guard lock(mutex_);
        dir = settings_.cacheDir;
    }
    std::error_code ec;
    const auto file = dir / std::filesystem::path(widen(videoId + ".mp4"));
    if (!dir.empty() && std::filesystem::exists(file, ec) && std::filesystem::file_size(file, ec) > 0) {
        return file;
    }
    return std::nullopt;
}

void VideoFetcher::request(const std::string& videoId, Done done, const std::string& url)
{
    std::lock_guard lock(mutex_);
    if (stop_) {
        return;
    }
    // only the newest play request matters; queued prefetches go too (the next track changes with it), lists stay
    std::erase_if(jobs_, [](const Job& j) { return j.kind == Kind::Download || j.kind == Kind::Prefetch; });
    Job job{ Kind::Download, videoId, url };
    job.done = std::move(done);
    jobs_.push_back(std::move(job));
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { loop(); });
    }
    cv_.notify_all();
}

void VideoFetcher::prefetch(const std::string& videoId, const std::string& url)
{
    // An older cached download may still need its compatibility copy. The worker's stamp check is cheap.
    std::lock_guard lock(mutex_);
    if (stop_) {
        return;
    }
    jobs_.push_back(Job{ Kind::Prefetch, videoId, url });
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { loop(); });
    }
    cv_.notify_all();
}

void VideoFetcher::requestList(const std::string& url, ListDone done, const std::string& items)
{
    std::lock_guard lock(mutex_);
    if (stop_) {
        return;
    }
    Job job{ Kind::List, url, items };
    job.listDone = std::move(done);
    jobs_.push_front(std::move(job));
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { loop(); });
    }
    cv_.notify_all();
}

void VideoFetcher::requestStream(const std::string& videoId, StreamDone done, const std::string& url)
{
    std::lock_guard lock(mutex_);
    if (stop_) {
        return;
    }
    Job job{ Kind::Stream, videoId, url };
    job.streamDone = std::move(done);
    jobs_.push_front(std::move(job));
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { loop(); });
    }
    cv_.notify_all();
}

void VideoFetcher::requestSubtitles(const std::string& videoId, const std::vector<std::string>& languages, SubtitlesDone done,
                                    const std::string& url)
{
    std::lock_guard lock(mutex_);
    if (stop_ || languages.empty()) {
        return;
    }
    // one queued job per video; a newer request (other languages, a callback) replaces it
    std::erase_if(jobs_, [&](const Job& j) { return j.kind == Kind::Subtitles && j.arg == videoId; });
    Job job{ Kind::Subtitles, videoId, url };
    job.languages = languages;
    job.subtitlesDone = std::move(done);
    jobs_.push_back(std::move(job));
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { loop(); });
    }
    cv_.notify_all();
}

std::filesystem::path VideoFetcher::subtitlesDir() const
{
    std::lock_guard lock(mutex_);
    return settings_.cacheDir.empty() ? std::filesystem::path{} : settings_.cacheDir / L"subs";
}

void VideoFetcher::runSubtitles(Job& job)
{
    auto finish = [&job](bool found) {
        if (job.subtitlesDone) {
            job.subtitlesDone(found);
        }
    };
    Settings s;
    {
        std::lock_guard lock(mutex_);
        s = settings_;
    }
    std::error_code ec;
    if (s.ytdlp.empty() || !std::filesystem::exists(s.ytdlp, ec)) {
        finish(false);
        return;
    }
    const auto dir = s.cacheDir / L"subs";
    std::filesystem::create_directories(dir, ec);
    const std::wstring id = widen(job.arg);
    auto written = [&](bool automatic) {
        for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
            const std::wstring name = it->path().filename().wstring();
            if (!name.starts_with(id + L".") || it->path().extension() != L".vtt") {
                continue;
            }
            const bool orig = name.ends_with(L"-orig.vtt");
            if (orig == automatic) {
                return true;
            }
        }
        return false;
    };
    // pass 1: the uploader's subtitles ("es", "es-419", "en-US"); pass 2: the automatic captions of the spoken language
    std::wstring manual, original, names;
    for (const auto& l : job.languages) {
        const std::wstring w = widen(l);
        manual += (manual.empty() ? L"" : L",") + w + L"," + w + L"-[A-Za-z0-9]+";
        original += (original.empty() ? L"" : L",") + w + L"-orig";
        names += std::wstring(names.empty() ? L"" : L", ") + w;
    }
    auto runPass = [&](bool automatic) {
        std::vector<std::wstring> args = baseArgs(s);
        args.insert(args.end(), { L"--no-playlist", L"--skip-download", automatic ? L"--write-auto-subs" : L"--write-subs",
                                  L"--sub-format", L"vtt", L"--sub-langs", automatic ? original : manual, L"-o",
                                  (dir / id).wstring() + L".%(ext)s", L"--", videoUrl(job) });
        std::wstring commandLine = quoteArg(s.ytdlp.wstring());
        for (const auto& a : args) {
            commandLine += L" " + quoteArg(a);
        }
        std::string out, err;
        const bool ran = runProcess(commandLine, out, err);
        if (!ran && !stopping_.load()) {
            logger_.warn("Subtitles for " + job.arg + ": yt-dlp failed - " + lastLine(err));
        }
        return ran;
    };
    if (written(false) || written(true)) {
        finish(true);
        return;
    }
    if (!runPass(false) || stopping_.load()) {
        finish(false);   // a refusal (HTTP 429) would refuse the second pass too
        return;
    }
    if (written(false)) {
        logger_.info("Subtitles for " + job.arg + ": the uploader's.");
        finish(true);
        return;
    }
    runPass(true);
    if (stopping_.load()) {
        return;
    }
    const bool found = written(true);
    std::string list;
    for (const auto& l : job.languages) {
        list += (list.empty() ? "" : ", ") + l;
    }
    logger_.info("Subtitles for " + job.arg + ": " + (found ? "automatic captions." : "none in " + list + "."));
    finish(found);
}

std::vector<std::wstring> VideoFetcher::baseArgs(const Settings& s) const
{
    std::vector<std::wstring> args = { L"--ignore-config", L"--no-warnings", L"--no-progress" };
    std::error_code ec;
    if (!s.deno.empty() && std::filesystem::exists(s.deno, ec)) {
        args.insert(args.end(), { L"--js-runtimes", L"deno:" + s.deno.wstring() });
    }
    return args;
}

void VideoFetcher::runList(Job& job)
{
    Settings s;
    {
        std::lock_guard lock(mutex_);
        s = settings_;
    }
    std::error_code ec;
    if (s.ytdlp.empty() || !std::filesystem::exists(s.ytdlp, ec)) {
        job.listDone(false, {}, "yt-dlp.exe not found");
        return;
    }
    std::vector<std::wstring> args = baseArgs(s);
    if (!job.url.empty()) {
        args.insert(args.end(), { L"--playlist-items", widen(job.url) });
    }
    // ie_key is set on the entries of a flat listing (each resolved later by its url); a video yt-dlp resolved here
    // has the media URL in url and its page in webpage_url.
    args.insert(args.end(), { L"--flat-playlist", L"--print",
                              L"%(extractor_key)s\t%(id)s\t%(live_status)s\t%(ie_key)s\t%(url)s\t%(webpage_url)s\t%(title)s",
                              L"--", widen(job.arg) });
    std::wstring commandLine = quoteArg(s.ytdlp.wstring());
    for (const auto& a : args) {
        commandLine += L" " + quoteArg(a);
    }
    std::string out;
    std::string err;
    const bool ran = runProcess(commandLine, out, err);
    if (stopping_.load()) {
        return;
    }
    struct Entry
    {
        std::string extractor, id, status, ieKey, url, page, title;
    };
    std::vector<Entry> entries;
    std::size_t start = 0;
    while (start < out.size()) {
        std::size_t end = out.find('\n', start);
        if (end == std::string::npos) {
            end = out.size();
        }
        std::string line = out.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::vector<std::string> f;
        for (std::size_t at = 0; f.size() < 6;) {
            const std::size_t tab = line.find('\t', at);
            if (tab == std::string::npos) {
                break;
            }
            f.push_back(line.substr(at, tab - at));
            at = tab + 1;
            if (f.size() == 6) {
                f.push_back(line.substr(at));   // the title, which may hold tabs
            }
        }
        if (f.size() == 7 && !f[1].empty() && f[1] != "NA") {
            entries.push_back({ f[0], f[1], f[2], f[3], f[4], f[5], f[6] });
        }
        start = end + 1;
    }
    // Where each video is fetched from: its own page when it has one, else the media URL (videos embedded in one
    // page share the page, which would always give the first).
    std::map<std::string, int> pages;
    for (const auto& e : entries) {
        ++pages[e.page];
    }
    std::vector<Track> tracks;
    std::set<std::string> keys, sources;
    for (const auto& e : entries) {
        const std::string extractor = e.extractor != "NA" ? e.extractor : e.ieKey;
        if ((extractor == "Youtube" || extractor == "YoutubeTab") && e.id.size() == 11 && youTubeId(e.id)) {
            if (keys.insert(e.id).second) {
                tracks.push_back(Track{ e.id, e.title, e.status, {} });
            }
            continue;
        }
        const bool flat = e.ieKey != "NA" && !e.ieKey.empty();
        std::string from = flat || e.page == "NA" || pages[e.page] > 1 ? e.url : e.page;
        if (from == "NA" || from.empty()) {
            from = e.page != "NA" ? e.page : job.arg;
        }
        if (!sources.insert(from).second) {
            continue;   // one video shown by two tags of the page
        }
        std::string key = trackKey(extractor, e.id);
        for (int n = 2; !keys.insert(key).second; ++n) {
            key = trackKey(extractor, e.id + "_" + std::to_string(n));
        }
        tracks.push_back(Track{ key, e.title == "NA" ? e.id : e.title, e.status, from });
    }
    if (tracks.empty()) {
        const std::string why = lastLine(err).empty() ? "no videos listed" : lastLine(err);
        logger_.warn("Could not list " + job.arg + ": " + why);
        job.listDone(false, {}, why);
        return;
    }
    logger_.info("Listed " + std::to_string(tracks.size()) + " video(s) from " + job.arg + (ran ? "" : " (yt-dlp reported an error)"));
    job.listDone(true, std::move(tracks), {});
}

void VideoFetcher::shutdown()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        stopping_.store(true);
        jobs_.clear();
        cv_.notify_all();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (job_) {
        CloseHandle(static_cast<HANDLE>(job_));
        job_ = nullptr;
    }
}

void VideoFetcher::loop()
{
    std::unique_lock lock(mutex_);
    while (!stop_) {
        cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
        if (stop_) {
            break;
        }
        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        lock.unlock();
        run(job);
        lock.lock();
    }
}

void VideoFetcher::run(Job& job)
{
    if (job.kind == Kind::List) {
        runList(job);
        return;
    }
    if (job.kind == Kind::Stream) {
        runStream(job);
        return;
    }
    if (job.kind == Kind::Subtitles) {
        runSubtitles(job);
        return;
    }
    auto finish = [&job](bool ok, const std::filesystem::path& file, const std::string& error) {
        if (job.done) {
            job.done(ok, file, error);
        }
    };
    const std::string& videoId = job.arg;
    auto prepareAndFinish = [&](const std::filesystem::path& original) {
        std::string error;
        auto prepared = prepareVideo(original, error);
        if (stopping_.load()) {
            return;
        }
        if (!prepared) {
            logger_.warn("Could not prepare video " + videoId + ": " + error);
            finish(false, {}, error);
            return;
        }
        finish(true, *prepared, {});
    };
    if (auto file = cachedFile(videoId)) {
        if (job.kind == Kind::Download) {
            logger_.info("Video " + videoId + " found in the cache.");
        }
        prepareAndFinish(*file);
        return;
    }
    Settings s;
    {
        std::lock_guard lock(mutex_);
        s = settings_;
    }
    std::error_code ec;
    if (s.ytdlp.empty() || !std::filesystem::exists(s.ytdlp, ec)) {
        finish(false, {}, "yt-dlp.exe not found (set ytdlp_path in ScreenSFSE.ini)");
        return;
    }
    std::filesystem::create_directories(s.cacheDir, ec);
    const bool haveFfmpeg = !s.ffmpeg.empty() && std::filesystem::exists(s.ffmpeg, ec);
    if (!haveFfmpeg) {
        finish(false, {}, "ffmpeg.exe is required to prepare videos (set ffmpeg_path in ScreenSFSE.ini)");
        return;
    }
    const std::wstring h = std::to_wstring(s.maxHeight);
    // Keep every fallback H.264: the compatibility filter deliberately rejects other codecs.
    const std::wstring format = L"bv*[vcodec^=avc1][height<=" + h +
        L"]+ba[ext=m4a]/b[vcodec^=avc1][ext=mp4][height<=" + h + L"]";
    // A live stream is never downloaded (it would record until the timeout): the filter skips it and the caller
    // streams it instead (kLiveError).
    std::vector<std::wstring> args = { L"--ignore-config", L"--no-warnings", L"--no-progress", L"--no-playlist",
                                       L"--no-part", L"--no-mtime", L"--force-overwrites", L"-f", format,
                                       L"--match-filter", L"!is_live" };
    if (!s.deno.empty() && std::filesystem::exists(s.deno, ec)) {
        args.insert(args.end(), { L"--js-runtimes", L"deno:" + s.deno.wstring() });
    }
    args.insert(args.end(), { L"--ffmpeg-location", s.ffmpeg.wstring(), L"--merge-output-format", L"mp4" });
    args.insert(args.end(), { L"-o", (s.cacheDir / std::filesystem::path(widen(videoId))).wstring() + L".%(ext)s",
                              L"--", videoUrl(job) });
    std::wstring commandLine = quoteArg(s.ytdlp.wstring());
    for (const auto& a : args) {
        commandLine += L" " + quoteArg(a);
    }
    logger_.info(std::string(job.kind == Kind::Prefetch ? "Prefetching" : "Downloading") + " video " + videoId +
                 " (H.264 <= " + std::to_string(s.maxHeight) + "p + AAC)");
    const auto started = std::chrono::steady_clock::now();
    std::string out;
    std::string err;
    const bool ran = runProcess(commandLine, out, err);
    if (stopping_.load()) {
        return;
    }
    auto file = cachedFile(videoId);
    if (ran && file) {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        logger_.info("Downloaded video " + videoId + " in " + std::to_string(static_cast<int>(seconds)) + " s (" +
                     std::to_string(std::filesystem::file_size(*file, ec) / (1024 * 1024)) + " MiB)");
        trimCache(*file);
        prepareAndFinish(*file);
    } else if (out.find("does not pass filter") != std::string::npos || err.find("does not pass filter") != std::string::npos) {
        logger_.info("Video " + videoId + " is a live stream: not downloaded.");
        finish(false, {}, kLiveError);
    } else if (err.find("Requested format is not available") != std::string::npos) {
        // other sites often offer WebM/VP9 or other codecs only: ffmpeg streams those (LivePlayer)
        logger_.info("Video " + videoId + " has no H.264 MP4 to download: it will be streamed.");
        finish(false, {}, kStreamOnlyError);
    } else {
        const std::string why = lastLine(err).empty() ? "yt-dlp failed" : lastLine(err);
        logger_.warn("Could not download video " + videoId + ": " + why);
        finish(false, {}, why);
    }
}

std::optional<std::filesystem::path> VideoFetcher::prepareVideo(const std::filesystem::path& original,
                                                              std::string& error)
{
    const auto dir = original.parent_path() / kPreparedDir;
    const auto prepared = dir / original.filename();
    const auto marker = dir / (original.stem().wstring() + L".stamp");
    const auto pending = dir / (original.stem().wstring() + L".pending.mp4");
    const auto pendingMarker = dir / (original.stem().wstring() + L".pending.stamp");
    const auto sourceStamp = fileStamp(original);
    const auto preparedStamp = fileStamp(prepared);
    std::ifstream in(marker);
    std::string savedSource, savedPrepared;
    if (!sourceStamp.empty() && !preparedStamp.empty() && std::getline(in, savedSource) &&
        std::getline(in, savedPrepared) && savedSource == sourceStamp && savedPrepared == preparedStamp) {
        return prepared;
    }
    in.close();
    std::filesystem::path ffmpeg;
    {
        std::lock_guard lock(mutex_);
        ffmpeg = settings_.ffmpeg;
    }
    std::error_code ec;
    if (ffmpeg.empty() || !std::filesystem::exists(ffmpeg, ec)) {
        error = "ffmpeg.exe is required to prepare this video (set ffmpeg_path in ScreenSFSE.ini)";
        return std::nullopt;
    }
    std::filesystem::create_directories(dir, ec);
    if (ec || sourceStamp.empty()) {
        error = "cannot access the video compatibility cache";
        return std::nullopt;
    }
    // h264_mp4toannexb also guards the filter: SEI is NAL type 6 only for H.264, not HEVC or AV1.
    // MP4 muxing restores length-prefixed packets. No picture or audio re-encoding is performed.
    const std::vector<std::wstring> args = { L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-y",
        L"-i", original.wstring(), L"-map", L"0:v:0", L"-map", L"0:a?", L"-c", L"copy",
        L"-bsf:v", L"h264_mp4toannexb,filter_units=remove_types=6", pending.wstring() };
    std::wstring commandLine = quoteArg(ffmpeg.wstring());
    for (const auto& arg : args) {
        commandLine += L" " + quoteArg(arg);
    }
    logger_.info("Preparing video " + original.stem().string() + " for Media Foundation (H.264 SEI cleanup, stream copy).");
    std::string out, err;
    const bool ran = runProcess(commandLine, out, err, ffmpeg);
    auto discardPending = [&] {
        std::filesystem::remove(pending, ec);
        std::filesystem::remove(pendingMarker, ec);
    };
    if (!ran || stopping_.load() || fileStamp(pending).empty() || fileStamp(original) != sourceStamp) {
        discardPending();
        error = lastLine(err).empty() ? "video compatibility preparation failed or was interrupted" : lastLine(err);
        return std::nullopt;
    }
    if (!MoveFileExW(pending.c_str(), prepared.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot publish the prepared video (error " + std::to_string(GetLastError()) + ")";
        discardPending();
        return std::nullopt;
    }
    // A partial/crashed conversion is never accepted: publish the identity marker only after the final MP4.
    std::ofstream stamp(pendingMarker, std::ios::trunc);
    stamp << sourceStamp << '\n' << fileStamp(prepared) << '\n';
    stamp.close();
    if (!stamp || !MoveFileExW(pendingMarker.c_str(), marker.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot save the video compatibility stamp";
        discardPending();
        return std::nullopt;
    }
    logger_.info("Prepared video " + original.stem().string() + "; original download preserved.");
    return prepared;
}

void VideoFetcher::runStream(Job& job)
{
    Settings s;
    {
        std::lock_guard lock(mutex_);
        s = settings_;
    }
    std::error_code ec;
    if (s.ytdlp.empty() || !std::filesystem::exists(s.ytdlp, ec)) {
        job.streamDone(false, {}, {}, 0.0, {}, "yt-dlp.exe not found");
        return;
    }
    const std::wstring h = std::to_wstring(s.maxHeight);
    std::vector<std::wstring> args = baseArgs(s);
    // live HLS: YouTube serves video and sound as separate playlists (two URLs); a muxed one if offered.
    // --print duration and the HTTP headers (one JSON line) come before the URLs (duration "NA" for a live stream).
    args.insert(args.end(), { L"--no-playlist", L"-f",
                              L"bv*[height<=" + h + L"][vcodec^=avc1]+ba/b[height<=" + h + L"]/bv*[height<=" + h + L"]+ba/b",
                              L"--print", L"duration", L"--print", L"%(http_headers)j", L"-g", L"--", videoUrl(job) });
    std::wstring commandLine = quoteArg(s.ytdlp.wstring());
    for (const auto& a : args) {
        commandLine += L" " + quoteArg(a);
    }
    std::string out;
    std::string err;
    const bool ran = runProcess(commandLine, out, err);
    if (stopping_.load()) {
        return;
    }
    std::vector<std::string> urls;
    double duration = 0.0;
    std::string headers;
    std::size_t start = 0;
    while (start < out.size()) {
        std::size_t end = out.find('\n', start);
        if (end == std::string::npos) {
            end = out.size();
        }
        std::string line = out.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        bool url = line.rfind("http", 0) == 0;
#ifdef SCREENSFSE_HARNESS
        url = url || line.rfind("file:", 0) == 0;   // test fixtures stream local files
#endif
        if (url) {
            urls.push_back(line);
        } else if (line.starts_with("{")) {
            headers = headerLines(line);
        } else if (!line.empty() && line.find_first_not_of("0123456789.") == std::string::npos) {
            duration = std::atof(line.c_str());
        }
        start = end + 1;
    }
    if (!ran || urls.empty()) {
        const std::string why = lastLine(err).empty() ? "no stream URL" : lastLine(err);
        logger_.warn("Could not open the stream of " + job.arg + ": " + why);
        job.streamDone(false, {}, {}, 0.0, {}, why);
        return;
    }
    logger_.info("Stream " + job.arg + ": " + std::to_string(urls.size()) + " URL(s)" +
                 (duration > 0 ? ", " + std::to_string(static_cast<int>(duration)) + " s." : "."));
    job.streamDone(true, urls[0], urls.size() > 1 ? urls[1] : std::string(), duration, headers, {});
}

void VideoFetcher::trimCache(const std::filesystem::path& requestedFile)
{
    std::filesystem::path dir;
    std::size_t keep = 20;
    {
        std::lock_guard lock(mutex_);
        dir = settings_.cacheDir;
        keep = (std::max<std::size_t>)(settings_.cacheFiles, 2);
    }
    std::error_code ec;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == L".mp4") {
            files.emplace_back(it->last_write_time(ec), it->path());
        }
    }
    if (files.size() <= keep) {
        return;
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = keep; i < files.size(); ++i) {
        if (files[i].second == requestedFile) {
            continue;   // timestamp ties must not evict the download we are about to prepare/play
        }
        const auto preparedDir = dir / kPreparedDir;
        std::filesystem::remove(preparedDir / files[i].second.filename(), ec);
        if (ec) {   // the prepared copy may be playing; keep its original and marker too
            continue;
        }
        std::filesystem::remove(preparedDir / (files[i].second.stem().wstring() + L".stamp"), ec);
        std::filesystem::remove(files[i].second, ec);   // a file being played stays (remove fails)
        const std::wstring subsPrefix = files[i].second.stem().wstring() + L".";
        std::vector<std::filesystem::path> subs;
        for (std::filesystem::directory_iterator it(dir / L"subs", ec), end; it != end && !ec; it.increment(ec)) {
            if (it->path().filename().wstring().starts_with(subsPrefix)) {
                subs.push_back(it->path());
            }
        }
        for (const auto& sub : subs) {
            std::filesystem::remove(sub, ec);
        }
    }
}

bool VideoFetcher::runProcess(const std::wstring& commandLine, std::string& out, std::string& err,
                              const std::filesystem::path& executable)
{
    std::filesystem::path exe;
    std::chrono::seconds timeout{};
    {
        std::lock_guard lock(mutex_);
        exe = executable.empty() ? settings_.ytdlp : executable;
        timeout = settings_.timeout;
    }
    if (!job_) {
        if (HANDLE job = CreateJobObjectW(nullptr, nullptr)) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            job_ = job;
        }
    }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&errRead, &errWrite, &sa, 0)) {
        err = "CreatePipe failed";
        for (HANDLE h : { outRead, outWrite, errRead, errWrite }) {
            if (h) {
                CloseHandle(h);
            }
        }
        return false;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    PROCESS_INFORMATION pi{};
    std::wstring environment = childEnvironment();
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    const std::wstring workDir = exe.parent_path().wstring();
    const BOOL created = CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                        environment.data(), workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (!created) {
        err = "CreateProcess failed for " + exe.filename().string() + " (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(outRead);
        CloseHandle(errRead);
        return false;
    }
    if (job_) {
        AssignProcessToJobObject(static_cast<HANDLE>(job_), pi.hProcess);
    }
    ResumeThread(pi.hThread);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool finished = false;
    bool timedOut = false;
    for (;;) {
        drain(outRead, out);
        drain(errRead, err);
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            finished = true;
            break;
        }
        if (stopping_.load()) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            break;
        }
    }
    if (!finished) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    drain(outRead, out);
    drain(errRead, err);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(outRead);
    CloseHandle(errRead);
    if (timedOut) {
        err += "\n" + exe.filename().string() + " timed out after " + std::to_string(timeout.count()) + " s";
        return false;
    }
    return finished && exitCode == 0;
}
