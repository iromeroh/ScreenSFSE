Scriptname SCREEN_SFSENative Hidden Native


; Programmable screen SFSE scaffold.
; Colors are passed as packed 32-bit integers and interpreted natively.

Bool Function pluginAvailable(ObjectReference screenRef) Global Native

Bool Function setSurfaceNodeName(ObjectReference screenRef, String nodeName) Global Native
String Function getSurfaceNodeName(ObjectReference screenRef) Global Native

Bool Function ensureScreen(ObjectReference screenRef, Int cols = 80, Int rows = 25) Global Native
Bool Function reset(ObjectReference screenRef) Global Native

Bool Function clear(ObjectReference screenRef, Int bgColor = 0) Global Native
Bool Function writeText(ObjectReference screenRef, Int x, Int y, String text, Int fgColor = -1, Int bgColor = -1) Global Native
Bool Function drawLine(ObjectReference screenRef, Int x0, Int y0, Int x1, Int y1, Int color = -1) Global Native
Bool Function drawRect(ObjectReference screenRef, Int x, Int y, Int width, Int height, Int color = -1, Bool filled = False) Global Native
Bool Function drawCircle(ObjectReference screenRef, Int cx, Int cy, Int radius, Int color = -1, Bool filled = False) Global Native
Bool Function blitImage(ObjectReference screenRef, String imageId, Int x, Int y, Int width = 0, Int height = 0, Int tintColor = -1) Global Native

Bool Function present(ObjectReference screenRef) Global Native

Int Function getPendingCommandCount(ObjectReference screenRef) Global Native
String Function getDebugSummary(ObjectReference screenRef) Global Native
String Function getLastError(ObjectReference screenRef) Global Native

; ---- Video screens (ScreenSFSE.dll) ----
; Called by SCREEN_VideoScreen_Script. registerScreen also passes the player position, so the DLL can verify its own
; direct read of the player position (used for spatial sound) against Papyrus.
; channel: the screen variant's channel id (SCREEN_VideoScreen_Script.Channel; wall 1-4, large 11-14).
Bool Function registerScreen(ObjectReference screenRef, Int channel, Float x, Float y, Float z, Float playerX, Float playerY, Float playerZ) Global Native
; Play / pause / start the configured video. Returns a short message for the player ("" = none).
String Function screenActivated(ObjectReference screenRef) Global Native
Bool Function screenUnloaded(ObjectReference screenRef) Global Native
Bool Function screenRemoved(ObjectReference screenRef) Global Native
; Menu actions: 0 play/pause, 1 next video, 2 previous video, 3 back, 4 forward, 5 volume +, 6 volume -,
; 7 next playlist, 8 stop, 9 refresh playlists. Returns a short message for the player ("" = none).
String Function screenAction(ObjectReference screenRef, Int action) Global Native
; One line: state, video, time, volume, playlist (the menu title).
String Function screenStatus(ObjectReference screenRef) Global Native
; Renames a Message (its title), as RadioSFSENative.setMessageName.
Bool Function setMessageName(Message msg, String name) Global Native
; Game music: whether it should be silenced now (a screen plays or loads within silent_meters of the player;
; mute_game_music in ScreenSFSE.ini). SCREEN_MainQuestScript.SyncMusic keeps the silence override in step.
Bool Function musicMuteWanted(Float playerX, Float playerY, Float playerZ) Global Native
; silent_meters when this screen is the sound source of a playing or loading channel, else 0.
Float Function soundSourceRange(ObjectReference screenRef) Global Native
; A line in ScreenSFSE.log (script diagnostics; Debug.Trace needs Papyrus tracing enabled in the game's INI).
Bool Function log(String text) Global Native

; ---- The Video Screen Remote (SCREEN_RemoteMenuScript) ----
; Screen IDs are form IDs as Int. remoteScreens snapshots the loaded screens in the player's area, nearest first (at
; most 12) and returns their count; remoteScreenId / remoteScreenLabel read that snapshot ("<model> - CH n - <m> - <state>").
Int Function remoteScreens(Float playerX, Float playerY, Float playerZ) Global Native
Int Function remoteScreenId(Int index) Global Native
String Function remoteScreenLabel(Int index) Global Native
; The label of one screen; "" when it is not in the player's area.
String Function remoteLabel(Int screenId, Float playerX, Float playerY, Float playerZ) Global Native
; A dashboard field of a screen in the player's area ("" otherwise): 1 state, 2 video, 3 time, 4 volume, 5 playlist,
; 6 subtitles.
String Function remoteField(Int screenId, Int field) Global Native
; screenAction on a screen in the player's area (its notices stay up twice as long); a message otherwise.
String Function remoteAction(Int screenId, Int action) Global Native
