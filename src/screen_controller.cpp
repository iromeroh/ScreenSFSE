#include "screen_controller.h"

#include "live_player.h"
#include "logger.h"
#include "media_player.h"
#include "osd.h"
#include "test_pattern.h"
#include "video_fetcher.h"
#include "video_surface.h"

#ifndef SCREENSFSE_HARNESS
#include "RE/P/PlayerCharacter.h"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

#include <windows.h>
#include <shlobj.h>

namespace
{
// World positions are in metres (RadioSFSE fades over 45 game units = metres). Not the NIF/.mesh scale (0.0142875):
// using that made 20 m read as 0.3 m, so the sound never faded (user, 2026-10-14).
constexpr float kMetersPerUnit = 1.0f;
constexpr float kPi = 3.14159265f;

// The player's position and heading straight from the game's player reference (CommonLibSF layout).
// Plain data and SEH only, so a wrong layout or pointer gives `false`, not a crash. Only trusted after
// registerScreen() has compared it with Papyrus' GetPosition values.
bool readPlayer(float& x, float& y, float& z, float& heading)
{
#ifdef SCREENSFSE_HARNESS   // tests/controller_harness.cpp: no game, no Address Library
    (void)x, (void)y, (void)z, (void)heading;
    return false;
#else
    __try {
        RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;
        }
        const RE::TESObjectREFR* ref = player;
        x = ref->data.location.x;
        y = ref->data.location.y;
        z = ref->data.location.z;
        heading = ref->data.angle.z;
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(heading);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}

std::string narrow(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>((std::max)(size, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

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

// %VAR% expansion (RadioSFSE.ini writes deno_path under %USERPROFILE%)
std::filesystem::path expand(const std::filesystem::path& path)
{
    const std::wstring in = path.wstring();
    if (in.find(L'%') == std::wstring::npos) {
        return path;
    }
    const DWORD size = ExpandEnvironmentStringsW(in.c_str(), nullptr, 0);
    std::wstring out(size, L' ');
    const DWORD written = ExpandEnvironmentStringsW(in.c_str(), out.data(), size);
    if (written == 0 || written > size) {
        return path;
    }
    out.resize(written - 1);
    return out;
}

std::filesystem::path documentsDir()
{
    PWSTR raw = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &raw)) && raw) {
        out = raw;
    }
    CoTaskMemFree(raw);
    return out;
}

std::filesystem::path onPath(const wchar_t* exe)
{
    wchar_t buffer[MAX_PATH] = {};
    if (SearchPathW(nullptr, exe, nullptr, MAX_PATH, buffer, nullptr) > 0) {
        return buffer;
    }
    return {};
}

// `key=value` from RadioSFSE.ini (same tool settings as the radio's YouTube media type)
std::filesystem::path radioIniValue(const char* key)
{
    std::ifstream in(std::filesystem::path("Data") / "SFSE" / "Plugins" / "RadioSFSE.ini");
    std::string line;
    const std::string want = key;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string k = line.substr(0, eq);
        k.erase(std::remove_if(k.begin(), k.end(), ::isspace), k.end());
        std::transform(k.begin(), k.end(), k.begin(), ::tolower);
        if (k == want) {
            std::string v = line.substr(eq + 1);
            v.erase(0, v.find_first_not_of(" \t\""));
            v.erase(v.find_last_not_of(" \t\r\"") + 1);
            return std::filesystem::path(widen(v));
        }
    }
    return {};
}

