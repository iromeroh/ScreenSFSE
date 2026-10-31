#pragma once
#include "screen_location.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class FrameSource;
class Logger;
class MediaPlayer;
class Playback;
class SubtitledSource;
class TestPatternSource;
class VideoFetcher;
namespace Subtitles
{
struct Cue;
struct Track;
}
using SubtitleCue = Subtitles::Cue;
using SubtitleTrack = Subtitles::Track;

// ScreenSFSE.ini [Video]
struct VideoConfig
{
    struct PlaylistEntry
    {
        std::string name;
        std::string source;   // a web address (YouTube or any site yt-dlp reads), or a folder / video file (absolute or
                              // relative to rootDir)
        std::string subtitles;   // playlist=Name|Source|subs=...: "" = subtitle_languages, "off", or languages "en,es"
        std::string items;       // |items=1,3-5: which of the page's videos (yt-dlp --playlist-items); "" = all
        bool streamOnly{ false };   // |stream: never downloaded, always streamed (sites whose links expire)
    };
    std::vector<PlaylistEntry> playlists;   // playlist=Name|Source lines, in order
    std::string source;                     // v2 single Source= (becomes a playlist "Video" when no playlist= lines)
    std::filesystem::path rootDir;          // default <game folder>\Data\VideoScreen (relative = to the game folder)
    std::filesystem::path playlistIni;      // only playlist=/Source= are reread on Refresh; empty in offline tests
    std::filesystem::path ytdlp;            // empty = RadioSFSE.ini's, <rootDir>\Tools, PATH
    std::filesystem::path deno;
    std::filesystem::path ffmpeg;
    int maxHeight{ 720 };
    float volume{ 0.8f };                   // starting volume (each channel's session keeps the player's choice)
    float volumeStep{ 0.1f };
    float seekSeconds{ 30.0f };
    float liveBufferSeconds{ 5.0f };         // live streams: stream kept ahead against network stalls
    float fullVolumeMeters{ 2.0f };         // full volume within this distance of the screen
    float silentMeters{ 25.0f };            // silent from this distance on
    // Subtitle languages in order of preference ("es", "en"); empty = none fetched for YouTube, and of local files only
    // the untagged Show.srt shows.
    std::vector<std::string> subtitleLanguages;
    float subtitleSize{ 1.0f };             // letter height: 1 = 6% of the screen height
    bool muteGameMusic{ true };             // the game's music is silenced while a screen plays within silentMeters
};

// playlist= value "Source|subs=en,es" -> source "Source", subtitles "en,es" (unchanged without the option).
void splitPlaylistOptions(std::string& source, std::string& subtitles);
// The same with every trailing option, in any order: |subs=en,es |items=1,3-5 |stream. An unknown one stays in the
// source (a URL may hold '|').
void splitPlaylistOptions(std::string& source, VideoConfig::PlaylistEntry& options);
// "es, en-US ,EN" -> {"es", "en-US"}: trimmed, duplicates (ignoring case) dropped.
std::vector<std::string> splitLanguages(const std::string& list);

// The buildable video screens, controlled like the radio. Screens belong to channels (the screen variant built:
// each has its own placeholder texture). Each channel plays on its own: playlist, video, position, volume and a
// session file (<root>\ScreenSFSE.session[.<key>].txt). Each save also gets a snapshot of every channel
// (<root>\Saves\<token>.txt, save_sync.cpp), so loading a save resumes what was playing in it. Screens of one channel show
// the same picture; the sound comes from the one last used. Playlists come from the INI (YouTube playlists/videos via
// yt-dlp, local folders) and are shared by all channels.
// The worker checks playback areas and suspended channels as well as spatial sound, end of track (-> next track),
// and each playing channel's position saved every 5 s. It keeps the landed-ship view of the surface world eligible.
class ScreenController
{
public:
    enum Action
    {
        PlayPause = 0,
        NextTrack = 1,
        PreviousTrack = 2,
        SeekBack = 3,
        SeekForward = 4,
        VolumeUp = 5,
        VolumeDown = 6,
        NextPlaylist = 7,
        Stop = 8,
        RefreshPlaylists = 9,
        CycleSubtitles = 10   // off -> each subtitle track of the video -> off
    };

