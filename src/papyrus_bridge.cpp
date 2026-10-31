#include "SFSE/Impl/PCH.h"

#include "papyrus_bridge.h"

#include "save_sync.h"
#include "screen_controller.h"

#include "RE/A/Array.h"
#include "RE/B/BGSMessage.h"
#include "RE/B/BSScriptUtil.h"
#include "RE/M/MemoryManager.h"
#include "RE/T/TESObjectREFR.h"
#include "RE/V/VirtualMachine.h"

#include <optional>

PapyrusBridge* PapyrusBridge::instance_ = nullptr;
ScreenController* PapyrusBridge::controller_ = nullptr;

PapyrusBridge::PapyrusBridge(Logger& logger, ScreenManager& screenManager) :
    logger_(logger),
    screenManager_(screenManager)
{}

bool PapyrusBridge::initialize(const SFSEInterface* sfse)
{
    if (sfse == nullptr) {
        logger_.warn("Papyrus bridge init failed: SFSE interface is null.");
        return false;
    }

    instance_ = this;
    shuttingDown_.store(false);

    if (!installMessagingListener(sfse)) {
        instance_ = nullptr;
        return false;
    }

    (void)tryRegisterNatives("plugin_load");
    return true;
}

void PapyrusBridge::shutdown()
{
    shuttingDown_.store(true);
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

void PapyrusBridge::onSFSEMessage(SFSEMessagingInterface::Message* message)
{
    if (instance_ == nullptr || message == nullptr) {
        return;
    }

    instance_->logger_.info(std::string("SFSE message received: ") + messageTypeName(message->type));
    (void)instance_->tryRegisterNatives(messageTypeName(message->type));
    if (message->type == SFSEMessagingInterface::kMessage_PostDataLoad && controller_) {
        SaveSync::install(instance_->logger_, *controller_);   // the save token GLOB exists once the data is loaded
    }
}

const char* PapyrusBridge::messageTypeName(const std::uint32_t type)
{
    switch (type) {
    case SFSEMessagingInterface::kMessage_PostLoad:
        return "PostLoad";
    case SFSEMessagingInterface::kMessage_PostPostLoad:
        return "PostPostLoad";
    case SFSEMessagingInterface::kMessage_PostDataLoad:
        return "PostDataLoad";
    case SFSEMessagingInterface::kMessage_PostPostDataLoad:
        return "PostPostDataLoad";
    case SFSEMessagingInterface::kMessage_PreSaveGame:
        return "PreSaveGame";
    case SFSEMessagingInterface::kMessage_PostSaveGame:
        return "PostSaveGame";
    case SFSEMessagingInterface::kMessage_PreLoadGame:
        return "PreLoadGame";
    case SFSEMessagingInterface::kMessage_PostLoadGame:
        return "PostLoadGame";
    default:
        return "Unknown";
    }
}

bool PapyrusBridge::installMessagingListener(const SFSEInterface* sfse)
{
    auto* messaging = static_cast<SFSEMessagingInterface*>(sfse->QueryInterface(kInterface_Messaging));
    if (messaging == nullptr) {
        logger_.warn("Papyrus bridge init failed: messaging interface unavailable.");
        return false;
    }

    pluginHandle_ = sfse->GetPluginHandle();
    if (pluginHandle_ == kPluginHandle_Invalid) {
        logger_.warn("Papyrus bridge init failed: invalid plugin handle.");
        return false;
    }

    if (!messaging->RegisterListener(pluginHandle_, "SFSE", &PapyrusBridge::onSFSEMessage)) {
        logger_.warn("Papyrus bridge init failed: could not register SFSE listener.");
        return false;
    }

    installed_ = true;
    logger_.info("Papyrus bridge listener installed.");
    return true;
}

bool PapyrusBridge::tryRegisterNatives(const char* reason)
{
    if (registered_) {
        return true;
    }

    auto* vmImpl = RE::BSScript::Internal::VirtualMachine::GetSingleton();
    if (vmImpl == nullptr) {
        if (!waitingLogged_) {
            waitingLogged_ = true;
            logger_.info(std::string("Waiting for Papyrus VM before native registration (trigger=") +
                         (reason ? reason : "unknown") + ").");
        }
        return false;
    }

    auto* vm = static_cast<RE::BSScript::IVirtualMachine*>(vmImpl);
    if (vm == nullptr) {
        logger_.warn("Papyrus bridge native registration failed: VM cast returned null.");
        return false;
    }

    waitingLogged_ = false;
    constexpr const char* kScriptName = "SCREEN_SFSENative";

    vm->BindNativeMethod(kScriptName, "pluginAvailable", &PapyrusBridge::nativePluginAvailable, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "setSurfaceNodeName", &PapyrusBridge::nativeSetSurfaceNodeName, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "getSurfaceNodeName", &PapyrusBridge::nativeGetSurfaceNodeName, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "ensureScreen", &PapyrusBridge::nativeEnsureScreen, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "reset", &PapyrusBridge::nativeReset, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "clear", &PapyrusBridge::nativeClear, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "writeText", &PapyrusBridge::nativeWriteText, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "drawLine", &PapyrusBridge::nativeDrawLine, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "drawRect", &PapyrusBridge::nativeDrawRect, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "drawCircle", &PapyrusBridge::nativeDrawCircle, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "blitImage", &PapyrusBridge::nativeBlitImage, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "present", &PapyrusBridge::nativePresent, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "getPendingCommandCount", &PapyrusBridge::nativeGetPendingCommandCount, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "getDebugSummary", &PapyrusBridge::nativeGetDebugSummary, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "getLastError", &PapyrusBridge::nativeGetLastError, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "registerScreen", &PapyrusBridge::nativeRegisterScreen, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "screenActivated", &PapyrusBridge::nativeScreenActivated, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "screenAction", &PapyrusBridge::nativeScreenAction, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "screenStatus", &PapyrusBridge::nativeScreenStatus, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "setMessageName", &PapyrusBridge::nativeSetMessageName, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "screenUnloaded", &PapyrusBridge::nativeScreenUnloaded, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "screenRemoved", &PapyrusBridge::nativeScreenRemoved, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "musicMuteWanted", &PapyrusBridge::nativeMusicMuteWanted, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "soundSourceRange", &PapyrusBridge::nativeSoundSourceRange, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "log", &PapyrusBridge::nativeLog, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "remoteScreens", &PapyrusBridge::nativeRemoteScreens, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "remoteScreenId", &PapyrusBridge::nativeRemoteScreenId, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "remoteScreenLabel", &PapyrusBridge::nativeRemoteScreenLabel, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "remoteLabel", &PapyrusBridge::nativeRemoteLabel, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "remoteField", &PapyrusBridge::nativeRemoteField, std::nullopt, false);
    vm->BindNativeMethod(kScriptName, "remoteAction", &PapyrusBridge::nativeRemoteAction, std::nullopt, false);

    registered_ = true;
    logger_.info(std::string("Papyrus natives registered (script=") + kScriptName +
                 ", trigger=" + (reason ? reason : "unknown") + ").");
    return true;
}

bool PapyrusBridge::nativePluginAvailable(std::monostate, RE::TESObjectREFR*)
{
    return instance_ != nullptr && !instance_->shuttingDown_.load();
}

bool PapyrusBridge::nativeSetSurfaceNodeName(std::monostate, RE::TESObjectREFR* screenRef, std::string nodeName)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return false;
    }
    return instance_->screenManager_.setSurfaceNodeName(screenRef, std::move(nodeName));
}

