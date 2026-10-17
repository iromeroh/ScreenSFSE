#pragma once

#include "logger.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace RE
{
class TESObjectREFR;
}

class ScreenManager
{
public:
    explicit ScreenManager(Logger& logger);

    bool setSurfaceNodeName(RE::TESObjectREFR* screenRef, std::string nodeName);
    std::string getSurfaceNodeName(RE::TESObjectREFR* screenRef) const;

    bool ensureScreen(RE::TESObjectREFR* screenRef, std::int32_t cols, std::int32_t rows);
    bool reset(RE::TESObjectREFR* screenRef);
    bool clear(RE::TESObjectREFR* screenRef, std::int32_t bgColor);
    bool writeText(
        RE::TESObjectREFR* screenRef,
        std::int32_t x,
        std::int32_t y,
        std::string text,
        std::int32_t fgColor,
        std::int32_t bgColor);
    bool drawLine(
        RE::TESObjectREFR* screenRef,
        std::int32_t x0,
        std::int32_t y0,
        std::int32_t x1,
        std::int32_t y1,
        std::int32_t color);
    bool drawRect(
        RE::TESObjectREFR* screenRef,
        std::int32_t x,
        std::int32_t y,
        std::int32_t width,
        std::int32_t height,
        std::int32_t color,
        bool filled);
    bool drawCircle(
        RE::TESObjectREFR* screenRef,
        std::int32_t cx,
        std::int32_t cy,
        std::int32_t radius,
        std::int32_t color,
        bool filled);
    bool blitImage(
        RE::TESObjectREFR* screenRef,
        std::string imageId,
        std::int32_t x,
        std::int32_t y,
        std::int32_t width,
        std::int32_t height,
        std::int32_t tintColor);
    bool present(RE::TESObjectREFR* screenRef);

    std::int32_t getPendingCommandCount(RE::TESObjectREFR* screenRef) const;
    std::string getDebugSummary(RE::TESObjectREFR* screenRef) const;
    std::string getLastError(RE::TESObjectREFR* screenRef) const;

private:
    enum class CommandType
    {
        Clear,
        WriteText,
        Line,
        Rect,
        Circle,
        Image
    };

    struct DrawCommand
    {
        CommandType type{ CommandType::Clear };
        std::int32_t a{ 0 };
        std::int32_t b{ 0 };
        std::int32_t c{ 0 };
        std::int32_t d{ 0 };
        std::int32_t color0{ 0 };
        std::int32_t color1{ 0 };
        bool flag{ false };
        std::string text;
    };

    struct ScreenState
    {
        std::int32_t cols{ 80 };
        std::int32_t rows{ 25 };
        std::int32_t clearColor{ 0 };
        std::uint32_t presentCount{ 0 };
        std::string surfaceNodeName{ "ScreenSurface" };
        std::string lastError;
        std::string lastPresentedSummary;
        std::vector<DrawCommand> pendingCommands;
    };

    static std::string trimAscii(std::string value);
    static std::string truncateText(std::string value, std::size_t maxLen);
    static std::string colorToHex(std::int32_t value);
    static std::int32_t clampDimension(std::int32_t value, std::int32_t fallback, std::int32_t minimum, std::int32_t maximum);
    static std::uint64_t keyFromRef(const RE::TESObjectREFR* screenRef);

    ScreenState* getOrCreateStateLocked(std::uint64_t key);
    const ScreenState* findStateLocked(std::uint64_t key) const;
    static std::string buildSummary(std::uint64_t key, const ScreenState& state);
    static void clearLastError(ScreenState& state);
    static void setLastError(ScreenState& state, const std::string& message);

    Logger& logger_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, ScreenState> screens_;
};
