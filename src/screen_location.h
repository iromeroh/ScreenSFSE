#pragma once
#include <cstdint>

namespace RE { class TESObjectREFR; }

namespace ScreenLocation
{
// Exterior cells share a world; interiors (including ship cabins) are distinct cells.
struct Area
{
    std::uint32_t world{ 0 };
    std::uint32_t cell{ 0 };
    bool operator==(const Area&) const = default;
};
struct Context
{
    Area area;
    std::uint32_t visibleWorld{ 0 }; // surface seen from inside the currently occupied, landed ship
    bool operator==(const Context&) const = default;
};
Area reference(RE::TESObjectREFR* ref);
Context player();
}
