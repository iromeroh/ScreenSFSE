Scriptname SCREEN_RemoteScript extends ObjectReference
{The Video Screen Remote (WEAP ScreenSFSE_Remote 0100082D), a weapon so it can be favourited to a hotkey. Equipping it
unequips it again and opens the remote terminal menu (SCREEN_MainQuestScript.RequestRemote), as All Gang's Here's
control weapon.}

Event OnEquipped(Actor akActor)
    If akActor != Game.GetPlayer()
        Return
    EndIf
    SCREEN_MainQuestScript mainQuest = SCREEN_MainQuestScript.Get()
    If mainQuest
        mainQuest.RequestRemote()
    EndIf
    akActor.UnequipItem(GetBaseObject(), False, True)
EndEvent
