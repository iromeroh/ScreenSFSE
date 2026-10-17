Scriptname SCREEN_ProgrammableScreen_Controller extends ObjectReference

String Property SurfaceNodeName = "ScreenSurface" Auto Const
Int Property Columns = 80 Auto Const
Int Property Rows = 25 Auto Const

; Signed 32-bit colors. These map to packed native 32-bit values.
Int Property BackgroundColor = 0 Auto Const
Int Property ForegroundColor = -1 Auto Const
Int Property AccentColor = -16711936 Auto Const

Event OnLoad()
    if !SCREEN_SFSENative.pluginAvailable(Self)
        Debug.Trace(Self + " ScreenSFSE not available")
        return
    endif

    SCREEN_SFSENative.setSurfaceNodeName(Self, SurfaceNodeName)
    SCREEN_SFSENative.ensureScreen(Self, Columns, Rows)
    SCREEN_SFSENative.clear(Self, BackgroundColor)
    SCREEN_SFSENative.drawRect(Self, 0, 0, Columns, Rows, AccentColor, False)
    SCREEN_SFSENative.writeText(Self, 2, 2, "SCREEN ONLINE", ForegroundColor, BackgroundColor)
    SCREEN_SFSENative.writeText(Self, 2, 4, "Awaiting renderer backend", AccentColor, BackgroundColor)
    SCREEN_SFSENative.present(Self)

    Debug.Trace(Self + " " + SCREEN_SFSENative.getDebugSummary(Self))
EndEvent