std::filesystem::path findTool(const std::filesystem::path& configured, const char* radioKey, const std::filesystem::path& root,
                               const wchar_t* exe)
{
    std::error_code ec;
    for (const auto& candidate : { expand(configured), expand(radioIniValue(radioKey)), root / L"Tools" / exe, onPath(exe) }) {
        if (!candidate.empty() && std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

bool isVideoFile(const std::filesystem::path& p)
{
    const std::wstring ext = p.extension().wstring();
    for (const wchar_t* e : { L".mp4", L".m4v", L".mov", L".wmv", L".avi", L".mkv" }) {
        if (_wcsicmp(ext.c_str(), e) == 0) {
            return true;
        }
    }
    return false;
}

std::string clockText(double seconds)
{
    const int s = static_cast<int>(std::max(0.0, seconds));
    char buf[32];
    if (s >= 3600) {
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    }
    return buf;
}

std::string shorten(const std::string& text, std::size_t max)
{
    if (text.size() <= max) {
        return text;
    }
    std::size_t cut = max - 3;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {   // keep UTF-8 whole
        --cut;
    }
    return text.substr(0, cut) + "...";
}

std::string fileKey(const std::string& name)
{
    std::string out;
    for (char c : name) {
        out.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    return out;
}
}

namespace
{
// TV-style notice over a channel's picture (osd.h): 3 s, 4 s with a second line.
void tvNotice(int channel, Osd::Icon icon, const std::string& label, const std::string& detail = {}, float level = -1.0f)
{
    Osd osd;
    osd.icon = icon;
    osd.label = osdText(label, 24);
    osd.detail = osdText(detail, 40);
    osd.level = level;
    osd.until = std::chrono::steady_clock::now() + std::chrono::seconds(detail.empty() ? 3 : 4);
    showOsd(channel, osd);
}
}

ScreenController::ScreenController(Logger& logger, const VideoConfig& config, const std::vector<ChannelInfo>& channels) :
    logger_(logger), config_(config)
{
    // Default Data\VideoScreen in the game folder, as the radio's root_path=Data\Radio (user: players did not find their
    // media under Documents). Not Data\Video: that is the game's own Bink video folder. A relative root_path is
    // relative to the game folder (the game's working directory).
    if (config_.rootDir.empty()) {
        config_.rootDir = std::filesystem::path(L"Data") / L"VideoScreen";
    }
    config_.rootDir = expand(config_.rootDir);
    if (config_.rootDir.is_relative()) {
        config_.rootDir = std::filesystem::absolute(config_.rootDir);
    }
    std::error_code ec;
    std::filesystem::create_directories(config_.rootDir / L"Videos", ec);
    std::filesystem::create_directories(config_.rootDir / L"Cache", ec);
    VideoFetcher::Settings fs;
    fs.ytdlp = findTool(config_.ytdlp, "ytdlp_path", config_.rootDir, L"yt-dlp.exe");
    fs.ffmpeg = findTool(config_.ffmpeg, "ffmpeg_path", config_.rootDir, L"ffmpeg.exe");
    fs.deno = findTool(config_.deno, "deno_path", config_.rootDir, L"deno.exe");
    if (fs.deno.empty()) {
        const auto home = expand(L"%USERPROFILE%") / L".deno" / L"bin" / L"deno.exe";
        if (std::filesystem::exists(home, ec)) {
            fs.deno = home;
        }
    }
    fs.cacheDir = config_.rootDir / L"Cache";
    fs.maxHeight = config_.maxHeight;
    fetcher_ = std::make_unique<VideoFetcher>(logger_);
    fetcher_->configure(fs);

    // playlists: playlist= lines; else the v2 Source= as "Video"; always the local Videos folder last
    auto entries = config_.playlists;
    if (entries.empty() && !config_.source.empty()) {
        entries.push_back({ "Video", config_.source });
    }
    bool haveVideosFolder = false;
    for (const auto& e : entries) {
        haveVideosFolder |= _wcsicmp(widen(e.source).c_str(), L"Videos") == 0;
    }
    if (!haveVideosFolder) {
        entries.push_back({ "My Videos", "Videos" });
    }
    for (const auto& e : entries) {
        Playlist pl;
        pl.name = e.name;
        pl.source = e.source;
        pl.youTube = e.source.find("youtube.com") != std::string::npos || e.source.find("youtu.be") != std::string::npos ||
                     (e.source.size() == 11 && VideoFetcher::youTubeId(e.source));
        playlists_.push_back(std::move(pl));
    }
    std::string names;
    for (const auto& pl : playlists_) {
        names += (names.empty() ? "" : ", ") + pl.name + (pl.youTube ? " (YouTube)" : " (folder)");
    }
    logger_.info("Video root " + narrow(config_.rootDir.wstring()) + "; yt-dlp " +
                 (fs.ytdlp.empty() ? std::string("NOT FOUND") : narrow(fs.ytdlp.wstring())) + "; ffmpeg " +
                 (fs.ffmpeg.empty() ? std::string("not found") : narrow(fs.ffmpeg.wstring())) + "; deno " +
                 (fs.deno.empty() ? std::string("not found") : narrow(fs.deno.wstring())) + ". Playlists: " + names);

    for (const auto& info : channels) {
        auto c = std::make_unique<Channel>();
        c->info = info;
        c->volume = config_.volume;
        // wall1 keeps the single-screen session file of v3
        c->sessionFile = config_.rootDir / widen(info.key == "wall1" ? std::string("ScreenSFSE.session.txt")
                                                                       : "ScreenSFSE.session." + info.key + ".txt");
        loadSession(*c);
        // stopped = switched off: black (user, 2026-10-14); the colour bars only while loading/buffering
        c->idleCard = std::make_shared<BlackSource>(info.width, info.height);
        setFrameSource(info.id, c->idleCard);
        if (c->sessionPlaying || !c->pendingTrackKey.empty()) {
            logger_.info(tag(*c) + "session: playlist " + playlists_[c->playlist].name + ", track '" + c->pendingTrackKey +
                         "' at " + clockText(c->resumeAt) + ", volume " + std::to_string(static_cast<int>(c->volume * 100 + 0.5f)) +
                         "%" + (c->sessionPlaying ? ", was playing" : ""));
        }
        channels_.push_back(std::move(c));
    }
    worker_ = std::thread([this] { spatialLoop(); });
}

ScreenController::~ScreenController()
{
    {
        std::lock_guard lock(mutex_);
        for (auto& c : channels_) {
            if (c->state == State::Playing || c->state == State::Paused) {
                c->resumeAt = positionLocked(*c);
                saveSessionLocked(*c);
            }
            ++c->generation;
        }
        quit_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    fetcher_->shutdown();
    for (auto& c : channels_) {
        setFrameSource(c->info.id, nullptr);
        c->player.reset();
    }
}

std::string ScreenController::tag(const Channel& c) const
{
    return "[" + c.info.key + "] ";
}

ScreenController::Channel* ScreenController::channelById(int id)
{
    for (auto& c : channels_) {
        if (c->info.id == id) {
            return c.get();
        }
    }
    return nullptr;
}

ScreenController::Channel* ScreenController::channelFor(std::uint32_t formId)
{
    const auto it = screens_.find(formId);
    Channel* c = it != screens_.end() ? channelById(it->second.channel) : nullptr;
    return c ? c : (channels_.empty() ? nullptr : channels_.front().get());
}

// ---- session -----------------------------------------------------------------------------------------------------

void ScreenController::loadSession(Channel& c)
{
    std::ifstream in(c.sessionFile);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        try {
            if (k == "playlist") {
                for (std::size_t i = 0; i < playlists_.size(); ++i) {
                    if (playlists_[i].name == v) {
                        c.playlist = i;
                    }
                }
            } else if (k == "track") {
                c.pendingTrackKey = v;
            } else if (k == "position") {
                c.resumeAt = std::max(0.0, std::stod(v));
            } else if (k == "volume") {
                c.volume = std::clamp(std::stof(v), 0.0f, 1.0f);
            } else if (k == "playing") {
                c.sessionPlaying = v == "1";
            } else if (k == "screen") {
                c.sessionScreen = static_cast<std::uint32_t>(std::stoul(v, nullptr, 16));
            }
        } catch (...) {
        }
    }
}

void ScreenController::saveSessionLocked(Channel& c)
{
    const Track* t = currentTrackLocked(c);
    const std::string key = t ? t->key : c.pendingTrackKey;
    const bool playing = c.state == State::Playing || c.state == State::Fetching;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", c.resumeAt);
    std::string text = "playlist=" + (playlists_.empty() ? std::string() : playlists_[c.playlist].name) + "\n" + "track=" + key +
                       "\n" + "position=" + buf + "\n";
    std::snprintf(buf, sizeof(buf), "%.2f", c.volume);
    text += std::string("volume=") + buf + "\n" + "playing=" + (playing ? "1" : "0") + "\n";
    std::snprintf(buf, sizeof(buf), "%08X", c.active);
    text += std::string("screen=") + buf + "\n";
    const auto temp = c.sessionFile.wstring() + L".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << text;
    }
    MoveFileExW(temp.c_str(), c.sessionFile.c_str(), MOVEFILE_REPLACE_EXISTING);
    c.lastSessionSave = std::chrono::steady_clock::now();
}

// ---- playlists ---------------------------------------------------------------------------------------------------

void ScreenController::listLocal(Playlist& pl) const
{
    pl.tracks.clear();
    std::filesystem::path p = widen(pl.source);
    if (p.is_relative()) {
        p = config_.rootDir / p;
    }
    std::error_code ec;
    if (std::filesystem::is_regular_file(p, ec)) {
        pl.tracks.push_back({ narrow(p.wstring()), narrow(p.stem().wstring()) });
    } else {
        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_iterator it(p, ec), end; it != end && !ec; it.increment(ec)) {
            if (it->is_regular_file(ec) && isVideoFile(it->path())) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            pl.tracks.push_back({ narrow(f.wstring()), narrow(f.stem().wstring()) });
        }
    }
    pl.listed = true;
}

void ScreenController::loadListCache(Playlist& pl) const
{
    std::ifstream in(config_.rootDir / L"Cache" / widen("list_" + fileKey(pl.name) + ".tsv"));
    std::string line;
    std::vector<Track> tracks;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // <id> TAB <live_status> TAB <title>; older caches: <id> TAB <title>
        const auto tab = line.find('\t');
        if (tab != 11) {
            continue;
        }
        const auto tab2 = line.find('\t', 12);
        const std::string mid = tab2 == std::string::npos ? std::string() : line.substr(12, tab2 - 12);
        const bool hasStatus = !mid.empty() && mid.size() <= 12 && mid.find(' ') == std::string::npos &&
                               (mid.find('_') != std::string::npos || mid == "NA");
        if (hasStatus) {
            tracks.push_back({ line.substr(0, 11), line.substr(tab2 + 1), mid });
        } else {
            tracks.push_back({ line.substr(0, 11), line.substr(12), std::string() });
        }
    }
    if (!tracks.empty()) {
        pl.tracks = std::move(tracks);
        pl.listed = true;
    }
}