std::string PapyrusBridge::nativeGetSurfaceNodeName(std::monostate, RE::TESObjectREFR* screenRef)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return {};
    }
    return instance_->screenManager_.getSurfaceNodeName(screenRef);
}

bool PapyrusBridge::nativeEnsureScreen(
    std::monostate,
    RE::TESObjectREFR* screenRef,
    const std::int32_t cols,
    const std::int32_t rows)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return false;
    }
    return instance_->screenManager_.ensureScreen(screenRef, cols, rows);
}

bool PapyrusBridge::nativeReset(std::monostate, RE::TESObjectREFR* screenRef)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return false;
    }
    return instance_->screenManager_.reset(screenRef);
}

bool PapyrusBridge::nativeClear(std::monostate, RE::TESObjectREFR* screenRef, const std::int32_t bgColor)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return false;
    }
    return instance_->screenManager_.clear(screenRef, bgColor);
}

bool PapyrusBridge::nativeWriteText(
    std::monostate,
    RE::TESObjectREFR* screenRef,
    const std::int32_t x,
    const std::int32_t y,
    std::string text,
    const std::int32_t fgColor,
    const std::int32_t bgColor)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return false;
    }
    return instance_->screenManager_.writeText(screenRef, x, y, std::move(text), fgColor, bgColor);
}

