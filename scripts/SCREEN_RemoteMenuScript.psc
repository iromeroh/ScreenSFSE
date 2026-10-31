Scriptname SCREEN_RemoteMenuScript extends TerminalMenu
{The Video Screen Remote's terminal menu (TMLM ScreenSFSE_RemoteMenu 0100082F), opened by the remote from any distance.
Its screen (SCREEN_MainQuestScript.RemoteScreen) is the one the player last activated, or chose with "Next screen"
(the screens in the player's area, nearest first); while that one is not in the player's area (the DLL's playback
area), the nearest is used.
The dashboard: tokens D0-D6 (screen, state, video, time, volume, playlist, subtitles) bound with
AddTextReplacementData to placeholder Messages renamed by setMessageName (MESG 01000830-836), as STAR Radio's tuner
panel, and rebound when a menu opens and after every action.
The menu is laid out as STAR Radio's tuner (build_plugin.py REMOTE_ROWS): the dashboard is the body text, above fixed
rows (the remote's keys, in the order on the remote NIF, tools/build_remote.py, and Next screen). Each row opens its
own result menu (TMLM ScreenSFSE_RemoteDone0-11, this script too): what was done above the dashboard, Refresh and
Back to remote. Item IDs: 100 + ScreenController action, 900 Next screen; the result menus' rows (0, 1) only navigate.}

Int ItemAction = 100
Int ItemNextScreen = 900
Int FirstToken = 0x830

Function Bind(ObjectReference akTerminalRef, Int index, String tokenName, String text)
    Message placeholder = Game.GetFormFromFile(FirstToken + index, "ScreenSFSE.esm") as Message
    If placeholder
        SCREEN_SFSENative.setMessageName(placeholder, text)
        akTerminalRef.AddTextReplacementData(tokenName, placeholder)
    EndIf
EndFunction

String Function SelectedLabel()
    Int screen = SCREEN_MainQuestScript.Get().RemoteScreen
    If screen == 0
        Return ""
    EndIf
    Actor player = Game.GetPlayer()
    Return SCREEN_SFSENative.remoteLabel(screen, player.GetPositionX(), player.GetPositionY(), player.GetPositionZ())
EndFunction

; The dashboard: the selected screen's fields, or a hint in D0 and dashes.
Function BindDashboard(ObjectReference akTerminalRef, String label)
    Int screen = SCREEN_MainQuestScript.Get().RemoteScreen
    Bind(akTerminalRef, 0, "D0", label)
    Int field = 1
    While field <= 6
        String value = "-"
        If screen != 0
            value = SCREEN_SFSENative.remoteField(screen, field)
            If value == ""
                value = "-"
            EndIf
        EndIf
        Bind(akTerminalRef, field, "D" + field, value)
        field += 1
    EndWhile
EndFunction

; The remote's screen in the player's area: the chosen one, or the nearest (then remembered). 0 if there is none.
Int Function CurrentScreen()
    SCREEN_MainQuestScript mainQuest = SCREEN_MainQuestScript.Get()
    If mainQuest.RemoteScreen != 0 && SelectedLabel() != ""
        Return mainQuest.RemoteScreen
    EndIf
    Actor player = Game.GetPlayer()
    If SCREEN_SFSENative.remoteScreens(player.GetPositionX(), player.GetPositionY(), player.GetPositionZ()) > 0
        mainQuest.RemoteScreen = SCREEN_SFSENative.remoteScreenId(0)
        Return mainQuest.RemoteScreen
    EndIf
    Return 0
EndFunction

; Next screen: the one after the current in the area's list (nearest first), wrapping around.
Function NextScreen()
    SCREEN_MainQuestScript mainQuest = SCREEN_MainQuestScript.Get()
    Actor player = Game.GetPlayer()
    Int count = SCREEN_SFSENative.remoteScreens(player.GetPositionX(), player.GetPositionY(), player.GetPositionZ())
    If count == 0
        Return
    EndIf
    Int i = 0
    While i < count && SCREEN_SFSENative.remoteScreenId(i) != mainQuest.RemoteScreen
        i += 1
    EndWhile
    mainQuest.RemoteScreen = SCREEN_SFSENative.remoteScreenId((i + 1) % count)
EndFunction

; Binds the dashboard for the remote's screen, or its hint when no screen is in the area.
Function ShowDashboard(ObjectReference akTerminalRef)
    If CurrentScreen() == 0
        BindDashboard(akTerminalRef, "No video screens in this area")
    Else
        BindDashboard(akTerminalRef, SelectedLabel())
    EndIf
EndFunction

Event OnTerminalMenuEnter(TerminalMenu akTerminalBase, ObjectReference akTerminalRef)
    ShowDashboard(akTerminalRef)
EndEvent

Event OnTerminalMenuItemRun(Int auiMenuItemID, TerminalMenu akTerminalBase, ObjectReference akTerminalRef)
    SCREEN_MainQuestScript mainQuest = SCREEN_MainQuestScript.Get()
    If auiMenuItemID >= ItemAction && auiMenuItemID <= ItemAction + 10
        SCREEN_SFSENative.remoteAction(CurrentScreen(), auiMenuItemID - ItemAction)
        mainQuest.SyncMusic()
    ElseIf auiMenuItemID == ItemNextScreen
        NextScreen()
    Else
        Return
    EndIf
    ShowDashboard(akTerminalRef)
EndEvent
