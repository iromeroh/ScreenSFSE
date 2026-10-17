#include "video_fetcher.h"

#include "logger.h"

#include <algorithm>
#include <cwctype>
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

void VideoFetcher::request(const std::string& videoId, Done done)
{
    std::lock_guard lock(mutex_);
    if (stop_) {
        return;
    }
    // only the newest play request matters; queued prefetches go too (the next track changes with it), lists stay
    std::erase_if(jobs_, [](const Job& j) { return j.kind == Kind::Download || j.kind == Kind::Prefetch; });
    jobs_.push_back(Job{ Kind::Download, videoId, std::move(done), {}, {} });
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { loop(); });
    }
    cv_.notify_all();
}

void VideoFetcher::prefetch(const std::string& videoId)
{
    if (cachedFile(videoId)) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (stop_) {
        return;
    }
    jobs_.push_back(Job{ Kind::Prefetch, videoId, {}, {}, {} });
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { loop(); });
    }
    cv_.notify_all();
}

void VideoFetcher::requestList(const std::string& url, ListDone done)
{
    std::lock_guard lock(mutex_);
    if (stop_) {
        return;
    }
    jobs_.push_front(Job{ Kind::List, url, {}, std::move(done), {} });
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { loop(); });
    }
    cv_.notify_all();
}

void VideoFetcher::requestStream(const std::string& videoId, StreamDone done)
{
    std::lock_guard lock(mutex_);
    if (stop_) {
        return;
    }
    jobs_.push_front(Job{ Kind::Stream, videoId, {}, {}, std::move(done) });
    if (!thread_.joinable()) {
        thread_ = std::thread([this] { loop(); });
    }
    cv_.notify_all();
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
    args.insert(args.end(), { L"--flat-playlist", L"--print", L"%(id)s\t%(live_status)s\t%(title)s", L"--", widen(job.arg) });
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
    std::vector<Track> tracks;
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
        // <id> TAB <live_status> TAB <title>; live_status: is_live, is_upcoming, was_live, not_live, post_live, NA
        const std::size_t tab = line.find('\t');
        const std::size_t tab2 = tab == 11 ? line.find('\t', 12) : std::string::npos;
        if (tab == 11 && tab2 != std::string::npos && youTubeId(line.substr(0, 11))) {
            tracks.push_back(Track{ line.substr(0, 11), line.substr(tab2 + 1), line.substr(12, tab2 - 12) });
        }
        start = end + 1;
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
    auto finish = [&job](bool ok, const std::filesystem::path& file, const std::string& error) {
        if (job.done) {
            job.done(ok, file, error);
        }
    };
    const std::string& videoId = job.arg;
    if (auto file = cachedFile(videoId)) {
        if (job.kind == Kind::Download) {
            logger_.info("Video " + videoId + " found in the cache.");
        }
        finish(true, *file, {});
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
    const std::wstring h = std::to_wstring(s.maxHeight);
    // With ffmpeg: best H.264 video up to maxHeight + best AAC, merged into one MP4. Without: a single-file MP4
    // (YouTube's progressive format, at most 360p).
    const std::wstring format = haveFfmpeg
        ? L"bv*[vcodec^=avc1][height<=" + h + L"]+ba[ext=m4a]/b[vcodec^=avc1][ext=mp4][height<=" + h + L"]/b[ext=mp4]"
        : L"b[vcodec^=avc1][ext=mp4]/b[ext=mp4]";
    // A live stream is never downloaded (it would record until the timeout): the filter skips it and the caller
    // streams it instead (kLiveError).
    std::vector<std::wstring> args = { L"--ignore-config", L"--no-warnings", L"--no-progress", L"--no-playlist",
                                       L"--no-part", L"--no-mtime", L"--force-overwrites", L"-f", format,
                                       L"--match-filter", L"!is_live" };
    if (!s.deno.empty() && std::filesystem::exists(s.deno, ec)) {
        args.insert(args.end(), { L"--js-runtimes", L"deno:" + s.deno.wstring() });
    }
    if (haveFfmpeg) {
        args.insert(args.end(), { L"--ffmpeg-location", s.ffmpeg.wstring(), L"--merge-output-format", L"mp4" });
    }
    args.insert(args.end(), { L"-o", (s.cacheDir / std::filesystem::path(widen(videoId))).wstring() + L".%(ext)s",
                              L"--", L"https://www.youtube.com/watch?v=" + widen(videoId) });
    std::wstring commandLine = quoteArg(s.ytdlp.wstring());
    for (const auto& a : args) {
        commandLine += L" " + quoteArg(a);
    }
    logger_.info(std::string(job.kind == Kind::Prefetch ? "Prefetching" : "Downloading") + " video " + videoId + (haveFfmpeg ? " (H.264 <= " + std::to_string(s.maxHeight) + "p + AAC)" : " (single-file MP4, no ffmpeg)"));
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
        trimCache();
        finish(true, *file, {});
    } else if (out.find("does not pass filter") != std::string::npos || err.find("does not pass filter") != std::string::npos) {
        logger_.info("Video " + videoId + " is a live stream: not downloaded.");
        finish(false, {}, kLiveError);
    } else {
        const std::string why = lastLine(err).empty() ? "yt-dlp failed" : lastLine(err);
        logger_.warn("Could not download video " + videoId + ": " + why);
        finish(false, {}, why);
    }
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
        job.streamDone(false, {}, {}, "yt-dlp.exe not found");
        return;
    }
    const std::wstring h = std::to_wstring(s.maxHeight);
    std::vector<std::wstring> args = baseArgs(s);
    // live HLS: YouTube serves video and sound as separate playlists (two URLs); a muxed one if offered
    args.insert(args.end(), { L"--no-playlist", L"-f",
                              L"bv*[height<=" + h + L"][vcodec^=avc1]+ba/b[height<=" + h + L"]/b", L"-g", L"--",
                              L"https://www.youtube.com/watch?v=" + widen(job.arg) });
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
        if (line.rfind("http", 0) == 0) {
            urls.push_back(line);
        }
        start = end + 1;
    }
    if (!ran || urls.empty()) {
        const std::string why = lastLine(err).empty() ? "no stream URL" : lastLine(err);
        logger_.warn("Could not open live stream " + job.arg + ": " + why);
        job.streamDone(false, {}, {}, why);
        return;
    }
    logger_.info("Live stream " + job.arg + ": " + std::to_string(urls.size()) + " HLS URL(s).");
    job.streamDone(true, urls[0], urls.size() > 1 ? urls[1] : std::string(), {});
}

void VideoFetcher::trimCache()
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
        std::filesystem::remove(files[i].second, ec);   // a file being played stays (remove fails)
    }
}

bool VideoFetcher::runProcess(const std::wstring& commandLine, std::string& out, std::string& err)
{
    std::filesystem::path exe;
    std::chrono::seconds timeout{};
    {
        std::lock_guard lock(mutex_);
        exe = settings_.ytdlp;
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
        err = "CreateProcess failed for yt-dlp (error " + std::to_string(GetLastError()) + ")";
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
        err += "\nyt-dlp timed out after " + std::to_string(timeout.count()) + " s";
        return false;
    }
    return finished && exitCode == 0;
}