bool PapyrusBridge::nativeDrawLine(
    std::monostate,
    RE::TESObjectREFR* screenRef,
    const std::int32_t x0,
    const std::int32_t y0,
    const std::int32_t x1,
    const std::int32_t y1,
    const std::int32_t color)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return false;
    }
    return instance_->screenManager_.drawLine(screenRef, x0, y0, x1, y1, color);
}

bool PapyrusBridge::nativeDrawRect(
    std::monostate,
    RE::TESObjectREFR* screenRef,
    const std::int32_t x,
    const std::int32_t y,
    const std::int32_t width,
    const std::int32_t height,
    const std::int32_t color,
    const bool filled)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return false;
    }
    return instance_->screenManager_.drawRect(screenRef, x, y, width, height, color, filled);
}

bool PapyrusBridge::nativeDrawCircle(
    std::monostate,
    RE::TESObjectREFR* screenRef,
    const std::int32_t cx,
    const std::int32_t cy,
    const std::int32_t radius,
    const std::int32_t color,
    const bool filled)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return false;
    }
    return instance_->screenManager_.drawCircle(screenRef, cx, cy, radius, color, filled);
}

bool PapyrusBridge::nativeBlitImage(
    std::monostate,
    RE::TESObjectREFR* screenRef,
    std::string imageId,
    const std::int32_t x,
    const std::int32_t y,
    const std::int32_t width,
    const std::int32_t height,
    const std::int32_t tintColor)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return false;
    }
    return instance_->screenManager_.blitImage(screenRef, std::move(imageId), x, y, width, height, tintColor);
}

bool PapyrusBridge::nativePresent(std::monostate, RE::TESObjectREFR* screenRef)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return false;
    }
    return instance_->screenManager_.present(screenRef);
}

std::int32_t PapyrusBridge::nativeGetPendingCommandCount(std::monostate, RE::TESObjectREFR* screenRef)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return 0;
    }
    return instance_->screenManager_.getPendingCommandCount(screenRef);
}

std::string PapyrusBridge::nativeGetDebugSummary(std::monostate, RE::TESObjectREFR* screenRef)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return {};
    }
    return instance_->screenManager_.getDebugSummary(screenRef);
}

std::string PapyrusBridge::nativeGetLastError(std::monostate, RE::TESObjectREFR* screenRef)
{
    if (instance_ == nullptr || instance_->shuttingDown_.load()) {
        return {};
    }
    return instance_->screenManager_.getLastError(screenRef);
}

bool PapyrusBridge::nativeRegisterScreen(std::monostate, RE::TESObjectREFR* screenRef, std::int32_t channel, float x, float y,
                                         float z, float playerX, float playerY, float playerZ)
{
    if (!controller_ || !screenRef) {
        return false;
    }
    controller_->registerScreen(screenRef->GetFormID(), channel, x, y, z, playerX, playerY, playerZ,
                                ScreenLocation::reference(screenRef));
    return true;
}

std::string PapyrusBridge::nativeScreenActivated(std::monostate, RE::TESObjectREFR* screenRef)
{
    if (!controller_) {
        return "Video Screen: ScreenSFSE video is disabled";
    }
    if (!screenRef) {
        return {};
    }
    return controller_->action(screenRef->GetFormID(), ScreenController::PlayPause);
}