void ScreenController::saveListCache(const Playlist& pl) const
{
    std::ofstream out(config_.rootDir / L"Cache" / widen("list_" + fileKey(pl.name) + ".tsv"), std::ios::trunc);
    for (const auto& t : pl.tracks) {
        out << t.key << '\t' << (t.status.empty() ? std::string("NA") : t.status) << '\t' << t.title << '\n';
    }
}

// Background refresh of a listed YouTube playlist, once per game session (new uploads). Channels keep pointing at
// the video they were on.
void ScreenController::refreshListLocked(Playlist& pl)
{
    if (pl.refreshed || pl.listing) {
        return;
    }
    pl.refreshed = true;
    pl.listing = true;
    const std::string name = pl.name;
    fetcher_->requestList(pl.source, [this, name](bool ok, std::vector<VideoFetcher::Track> tracks, const std::string&) {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < playlists_.size(); ++i) {
            Playlist& p = playlists_[i];
            if (p.name != name) {
                continue;
            }
            p.listing = false;
            if (!ok) {
                continue;
            }
            std::vector<std::pair<Channel*, std::string>> keep;
            for (auto& c : channels_) {
                if (c->playlist == i && currentTrackLocked(*c)) {
                    keep.emplace_back(c.get(), currentTrackLocked(*c)->key);
                }
            }
            p.tracks.clear();
            for (auto& t : tracks) {
                p.tracks.push_back({ t.id, t.title, t.status });
            }
            saveListCache(p);
            for (auto& [c, key] : keep) {
                for (std::size_t k = 0; k < p.tracks.size(); ++k) {
                    if (p.tracks[k].key == key) {
                        c->track = k;
                    }
                }
            }
        }
    });
}

