#include "live_player.h"

#include "logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <xaudio2.h>

namespace
{
constexpr double kFps = 30.0;
constexpr std::size_t kMaxFrames = 12;          // decoded, not yet shown; beyond this the oldest is dropped
constexpr std::size_t kKeepFrames = 4;          // buffer kept against bursts; more than this is shown faster
constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kChannels = 2;
constexpr std::size_t kChunkBytes = 2400 * 4;   // 50 ms of 48 kHz stereo s16
constexpr UINT32 kMaxQueued = 20;               // 1 s of sound queued at most: beyond that a chunk is dropped
constexpr std::size_t kMaxBufferedBytes = 64u << 20;   // compressed stream kept ahead at most (the fetcher waits)

std::wstring widen(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>((std::max)(size, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// CommandLineToArgvW quoting (as video_fetcher.cpp)
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

}

LivePlayer::LivePlayer(Logger& logger, std::uint32_t width, std::uint32_t height, std::filesystem::path ffmpeg,
                       double bufferSeconds) :
    logger_(logger), width_(width), height_(height), ffmpeg_(std::move(ffmpeg)),
    bufferSeconds_(std::clamp(bufferSeconds, 0.0, 30.0))
{
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
}

LivePlayer::~LivePlayer()
{
    stop();
    if (job_) {
        CloseHandle(static_cast<HANDLE>(job_));
    }
    WSACleanup();
}

void LivePlayer::fail(const std::string& message)
{
    logger_.warn("Live: " + message);
    std::lock_guard lock(errorMutex_);
    lastError_ = message;
}

std::string LivePlayer::lastError() const
{
    std::lock_guard lock(errorMutex_);
    return lastError_;
}

bool LivePlayer::open(const std::string& videoUrl, const std::string& audioUrl)
{
    url_ = videoUrl;
    audioUrl_ = audioUrl;
    std::error_code ec;
    if (ffmpeg_.empty() || !std::filesystem::exists(ffmpeg_, ec)) {
        fail("ffmpeg.exe not found (live streams need it; see ScreenSFSE.ini)");
        return false;
    }
    playing_.store(true);
    return start();
}

// Starts a hidden child process (killed with the game through the job object). Pipes: stdin (write end returned)
// when stdinWrite is given, stdout/stderr (read ends returned).
void* LivePlayer::spawn(const std::vector<std::wstring>& args, void** stdinWrite, void** stdoutRead, void** stderrRead)
{
    std::wstring commandLine = quoteArg(ffmpeg_.wstring());
    for (const auto& a : args) {
        commandLine += L" " + quoteArg(a);
    }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    bool ok = CreatePipe(&outRead, &outWrite, &sa, 1 << 22) && CreatePipe(&errRead, &errWrite, &sa, 0);
    if (ok && stdinWrite) {
        ok = CreatePipe(&inRead, &inWrite, &sa, 1 << 20);
    }
    auto closeAll = [&] {
        for (HANDLE x : { inRead, inWrite, outRead, outWrite, errRead, errWrite }) {
            if (x) {
                CloseHandle(x);
            }
        }
    };
    if (!ok) {
        closeAll();
        return nullptr;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    if (inWrite) {
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');
    const BOOL created = CreateProcessW(nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    for (HANDLE x : { inRead, outWrite, errWrite }) {   // the child's ends
        if (x) {
            CloseHandle(x);
        }
    }
    if (!created) {
        for (HANDLE x : { inWrite, outRead, errRead }) {
            if (x) {
                CloseHandle(x);
            }
        }
        fail("cannot start ffmpeg (error " + std::to_string(GetLastError()) + ")");
        return nullptr;
    }
    if (!job_) {
        if (HANDLE job = CreateJobObjectW(nullptr, nullptr)) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            job_ = job;
        }
    }
    if (job_) {
        AssignProcessToJobObject(static_cast<HANDLE>(job_), pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    if (stdinWrite) {
        *stdinWrite = inWrite;
    }
    *stdoutRead = outRead;
    *stderrRead = errRead;
    return pi.hProcess;
}

bool LivePlayer::start()
{
    quit_.store(false);
    userStopped_.store(false);
    ended_.store(false);
    {
        std::lock_guard lock(framesMutex_);
        for (auto& f : frames_) {
            pool_.push_back(std::move(f));
        }
        frames_.clear();
        framesRead_ = 0;
        nextShow_ = {};
    }
    {
        std::lock_guard lock(ringMutex_);
        ring_.clear();
        ringBytes_ = 0;
        fetchEnded_ = false;
        firstByte_ = {};
    }

    // sound: the decoder connects to this localhost port
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int len = sizeof(addr);
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener, 1) != 0 || getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        if (listener != INVALID_SOCKET) {
            closesocket(listener);
        }
        fail("cannot open the local sound socket");
        return false;
    }
    const int port = ntohs(addr.sin_port);

    // 1. fetch: the live HLS as fast as it comes (no decoding, no -re) -> MPEG-TS -> our buffer
    std::vector<std::wstring> fetchArgs = { L"-hide_banner", L"-loglevel", L"error", L"-nostdin", L"-i", widen(url_) };
    if (!audioUrl_.empty()) {
        fetchArgs.insert(fetchArgs.end(), { L"-i", widen(audioUrl_) });
    }
    fetchArgs.insert(fetchArgs.end(), { L"-map", L"0:v:0", L"-map", audioUrl_.empty() ? L"0:a:0" : L"1:a:0", L"-c", L"copy",
                                        L"-f", L"mpegts", L"pipe:1" });
    void* fetchOut = nullptr;
    void* fetchErr = nullptr;
    fetchProcess_ = spawn(fetchArgs, nullptr, &fetchOut, &fetchErr);

    // 2. decode: from the buffer at the stream's pace (-re) -> screen-sized RGBA frames + PCM sound
    const std::wstring w = std::to_wstring(width_);
    const std::wstring h = std::to_wstring(height_);
    const std::wstring filter = L"scale=" + w + L":" + h + L":force_original_aspect_ratio=decrease,pad=" + w + L":" + h +
                                L":(ow-iw)/2:(oh-ih)/2:color=black,fps=30,format=rgba";
    const std::vector<std::wstring> decodeArgs = {
        L"-hide_banner", L"-loglevel", L"error", L"-re", L"-f", L"mpegts", L"-i", L"pipe:0",
        L"-map", L"0:v:0", L"-vf", filter, L"-f", L"rawvideo", L"-pix_fmt", L"rgba", L"pipe:1",
        L"-map", L"0:a:0", L"-ac", L"2", L"-ar", L"48000", L"-f", L"s16le", L"tcp://127.0.0.1:" + std::to_wstring(port) };
    void* decodeIn = nullptr;
    void* decodeOut = nullptr;
    void* decodeErr = nullptr;
    process_ = fetchProcess_ ? spawn(decodeArgs, &decodeIn, &decodeOut, &decodeErr) : nullptr;
    if (!fetchProcess_ || !process_) {
        closesocket(listener);
        if (fetchProcess_) {
            TerminateProcess(static_cast<HANDLE>(fetchProcess_), 0);
            for (void* x : { fetchOut, fetchErr }) {
                CloseHandle(static_cast<HANDLE>(x));
            }
            CloseHandle(static_cast<HANDLE>(fetchProcess_));
            fetchProcess_ = nullptr;
        }
        return false;
    }
    {
        std::lock_guard lock(clockMutex_);
        started_ = std::chrono::steady_clock::now();
    }
    logger_.info("Live stream started (ffmpeg fetch + decode, " + std::to_string(width_) + "x" + std::to_string(height_) +
                 ", " + std::to_string(bufferSeconds_) + " s buffer, sound on port " + std::to_string(port) + ").");
    fetchThread_ = std::thread([this, fetchOut] { fetchLoop(fetchOut); });
    feedThread_ = std::thread([this, decodeIn] { feedLoop(decodeIn); });
    fetchErrorThread_ = std::thread([this, fetchErr] { errorLoop(fetchErr); });
    videoThread_ = std::thread([this, decodeOut] { videoLoop(decodeOut); });
    errorThread_ = std::thread([this, decodeErr] { errorLoop(decodeErr); });
    audioThread_ = std::thread([this, listener] { audioLoop(static_cast<std::uintptr_t>(listener)); });
    return true;
}

// Stage 1 output -> the buffer (blocks, so the fetcher waits, when the buffer is far ahead).
void LivePlayer::fetchLoop(void* pipe)
{
    std::vector<char> chunk(1 << 16);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(static_cast<HANDLE>(pipe), chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr) || read == 0) {
            break;
        }
        std::unique_lock lock(ringMutex_);
        ringCv_.wait(lock, [this] { return quit_.load() || ringBytes_ < kMaxBufferedBytes; });
        if (quit_.load()) {
            break;
        }
        if (firstByte_ == std::chrono::steady_clock::time_point{}) {
            firstByte_ = std::chrono::steady_clock::now();
        }
        ring_.emplace_back(chunk.begin(), chunk.begin() + read);
        ringBytes_ += read;
        ringCv_.notify_all();
    }
    CloseHandle(static_cast<HANDLE>(pipe));
    std::lock_guard lock(ringMutex_);
    fetchEnded_ = true;
    ringCv_.notify_all();
}

// The buffer -> stage 2's stdin, once `bufferSeconds_` of stream have arrived. -re makes stage 2 read in real time,
// so the buffer keeps about that much ahead and covers network stalls up to that long.
void LivePlayer::feedLoop(void* pipe)
{
    {
        std::unique_lock lock(ringMutex_);
        const auto filled = [this] {
            return quit_.load() || fetchEnded_ ||
                   (firstByte_ != std::chrono::steady_clock::time_point{} &&
                    std::chrono::steady_clock::now() - firstByte_ >= std::chrono::duration<double>(bufferSeconds_));
        };
        while (!filled()) {
            ringCv_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (!quit_.load()) {
            logger_.info("Live buffer filled: " + std::to_string(ringBytes_ / 1024) + " KiB before playback.");
        }
    }
    std::size_t lowest = SIZE_MAX;
    auto lastReport = std::chrono::steady_clock::now();
    for (;;) {
        std::vector<char> chunk;
        {
            std::unique_lock lock(ringMutex_);
            ringCv_.wait(lock, [this] { return quit_.load() || !ring_.empty() || fetchEnded_; });
            if (quit_.load() || ring_.empty()) {
                break;   // stopped, or the fetcher ended and everything was fed
            }
            chunk = std::move(ring_.front());
            ring_.pop_front();
            ringBytes_ -= chunk.size();
            lowest = std::min(lowest, ringBytes_);
            ringCv_.notify_all();
        }
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(pipe), chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr)) {
            break;
        }
        if (std::chrono::steady_clock::now() - lastReport >= std::chrono::seconds(60)) {
            std::lock_guard lock(ringMutex_);
            logger_.info("Live buffer: " + std::to_string(ringBytes_ / 1024) + " KiB now, lowest " + std::to_string(lowest / 1024) +
                         " KiB in the last minute.");
            lowest = SIZE_MAX;
            lastReport = std::chrono::steady_clock::now();
        }
    }
    CloseHandle(static_cast<HANDLE>(pipe));   // end of input: the decoder finishes and exits
}

void LivePlayer::stop()
{
    userStopped_.store(true);
    quit_.store(true);
    {
        std::lock_guard lock(ringMutex_);
        ringCv_.notify_all();
    }
    for (void* p : { fetchProcess_, process_ }) {
        if (p) {
            TerminateProcess(static_cast<HANDLE>(p), 0);   // closes their pipes and socket: the threads unblock
        }
    }
    for (std::thread* t : { &fetchThread_, &feedThread_, &fetchErrorThread_, &videoThread_, &audioThread_, &errorThread_ }) {
        if (t->joinable()) {
            t->join();
        }
    }
    for (void** p : { &fetchProcess_, &process_ }) {
        if (*p) {
            CloseHandle(static_cast<HANDLE>(*p));
            *p = nullptr;
        }
    }
}

void LivePlayer::play()
{
    if (playing_.exchange(true)) {
        return;
    }
    start();   // back to the live edge
}

void LivePlayer::pause()
{
    if (!playing_.exchange(false)) {
        return;
    }
    const double run = clock();
    {
        std::lock_guard lock(framesMutex_);
        logger_.info("Live run: " + std::to_string(framesRead_) + " frames read in " + std::to_string(run) + " s, " +
                     std::to_string(droppedFull_) + " dropped (burst), " + std::to_string(skippedLate_) + " skipped late.");
    }
    stop();
    std::lock_guard lock(clockMutex_);
    offset_ += run;
}

bool LivePlayer::finished() const
{
    if (!ended_.load() || userStopped_.load()) {
        return false;
    }
    std::lock_guard lock(framesMutex_);
    return frames_.empty();
}

// Seconds into the current run (ffmpeg -re paces video and sound in real time).
double LivePlayer::clock() const
{
    std::lock_guard lock(clockMutex_);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

double LivePlayer::positionSeconds() const
{
    std::lock_guard lock(clockMutex_);
    if (!playing_.load()) {
        return offset_;
    }
    return offset_ + std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

void LivePlayer::setGains(float left, float right)
{
    std::lock_guard lock(audioMutex_);
    gainL_ = std::clamp(left, 0.0f, 1.0f);
    gainR_ = std::clamp(right, 0.0f, 1.0f);
    applyGains();
}

// Caller holds audioMutex_.
void LivePlayer::applyGains()
{
    if (!voice_) {
        return;
    }
    std::vector<float> m(static_cast<std::size_t>(kChannels) * masterChannels_, 0.0f);
    m[0 * kChannels + 0] = gainL_;   // source L -> front left
    m[1 * kChannels + 1] = gainR_;   // source R -> front right
    voice_->SetOutputMatrix(nullptr, kChannels, masterChannels_, m.data());
}

std::uint64_t LivePlayer::copyLatest(std::uint64_t lastSeq, std::uint8_t* dst, std::uint32_t dstPitch)
{
    const auto now = std::chrono::steady_clock::now();
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / kFps));
    std::unique_lock lock(framesMutex_);
    if (!frames_.empty() && (current_.seq == 0 || now >= nextShow_ || frames_.size() > kKeepFrames)) {
        while (frames_.size() > kKeepFrames + 1) {   // too far behind: skip ahead, keep the buffer short
            ++skippedLate_;
            pool_.push_back(std::move(frames_.front()));
            frames_.pop_front();
        }
        if (current_.seq != 0) {
            pool_.push_back(std::move(current_));
        }
        current_ = std::move(frames_.front());
        frames_.pop_front();
        nextShow_ = std::max(nextShow_ + period, now - period);
    }
    if (current_.seq == 0 || current_.seq == lastSeq) {
        return 0;
    }
    const std::size_t rowBytes = std::size_t{ width_ } * 4;
    for (std::uint32_t y = 0; y < height_; ++y) {
        std::memcpy(dst + std::size_t{ y } * dstPitch, current_.pixels.data() + std::size_t{ y } * width_, rowBytes);
    }
    return current_.seq;
}

void LivePlayer::videoLoop(void* pipe)
{
    const std::size_t frameBytes = std::size_t{ width_ } * height_ * 4;
    for (;;) {
        Frame frame;
        {
            std::lock_guard lock(framesMutex_);
            if (!pool_.empty()) {
                frame = std::move(pool_.back());
                pool_.pop_back();
            }
        }
        frame.pixels.resize(std::size_t{ width_ } * height_);
        auto* bytes = reinterpret_cast<std::uint8_t*>(frame.pixels.data());
        std::size_t got = 0;
        while (got < frameBytes) {
            DWORD read = 0;
            if (!ReadFile(static_cast<HANDLE>(pipe), bytes + got, static_cast<DWORD>(std::min<std::size_t>(frameBytes - got, 1 << 20)), &read, nullptr) || read == 0) {
                break;
            }
            got += read;
        }
        if (got < frameBytes) {
            break;   // ffmpeg ended (stream over, error, or stopped)
        }
        if (framesRead_ == 0) {
            std::lock_guard clockLock(clockMutex_);
            started_ = std::chrono::steady_clock::now();   // the run's clock starts with its first frame
        }
        std::lock_guard lock(framesMutex_);
        frame.pts = static_cast<double>(framesRead_++) / kFps;
        frame.seq = nextSeq_++;
        frames_.push_back(std::move(frame));
        while (frames_.size() > kMaxFrames) {
            ++droppedFull_;
            pool_.push_back(std::move(frames_.front()));
            frames_.pop_front();
        }
    }
    CloseHandle(static_cast<HANDLE>(pipe));
    if (!userStopped_.load()) {
        ended_.store(true);
        logger_.info("Live stream ended after " + std::to_string(framesRead_) + " frames.");
    }
}

void LivePlayer::errorLoop(void* pipe)
{
    std::string text;
    char buffer[1024];
    DWORD read = 0;
    while (ReadFile(static_cast<HANDLE>(pipe), buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        text.append(buffer, read);
        std::size_t nl;
        while ((nl = text.find('\n')) != std::string::npos) {
            std::string line = text.substr(0, nl);
            text.erase(0, nl + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty() && !userStopped_.load()) {
                fail("ffmpeg: " + line.substr(0, 200));
            }
        }
    }
    CloseHandle(static_cast<HANDLE>(pipe));
}

void LivePlayer::audioLoop(std::uintptr_t listenSocket)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // XAudio2 needs COM on this thread
    const SOCKET listener = static_cast<SOCKET>(listenSocket);
    SOCKET client = INVALID_SOCKET;
    while (!quit_.load() && client == INVALID_SOCKET) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval tv{ 0, 200000 };
        if (select(0, &set, nullptr, nullptr, &tv) > 0) {
            client = accept(listener, nullptr, nullptr);
        }
        if (ended_.load()) {
            break;
        }
    }
    closesocket(listener);
    if (client == INVALID_SOCKET) {
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        return;
    }
    DWORD timeoutMs = 200;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = kChannels;
    wfx.nSamplesPerSec = kRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = kRate * 4;
    HRESULT h = E_FAIL;
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
            h = xaudio_->CreateSourceVoice(&voice_, &wfx);
        }
        if (SUCCEEDED(h)) {
            applyGains();
            voice_->Start(0);
        }
    }
    if (FAILED(h)) {
        fail("XAudio2 output failed; playing silent");
    }
    std::deque<std::vector<char>> inflight;
    std::vector<char> chunk(kChunkBytes);
    std::size_t filled = 0;
    while (!quit_.load()) {
        const int n = recv(client, chunk.data() + filled, static_cast<int>(kChunkBytes - filled), 0);
        if (n == 0) {
            break;   // ffmpeg closed the stream
        }
        if (n < 0) {
            if (WSAGetLastError() == WSAETIMEDOUT) {
                continue;
            }
            break;
        }
        filled += static_cast<std::size_t>(n);
        if (filled < kChunkBytes || FAILED(h)) {
            if (filled == kChunkBytes) {
                filled = 0;
            }
            continue;
        }
        filled = 0;
        std::lock_guard lock(audioMutex_);
        XAUDIO2_VOICE_STATE state{};
        voice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        while (inflight.size() > state.BuffersQueued) {
            inflight.pop_front();
        }
        if (state.BuffersQueued >= kMaxQueued) {
            continue;   // behind: drop this chunk to keep the delay short
        }
        inflight.push_back(chunk);
        XAUDIO2_BUFFER xb{};
        xb.AudioBytes = static_cast<UINT32>(kChunkBytes);
        xb.pAudioData = reinterpret_cast<const BYTE*>(inflight.back().data());
        if (FAILED(voice_->SubmitSourceBuffer(&xb))) {
            inflight.pop_back();
        }
    }
    closesocket(client);
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
    if (xaudio_) {
        xaudio_->Release();
        xaudio_ = nullptr;
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
}