std::string PapyrusBridge::nativeScreenAction(std::monostate, RE::TESObjectREFR* screenRef, std::int32_t action)
{
    if (!controller_) {
        return "Video Screen: ScreenSFSE video is disabled";
    }
    if (!screenRef) {
        return {};
    }
    return controller_->action(screenRef->GetFormID(), action);
}

std::string PapyrusBridge::nativeScreenStatus(std::monostate, RE::TESObjectREFR* screenRef)
{
    if (!controller_) {
        return "VIDEO SCREEN (ScreenSFSE.dll video disabled)";
    }
    return controller_->status(screenRef ? screenRef->GetFormID() : 0);
}

// Renames a Message form (its FULL, the box title): the menu shows what is playing (RadioSFSE's setMessageName).
bool PapyrusBridge::nativeSetMessageName(std::monostate, RE::BGSMessage* message, std::string name)
{
    if (message == nullptr) {
        return false;
    }
    static_cast<RE::TESFullName*>(message)->fullName = std::string_view{ name };
    return true;
}

bool PapyrusBridge::nativeScreenUnloaded(std::monostate, RE::TESObjectREFR* screenRef)
{
    if (controller_ && screenRef) {
        controller_->unloaded(screenRef->GetFormID());
    }
    return controller_ != nullptr;
}

bool PapyrusBridge::nativeScreenRemoved(std::monostate, RE::TESObjectREFR* screenRef)
{
    if (controller_ && screenRef) {
        controller_->removed(screenRef->GetFormID());
    }
    return controller_ != nullptr;
}

bool PapyrusBridge::nativeMusicMuteWanted(std::monostate, float playerX, float playerY, float playerZ)
{
    return controller_ && controller_->musicMuteWanted(playerX, playerY, playerZ);
}

float PapyrusBridge::nativeSoundSourceRange(std::monostate, RE::TESObjectREFR* screenRef)
{
    return controller_ && screenRef ? controller_->soundSourceRange(screenRef->GetFormID()) : 0.0f;
}

// The Video Screen Remote. Screen IDs travel through Papyrus as Int (the form ID's bits).
std::int32_t PapyrusBridge::nativeRemoteScreens(std::monostate, float playerX, float playerY, float playerZ)
{
    return controller_ ? controller_->remoteScreens(playerX, playerY, playerZ) : 0;
}

std::int32_t PapyrusBridge::nativeRemoteScreenId(std::monostate, std::int32_t index)
{
    return controller_ ? static_cast<std::int32_t>(controller_->remoteScreenId(index)) : 0;
}

std::string PapyrusBridge::nativeRemoteScreenLabel(std::monostate, std::int32_t index)
{
    return controller_ ? controller_->remoteScreenLabel(index) : std::string();
}

std::string PapyrusBridge::nativeRemoteLabel(std::monostate, std::int32_t screenId, float playerX, float playerY, float playerZ)
{
    return controller_ ? controller_->remoteLabel(static_cast<std::uint32_t>(screenId), playerX, playerY, playerZ) : std::string();
}

std::string PapyrusBridge::nativeRemoteField(std::monostate, std::int32_t screenId, std::int32_t field)
{
    return controller_ ? controller_->remoteField(static_cast<std::uint32_t>(screenId), field) : std::string();
}

std::string PapyrusBridge::nativeRemoteAction(std::monostate, std::int32_t screenId, std::int32_t action)
{
    if (!controller_) {
        return "Video Screen: ScreenSFSE video is disabled";
    }
    return controller_->remoteAction(static_cast<std::uint32_t>(screenId), action);
}

// Script diagnostics into ScreenSFSE.log: Debug.Trace only reaches the Papyrus log when tracing is enabled in the INI.
bool PapyrusBridge::nativeLog(std::monostate, std::string text)
{
    if (instance_) {
        instance_->logger_.info("[script] " + text);
    }
    return instance_ != nullptr;
}
