#include "SFSE/Impl/PCH.h"
#include "screen_location.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/T/TESWorldSpace.h"
#include "RE/IDs.h"
#include <windows.h>

namespace
{
ScreenLocation::Area areaOf(RE::TESObjectREFR* ref)
{
    auto* cell = ref ? ref->parentCell : nullptr;
    if (!cell) return {};
    auto* world = cell->IsInterior() ? nullptr : cell->cellWorldspace;
    return { world ? world->GetFormID() : 0, cell->GetFormID() };
}

ScreenLocation::Context playerContext()
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    ScreenLocation::Context context{ areaOf(player) };
    if (!player || !context.area.cell || context.area.world) return context;
    // Use only resolved CommonLib address IDs; IsSpaceshipLanded/GetParentWorldSpace have no ID in this version.
    using Ship = RE::TESObjectREFR* (*)(RE::TESObjectREFR*, bool);
    using InSpace = bool (*)(RE::TESObjectREFR*, bool);
    static REL::Relocation<Ship> getShip{ RE::ID::TESObjectREFR::GetSpaceship };
    static REL::Relocation<InSpace> inSpace{ RE::ID::TESObjectREFR::IsInSpace };
    auto* ship = getShip(player, true);
    if (ship && !inSpace(ship, true)) {
        // A surface world on the current ship's exterior reference permits the view through its windows.
        // In orbit this allowance is withdrawn even if the cabin and exterior textures remain resident.
        context.visibleWorld = areaOf(ship).world;
    }
    return context;
}
}

ScreenLocation::Area ScreenLocation::reference(RE::TESObjectREFR* ref)
{
    __try { return areaOf(ref); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
}

ScreenLocation::Context ScreenLocation::player()
{
    __try { return playerContext(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
}
