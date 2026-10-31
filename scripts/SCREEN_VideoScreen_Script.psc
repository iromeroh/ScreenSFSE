Scriptname SCREEN_VideoScreen_Script extends ObjectReference
{ScreenSFSE video screen: registers its position with ScreenSFSE.dll and opens the control menu on activation
(the Message box keeps the screen visible behind it). The second activation "Playlists / Subtitles" (perk
ScreenSFSE_ScreenPerk, SCREEN_ScreenPerkScript) opens OpenOptionsMenu. No timers: the DLL reads the player position
itself. The Video Billboard's base is the engine's auto-foundation (ScreenSFSE.esm), not this script.}

Int Property Channel = 1 Auto Const   ; set per screen variant in ScreenSFSE.esm (wall 1-4, large 11-14)
Int Property MenuFormID = 0x803 AutoReadOnly   ; MESG ScreenSFSE_VideoMenu in ScreenSFSE.esm
Int Property OptionsMenuFormID = 0x80E AutoReadOnly   ; MESG ScreenSFSE_PlaylistMenu
Int Property PerkFormID = 0x811 AutoReadOnly   ; PERK ScreenSFSE_ScreenPerk (the second activation)
Int Property ButtonStop = 7 AutoReadOnly   ; main menu since 0.3: 0-6 = actions 0-6, 7 = Stop (action 8), 8 = Close

; Earlier builds raised billboards from a script menu on placed metal columns. Placed columns could not be
; moved under the frame, and the native auto-foundation replaced them. Columns those builds left in a save are deleted
; here when their billboard loads, moves or is removed.
ObjectReference[] Supports

Event OnLoad()
    ClearSupport()
    RegisterWithPlugin()
    UpdateMusic()
EndEvent

Event OnWorkshopObjectPlaced(ObjectReference akReference)
    RegisterWithPlugin()
    UpdateMusic()
EndEvent

Event OnWorkshopObjectMoved(ObjectReference akReference)
    ClearSupport()
    RegisterWithPlugin()
    UpdateMusic()
EndEvent

Event OnUnload()
    SCREEN_SFSENative.screenUnloaded(Self)
    UpdateMusic()
EndEvent

Event OnWorkshopObjectRemoved(ObjectReference akReference)
    ClearSupport()
    SCREEN_SFSENative.screenRemoved(Self)
    UpdateMusic()
EndEvent

; the player crossed silent_meters around this screen while it is the sound source (registered in UpdateMusic)
Event OnDistanceLessThan(ObjectReference akObj1, ObjectReference akObj2, Float afDistance, Int aiEventID)
    UpdateMusic()
EndEvent

Event OnDistanceGreaterThan(ObjectReference akObj1, ObjectReference akObj2, Float afDistance, Int aiEventID)
    UpdateMusic()
EndEvent

Event OnActivate(ObjectReference akActionRef)
    If akActionRef != Game.GetPlayer()
        Return
    EndIf
    RegisterWithPlugin()
    SelectForRemote()
    Message menu = Game.GetFormFromFile(MenuFormID, "ScreenSFSE.esm") as Message
    If menu == None
        ; no menu record: plain play/pause
        Notify(SCREEN_SFSENative.screenAction(Self, 0))
        Return
    EndIf
    SCREEN_SFSENative.setMessageName(menu, SCREEN_SFSENative.screenStatus(Self))
    Int button = menu.Show()
    If button == ButtonStop
        Notify(SCREEN_SFSENative.screenAction(Self, 8))
    ElseIf button >= 0 && button < ButtonStop
        Notify(SCREEN_SFSENative.screenAction(Self, button))
    EndIf
    UpdateMusic()
EndEvent

; The second activation: Next Playlist / Refresh Playlists / Subtitles / Close.
Function OpenOptionsMenu()
    RegisterWithPlugin()
    SelectForRemote()
    Message options = Game.GetFormFromFile(OptionsMenuFormID, "ScreenSFSE.esm") as Message
    If options == None
        Return
    EndIf
    SCREEN_SFSENative.setMessageName(options, SCREEN_SFSENative.screenStatus(Self))
    Int selection = options.Show()
    If selection == 0
        Notify(SCREEN_SFSENative.screenAction(Self, 7))
    ElseIf selection == 1
        Notify(SCREEN_SFSENative.screenAction(Self, 9))
    ElseIf selection == 2
        ; Subtitles: off -> each subtitle track of the video -> off
        Notify(SCREEN_SFSENative.screenAction(Self, 10))
    EndIf
    UpdateMusic()
EndFunction

Function ClearSupport()
    If Supports == None
        Return
    EndIf
    Int i = 0
    While i < Supports.Length
        If Supports[i]
            Supports[i].Disable()
            Supports[i].Delete()
            Supports[i] = None
        EndIf
        i += 1
    EndWhile
    Supports = None
EndFunction

; The game's music: silenced while a screen plays within silent_meters (SCREEN_MainQuestScript.SyncMusic). While this
; screen is the sound source, the engine reports the player crossing that distance; no timers.
Function UpdateMusic()
    SCREEN_MainQuestScript main = SCREEN_MainQuestScript.Get()
    If main
        main.SyncMusic()
    EndIf
    Actor player = Game.GetPlayer()
    UnregisterForDistanceEvents(Self, player)
    Float range = SCREEN_SFSENative.soundSourceRange(Self)
    If range > 0.0
        If GetDistance(player) < range
            RegisterForDistanceGreaterThanEvent(Self, player, range)
        Else
            RegisterForDistanceLessThanEvent(Self, player, range)
        EndIf
    EndIf
EndFunction

Function Notify(String text)
    If text != ""
        Debug.Notification(text)
    EndIf
EndFunction

; The screen the player last activated (E or the second activation) is the one the Video Screen Remote controls.
Function SelectForRemote()
    SCREEN_MainQuestScript mainQuest = SCREEN_MainQuestScript.Get()
    If mainQuest
        mainQuest.RemoteScreen = GetFormID()
    EndIf
EndFunction

Function RegisterWithPlugin()
    Actor player = Game.GetPlayer()
    SCREEN_SFSENative.registerScreen(Self, Channel, GetPositionX(), GetPositionY(), GetPositionZ(), player.GetPositionX(), player.GetPositionY(), player.GetPositionZ())
    ; the second activation's perk, for saves where ScreenSFSE_MainQuest had started before 0.3 added it
    Perk screenPerk = Game.GetFormFromFile(PerkFormID, "ScreenSFSE.esm") as Perk
    If screenPerk && !player.HasPerk(screenPerk)
        player.AddPerk(screenPerk)
    EndIf
EndFunction
