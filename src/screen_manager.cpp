#include "screen_manager.h"

#include "RE/T/TESObjectREFR.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace
{
constexpr std::int32_t kMinCols = 1;
constexpr std::int32_t kMaxCols = 512;
constexpr std::int32_t kMinRows = 1;
constexpr std::int32_t kMaxRows = 512;
constexpr std::size_t kMaxTextLen = 512;
constexpr std::size_t kMaxImageIdLen = 128;
constexpr std::size_t kMaxPendingCommands = 4096;
}

ScreenManager::ScreenManager(Logger& logger) :
    logger_(logger)
{}

bool ScreenManager::setSurfaceNodeName(RE::TESObjectREFR* screenRef, std::string nodeName)
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return false;
    }

    nodeName = trimAscii(std::move(nodeName));
    if (nodeName.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ScreenState* state = getOrCreateStateLocked(key);
    state->surfaceNodeName = truncateText(std::move(nodeName), 128);
    clearLastError(*state);
    return true;
}

std::string ScreenManager::getSurfaceNodeName(RE::TESObjectREFR* screenRef) const
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return {};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (const ScreenState* state = findStateLocked(key)) {
        return state->surfaceNodeName;
    }
    return "ScreenSurface";
}

bool ScreenManager::ensureScreen(RE::TESObjectREFR* screenRef, std::int32_t cols, std::int32_t rows)
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return false;
    }

    const std::int32_t safeCols = clampDimension(cols, 80, kMinCols, kMaxCols);
    const std::int32_t safeRows = clampDimension(rows, 25, kMinRows, kMaxRows);

    std::lock_guard<std::mutex> lock(mutex_);
    ScreenState* state = getOrCreateStateLocked(key);
    state->cols = safeCols;
    state->rows = safeRows;
    clearLastError(*state);
    return true;
}

bool ScreenManager::reset(RE::TESObjectREFR* screenRef)
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    screens_.erase(key);
    return true;
}

bool ScreenManager::clear(RE::TESObjectREFR* screenRef, std::int32_t bgColor)
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ScreenState* state = getOrCreateStateLocked(key);
    state->clearColor = bgColor;
    if (state->pendingCommands.size() >= kMaxPendingCommands) {
        setLastError(*state, "Command queue is full.");
        return false;
    }

    DrawCommand command;
    command.type = CommandType::Clear;
    command.color0 = bgColor;
    state->pendingCommands.push_back(std::move(command));
    clearLastError(*state);
    return true;
}

bool ScreenManager::writeText(
    RE::TESObjectREFR* screenRef,
    std::int32_t x,
    std::int32_t y,
    std::string text,
    std::int32_t fgColor,
    std::int32_t bgColor)
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return false;
    }

    text = truncateText(trimAscii(std::move(text)), kMaxTextLen);
    if (text.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ScreenState* state = getOrCreateStateLocked(key);
    if (state->pendingCommands.size() >= kMaxPendingCommands) {
        setLastError(*state, "Command queue is full.");
        return false;
    }

    DrawCommand command;
    command.type = CommandType::WriteText;
    command.a = x;
    command.b = y;
    command.color0 = fgColor;
    command.color1 = bgColor;
    command.text = std::move(text);
    state->pendingCommands.push_back(std::move(command));
    clearLastError(*state);
    return true;
}

bool ScreenManager::drawLine(
    RE::TESObjectREFR* screenRef,
    std::int32_t x0,
    std::int32_t y0,
    std::int32_t x1,
    std::int32_t y1,
    std::int32_t color)
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ScreenState* state = getOrCreateStateLocked(key);
    if (state->pendingCommands.size() >= kMaxPendingCommands) {
        setLastError(*state, "Command queue is full.");
        return false;
    }

    DrawCommand command;
    command.type = CommandType::Line;
    command.a = x0;
    command.b = y0;
    command.c = x1;
    command.d = y1;
    command.color0 = color;
    state->pendingCommands.push_back(std::move(command));
    clearLastError(*state);
    return true;
}

bool ScreenManager::drawRect(
    RE::TESObjectREFR* screenRef,
    std::int32_t x,
    std::int32_t y,
    std::int32_t width,
    std::int32_t height,
    std::int32_t color,
    bool filled)
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return false;
    }
    if (width <= 0 || height <= 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ScreenState* state = getOrCreateStateLocked(key);
    if (state->pendingCommands.size() >= kMaxPendingCommands) {
        setLastError(*state, "Command queue is full.");
        return false;
    }

    DrawCommand command;
    command.type = CommandType::Rect;
    command.a = x;
    command.b = y;
    command.c = width;
    command.d = height;
    command.color0 = color;
    command.flag = filled;
    state->pendingCommands.push_back(std::move(command));
    clearLastError(*state);
    return true;
}

