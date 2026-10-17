#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class FrameSource;
class Logger;
class MediaPlayer;
class Playback;
class TestPatternSource;
class VideoFetcher;

// ScreenSFSE.ini [Video]
struct VideoConfig
{
    struct PlaylistEntry
    {
        std::string name;
        std::string source;   // YouTube playlist/video URL, or a folder / video file (absolute or relative to rootDir)
    };
    std::vector<PlaylistEntry> playlists;   // playlist=Name|Source lines, in order
    std::string source;                     // v2 single Source= (becomes a playlist "Video" when no playlist= lines)
    std::filesystem::path rootDir;          // default <game folder>\Data\VideoScreen (relative = to the game folder)
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
};

// The buildable video screens, controlled like the radio. Screens belong to channels (the screen variant built:
// each has its own placeholder texture). Each channel plays on its own: playlist, video, position, volume and a
// session file (<root>\ScreenSFSE.session[.<key>].txt), so a reload resumes where it was. Screens of one channel show
// the same picture; the sound comes from the one last used. Playlists come from the INI (YouTube playlists/videos via
// yt-dlp, local folders) and are shared by all channels.
// A 10 Hz worker runs only while some channel plays: spatial sound from the player's position/facing, end of track
// (-> next track), and each playing channel's position saved every 5 s.
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
        Stop = 8
    };

    struct ChannelInfo
    {
        int id;
        std::string key;   // session file name part ("wall1": the v2 file name)
        std::uint32_t width;
        std::uint32_t height;
        int number;        // channel number shown to the player (1-4)
    };

    ScreenController(Logger& logger, const VideoConfig& config, const std::vector<ChannelInfo>& channels);
    ~ScreenController();

    void registerScreen(std::uint32_t formId, int channel, float x, float y, float z, float playerX, float playerY, float playerZ);
    // A menu action on a screen; returns a short message for the player (empty = none).
    std::string action(std::uint32_t formId, int action);
    // One line for the menu title: channel, state, track, time, volume, playlist.
    std::string status(std::uint32_t formId);
    void unloaded(std::uint32_t formId);
    void removed(std::uint32_t formId);

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
    };

    struct Track
    {
        std::string key;     // YouTube video ID, or file path
        std::string title;
        std::string status;  // YouTube live_status (is_live / is_upcoming: streamed live, never downloaded)
    };

    struct Playlist
    {
        std::string name;
        std::string source;
        bool youTube{ false };
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
        std::shared_ptr<FrameSource> idleCard;
        std::shared_ptr<TestPatternSource> loadingCard;
        std::size_t playlist{ 0 };
        std::size_t track{ 0 };
        std::string pendingTrackKey;     // from the session file, resolved once the playlist is listed
        float volume{ 0.8f };
        double resumeAt{ 0.0 };          // where play starts when idle (the saved position)
        bool sessionPlaying{ false };    // the session says it was playing: resume when one of its screens loads
        std::uint32_t sessionScreen{ 0 };
        std::filesystem::path sessionFile;
        std::chrono::steady_clock::time_point lastSessionSave{};
    };

    Channel* channelFor(std::uint32_t formId);
    Channel* channelById(int id);
    std::string playCurrentLocked(Channel& c, double at);
    void startFileLocked(Channel& c, const std::filesystem::path& file, double at);
    void streamLocked(Channel& c, const Track& track);
    void startLiveLocked(Channel& c, const std::string& videoUrl, const std::string& audioUrl);
    void setTrackStatusLocked(const std::string& key, const std::string& status);
    void stopLocked(Channel& c, const char* why);
    void pauseLocked(Channel& c);
    void resumeLocked(Channel& c);
    void showLoadingLocked(Channel& c);
    void showIdleLocked(Channel& c);
    bool ensureListedLocked(Channel& c, Playlist& pl, double at);
    void listLocal(Playlist& pl) const;
    void loadListCache(Playlist& pl) const;
    void saveListCache(const Playlist& pl) const;
    void refreshListLocked(Playlist& pl);
    void prefetchNextLocked(const Channel& c);
    void onTrackFinishedLocked(Channel& c);
    void spatialLoop();
    void updateGainsLocked(Channel& c);
    double positionLocked(const Channel& c) const;
    const Track* currentTrackLocked(const Channel& c) const;
    std::string trackLabelLocked(const Channel& c) const;
    void loadSession(Channel& c);
    void saveSessionLocked(Channel& c);
    std::string tag(const Channel& c) const;

    Logger& logger_;
    VideoConfig config_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::uint32_t, Screen> screens_;
    std::vector<std::unique_ptr<Channel>> channels_;
    std::vector<Playlist> playlists_;
    std::unique_ptr<VideoFetcher> fetcher_;
    bool playerPositionVerified_{ false };
    bool playerPositionChecked_{ false };
    float lastPlayer_[3]{};   // last player position from Papyrus (fallback when the direct read is not verified)

    std::thread worker_;
    bool quit_{ false };
};
