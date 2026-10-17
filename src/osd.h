#pragma once

#include <chrono>
#include <cstdint>
#include <string>

// On-screen display, like a TV's: a short notice drawn over the picture for a few seconds (play, pause, stop,
// next/previous, seek, volume, playlist, live). Classic TV look: green 5x7 bitmap capitals with a black shadow, top left.
struct Osd
{
    enum class Icon
    {
        None,
        Play,
        Pause,
        Stop,
        Next,
        Previous,
        Rewind,
        Forward,
        Volume
    };

    Icon icon{ Icon::None };
    std::string label;          // big line next to the icon ("PAUSE", "VOL 70")
    std::string detail;         // smaller second line (a title), may be empty
    float level{ -1.0f };       // 0..1: a segment bar (volume); < 0: none
    std::chrono::steady_clock::time_point until{};
};

// Draws the notice into an RGBA frame (DXGI R8G8B8A8 byte order) of width x height, rows `pitch` bytes apart.
void drawOsd(std::uint8_t* frame, std::uint32_t pitch, std::uint32_t width, std::uint32_t height, const Osd& osd);

// Upper-case ASCII for the bitmap font: Spanish/Latin accents to their base letter, anything else unknown -> '?'.
std::string osdText(const std::string& utf8, std::size_t maxChars);