// True when the playlist's tracks are known. Otherwise starts listing it (YouTube) and plays channel c from `at` when done.
bool ScreenController::ensureListedLocked(Channel& c, Playlist& pl, double at)
{
    if (!pl.youTube) {
        if (!pl.listed) {
            listLocal(pl);
        }
        return true;
    }
    if (!pl.listed) {
        loadListCache(pl);   // last session's listing: start at once, refreshed in the background
    }
    if (pl.listed) {
        refreshListLocked(pl);
        return true;
    }
    if (!pl.listing) {
        pl.listing = true;
        pl.refreshed = true;
        const std::string name = pl.name;
        fetcher_->requestList(pl.source, [this, name](bool ok, std::vector<VideoFetcher::Track> tracks, const std::string& error) {
            std::lock_guard lock(mutex_);
            for (auto& p : playlists_) {
                if (p.name != name) {
                    continue;
                }
                p.listing = false;
                if (ok) {
                    p.tracks.clear();
                    for (auto& t : tracks) {
                        p.tracks.push_back({ t.id, t.title, t.status });
                    }
                    p.listed = true;
                    saveListCache(p);
                }
            }
            // every channel waiting on this playlist goes on (or reports the failure)
            for (auto& ch : channels_) {
                if (ch->state != State::Fetching || playlists_[ch->playlist].name != name || ch->player) {
                    continue;
                }
                if (!ok) {
                    ch->state = State::Idle;
                    showIdleLocked(*ch);
                    ch->pendingMessage = "Video Screen: cannot list the playlist - " + shorten(error, 100);
                    continue;
                }
                playCurrentLocked(*ch, ch->resumeAt);
            }
        });
    }
    c.resumeAt = at;
    c.state = State::Fetching;
    showLoadingLocked(c);
    return false;
}

const ScreenController::Track* ScreenController::currentTrackLocked(const Channel& c) const
{
    if (playlists_.empty()) {
        return nullptr;
    }
    const Playlist& pl = playlists_[c.playlist];
    return c.track < pl.tracks.size() ? &pl.tracks[c.track] : nullptr;
}

std::string ScreenController::trackLabelLocked(const Channel& c) const
{
    const Track* t = currentTrackLocked(c);
    return t ? shorten(t->title, 60) : std::string("(no video)");
}

// ---- playback ----------------------------------------------------------------------------------------------------

void ScreenController::showLoadingLocked(Channel& c)
{
    if (!c.loadingCard) {
        c.loadingCard = std::make_shared<TestPatternSource>(c.info.width, c.info.height, 30.0, true);
    }
    setFrameSource(c.info.id, c.loadingCard);
}

void ScreenController::showIdleLocked(Channel& c)
{
    setFrameSource(c.info.id, c.idleCard);
    c.loadingCard.reset();   // its thread ends with it
}