bool ScreenManager::drawCircle(
    RE::TESObjectREFR* screenRef,
    std::int32_t cx,
    std::int32_t cy,
    std::int32_t radius,
    std::int32_t color,
    bool filled)
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return false;
    }
    if (radius <= 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ScreenState* state = getOrCreateStateLocked(key);
    if (state->pendingCommands.size() >= kMaxPendingCommands) {
        setLastError(*state, "Command queue is full.");
        return false;
    }

    DrawCommand command;
    command.type = CommandType::Circle;
    command.a = cx;
    command.b = cy;
    command.c = radius;
    command.color0 = color;
    command.flag = filled;
    state->pendingCommands.push_back(std::move(command));
    clearLastError(*state);
    return true;
}

bool ScreenManager::blitImage(
    RE::TESObjectREFR* screenRef,
    std::string imageId,
    std::int32_t x,
    std::int32_t y,
    std::int32_t width,
    std::int32_t height,
    std::int32_t tintColor)
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return false;
    }

    imageId = truncateText(trimAscii(std::move(imageId)), kMaxImageIdLen);
    if (imageId.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ScreenState* state = getOrCreateStateLocked(key);
    if (state->pendingCommands.size() >= kMaxPendingCommands) {
        setLastError(*state, "Command queue is full.");
        return false;
    }

    DrawCommand command;
    command.type = CommandType::Image;
    command.a = x;
    command.b = y;
    command.c = width;
    command.d = height;
    command.color0 = tintColor;
    command.text = std::move(imageId);
    state->pendingCommands.push_back(std::move(command));
    clearLastError(*state);
    return true;
}

bool ScreenManager::present(RE::TESObjectREFR* screenRef)
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return false;
    }

    std::string summary;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ScreenState* state = getOrCreateStateLocked(key);
        ++state->presentCount;
        state->lastPresentedSummary = buildSummary(key, *state);
        summary = state->lastPresentedSummary;
        state->pendingCommands.clear();
        clearLastError(*state);
    }

    logger_.info(summary + " | renderer backend not installed yet");
    return true;
}

std::int32_t ScreenManager::getPendingCommandCount(RE::TESObjectREFR* screenRef) const
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (const ScreenState* state = findStateLocked(key)) {
        return static_cast<std::int32_t>(state->pendingCommands.size());
    }
    return 0;
}

std::string ScreenManager::getDebugSummary(RE::TESObjectREFR* screenRef) const
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return {};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (const ScreenState* state = findStateLocked(key)) {
        if (!state->pendingCommands.empty()) {
            return buildSummary(key, *state);
        }
        return state->lastPresentedSummary;
    }
    return {};
}

std::string ScreenManager::getLastError(RE::TESObjectREFR* screenRef) const
{
    const std::uint64_t key = keyFromRef(screenRef);
    if (key == 0) {
        return {};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (const ScreenState* state = findStateLocked(key)) {
        return state->lastError;
    }
    return {};
}

std::string ScreenManager::trimAscii(std::string value)
{
    auto isSpace = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };

    const auto begin = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto end = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

std::string ScreenManager::truncateText(std::string value, const std::size_t maxLen)
{
    if (value.size() > maxLen) {
        value.resize(maxLen);
    }
    return value;
}

std::string ScreenManager::colorToHex(const std::int32_t value)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
        << static_cast<std::uint32_t>(value);
    return out.str();
}

std::int32_t ScreenManager::clampDimension(
    const std::int32_t value,
    const std::int32_t fallback,
    const std::int32_t minimum,
    const std::int32_t maximum)
{
    return std::clamp(value > 0 ? value : fallback, minimum, maximum);
}

std::uint64_t ScreenManager::keyFromRef(const RE::TESObjectREFR* screenRef)
{
    if (screenRef == nullptr) {
        return 0;
    }
    return screenRef->GetFormID();
}

ScreenManager::ScreenState* ScreenManager::getOrCreateStateLocked(const std::uint64_t key)
{
    return &screens_[key];
}

const ScreenManager::ScreenState* ScreenManager::findStateLocked(const std::uint64_t key) const
{
    const auto it = screens_.find(key);
    if (it == screens_.end()) {
        return nullptr;
    }
    return &it->second;
}

std::string ScreenManager::buildSummary(const std::uint64_t key, const ScreenState& state)
{
    std::string preview;
    for (const DrawCommand& command : state.pendingCommands) {
        if (command.type == CommandType::WriteText && !command.text.empty()) {
            preview = truncateText(command.text, 48);
            break;
        }
    }

    std::ostringstream out;
    out << "Screen "
        << "ref=0x" << std::uppercase << std::hex << key << std::dec
        << " node=" << state.surfaceNodeName
        << " size=" << state.cols << 'x' << state.rows
        << " clear=" << colorToHex(state.clearColor)
        << " queued=" << state.pendingCommands.size()
        << " presents=" << state.presentCount;
    if (!preview.empty()) {
        out << " preview=\"" << preview << '"';
    }
    return out.str();
}

void ScreenManager::clearLastError(ScreenState& state)
{
    state.lastError.clear();
}

void ScreenManager::setLastError(ScreenState& state, const std::string& message)
{
    state.lastError = message;
}
