#pragma once

class Logger;
class ScreenController;

// Per-save screen state. SFSE 0.2.21 never sends its PreSaveGame/PostLoadGame messages, so the game's own events are
// used: SaveLoadEvent (saves, and the start of a load) and TESLoadGameEvent (load finished). Each save gets a random
// token in the GLOB ScreenSFSE_SaveToken (saved with the game) and a snapshot file of that name written by the
// ScreenController; loading reads the token back and applies that snapshot. No timers: work happens only on save/load.
namespace SaveSync
{
// After the game data has loaded (the GLOB and the event sources exist). Safe to call more than once.
void install(Logger& logger, ScreenController& controller);
}
