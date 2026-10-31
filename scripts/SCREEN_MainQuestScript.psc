Scriptname SCREEN_MainQuestScript extends Quest
{ScreenSFSE_MainQuest, start-game enabled: gives the player ScreenSFSE_ScreenPerk, the screens' second activation
"Playlists / Subtitles". Screens also give it when used (SCREEN_VideoScreen_Script.RegisterWithPlugin).
SyncMusic silences the game's music while a screen plays nearby, as STAR Radio's Mute Location Music: one music
override (vanilla _MUSExplore_WwiseSilence), added and removed in pairs; MusicMuted is saved with the game.}

Perk Property ScreenPerk Auto Const Mandatory
MusicType Property SilenceMusic Auto Const   ; _MUSExplore_WwiseSilence 0013E4E7, as STAR Radio's MusicSilenceOverride
Bool Property MusicMuted = False Auto Hidden
SpaceshipReference ObservedShip

; The Video Screen Remote: given once (also to saves from before it existed), opens the invisible remote terminal.
Weapon Property ScreenRemote Auto Const          ; WEAP ScreenSFSE_Remote 0100082D
Form Property RemoteTerminal Auto Const          ; TERM ScreenSFSE_RemoteTerminal 0100082E
ObjectReference Property HeldRemoteTerminal Auto Const   ; REFR ScreenSFSE_RemoteTerminalRef 01000850, its only reference
Bool Property RemoteGiven = False Auto Hidden
Int Property RemoteScreen = 0 Auto Hidden        ; the screen the remote controls (form ID), kept until changed
ObjectReference RemoteTerminalRef                ; a terminal placed at the player by earlier 0.3 builds

; Screens register with the DLL from their OnLoad, which the game does not run when a save loads in their own cell:
; RegisterNearbyScreens registers the loaded ones around the player (save load, cell load, opening the remote), so they
; resume and appear in the remote's list without being activated first.
Keyword Property ScreenKeyword Auto Const       ; KYWD ScreenSFSE_VideoScreenKeyword 01000810, on every screen
Float Property NearbyScreenRadius = 400.0 Auto Const

Function RegisterNearbyScreens()
    If ScreenKeyword == None
        Return
    EndIf
    ObjectReference[] screens = Game.GetPlayer().FindAllReferencesWithKeyword(ScreenKeyword, NearbyScreenRadius)
    Int i = 0
    While i < screens.Length
        SCREEN_VideoScreen_Script screen = screens[i] as SCREEN_VideoScreen_Script
        If screen && screen.Is3DLoaded() && !screen.IsDisabled()
            screen.RegisterWithPlugin()
        EndIf
        i += 1
    EndWhile
EndFunction

Event OnQuestInit()
    TrackPlayer()
    Actor player = Game.GetPlayer()
    If !player.HasPerk(ScreenPerk)
        player.AddPerk(ScreenPerk)
    EndIf
EndEvent

SCREEN_MainQuestScript Function Get() Global
    Return Game.GetFormFromFile(0x812, "ScreenSFSE.esm") as SCREEN_MainQuestScript
EndFunction

Function GiveRemote()
    If !RemoteGiven && ScreenRemote
        RemoteGiven = True
        Game.GetPlayer().AddItem(ScreenRemote, 1, False)
    EndIf
EndFunction

; Called by the remote (SCREEN_RemoteScript) when equipped: the menu opens at once, as All Gang's Here's control
; weapon (Aurie:tgah:referencealias:playeraliasscript.OnItemEquipped).
Function RequestRemote()
    If Game.IsMenuControlsEnabled() || Game.IsFavoritesControlsEnabled()
        OpenRemote()
    EndIf
EndFunction

; The remote's terminal is one persistent reference in ScreenSFSE_HoldingCell, activated from wherever the player is
; (All Gang's Here's TGAH_TerminalRef): nothing is placed, moved or left behind.
Function OpenRemote()
    RemoveRemoteTerminal()
    If HeldRemoteTerminal == None
        SCREEN_SFSENative.log("Remote: HeldRemoteTerminal is not set")
        Return
    EndIf
    RegisterNearbyScreens()
    HeldRemoteTerminal.Activate(Game.GetPlayer())
EndFunction

Function RemoveRemoteTerminal()
    If RemoteTerminalRef
        UnregisterForRemoteEvent(RemoteTerminalRef, "OnExitFurniture")
        RemoteTerminalRef.Disable()
        RemoteTerminalRef.Delete()
        RemoteTerminalRef = None
    EndIf
EndFunction


Event ObjectReference.OnExitFurniture(ObjectReference sender, ObjectReference akActionRef)
    If sender == RemoteTerminalRef
        RemoveRemoteTerminal()
    EndIf
EndEvent

; Playback area checks run in the DLL. These events keep the music override in step with travel, too.
Function TrackPlayer()
    GiveRemote()
    Actor player = Game.GetPlayer()
    RegisterForRemoteEvent(player, "OnLocationChange")
    RegisterForRemoteEvent(player, "OnCellLoad")
    RegisterForRemoteEvent(player, "OnPlayerLoadGame")
    SpaceshipReference ship = player.GetCurrentShipRef()
    If ship != ObservedShip
        If ObservedShip
            UnregisterForRemoteEvent(ObservedShip, "OnShipTakeOff")
            UnregisterForRemoteEvent(ObservedShip, "OnShipLanding")
            UnregisterForRemoteEvent(ObservedShip, "OnShipFarTravel")
            UnregisterForRemoteEvent(ObservedShip, "OnShipGravJump")
        EndIf
        ObservedShip = ship
        If ObservedShip
            RegisterForRemoteEvent(ObservedShip, "OnShipTakeOff")
            RegisterForRemoteEvent(ObservedShip, "OnShipLanding")
            RegisterForRemoteEvent(ObservedShip, "OnShipFarTravel")
            RegisterForRemoteEvent(ObservedShip, "OnShipGravJump")
        EndIf
    EndIf
EndFunction

Event Actor.OnLocationChange(Actor sender, Location oldLocation, Location newLocation)
    SyncMusic()
EndEvent

Event ObjectReference.OnCellLoad(ObjectReference sender)
    RegisterNearbyScreens()
    SyncMusic()
EndEvent

Event Actor.OnPlayerLoadGame(Actor sender)
    RemoveRemoteTerminal()   ; a placed terminal left in saves from earlier builds
    RegisterNearbyScreens()
    SyncMusic()
EndEvent

Event SpaceshipReference.OnShipTakeOff(SpaceshipReference sender, Bool complete)
    SyncMusic()
EndEvent

Event SpaceshipReference.OnShipLanding(SpaceshipReference sender, Bool complete)
    SyncMusic()
EndEvent

Event SpaceshipReference.OnShipFarTravel(SpaceshipReference sender, Location departure, Location arrival, Int eventState)
    SyncMusic()
EndEvent

Event SpaceshipReference.OnShipGravJump(SpaceshipReference sender, Location destination, Int eventState)
    SyncMusic()
EndEvent

; Called by the screens on their events (menu actions, load/unload, removal, the player crossing silent_meters).
Function SyncMusic()
    TrackPlayer()
    If SilenceMusic == None
        Return
    EndIf
    Actor player = Game.GetPlayer()
    Bool wanted = SCREEN_SFSENative.musicMuteWanted(player.GetPositionX(), player.GetPositionY(), player.GetPositionZ())
    If wanted && !MusicMuted
        SilenceMusic.Add()
        MusicMuted = True
    ElseIf !wanted && MusicMuted
        SilenceMusic.Remove()
        MusicMuted = False
    EndIf
EndFunction
