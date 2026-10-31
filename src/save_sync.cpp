#include "SFSE/Impl/PCH.h"

#include "save_sync.h"

#include "logger.h"
#include "screen_controller.h"
#include "video_surface.h"

#include "RE/B/BSTEvent.h"
#include "RE/IDs.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESGlobal.h"
#include "RE/T/TESObjectREFR.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/P/PlayerCharacter.h"

#include <cmath>
#include <mutex>
#include <random>
#include <string>

namespace
{
constexpr const char* kTokenGlobal = "ScreenSFSE_SaveToken";   // GLOB 0100080F in ScreenSFSE.esm

// RE/E/Events.h SaveLoadEvent / TESLoadGameEvent, mirrored: that header only compiles inside the whole RE/Starfield.h
// include chain (which needs D3D12 headers this build does not have). Same layout and address IDs.
struct SaveLoadEvent
{
    enum class OpType : std::uint8_t
    {
        kAutosave = 1,
        kLoadMostRecent = 2,
        kQuicksave = 3,
        kQuickload = 4,
        kManualSave = 5,
        kLoad = 6,
        kExitSaveToMainMenu = 7,
        kExitSaveToDesktop = 8,
        kLoadNamedFile = 0xB,
    };
    enum class Status : std::uint8_t
    {
        kBegin = 0,
        kLoadSucceeded = 1,
        kFailed = 3,
        kSaveCompleted = 4,
        kLoadDispatchRefused = 5,
    };
    std::uint32_t elapsedMs;      // 00
    std::uint32_t fileSizeBytes;  // 04
    std::uint32_t unk08;          // 08
    OpType opType;                // 0C
    Status status;                // 0D
};
static_assert(sizeof(SaveLoadEvent) == 0x10);

struct TESLoadGameEvent
{};

template <class Event>
RE::BSTEventSource<Event>* eventSource(REL::ID id)
{
    using func_t = RE::BSTEventSource<Event>* (*)();
    const REL::Relocation<func_t> func{ id };
    return func();
}

using Op = SaveLoadEvent::OpType;
using Status = SaveLoadEvent::Status;

bool isSave(Op op)
{
    return op == Op::kAutosave || op == Op::kQuicksave || op == Op::kManualSave || op == Op::kExitSaveToMainMenu ||
           op == Op::kExitSaveToDesktop;
}

bool isLoad(Op op)
{
    return op == Op::kLoadMostRecent || op == Op::kQuickload || op == Op::kLoad || op == Op::kLoadNamedFile;
}

class Sync :
    public RE::BSTEventSink<SaveLoadEvent>,
    public RE::BSTEventSink<TESLoadGameEvent>
{
public:
    Sync(Logger& logger, ScreenController& controller, RE::TESGlobal* token) :
        logger_(logger), controller_(controller), token_(token)
    {}

    // A save: the snapshot gets a new token, which goes into the GLOB that is saved with the game. Whether this event
    // comes before or after the game writes its globals is not documented, so both orders work:
    // - Begin arrives (before the save is written): new token now; the save stores it.
    // - only SaveCompleted arrives (after): the save stored the token already in the GLOB (fresh since the last
    //   save or load), so the snapshot takes that one; then a new token for the next save.
    RE::BSEventNotifyControl ProcessEvent(const SaveLoadEvent& e, RE::BSTEventSource<SaveLoadEvent>*) override
    {
        std::lock_guard lock(mutex_);
        logger_.info("Save/load event: op " + std::to_string(static_cast<int>(e.opType)) + ", status " +
                     std::to_string(static_cast<int>(e.status)) + ", token " + std::to_string(read()));
        if (isSave(e.opType)) {
            if (e.status == Status::kBegin) {
                const auto t = fresh();
                controller_.saveSnapshot(t);
                write(t);
                saveBegun_ = true;
            } else if (e.status == Status::kSaveCompleted || e.status == Status::kFailed) {
                if (!saveBegun_ && e.status == Status::kSaveCompleted) {
                    if (const auto t = read(); t != 0) {
                        controller_.saveSnapshot(t);
                    }
                    write(fresh());
                }
                saveBegun_ = false;
            }
        } else if (isLoad(e.opType) && e.status == Status::kBegin) {
            controller_.beginLoad();
        } else if (isLoad(e.opType) && (e.status == Status::kFailed || e.status == Status::kLoadDispatchRefused)) {
            // the current game goes on: no snapshot for its token yet, so the session files beginLoad just wrote apply
            controller_.finishLoad(read(), [this](std::uint32_t f, int ch, float& x, float& y, float& z, ScreenLocation::Area& area) { return inWorld(f, ch, x, y, z, area); });
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl ProcessEvent(const TESLoadGameEvent&, RE::BSTEventSource<TESLoadGameEvent>*) override
    {
        std::lock_guard lock(mutex_);
        const auto t = read();
        logger_.info("Game loaded: save token " + std::to_string(t) + ".");
        controller_.finishLoad(t, [this](std::uint32_t f, int ch, float& x, float& y, float& z, ScreenLocation::Area& area) { return inWorld(f, ch, x, y, z, area); });
        write(fresh());   // the loaded save's snapshot stays as it is; the next save gets its own
        saveBegun_ = false;
        return RE::BSEventNotifyControl::kContinue;
    }

private:
    // Whether a saved screen that has not registered is in the world: its OnLoad does not run when a save loads in its
    // own cell. Its reference with loaded 3D, or else its channel's texture existing (the texture is created when a
    // screen of that channel loads, before the load-finished event), with the position saved in the snapshot.
    bool inWorld(std::uint32_t formId, int channel, float& x, float& y, float& z, ScreenLocation::Area& area)
    {
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", formId);
        const char* why = nullptr;
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(formId);
        // No walk of the player's cell here (TESObjectCELL::ForEachReference): its lock and layout are unverified for
        // this game version, and walking it could crash while loading a save. A screen
        // LookupByID misses falls back to its channel texture plus the area saved in the snapshot.
        if (ref) area = ScreenLocation::reference(ref);
        if (!ref) {
            why = "not found";
        } else if (ref->IsDeleted() || ref->IsDisabled()) {
            why = "disabled or deleted";
        } else if (const auto loaded = ref->loadedData.LockRead(); !*loaded) {
            why = "3D not loaded";
        } else {
            x = ref->GetPositionX();
            y = ref->GetPositionY();
            z = ref->GetPositionZ();
            logger_.info(std::string("Saved screen ") + id + " found in the world.");
            return true;
        }
        const bool live = surfaceLive(channel);
        logger_.info(std::string("Saved screen ") + id + ": reference " + why + "; channel " + std::to_string(channel) +
                     (live ? " texture loaded, resuming there." : " texture not loaded, waiting for the screen."));
        return live;
    }

    std::uint32_t read() const
    {
        const float v = token_->GetValue();
        return v >= 1.0f && v <= 16777215.0f ? static_cast<std::uint32_t>(std::lround(v)) : 0;
    }

    void write(std::uint32_t t) { token_->value = static_cast<float>(t); }

    // 1..2^24-1: a float holds every integer in that range exactly
    std::uint32_t fresh()
    {
        std::uniform_int_distribution<std::uint32_t> pick(1, 0xFFFFFF);
        std::uint32_t t;
        do {
            t = pick(rng_);
        } while (t == read());
        return t;
    }

    Logger& logger_;
    ScreenController& controller_;
    RE::TESGlobal* token_;
    std::mutex mutex_;
    std::mt19937 rng_{ std::random_device{}() };
    bool saveBegun_{ false };
};

std::once_flag g_installed;
}

namespace SaveSync
{
void install(Logger& logger, ScreenController& controller)
{
    std::call_once(g_installed, [&] {
        auto* token = RE::TESForm::LookupByEditorID<RE::TESGlobal>(kTokenGlobal);
        auto* saves = eventSource<SaveLoadEvent>(RE::ID::SaveLoadEvent::GetEventSource);
        auto* loads = eventSource<TESLoadGameEvent>(RE::ID::TESLoadGameEvent::GetEventSource);
        if (!token || !saves || !loads) {
            logger.warn(std::string("Per-save resume off: ") + (!token ? "GLOB ScreenSFSE_SaveToken not found (old ScreenSFSE.esm?)"
                                                                      : "save/load event source unavailable") + ".");
            return;
        }
        static Sync sync(logger, controller, token);
        saves->RegisterSink(&sync);
        loads->RegisterSink(&sync);
        logger.info("Per-save resume on (save token GLOB found, save/load events registered).");
    });
}
}