std::string ScreenController::playCurrentLocked(Channel& c, double at)
{
    if (playlists_.empty()) {
        return "Video Screen: no playlists";
    }
    Playlist& pl = playlists_[c.playlist];
    const std::uint64_t generation = ++c.generation;
    if (!ensureListedLocked(c, pl, at)) {
        return "Video Screen: loading playlist " + pl.name + "...";
    }
    if (!c.pendingTrackKey.empty()) {   // the session's video, once its playlist is known
        for (std::size_t i = 0; i < pl.tracks.size(); ++i) {
            if (pl.tracks[i].key == c.pendingTrackKey) {
                c.track = i;
            }
        }
        c.pendingTrackKey.clear();
    }
    if (pl.tracks.empty()) {
        c.state = State::Idle;
        showIdleLocked(c);
        return "Video Screen: " + pl.name + " is empty" + (pl.youTube ? "" : " (add videos to Data\\Video\\Videos)");
    }
    if (c.track >= pl.tracks.size()) {
        c.track = 0;
    }
    const Track track = pl.tracks[c.track];
    if (!pl.youTube) {
        startFileLocked(c, widen(track.key), at);
        return c.state == State::Playing ? "Playing: " + shorten(track.title, 60) : "Video Screen: cannot play " + shorten(track.title, 60);
    }
    if (!fetcher_->available() && !fetcher_->cachedFile(track.key)) {
        c.state = State::Idle;
        showIdleLocked(c);
        return "Video Screen: yt-dlp.exe not found (see ScreenSFSE.ini)";
    }
    if (track.status == "is_live" || track.status == "is_upcoming") {
        streamLocked(c, track);
        return "Live: " + shorten(track.title, 60);
    }
    c.state = State::Fetching;
    const bool cached = fetcher_->cachedFile(track.key).has_value();
    if (!cached) {
        showLoadingLocked(c);
    }
    const int id = c.info.id;
    fetcher_->request(track.key, [this, id, generation, at, title = track.title](bool ok, const std::filesystem::path& file,
                                                                                 const std::string& error) {
        std::lock_guard lock(mutex_);
        Channel* ch = channelById(id);
        if (!ch || generation != ch->generation || ch->state != State::Fetching) {
            return;   // cancelled or replaced meanwhile
        }
        if (!ok && error == VideoFetcher::kLiveError) {   // listed before it went live: stream it
            if (const Track* t = currentTrackLocked(*ch)) {
                setTrackStatusLocked(t->key, "is_live");
                streamLocked(*ch, *currentTrackLocked(*ch));
            }
            return;
        }
        if (!ok) {
            ch->state = State::Idle;
            showIdleLocked(*ch);
            ch->pendingMessage = "Video Screen: download failed (" + shorten(title, 40) + ") - " + shorten(error, 80);
            return;
        }
        startFileLocked(*ch, file, at);
    });
    return (cached ? "Playing: " : "Downloading: ") + shorten(track.title, 60);
}

void ScreenController::setTrackStatusLocked(const std::string& key, const std::string& status)
{
    for (auto& pl : playlists_) {
        bool changed = false;
        for (auto& t : pl.tracks) {
            if (t.key == key && t.status != status) {
                t.status = status;
                changed = true;
            }
        }
        if (changed && pl.youTube) {
            saveListCache(pl);
        }
    }
}

// A live (or about to start) YouTube video: its HLS URLs from yt-dlp, then LivePlayer. Never downloaded.
void ScreenController::streamLocked(Channel& c, const Track& track)
{
    c.state = State::Fetching;
    showLoadingLocked(c);
    const int id = c.info.id;
    const std::uint64_t generation = c.generation;
    const std::string title = track.title;
    fetcher_->requestStream(track.key, [this, id, generation, title](bool ok, const std::string& video, const std::string& audio,
                                                                     const std::string& error) {
        std::lock_guard lock(mutex_);
        Channel* ch = channelById(id);
        if (!ch || generation != ch->generation || ch->state != State::Fetching) {
            return;
        }
        if (!ok) {
            ch->state = State::Idle;
            showIdleLocked(*ch);
            ch->pendingMessage = "Live stream unavailable (" + shorten(title, 40) + ") - " + shorten(error, 80);
            return;
        }
        startLiveLocked(*ch, video, audio);
    });
}

void ScreenController::startLiveLocked(Channel& c, const std::string& videoUrl, const std::string& audioUrl)
{
    auto player = std::make_shared<LivePlayer>(logger_, c.info.width, c.info.height, fetcher_->settings().ffmpeg,
                                               config_.liveBufferSeconds);
    if (!player->open(videoUrl, audioUrl)) {
        c.state = State::Idle;
        showIdleLocked(c);
        c.pendingMessage = "Video Screen: " + player->lastError();
        return;
    }
    c.player = player;
    c.currentFile.clear();
    c.state = State::Playing;
    c.resumeAt = 0;
    c.started = std::chrono::steady_clock::now();
    updateGainsLocked(c);
    setFrameSource(c.info.id, c.player);
    c.loadingCard.reset();
    saveSessionLocked(c);
    cv_.notify_all();
}

void ScreenController::startFileLocked(Channel& c, const std::filesystem::path& file, double at)
{
    auto player = std::make_shared<MediaPlayer>(logger_, c.info.width, c.info.height);
    if (!player->open(file, at)) {
        c.state = State::Idle;
        showIdleLocked(c);
        c.pendingMessage = "Video Screen: " + player->lastError();
        return;
    }
    c.started = std::chrono::steady_clock::now();
    c.player = player;
    c.currentFile = file;
    c.state = State::Playing;
    c.resumeAt = at;
    updateGainsLocked(c);
    setFrameSource(c.info.id, c.player);
    c.loadingCard.reset();
    saveSessionLocked(c);
    prefetchNextLocked(c);
    cv_.notify_all();
}

void ScreenController::prefetchNextLocked(const Channel& c)
{
    const Playlist& pl = playlists_[c.playlist];
    if (pl.youTube && pl.tracks.size() > 1) {
        const Track& next = pl.tracks[(c.track + 1) % pl.tracks.size()];
        if (next.status != "is_live" && next.status != "is_upcoming") {
            fetcher_->prefetch(next.key);
        }
    }
}

