#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class FrameSource;
struct Osd;

class Logger;

// Settings for the D3D12 screen surface (ScreenSFSE.ini [Surface] / [Render]).
struct SurfaceConfig
{
    bool enabled{ true };
    // Screen channels: each placeholder texture is recognised by its exact size (one mip, one array slice) and an
    // RGBA8 format. Default: src/screen_channels.h (generated from config/screen.json).
    struct Channel
    {
        int id;
        std::uint32_t width;
        std::uint32_t height;
    };
    std::vector<Channel> channels;
    std::vector<std::uint32_t> formats{ 27, 28, 29 };   // R8G8B8A8 TYPELESS / UNORM / UNORM_SRGB
    double fps{ 30.0 };
    // Create the matched texture with ALLOW_SIMULTANEOUS_ACCESS so it decays to COMMON after every submission and
    // our copy needs no state transitions. If that is off or not possible, the copy is wrapped in transitions from
    // and back to assumedState.
    bool simultaneousAccess{ true };
    std::uint32_t assumedState{ 0xC0 };   // PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE
    // Diagnostics: log every 2D texture created with the configured width or height (any format/mips).
    bool logNearMisses{ true };
};

// Installs the device/queue vtable hooks. Safe to call once, at plugin load.
bool installVideoSurface(Logger& logger, const SurfaceConfig& config);

// What a channel's screen textures show. Swapped at any time (the old source is released outside the upload lock).
// A channel without a source is not written to (its textures keep their baked "NO SIGNAL" card).
void setFrameSource(int channel, std::shared_ptr<FrameSource> source);

// Whether a texture of the channel currently exists (one of its screens is loaded).
bool surfaceLive(int channel);

// Shows a TV-style notice (osd.h) over the channel's picture until osd.until.
void showOsd(int channel, const Osd& osd);