    struct ChannelInfo
    {
        int id;
        std::string key;   // session file name part ("wall1": the v2 file name)
        std::uint32_t width;
        std::uint32_t height;
        int number;        // channel number shown to the player (1-4)
        float soundRange{ 1.0f };   // its model's multiplier of full_volume_meters / silent_meters
    };

    ScreenController(Logger& logger, const VideoConfig& config, const std::vector<ChannelInfo>& channels);
    ~ScreenController();

    void registerScreen(std::uint32_t formId, int channel, float x, float y, float z, float playerX, float playerY, float playerZ,
                        ScreenLocation::Area area = {});
    // Explicit context input for offline lifecycle tests. Runtime reads the player's area on its existing worker.
    void setPlayerArea(ScreenLocation::Context context);
    // A menu action on a screen; returns a short message for the player (empty = none).
    std::string action(std::uint32_t formId, int action);
    // One line for the menu title: channel, state, track, time, volume, playlist.
    std::string status(std::uint32_t formId);
    void unloaded(std::uint32_t formId);
    void removed(std::uint32_t formId);

    // The Video Screen Remote (SCREEN_RemoteMenuScript): the loaded screens in the player's area, nearest first (at
    // most kRemoteMax), each with a label "<model> - CH n - <distance> - <state>". remoteScreens takes the snapshot
    // the index functions read. Actions and status only work on screens in the player's area ("" / a message
    // otherwise); remote actions keep the picture's notices up twice as long (the terminal hides the game meanwhile).
    static constexpr int kRemoteMax = 12;
    int remoteScreens(float playerX, float playerY, float playerZ);
    std::uint32_t remoteScreenId(int index);
    std::string remoteScreenLabel(int index);
    std::string remoteLabel(std::uint32_t formId, float playerX, float playerY, float playerZ);
    // One dashboard field of a screen in the player's area ("" otherwise): 1 state, 2 video, 3 time, 4 volume,
    // 5 playlist, 6 subtitles.
    std::string remoteField(std::uint32_t formId, int field);
    std::string remoteAction(std::uint32_t formId, int action);

    // Game music (SCREEN_MainQuestScript.SyncMusic): true while a channel plays or loads (paused: no) and the screen
    // its sound comes from is loaded and within silent_meters of the player. Always false with mute_game_music off.
    bool musicMuteWanted(float playerX, float playerY, float playerZ);
    // silent_meters when this screen is the sound source of a playing or loading channel (and mute_game_music is on),
    // else 0: the screen script watches the player crossing that distance (Papyrus distance events, no polling).
    float soundSourceRange(std::uint32_t formId);

    // Per-save state, driven by the game's save/load events (save_sync.cpp).
    // Writes every channel's state (what plays, where, volume, its screen) to <root>\Saves\<token>.txt.
    void saveSnapshot(std::uint32_t token);
    // A save starts loading: every channel stops and the screens of the previous game are forgotten.
    void beginLoad();
    // The save has loaded: its snapshot is applied (token 0 or no snapshot file: the session files, as before 0.3).
    // A channel that was playing resumes when its screen loads, or at once if it already has. The game does not run
    // the screen's OnLoad when a save loads in the screen's own cell, so the saved screen that has not registered is
    // asked about through probe: true when it is in the world with its 3D loaded. x/y/z come in as the position saved in
    // the snapshot (0 if none) and the probe may correct them.
    using ScreenProbe = std::function<bool(std::uint32_t formId, int channel, float& x, float& y, float& z, ScreenLocation::Area& area)>;
    void finishLoad(std::uint32_t token, const ScreenProbe& probe = {});

private:
    enum class State
    {
        Idle,
        Fetching,
        Playing,
        Paused
    };

