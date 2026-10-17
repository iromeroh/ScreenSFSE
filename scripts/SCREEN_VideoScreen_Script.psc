Scriptname SCREEN_VideoScreen_Script extends ObjectReference
{ScreenSFSE video screen: registers its position with ScreenSFSE.dll and opens the control menu on activation
(the Message box keeps the screen visible behind it). No timers: the DLL reads the player position itself.}

Int Property Channel = 1 Auto Const   ; set per screen variant in ScreenSFSE.esm (wall 1-4, large 11-14)
Int Property MenuFormID = 0x803 AutoReadOnly   ; MESG ScreenSFSE_VideoMenu in ScreenSFSE.esm
Int Property ButtonClose = 9 AutoReadOnly

Event OnLoad()
    RegisterWithPlugin()
EndEvent

Event OnWorkshopObjectPlaced(ObjectReference akReference)
    RegisterWithPlugin()
EndEvent

Event OnWorkshopObjectMoved(ObjectReference akReference)
    RegisterWithPlugin()
EndEvent

Event OnUnload()
    SCREEN_SFSENative.screenUnloaded(Self)
EndEvent

Event OnWorkshopObjectRemoved(ObjectReference akReference)
    SCREEN_SFSENative.screenRemoved(Self)
EndEvent

Event OnActivate(ObjectReference akActionRef)
    If akActionRef != Game.GetPlayer()
        Return
    EndIf
    RegisterWithPlugin()
    Message menu = Game.GetFormFromFile(MenuFormID, "ScreenSFSE.esm") as Message
    If menu == None
        ; no menu record: plain play/pause
        Notify(SCREEN_SFSENative.screenAction(Self, 0))
        Return
    EndIf
    SCREEN_SFSENative.setMessageName(menu, SCREEN_SFSENative.screenStatus(Self))
    Int button = menu.Show()
    If button >= 0 && button < ButtonClose
        Notify(SCREEN_SFSENative.screenAction(Self, button))
    EndIf
EndEvent

Function Notify(String text)
    If text != ""
        Debug.Notification(text)
    EndIf
EndFunction

Function RegisterWithPlugin()
    Actor player = Game.GetPlayer()
    SCREEN_SFSENative.registerScreen(Self, Channel, GetPositionX(), GetPositionY(), GetPositionZ(), player.GetPositionX(), player.GetPositionY(), player.GetPositionZ())
EndFunction
