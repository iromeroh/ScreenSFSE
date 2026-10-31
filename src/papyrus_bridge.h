#pragma once

#include "logger.h"
#include "screen_manager.h"

#include "sfse/PluginAPI.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <variant>

namespace RE
{
class TESObjectREFR;
class BGSMessage;
}

class ScreenController;

class PapyrusBridge
{
public:
    PapyrusBridge(Logger& logger, ScreenManager& screenManager);

    // Video screen natives forward here (none: they report the plugin is not ready).
    static void setScreenController(ScreenController* controller) { controller_ = controller; }

    bool initialize(const SFSEInterface* sfse);
    void shutdown();

private:
    static PapyrusBridge* instance_;
    static ScreenController* controller_;

    static void onSFSEMessage(SFSEMessagingInterface::Message* message);
    static const char* messageTypeName(std::uint32_t type);

    bool installMessagingListener(const SFSEInterface* sfse);
    bool tryRegisterNatives(const char* reason);

    static bool nativePluginAvailable(std::monostate, RE::TESObjectREFR* screenRef);
    static bool nativeSetSurfaceNodeName(std::monostate, RE::TESObjectREFR* screenRef, std::string nodeName);
    static std::string nativeGetSurfaceNodeName(std::monostate, RE::TESObjectREFR* screenRef);
    static bool nativeEnsureScreen(std::monostate, RE::TESObjectREFR* screenRef, std::int32_t cols, std::int32_t rows);
    static bool nativeReset(std::monostate, RE::TESObjectREFR* screenRef);
    static bool nativeClear(std::monostate, RE::TESObjectREFR* screenRef, std::int32_t bgColor);
    static bool nativeWriteText(
        std::monostate,
        RE::TESObjectREFR* screenRef,
        std::int32_t x,
        std::int32_t y,
        std::string text,
        std::int32_t fgColor,
        std::int32_t bgColor);
    static bool nativeDrawLine(
        std::monostate,
        RE::TESObjectREFR* screenRef,
        std::int32_t x0,
        std::int32_t y0,
        std::int32_t x1,
        std::int32_t y1,
        std::int32_t color);
    static bool nativeDrawRect(
        std::monostate,
        RE::TESObjectREFR* screenRef,
        std::int32_t x,
        std::int32_t y,
        std::int32_t width,
        std::int32_t height,
        std::int32_t color,
        bool filled);
    static bool nativeDrawCircle(
        std::monostate,
        RE::TESObjectREFR* screenRef,
        std::int32_t cx,
        std::int32_t cy,
        std::int32_t radius,
        std::int32_t color,
        bool filled);
    static bool nativeBlitImage(
        std::monostate,
        RE::TESObjectREFR* screenRef,
        std::string imageId,
        std::int32_t x,
        std::int32_t y,
        std::int32_t width,
        std::int32_t height,
        std::int32_t tintColor);
    static bool nativePresent(std::monostate, RE::TESObjectREFR* screenRef);
    static std::int32_t nativeGetPendingCommandCount(std::monostate, RE::TESObjectREFR* screenRef);
    static std::string nativeGetDebugSummary(std::monostate, RE::TESObjectREFR* screenRef);
    static std::string nativeGetLastError(std::monostate, RE::TESObjectREFR* screenRef);
    static bool nativeRegisterScreen(std::monostate, RE::TESObjectREFR* screenRef, std::int32_t channel, float x, float y,
                                     float z, float playerX, float playerY, float playerZ);
    static std::string nativeScreenActivated(std::monostate, RE::TESObjectREFR* screenRef);
    static std::string nativeScreenAction(std::monostate, RE::TESObjectREFR* screenRef, std::int32_t action);
    static std::string nativeScreenStatus(std::monostate, RE::TESObjectREFR* screenRef);
    static bool nativeSetMessageName(std::monostate, RE::BGSMessage* message, std::string name);
    static bool nativeScreenUnloaded(std::monostate, RE::TESObjectREFR* screenRef);
    static bool nativeScreenRemoved(std::monostate, RE::TESObjectREFR* screenRef);
    static bool nativeMusicMuteWanted(std::monostate, float playerX, float playerY, float playerZ);
    static float nativeSoundSourceRange(std::monostate, RE::TESObjectREFR* screenRef);
    static bool nativeLog(std::monostate, std::string text);
    static std::int32_t nativeRemoteScreens(std::monostate, float playerX, float playerY, float playerZ);
    static std::int32_t nativeRemoteScreenId(std::monostate, std::int32_t index);
    static std::string nativeRemoteScreenLabel(std::monostate, std::int32_t index);
    static std::string nativeRemoteLabel(std::monostate, std::int32_t screenId, float playerX, float playerY, float playerZ);
    static std::string nativeRemoteField(std::monostate, std::int32_t screenId, std::int32_t field);
    static std::string nativeRemoteAction(std::monostate, std::int32_t screenId, std::int32_t action);

    Logger& logger_;
    ScreenManager& screenManager_;
    PluginHandle pluginHandle_{ static_cast<PluginHandle>(kPluginHandle_Invalid) };
    bool installed_{ false };
    bool registered_{ false };
    bool waitingLogged_{ false };
    std::atomic<bool> shuttingDown_{ false };
};