void ScreenController::pauseLocked(Channel& c)
{
    if (c.state == State::Playing && c.player) {
        c.player->pause();
        c.state = State::Paused;
        c.resumeAt = positionLocked(c);
        saveSessionLocked(c);
    }
}

void ScreenController::resumeLocked(Channel& c)
{
    if (c.state == State::Paused && c.player) {
        c.player->play();
        c.state = State::Playing;
        updateGainsLocked(c);
        saveSessionLocked(c);
        cv_.notify_all();
    }
}

void ScreenController::stopLocked(Channel& c, const char* why)
{
    ++c.generation;
    if (c.state != State::Idle) {
        logger_.info(tag(c) + "Video stopped: " + why);
    }
    c.state = State::Idle;
    c.resumeOnLoad = false;
    showIdleLocked(c);
    c.player.reset();   // the surface released its reference in setFrameSource; this joins the decode threads
}

double ScreenController::positionLocked(const Channel& c) const
{
    return c.player ? c.player->positionSeconds() : c.resumeAt;
}

void ScreenController::onTrackFinishedLocked(Channel& c)
{
    if (c.player && c.player->live() && std::chrono::steady_clock::now() - c.started < std::chrono::seconds(15)) {
        // a live stream that fails at once would restart in a loop: stop and say why
        const std::string why = c.player->lastError();
        logger_.info(tag(c) + "Live stream stopped early: " + why);
        stopLocked(c, "live stream failed");
        c.pendingMessage = "Live stream stopped" + (why.empty() ? std::string() : " - " + shorten(why, 100));
        saveSessionLocked(c);
        return;
    }
    logger_.info(tag(c) + "Video finished: " + trackLabelLocked(c));
    stopLocked(c, "finished");
    Playlist& pl = playlists_[c.playlist];
    if (pl.tracks.empty()) {
        return;
    }
    c.track = (c.track + 1) % pl.tracks.size();   // the playlist loops, like the radio's
    c.resumeAt = 0;
    playCurrentLocked(c, 0);
    tvNotice(c.info.id, Osd::Icon::Play, "PLAY", trackLabelLocked(c));
}

// ---- Papyrus entry points -----------------------------------------------------------------------------------------

void ScreenController::registerScreen(std::uint32_t formId, int channel, float x, float y, float z, float px, float py, float pz)
{
    std::lock_guard lock(mutex_);
    Channel* c = channelById(channel);
    if (!c) {
        logger_.warn("Screen " + std::to_string(formId) + " registered with unknown channel " + std::to_string(channel) + "; using the first.");
        c = channels_.front().get();
    }
    Screen& s = screens_[formId];
    s.channel = c->info.id;
    s.x = x;
    s.y = y;
    s.z = z;
    s.loaded = true;
    lastPlayer_[0] = px;
    lastPlayer_[1] = py;
    lastPlayer_[2] = pz;
    float rx = 0, ry = 0, rz = 0, heading = 0;
    const bool read = readPlayer(rx, ry, rz, heading);
    const float diff = read ? std::sqrt((rx - px) * (rx - px) + (ry - py) * (ry - py) + (rz - pz) * (rz - pz)) : -1.0f;
    const bool ok = read && diff < 100.0f;   // the player can walk a little between the two reads
    if (!playerPositionChecked_ || ok != playerPositionVerified_) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Player position check: Papyrus (%.0f, %.0f, %.0f), direct read %s (%.0f, %.0f, %.0f) -> %s",
                      px, py, pz, read ? "ok" : "FAILED", rx, ry, rz,
                      ok ? "verified, spatial sound follows the player" : "NOT verified, sound is not positional");
        logger_.info(buf);
    }
    playerPositionChecked_ = true;
    playerPositionVerified_ = ok;
    if (formId == c->active && c->resumeOnLoad && c->state == State::Paused && c->player) {
        c->resumeOnLoad = false;
        resumeLocked(*c);
        logger_.info(tag(*c) + "Video resumed: its screen is loaded again.");
    } else if (c->sessionPlaying && c->state == State::Idle && (formId == c->sessionScreen || c->sessionScreen == 0)) {
        c->sessionPlaying = false;   // once per game session
        c->active = formId;
        logger_.info(tag(*c) + "Resuming the saved session at " + clockText(c->resumeAt) + ".");
        playCurrentLocked(*c, c->resumeAt);
        tvNotice(c->info.id, Osd::Icon::Play, "PLAY", trackLabelLocked(*c));
    }
}