    struct Screen
    {
        int channel{ 0 };
        float x{ 0 }, y{ 0 }, z{ 0 };
        bool loaded{ false };
        ScreenLocation::Area area;
    };

    struct Track
    {
        std::string key;     // YouTube video ID, "<extractor>_<id>" (VideoFetcher::trackKey), or file path
        std::string title;
        std::string status;  // yt-dlp live_status (is_live / is_upcoming: streamed live, never downloaded);
                             // "stream": no H.264 download offered, streamed on demand
        std::string url;     // other sites: where yt-dlp gets the video (empty for YouTube and files)
    };

    struct Playlist
    {
        std::string name;
        std::string source;
        std::string subtitles;     // the entry's subs= option ("" = subtitle_languages)
        std::string items;         // the entry's items= option
        bool streamOnly{ false };  // the entry's stream option
        bool online{ false };      // listed and fetched with yt-dlp (else a local folder or file)
        bool listed{ false };
        bool listing{ false };
        bool refreshed{ false };   // background listing refresh done this game session
        std::vector<Track> tracks;
    };

    struct Channel
    {
        ChannelInfo info;
        State state{ State::Idle };
        std::uint32_t active{ 0 };       // the screen whose position the sound comes from
        bool resumeOnLoad{ false };
        std::uint64_t generation{ 0 };   // bumps on every start/stop: stale download/list results are ignored
        std::string pendingMessage;
        std::shared_ptr<Playback> player;   // MediaPlayer (file) or LivePlayer (live stream)
        std::chrono::steady_clock::time_point started{};
        std::filesystem::path currentFile;
        std::string streamVideo, streamAudio;   // an on-demand stream's URLs (seeks reopen them), else empty
        std::string streamHeaders;               // and the HTTP headers they need
        double streamDuration{ 0.0 };
        std::shared_ptr<FrameSource> idleCard;
        std::shared_ptr<TestPatternSource> loadingCard;
        std::size_t playlist{ 0 };
        std::size_t track{ 0 };
        std::string pendingTrackKey;     // from the session file, resolved once the playlist is listed
        std::string deadLiveKey;         // a live video that failed: the playlist is relisted, its new broadcast plays
        float volume{ 0.8f };
        double resumeAt{ 0.0 };          // where play starts when idle (the saved position)
        bool sessionPlaying{ false };    // the session/save says it was playing: resume when one of its screens loads
        std::uint32_t sessionScreen{ 0 };
        float sessionWhere[3]{};         // that screen's position when saved (0 = not known)
        ScreenLocation::Area sessionArea;
        std::string subtitleChoice;      // "" = by subtitle_languages, "off", or a track's language ("-" = untagged)
        std::shared_ptr<SubtitledSource> captions;   // the player with its subtitles drawn over (file playback only)
        std::string subtitleShown;       // the shown track's language, "" with subtitleOn = untagged
        bool subtitleOn{ false };
        std::filesystem::path subtitleFile;   // parsed cues of it, kept across seeks
        std::shared_ptr<const std::vector<SubtitleCue>> subtitleCues;
        std::filesystem::path sessionFile;
        std::chrono::steady_clock::time_point lastSessionSave{};
    };

