Scriptname SCREEN_ScreenPerkScript extends Perk Hidden Const
{Fragment script of ScreenSFSE_ScreenPerk (tools/build_plugin.py). Entry index 0 = the screens' second activation
"Playlists / Subtitles"; the menu itself is SCREEN_VideoScreen_Script.OpenOptionsMenu.}

Function Fragment_Entry_00(ObjectReference akTargetRef, Actor akActor)
    SCREEN_VideoScreen_Script screen = akTargetRef as SCREEN_VideoScreen_Script
    If screen
        screen.OpenOptionsMenu()
    EndIf
EndFunction