std::string ScreenController::status(std::uint32_t formId)
{
    std::lock_guard lock(mutex_);
    Channel* c = channelFor(formId);
    if (!c || playlists_.empty()) {
        return "VIDEO SCREEN";
    }
    const Playlist& pl = playlists_[c->playlist];
    const char* state = c->state == State::Playing ? "PLAYING" : c->state == State::Paused ? "PAUSED"
                                                             : c->state == State::Fetching ? "LOADING" : "STOPPED";
    std::string time = clockText(positionLocked(*c));
    if (c->player && c->player->live()) {
        time = "LIVE " + time;
    } else if (c->player && c->player->durationSeconds() > 0) {
        time += " / " + clockText(c->player->durationSeconds());
    }
    std::string where = pl.name;
    if (pl.listed && !pl.tracks.empty()) {
        where += " " + std::to_string(c->track + 1) + "/" + std::to_string(pl.tracks.size());
    }
    return "CH " + std::to_string(c->info.number) + "  " + state + "  " + trackLabelLocked(*c) + "  " + time + "  |  Vol " +
           std::to_string(static_cast<int>(c->volume * 100 + 0.5f)) + "%  |  " + shorten(where, 40);
}

std::string ScreenController::action(std::uint32_t formId, int act)
{
    std::lock_guard lock(mutex_);
    Channel* cp = channelFor(formId);
    if (!cp) {
        return {};
    }
    Channel& c = *cp;
    if (!c.pendingMessage.empty() && c.state == State::Idle && act == PlayPause) {   // e.g. a failed download
        std::string message = std::move(c.pendingMessage);
        c.pendingMessage.clear();
        return message;
    }
    c.pendingMessage.clear();
    if (formId != c.active) {   // the sound moves to the screen just used (same channel, same picture)
        c.active = formId;
        updateGainsLocked(c);
    }
    c.sessionPlaying = false;
    const bool wasActive = c.state != State::Idle;
    Playlist* pl = playlists_.empty() ? nullptr : &playlists_[c.playlist];
    switch (act) {
    case PlayPause:
        switch (c.state) {
        case State::Idle: {
            const std::string m = playCurrentLocked(c, c.resumeAt);
            if (c.state != State::Idle) {
                tvNotice(c.info.id, Osd::Icon::Play, m.rfind("Live", 0) == 0 ? "LIVE" : "PLAY", trackLabelLocked(c));
            }
            return m;
        }
        case State::Fetching:
            stopLocked(c, "cancelled while loading");
            saveSessionLocked(c);
            tvNotice(c.info.id, Osd::Icon::Stop, "STOP");
            return "Video Screen: cancelled";
        case State::Playing:
            pauseLocked(c);
            tvNotice(c.info.id, Osd::Icon::Pause, "PAUSE");
            return "Paused at " + clockText(c.resumeAt);
        case State::Paused:
            resumeLocked(c);
            tvNotice(c.info.id, Osd::Icon::Play, c.player && c.player->live() ? "LIVE" : "PLAY");
            return "Playing: " + trackLabelLocked(c);
        }
        break;
    case NextTrack:
    case PreviousTrack:
        if (!pl) {
            return {};
        }
        if (!pl->listed) {
            ensureListedLocked(c, *pl, 0);
            return "Video Screen: loading playlist " + pl->name + "...";
        }
        if (pl->tracks.empty()) {
            return "Video Screen: " + pl->name + " is empty";
        }
        if (pl->tracks.size() == 1 && wasActive && !(act == PreviousTrack && positionLocked(c) > 5.0)) {
            return pl->name + " has one video. Next Playlist switches lists.";
        }
        if (act == PreviousTrack && positionLocked(c) > 5.0 && wasActive) {
            // like a CD player: back to the start of this video first
        } else if (act == NextTrack) {
            c.track = (c.track + 1) % pl->tracks.size();
        } else {
            c.track = (c.track + pl->tracks.size() - 1) % pl->tracks.size();
        }
        stopLocked(c, act == NextTrack ? "next track" : "previous track");
        c.resumeAt = 0;
        {
            const std::string m = playCurrentLocked(c, 0);
            tvNotice(c.info.id, act == NextTrack ? Osd::Icon::Next : Osd::Icon::Previous,
                     act == NextTrack ? "NEXT" : "PREV", trackLabelLocked(c));
            return m;
        }
    case SeekBack:
    case SeekForward: {
        if (c.player && c.player->live()) {
            return "Live stream: no seeking";
        }
        const double delta = act == SeekForward ? config_.seekSeconds : -config_.seekSeconds;
        double target = std::max(0.0, positionLocked(c) + delta);
        if (c.player && c.player->durationSeconds() > 0) {
            target = std::min(target, std::max(0.0, c.player->durationSeconds() - 2.0));
        }
        if ((c.state == State::Playing || c.state == State::Paused) && !c.currentFile.empty()) {
            const bool paused = c.state == State::Paused;
            const auto file = c.currentFile;
            stopLocked(c, "seek");
            startFileLocked(c, file, target);
            if (paused) {
                pauseLocked(c);
            }
        } else {
            c.resumeAt = target;
            saveSessionLocked(c);
        }
        tvNotice(c.info.id, act == SeekForward ? Osd::Icon::Forward : Osd::Icon::Rewind, clockText(target));
        return (act == SeekForward ? "Forward to " : "Back to ") + clockText(target);
    }
    case VolumeUp:
    case VolumeDown:
        c.volume = std::clamp(c.volume + (act == VolumeUp ? config_.volumeStep : -config_.volumeStep), 0.0f, 1.0f);
        c.volume = std::round(c.volume * 100.0f) / 100.0f;
        updateGainsLocked(c);
        saveSessionLocked(c);
        tvNotice(c.info.id, Osd::Icon::Volume, "VOL " + std::to_string(static_cast<int>(c.volume * 100 + 0.5f)), {}, c.volume);
        return "Volume " + std::to_string(static_cast<int>(c.volume * 100 + 0.5f)) + "%";
    case NextPlaylist: {
        if (playlists_.empty()) {
            return {};
        }
        if (wasActive) {
            stopLocked(c, "playlist changed");
        }
        c.playlist = (c.playlist + 1) % playlists_.size();
        c.track = 0;
        c.resumeAt = 0;
        c.pendingTrackKey.clear();
        if (wasActive) {
            const std::string m = playCurrentLocked(c, 0);
            tvNotice(c.info.id, Osd::Icon::Play, playlists_[c.playlist].name, trackLabelLocked(c));
            return "Playlist: " + playlists_[c.playlist].name + " - " + m;
        }
        saveSessionLocked(c);
        tvNotice(c.info.id, Osd::Icon::None, playlists_[c.playlist].name);
        return "Playlist: " + playlists_[c.playlist].name;
    }
    case Stop:
        stopLocked(c, "stopped by the player");
        c.resumeAt = 0;
        saveSessionLocked(c);
        tvNotice(c.info.id, Osd::Icon::Stop, "STOP");
        return "Video Screen: stopped";
    default:
        break;
    }
    return {};
}