    Channel* channelFor(std::uint32_t formId);
    Channel* channelById(int id);
    std::string playCurrentLocked(Channel& c, double at);
    void startFileLocked(Channel& c, const std::filesystem::path& file, double at);
    // A live stream (status is_live/is_upcoming), or a video streamed on demand from `at`.
    void streamLocked(Channel& c, const Track& track, double at = 0.0);
    void liveFailedLocked(Channel& c, const std::string& key, const std::string& title);
    // startAt >= 0: on demand (LivePlayer::open).
    void startLiveLocked(Channel& c, const std::string& videoUrl, const std::string& audioUrl, double startAt = -1.0,
                         double duration = 0.0, const std::string& headers = {});
    bool streamedLocked(const Channel& c, const Track& track) const;
    void setTrackStatusLocked(const std::string& key, const std::string& status);
    void stopLocked(Channel& c, const char* why);
    void pauseLocked(Channel& c);
    void resumeLocked(Channel& c);
    void showLoadingLocked(Channel& c);
    void showIdleLocked(Channel& c);
    void showErrorLocked(Channel& c, const std::string& label, const std::string& detail);
    bool ensureListedLocked(Channel& c, Playlist& pl, double at);
    void listLocal(Playlist& pl) const;
    std::vector<Playlist> buildPlaylists(const std::vector<VideoConfig::PlaylistEntry>& entries,
                                         const std::string& source) const;
    std::string refreshPlaylistsLocked();
    void loadListCache(Playlist& pl) const;
    void saveListCache(const Playlist& pl) const;
    void refreshListLocked(Playlist& pl);
    void prefetchNextLocked(const Channel& c);
    // Subtitles: the tracks of the channel's current video, the languages wanted for its playlist, and applying them
    // to the playing video (fetching YouTube's when none are cached yet).
    std::vector<SubtitleTrack> subtitleTracksLocked(const Channel& c) const;
    std::vector<std::string> subtitleLanguagesLocked(const Channel& c, bool& allowUntagged) const;
    void applySubtitlesLocked(Channel& c, bool fetch);
    void fetchSubtitlesLocked(const std::string& videoId, const std::vector<std::string>& languages, Channel* forChannel,
                              const std::string& url = {});
    std::string cycleSubtitlesLocked(Channel& c);
    void onTrackFinishedLocked(Channel& c);
    void spatialLoop();
    void updateGainsLocked(Channel& c);
    void updatePlayerAreaLocked();
    std::string remoteLabelLocked(const Screen& screen, float px, float py, float pz);
    bool remoteEligibleLocked(std::uint32_t formId);
    bool screenEligibleLocked(const Screen& screen) const;
    bool selectAudienceLocked(Channel& c);
    void suspendLocked(Channel& c);
    void reconcileAudienceLocked();
    double positionLocked(const Channel& c) const;
    const Track* currentTrackLocked(const Channel& c) const;
    std::string trackLabelLocked(const Channel& c) const;
    void loadSession(Channel& c);
    void readSession(Channel& c, std::istream& in);
    void resetSessionLocked(Channel& c);
    std::string sessionTextLocked(const Channel& c, double position, bool playing) const;
    void saveSessionLocked(Channel& c);
    void trimSnapshots(const std::filesystem::path& dir) const;
    std::string tag(const Channel& c) const;

    Logger& logger_;
    VideoConfig config_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::uint32_t, Screen> screens_;
    std::vector<std::unique_ptr<Channel>> channels_;
    std::vector<Playlist> playlists_;
    std::uint64_t playlistGeneration_{ 0 }; // rejects list callbacks from a catalog replaced by Refresh
    std::unique_ptr<VideoFetcher> fetcher_;
    bool playerPositionVerified_{ false };
    bool playerPositionChecked_{ false };
    float lastPlayer_[3]{};   // last player position from Papyrus (fallback when the direct read is not verified)
    bool loading_{ false };   // between beginLoad and finishLoad: screens register, nothing resumes yet
#ifdef SCREENSFSE_HARNESS
    bool areaTracking_{ false }; // older harnesses do not simulate game locations
#else
    bool areaTracking_{ true };
#endif
    ScreenLocation::Context playerArea_;
    std::vector<std::pair<std::uint32_t, std::string>> remoteList_;   // remoteScreens' snapshot
    std::set<std::string> subtitlesRequested_;   // "<video id>|<languages>" asked of yt-dlp this game session
    int lastMusicMute_{ -1 };   // last musicMuteWanted answer, for the log (-1 = none yet)

    std::thread worker_;
    bool quit_{ false };
};