void ScreenController::unloaded(std::uint32_t formId)
{
    std::lock_guard lock(mutex_);
    auto it = screens_.find(formId);
    if (it == screens_.end()) {
        return;
    }
    it->second.loaded = false;
    Channel* c = channelById(it->second.channel);
    if (c && formId == c->active && c->state == State::Playing && c->player) {
        pauseLocked(*c);
        c->resumeOnLoad = true;
        logger_.info(tag(*c) + "Video paused: its screen unloaded.");
    }
}

void ScreenController::removed(std::uint32_t formId)
{
    std::lock_guard lock(mutex_);
    auto it = screens_.find(formId);
    if (it == screens_.end()) {
        return;
    }
    Channel* c = channelById(it->second.channel);
    if (c && formId == c->active) {
        c->resumeAt = positionLocked(*c);
        stopLocked(*c, "its screen was removed");
        saveSessionLocked(*c);
    }
    screens_.erase(it);
}

// ---- spatial sound + housekeeping (only while something plays) ---------------------------------------------------

// Caller holds mutex_.
void ScreenController::updateGainsLocked(Channel& c)
{
    if (!c.player) {
        return;
    }
    const auto it = screens_.find(c.active);
    if (it == screens_.end()) {
        c.player->setGains(c.volume, c.volume);
        return;
    }
    float px = lastPlayer_[0], py = lastPlayer_[1], pz = lastPlayer_[2], heading = 0;
    bool facing = false;
    if (playerPositionVerified_) {
        facing = readPlayer(px, py, pz, heading);
    }
    const Screen& s = it->second;
    const float dx = s.x - px, dy = s.y - py, dz = s.z - pz;
    const float meters = std::sqrt(dx * dx + dy * dy + dz * dz) * kMetersPerUnit;
    float gain = 1.0f;
    if (meters >= config_.silentMeters) {
        gain = 0.0f;
    } else if (meters > config_.fullVolumeMeters) {
        const float t = (config_.silentMeters - meters) / (config_.silentMeters - config_.fullVolumeMeters);
        gain = t * t;
    }
    gain *= c.volume;
    float left = gain, right = gain;
    if (facing) {
        // heading: radians clockwise from +Y; pan +1 = screen on the right. Centred when standing at the screen.
        const float bearing = std::atan2(dx, dy);
        const float pan = std::sin(bearing - heading) * std::min(1.0f, meters / std::max(0.1f, config_.fullVolumeMeters));
        const float theta = (pan + 1.0f) * kPi / 4.0f;
        left = gain * std::min(1.0f, std::cos(theta) * 1.41421356f);
        right = gain * std::min(1.0f, std::sin(theta) * 1.41421356f);
    }
    c.player->setGains(left, right);
}

void ScreenController::spatialLoop()
{
    std::unique_lock lock(mutex_);
    auto anyPlaying = [this] {
        return std::any_of(channels_.begin(), channels_.end(), [](const auto& c) { return c->state == State::Playing; });
    };
    while (!quit_) {
        cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return quit_ || anyPlaying(); });
        if (quit_) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        for (auto& cp : channels_) {
            Channel& c = *cp;
            if (c.state != State::Playing || !c.player) {
                continue;
            }
            if (c.player->finished()) {
                onTrackFinishedLocked(c);
                continue;
            }
            updateGainsLocked(c);
            if (now - c.lastSessionSave >= std::chrono::seconds(5)) {
                c.resumeAt = positionLocked(c);
                saveSessionLocked(c);
            }
        }
        cv_.wait_for(lock, std::chrono::milliseconds(100), [this] { return quit_; });
    }
}
