# proton-wine PROGRESS_LOG (branches `proton_11.14.1-DroidDeck`, `proton_11.14-GE` and their staging branches)

Newest entry at the top. The main log lives on `proton_11.0-2`; this file covers the GE-Proton 11.0-14 layer and, on
`proton_11.14.1-DroidDeck`, the DroidDeck-Proton 11.0-14.1 layer built on it.

## 2026-11-28 — win10 round 5: win10.msstyles (Light + Dark) + start-up theme fix (branch, CI running, NOT device-tested)
- 23:5x EST 11-27 user tested round 4: caption hover DEVICE-PROVEN (FIT screenshots: close 0xe81123, minimize 0x3a3a3a,
  neighbour unlit), label fits. User Q "I can set the theme dark in the container settings though?": BL
  WineThemeManager (DARK) writes Control Panel\Colors only (ButtonFace 33, Window 48, text white), never the msstyles.
  Found bug: systray.c start-up apply_xp_controls( xp_style && xp_controls ) re-applied winexp (SilverDark when dark) in
  the win10 style every container start. User picked option 1: a Windows 10 msstyles, light + dark.
- Built 023a2c41:
  - in tree: dlls/win10.msstyles (genimages.py = winexp's framework + flat Windows 10 artwork; 94 bitmaps, win10.rc;
    ColorNames Light/Dark, FILERESNAMES LIGHT_INI/DARK_INI), configure.ac WINE_CONFIG_MAKEFILE, loader/wine.inf.in
    10,resources\themes\win10,win10.msstyles. Dark palette matched to BL dark colours (dialog 0x212121, fields 0x303030).
  - explorer patch: apply_xp_controls( use, win10, scheme ): win10 style always applies win10.msstyles, ColorName Dark when
    is_dark_theme() else Light; switching winexp<->win10 rewrites DllName+ColorName; start-up call fixed; leaving
    win10/xp restores the saved theme as before.
  - Preview sheet checked (scratchpad win10-sheet.png). systray.c MinGW syntax clean.
- CI 46110049864 (configure.ac changed -> wine-tools cache miss, longer build).
- Device test needs win10.msstyles copied to C:\windows\resources\themes\win10\ (wineboot -u only runs when wine.inf
  changes; a layer update would install it).

## 2026-11-27 — win10 round 4: label fix + caption button hover (branch, CI running, NOT device-tested)
- 23:2x EST user tested round 3 (dark title bars, border, tiles, Control Panel icon, picker button): works.
  User switched to a dark theme (registry: ThemeManager DllName winexp.msstyles, ColorName SilverDark, set after the
  last Display Properties apply) -> dialogs dark, dark wallpaper.
- eca1929d: win10 wallpaper label shortened to "Wine Windows 10 desktop &background" (was clipped; XP label unchanged),
  user "fix it". CI 46107920242.
- 7bc6949f (user 23:3x "window title bar buttons don't get highlighted like start menu options"): win10 caption buttons
  hover: min/max light grey (dark 0x3a3a3a / light 0xe5e5e5), close red 0xe81123, pressed lighter (close 0xf1707a);
  WM_NCMOUSEMOVE lights, SYSTEM_TIMER_CAPTION_HOT (0xfff9, 100 ms, dispatched in NtUserDispatchMessage) puts it out
  when the pointer leaves the button/window (no TrackMouseEvent, so programs' own tracking is untouched); grayed
  buttons stay unlit. Files: defwnd.c, message.c, ntuser_private.h, win32u_private.h (all in the frames patch; none of
  the 15 later patches touch them). Not compiled locally (unix side). CI 46108531492.
- User 23:35 "why dont other windows also follow the dark theme correctly" (Pale Moon): answered = theme file in use is
  winexp SilverDark (XP parts, some light), win10 has no msstyles of its own (reuses light.msstyles, no dark variant),
  Pale Moon/Steam/CEF apps draw their own title bars, web pages are the site's. Offered: win10.msstyles light+dark
  (big job). Awaiting user.

## 2026-11-27 — win10 round 3: accent color picker, dark grey default, dark title bars (branch, CI running, NOT device-tested)
- Why: user 22:54 EST screenshot of round 2 (2/3 height, tiles, hover scroll bar, cog, bold glass all seen working);
  "are the tiles supposed to be gray?" (accent came from Scheme 2 = slate); user 22:5x "B, I feel like a dark gray to
  match the start bar would fit as a default"; "even the windows should be a dark gray to match the start menu and bar
  right?"; "this is all just for this theme right? doesn't effect classic or xp?" (answer: yes, win10 only).
- Built ef757bea (both patches):
  - explorer: Taskbar "Accent" (REG_DWORD COLORREF, default RGB(0x33,0x33,0x33) = pressed start button grey) and
    "Dark" (default 1). Display Properties in win10: "Accent color" owner-draw swatch button replaces the scheme combo,
    opens IDD_ACCENT_COLORS (8x7 grid: 48 Windows 10 accents + 8 greys, default first; tap selects, double-tap/OK
    applies); "Dark title bars" checkbox replaces the disabled controls box. Preview uses accent + dark. Task underline
    tint 4/10 (dark accents show). Desktop selection box fixed Windows 10 blue 0x0078d7 (not the accent). Control Panel
    tile icon = shell32 IDI_SHELL_CONTROL_PANEL_ID (as the XP menu).
  - win32u defwnd.c: win10_frames[2][2] (light/dark x active/inactive), active border = Accent, Dark=1 -> dark title
    bars (0x202020) whatever the window colours; XP/classic paths unchanged.
  - Local: MinGW syntax clean for startmenu.c + systray.c (desktop.c only the old Wine-only errors); defwnd.c CI only.
- CI: run 46105812070 (headSha ef757bea asserted).
- Separate (Bannerlator, not Wine): user "can we stop having the wfm open on container launch?" -> BL starts
  `winhandler.exe "wfm.exe"` (XServerDisplayActivity ~16769) and winhandler.c defaults to wfm.exe with no args;
  Wine rundll32.exe with no args exits silently (GUI, no window) -> one-line BL change possible; proposal to user.

## 2026-11-27 — win10 round 2: Start menu fixes + tiles + bold start glass (branch, CI green, staged, NOT device-tested)
- Why: user 22:25 EST screenshot "start menu looking off"; I listed 5 differences from Windows 10 (too tall, hard grey
  scroll bar, Settings icon a sun, cramped top/small text, no tiles); user 22:2x "all 5, same branch, one new build".
  Bold start glass (proposed earlier, held until the pointer was sorted) folded into the same build.
- Pointer flicker: user 22:25 "cursor works now". I changed NOTHING for it (checked 22:26 EST: FIT win32u a4b08af9 +
  explorer bbd03b35 unchanged, container 1 has no +waylanddrv, BL lastUpdate 20:48). Flicker = intermittent, cause
  still unknown; the +waylanddrv trace plan (container 1 env, backup first) stays ready if it comes back.
- Built 6dc03bc4 (programs_explorer_win10_theme.patch only; defwnd patch unchanged):
  - startmenu.c: height = min(tray top, max(360, screen*2/3)); scroll bar 2 px, colour 0x6a6a6a, only while the pointer
    is over the list or a drag scrolls; Settings = 8-tooth cog polyline + hole (math.h); first heading 10 px down; list
    font 13 px; tiles: Wine group (file manager, Control Panel, Wine config, Command Prompt, Task Manager) + Desktop
    group (*.lnk of user + common desktop, max 12 tiles), 96 px squares 3 per row on the accent colour, hot = light
    border, click launches (W10_LAUNCH); menu width 320 + 316 with tiles. New strings IDS_W10_TILES_WINE 65 /
    IDS_W10_TILES_DESKTOP 66.
  - systray.c: render_wine_glass( size, bold ): bold = 5.6 pen + solid wine; start button glyph 22 px bold; Display
    Properties preview bold; wallpaper glass unchanged.
  - Local check: MinGW -fsyntax-only clean for startmenu.c + systray.c (only the old GetDpiForWindow header warning);
    cog rendered in Python = reads as a cog.
- CI: run 46103493264 (headSha 6dc03bc4 asserted), same branch = warm cache.
- 22:46 EST CI GREEN (both legs, 5/5 markers, verify-layer PASS x2, explorer recompiled, no warnings); win32u.so in the
  artifact = a4b08af9 (unchanged). Staged /sdcard/Download/.tmp-x/explorer-win10-6dc03bc4.exe (md5 07b74e3c, host == device);
  swap waits for the user's OK (FIT screen held by another session meanwhile).
- Next: CI green -> stage explorer.exe on the FIT, swap (win10-swap.sh, explorer only; win32u unchanged) with the user's
  OK when the container is closed; user tests start menu (tiles, scroll bar, cog), then the rest of the test list.
- Resume: worktree /root/src/proton-wine-wt-win10 (branch staging/proton_11.14.1-DroidDeck/win10 6dc03bc4), sim tree
  /root/src/sim-win10 (working tree = all win10 changes vs baseline).

## 2026-11-27 — staging/proton_11.14.1-DroidDeck/win10: Windows 10 style ("win10") for the Wine desktop (branch, CI green, on the FIT for testing)
- Why: user 19:2x EST "could we also do a windows 10 theme as well?"; HTML preview /sdcard/Download/
  Wine-Windows10-Style-Preview.html (20:31 EST); user 20:4x "do it all on a new branch from the layer, win10 we will call
  the theme", then "finish it entirely and only then give it to me to test".
- Built 45f5b3a4 (branch off parent b53c266f, which has desktop-select): two patches after
  programs_explorer_desktop.c_icon_selection.patch in both build steps + 5 markers:
  - programs_explorer_win10_theme.patch (systray.c, startmenu.c, desktop.c, explorer.rc, resource.h, explorer_private.h):
    win10_style on top of xp_style (Style "win10" / WINE_TASKBAR_STYLE=win10); flat WIN10_BAR #101010 taskbar, 48 px
    start button with a vector Wine glass (render_wine_glass: GDI at 4x, averaged; no Microsoft art), search button ->
    Run (do_run_dialog), icon only task buttons (big icon 24 px, accent underline, tooltips via TTN_GETDISPINFOW), clock
    time + date, Show desktop strip (minimize / restore by PostMessage SC_*), accent per scheme blue #0078d7 / green
    #498205 / slate #68768a; do_win10_startmenu (rail: expand/user/Documents/Pictures/Settings->Display Properties/
    Power->Shut down; A-Z list of both start menus with letter headings, folders inline, Wine System folder; wheel/drag/
    keys); build_win10_wallpaper (radial glow + 4 beams + glass, darker in dark mode; app default wallpaper only); desktop
    icons: light box + shadowed titles, accent band; Display Properties: third style "Windows 10 style", labels switch,
    controls checkbox off for win10 (flat default Wine theme), win10 preview.
  - dlls_win32u_defwnd.c_win10_frames.patch: win10_frame_schemes (white / dark title bars, 1 px accent border, inactive
    gray), win10_draw_caption_button (flat, thin glyphs, close red when pressed), regular caption font weight.
- Checks: sims arm64ec 116/116, x86_64 101/101, 5/5 markers; systray.c + startmenu.c compile clean (clang -Wall, mingw
  headers + Wine shim), desktop.c clean in the new code; defwnd.c not compiled locally (CI). CI 46096484037 (45f5b3a4)
  started 21:07 EST -> GREEN both legs 21:33 EST (both patches applied, 5/5 markers ok, verify-layer PASS, no compiler
  output for the changed files).
- 21:34 EST SWAPPED on the FIT (container 1, layer 11.0-14.1-arm64ec-18): explorer.exe md5 bbd03b35 (layer + C:\windows +
  system32) and lib/wine/aarch64-unix/win32u.so md5 a4b08af9 (only file that changes: the PE win32u.dll differs by its
  4-byte timestamp; win32u.so is reproducible, previous build == installed 477deb4d). Backups: win32u.so.orig 477deb4d in
  /sdcard/Download/win10-backup/, explorer originals 9b995bcd in desk-select-backup/. Undo: rootsh 'sh
  /sdcard/Download/.tmp-x/win10-undo.sh' (Wine stopped). Device test: pending (user switches to "Windows 10 style").
- Test plan: swap explorer.exe (64-bit) + win32u (aarch64-unix win32u.so, aarch64-windows win32u.dll if changed) into the
  FIT 14.1 layer (backups), user picks "Windows 10 style" in Display Properties.

## 2026-11-27 — staging/proton_11.14.1-DroidDeck/desktop-select MERGED into proton_11.14.1-DroidDeck (--no-ff)
- Why: user 19:0x EST "merge but wait to copy to other layers still" (all four parts device-proven on the FIT).
- Merged 15003425 (branch head 5bd4f31b) as merge commit 6d9a34fd on parent 6d899c3b; only conflict PROGRESS_LOG.md (both
  entries kept); every other file == the tested build 15003425 (git diff empty). Backup ref
  refs/backup/20261127/proton_11.14.1-DroidDeck-before-desktop-select = 6d899c3b.
- Contents: programs_explorer_desktop.c_icon_selection.patch (desktop.c, explorer.rc, resource.h) + 4 markers in both build
  steps: click / Ctrl+click selection, selection box, icon right-click menu (Open, Cut, Copy, Create Link, Delete, Rename,
  Properties), combined Properties for several icons. Release-note line (own work, The412Banner): "Wine XP desktop: icons can
  be selected (click, Ctrl+click, drag a box), right-click menu with Open / Cut / Copy / Create Link / Delete / Rename /
  Properties, one Properties window for several icons."
- NOT copied to the other 5 parents (user: wait). Not released. Parent CI 46087088150 (headSha a0e2578a) GREEN both legs
  19:25 EST: patch applied, 4 markers ok, verify-layer PASS; release skipped.

## 2026-11-27 — NEXT RELEASE: credits for the launcher fixes (user order, 18:5x EST)
- Why: the user asked that the next release that ships the 11-27 launcher work (EA app, Ubisoft Connect, Epic, GOG
  Galaxy, Vortex) credits the code it builds on. Not released yet; this is the list to copy into those notes (and into
  DroidDeck-Proton / DroidDeck / Bannerlator notes if they announce these fixes).
- Release-note lines, ready to paste:
  - **GameNative fixes these build on** ([GameNative/proton-wine](https://github.com/GameNative/proton-wine),
    LGPL-2.1, credit in each patch):
    - #122 GPU drawing for launchers that draw from a second process, now **on by default**; our Wayland change shows
      those frames in the window (EA app, Steam client) — Dmage22
    - #133 system call writes into emulator-protected code, extended to writable sections of the game's own exe
      (Need for Speed Heat) — Utkarsh Dalal
    - #107 icu checks its backends, completed for icuuc/icuin so .NET 9 falls back to NLS (Vortex) — butilly
      (GameNative AI help)
  - **Upstream Wine**: ncrypt ECDSA keys, refcounted providers, import/encrypt fixes — Hans Leidekker, Piotr Caban;
    persisted keys for the Epic Online Services sign-in key added by The412Banner
  - Own work (The412Banner): V8 sandbox address-space report (CEF/Electron), Wayland other-process frames, GOG Galaxy
    desktop + QtWebEngine software path, Electron main-process report (Vortex), wine_get_* hidden from EA's
    Steam/Epic launchers, fixed MAC for Android random MACs.
- Not used (no credit owed, do not list as sources): GameNative #32 (RDR2 Social Club V8, open), #141 (Ubisoft
  adapter MAC, open).
## 2026-11-27 — staging/proton_11.14.1-DroidDeck/desktop-select: desktop icon selection, selection box, icon menu, combined Properties (branch, DEVICE-PROVEN, not merged)
- Why: user 15:2x EST: a click on a desktop icon (XP desktop) gives no highlight like Windows. User picked the basic
  version + right-click selects; LATER (user wish): a click-and-drag selection box shown until the button is released.
- Built bec8724f patch programs_explorer_desktop.c_icon_selection.patch (after programs_explorer_desktop.c.patch in
  both build steps + marker "desktop icons selected by a click"): left/right button down on an icon selects it alone
  (icon ImageList_DrawEx ILD_BLEND50 with COLOR_HIGHLIGHT, title on a COLOR_HIGHLIGHT box in COLOR_HIGHLIGHTTEXT,
  DrawFocusRect on the last clicked one); empty spot clears; right click on a selected icon keeps it; double click
  opens as before; messages still go on to the original desktop proc. Selection is a flag per launcher (ready for a
  drag box / several selected); focus pointer cleared in free_launcher (folder watcher thread).
- Checks: sims arm64ec 114/114, x86_64 99/99 (desktop.c == edited, marker found); new functions compile clean
  (clang -fsyntax-only, x86_64 + aarch64 mingw headers, extracted).
- CI 46063847659 (headSha 681cc0ce) GREEN both legs 15:54 EST: patch applied, marker ok, verify-layer PASS, no
  desktop.c compiler output. New aarch64-windows explorer.exe md5 95dedd62 (imports ImageList_Create/DrawEx/
  ReplaceIcon/Destroy; old 9b995bcd has none), staged /sdcard/Download/.tmp-x/explorer-desk-681cc0ce.exe (sha256
  d3aea694 host == device).
- 15:57 EST SWAPPED on the FIT (root-bf OK): 64-bit explorer.exe in layer 11.0-14.1-arm64ec-18 (= CI fa021603, parent
  tree) + container 1 C:\windows + system32; originals in /sdcard/Download/desk-select-backup/; undo =
  rootsh 'sh /sdcard/Download/.tmp-x/desk-undo.sh' (Wine stopped). Device test: pending (user opens container 1).
- Plan: CI -> swap only explorer.exe into the FIT 14.1 layer + container prefix (backups) -> test click/empty/right
  click/double click on X11 + Wayland -> then the other 5 parents; release = user word.
- 16:01 EST USER TEST of 681cc0ce (FIT, Wayland, container 1): clicked GOG GALAXY -> blue title box, white text,
  dotted focus outline = DEVICE-PROVEN; icon tint not confirmed from the screenshot; logcat 15:57-16:02 0 FATAL/crash.
- User: "can't tap and hold, then drag with a second finger to select multiple" -> selection box built in acb21caa
  (same patch file): press on an empty spot + move past SM_CXDRAG = XP band (COLOR_HIGHLIGHT, GdiAlphaBlend 0x50 fill +
  FrameRect), selects launchers whose icon/title rect it touches, Ctrl inverts; Ctrl+click toggles; click on one of
  several selected = selects it alone at button up; double-click on empty = press (tap then drag); capture released on
  WM_CAPTURECHANGED. BL main 4e42eda9f TouchpadView: Box Select (Cursor to Touch) and the two-finger wide hold both send
  LMB down/move/up, so no BL change. Markers: mouse_down + update_band.
- CI 46068123426 (acb21caa) GREEN both legs 16:15 EST (patch applied, 2 markers ok, verify-layer PASS). explorer.exe md5
  bddc40b2, staged /sdcard/Download/.tmp-x/explorer-desk-acb21caa.exe (sha256 2da4a6a2 host == device). Swap waits for
  the container to close (wineserver running).
- 16:2x-16:53 EST USER TESTS of acb21caa (swapped, md5 bddc40b2; videos screen-20261127-164957 / -165308): selection box
  "works right" = DEVICE-PROVEN (EA/GOG/Vortex boxed, icon tint visible); clicks move the highlight icon to icon (frames
  checked); logcat since 16:20 0 FATAL. Right click on an icon still showed the desktop menu.
- User: "right click needs a delete and such like a real windows" -> 89d17f10: right click on a selected icon = Open (default,
  new IDS_DESKTOP_OPEN) + the shell's file menu (Cut, Copy, Create Link, Delete, Rename, Properties) per desktop folder (Wine's
  desktop folder is the user Desktop only, so user + Public get their own IContextMenu; command invoked on both); Delete = one
  SHFileOperationW FO_DELETE FOF_ALLOWUNDO for all selected (Trash, one confirm); Rename = in-place edit -> MoveFileW of the
  .lnk (shell rename needs a shell view), greyed for several; OleInitialize once for Cut/Copy; dialogs without owner.
  Marker "desktop icon right-click menu". Sims 114/114 + 99/99. CI 46074321517 GREEN 17:13 EST (3 markers ok, verify-layer
  PASS); explorer md5 5a8e0eaa swapped on the FIT 17:1x EST (sha256 5f2de2ff host == device). Device test: pending.
- 17:19 EST USER TEST of 89d17f10 ("works right", video screen-20261127-171945): menu Open/Cut/Copy/Create Link/Delete/
  Rename (greyed for 3 selected)/Properties = DEVICE-PROVEN; logcat 17:15-17:2x 0 FATAL. Properties on Ubisoft (user) + EA +
  GOG (Public) opened 2 single-file sheets (Ubisoft Connect.lnk, then EA.lnk only: Wine DoOpenProperties shows the first
  item; SHMultiFileProperties = stub). User wants ONE combined sheet like Windows.
- 15003425: own combined sheet for >1 selected (PropertySheetW, page IDD_MULTI_PROPERTIES "General": IDI_SHELL_FILES icon,
  "N files", "All of type X"/"Multiple types", "All in <dir>"/"Various folders", total size (StrFormatByteSizeW + bytes),
  Read Only/Hidden/Archive AUTO3STATE (2-state when uniform), PSN_APPLY sets/clears on all); 1 selected = shell sheet. New
  strings IDS_MP_* 38-43, dialog 220-228. Marker "combined Properties for several desktop icons". Sims 114/114 + 99/99,
  extracted compile clean. CI 46076540126 started 17:25 EST. NOT device-tested.
- RESUME (user leaving work, Wi-Fi lost): when CI green -> fastdl artifact proton-arm64ec-sdk28 -> extract
  lib/wine/aarch64-windows/explorer.exe -> stage /sdcard/Download/.tmp-x/explorer-desk-15003425.exe (sha256 host==device)
  -> container 1 closed (no wineserver) -> rootsh 'sh /sdcard/Download/.tmp-x/desk-swap.sh <exe>' -> user tests Properties
  on 2-3 mixed icons. FIT now has 89d17f10 explorer (md5 5a8e0eaa); originals 9b995bcd in /sdcard/Download/desk-select-backup/.
- 18:07 EST CI 46076540126 (15003425) GREEN both legs (4 markers ok, verify-layer PASS); explorer md5 dc513086 swapped on the
  FIT 18:12 EST (after root-bf's BL reinstall ae6ebb3c, which left the layer alone).
- 18:27 EST USER TEST (screenshot): EA + GOG + Vortex -> one sheet "Properties for EA, GOG GALAXY, ...", 3 files, All of type
  Shortcut, All in C:\users\Public\Desktop, 1.90 KB (1949 bytes), attributes = DEVICE-PROVEN. User saw the cursor stay an
  I-beam/resize shape once; 18:59 video (4 icons from both folders: "4 files", "Various folders", 2.60 KB (2664 bytes)) cursor
  normal, user: "seems ok I might have just hit a fluke" -> not reproduced, cause unknown. logcat 18:12-19:00 0 FATAL.
- STATE: all four parts device-proven on the FIT (arm64ec, Wayland, container 1). Not merged, not ported to the other 5 parents,
  not released. FIT keeps the test explorer.exe (dc513086) until the user decides; undo = desk-undo.sh.

## 2026-11-27 — staging/proton_11.14.1-DroidDeck/ncrypt-ecdsa MERGED into proton_11.14.1-DroidDeck (fast-forward)
- Why: the Epic Games Launcher's account linking (Ubisoft) failed with "Invalid Client / 400 Bad Request". Its EOS SDK
  opens or creates the ECDSA_P256 key "Epic/EpicGamesLauncher/EOS/Auth" in the software key storage provider for DPoP;
  our ncrypt had RSA only, no persisted keys and a stub NCryptOpenKey -> "GenerateDpop failed to parse public key" ->
  EOS_NotConfigured on every login retry. User 14:2x EST: "fix it"; 15:0x EST: "merge it".
- Merged fa021603 dlls_ncrypt_main.c_ecdsa_persisted_keys.patch: upstream Wine ncrypt (wine-mirror 2026-10-29: ECDSA
  P256/P384/P521, common object header, refcounted provider handles, import/encrypt/decrypt fixes; Hans Leidekker,
  Piotr Caban) + persisted keys (ours): private blob + algorithm in the prefix registry
  HKCU|HKLM Software\\Wine\\NCrypt\\UserKeys|MachineKeys\\<name>; NCryptOpenKey loads (NTE_BAD_KEYSET when missing),
  named NCryptCreatePersistedKey returns NTE_EXISTS without NCRYPT_OVERWRITE_KEY_FLAG, NCryptFinalizeKey stores,
  NCryptDeleteKey removes; ncrypt imports advapi32; ncrypt_internal.h folded into main.c as upstream.
  Backup ref refs/backup/20261127/proton_11.14.1-DroidDeck-before-ncrypt-ecdsa = 1e9d640d.
- CI 46055749323 (fa021603) GREEN both legs; sims arm64ec 113/113, x86_64 98/98; clang syntax check 0 errors.
- Device (FIT, arm64ec, container 1, Wayland), 14:52 EST: EpicGamesLauncher.log "EOS Auth Login succeeded",
  "OnEOSSDKLoginComplete success 1 error 0" (before: EOS_NotConfigured every retry); key stored in user.reg
  (Algorithm ECDSA_P256); only remaining ncrypt log line = NCryptOpenStorageProvider stub fixme. NOT yet proven: the
  Ubisoft link itself, key reuse on a later launch. x86_64 not device-tested. No release.

## 2026-11-27 — staging/proton_11.14.1-DroidDeck/gog-galaxy MERGED into proton_11.14.1-DroidDeck (fast-forward)
- Why: GOG Galaxy crashed / stayed white, Vortex died at start and then failed its .NET 9 check, in Bannerlator container
  desktops (FIT, Wayland). User 12:2x EST: "get vortex finished then merge all to parent branch".
- Merged (parent b9694e6c -> this log commit on 98328296; backup ref
  refs/backup/20261127/proton_11.14.1-DroidDeck-before-gog-galaxy = b9694e6c):
  - 129c6d81 win32u: an explicit WinSta0\\Default desktop follows the configured desktop (Galaxy's /deelevated relaunch
    through its service crashed on a desktop of its own).
  - efd9e665 + 29af2ff3 ntdll: per-app environment defaults, set only when unset: galaxyclient.exe gets
    QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu --disable-gpu-compositing" and QT_QUICK_BACKEND=software (QtWebEngine
    shared D3D11 textures by NT handle -> DXVK "Failed to open shared NT handle" ~600x -> white window; now 0).
  - 8c2bfa2d ntdll: the V8 address-space report also covers Electron main processes (resources\\app.asar beside the
    exe): Vortex 2.11 main process hit the V8 sandbox CHECK (int3) at start.
  - 98328296 icuuc, icuin: refuse to load when icu.dll is unusable (completes GN #107): .NET 9 loaded the data-only
    forwarders after icu.dll refused and aborted ("Cannot get symbol u_charsToUChars from libicuuc / Error: 127").
  - f0ca65e8 (upstream 08464dbe9b8 client surface without PutImage) was tried and DROPPED again (02c3d752, d724713d):
    it did not fix Galaxy and changes every OpenGL window on Wayland.
- CI: 46047905127 (98328296) GREEN both legs; sims arm64ec 112/112, x86_64 97/97.
- Device (FIT, arm64ec layer from CI 46047905127, container 1, Wayland, BL test build 6270eb23), 13:38-13:48 EST:
  Vortex opens (main window, no .NET dialog; dotnetprobe.exe 9 -> "Success: Found .NET 9.0.23", exit 0; 0
  find_forwarded_export errors); GOG Galaxy signed in, store pages drawn, 4 hack lines, 0 NT-handle failures, no SEH;
  AIO 2.x 64-bit --bench 10: OpenGL avg 2363.0 / 1 % low 680.3, D3D11 avg 2577.5 / 454.7; EA app 13.836: SPA fully
  loaded 9.7 s, authenticated, 0 breakpoints, 7 V8 report lines. x86_64 leg not device-tested.
- No release (release job `if: false`); catalog unchanged.

## 2026-11-27 — parent build 85ab3217 device-smoke-tested + staged (NOT released)
- Parent CI 46013005665 (85ab3217) GREEN both legs, release skipped. arm64ec layer installed on the FIT
  (contents/Proton/11.0-14.1-arm64ec-18; released v18 kept in files/layer-backup-ea/): 09:28 EST NFS Most Wanted from
  EA app Play in a container desktop -> title 60 fps; 09:33 NFS Payback shortcut -> title ~279 fps; clean exits. This
  build includes 89e2992e (hide wine_get_* from EA launchers) -> now device-proven as a build. x86_64 not device-tested.
- User 09:4x EST: "keep as staged wcp and parent branch build" -> no release, catalog unchanged. Staged on the FIT:
  /sdcard/Download/DroidDeck-Proton-11.0-14.1-EA-85ab3217/ (arm64ec sha256 c6f68b3a..., x86_64 d13afe4d...,
  SHA256SUMS.txt checked host + device, README).
- Related: NFS MW's EA licence runs out in ~30 min and OOA 4.2 cannot renew it with the Steam-linked JWS token; fixed
  on the Bannerlator side (SteamLite agent drops licences older than 10 min, BL main f01f0fad).

## 2026-11-27 — staging/proton_11.14.1-DroidDeck/ea-cef128 MERGED into proton_11.14.1-DroidDeck (fast-forward)
- 09:0x EST user: "merge 14.1 work and build so parent branch produces it". Parent was d6fdead0 (= staging base) ->
  fast-forward to the staging head + this log commit (not [skip ci], so the push builds the parent). Backup ref
  refs/backup/20261127/proton_11.14.1-DroidDeck-before-ea-cef128 = d6fdead0 (pushed).
- Merged: 67eb63c1 V8 sandbox address space report, d576450f winewayland other-process frames, 1763102e marker token,
  34308f42 emulated code write into image sections (NFS Heat), 89e2992e hide wine_get_* from EA Steam/Epic launchers,
  83b31019 fixed MAC for Android random-MAC interfaces.
- Device proof so far (FIT): 34308f42 build + 83b31019 nsiproxy.so; 89e2992e only proven by an equivalent hand patch
  (EASteamLauncher string) -> the parent-built layer gets a FIT smoke test (EA app + NFS MW/Payback) before any release.
- No release (workflow release job is `if: false`); catalog unchanged (v18 rows still the released 11.0-14.1 build).

## 2026-11-27 — Fixed MAC for Android random-MAC interfaces: EA OOA licences stop going stale (ea-cef128 83b31019, device-proven FIT)
- Why: NFS Most Wanted's OOA activation failed again after the user changed Wi-Fi ("unable to connect to EA servers to
  activate"). EA said "License files valid [true]" both times; OOA itself rejected the local licence and asked the server
  with the long JWS token, which its 256-char buffer cuts (see the 11-27 NFS MW entry).
- Evidence: a test exe in container 1 printed what Windows programs see: wlan0 MAC = Android's per-network random MAC
  (92:be:b2:90:f9:eb on the current SSID; the home SSID has its own; WifiConfigStore MacRandomizationSetting=3),
  dummy0 MAC random per boot; volume serial, computer/user name, ProductId stable.
- Fix 83b31019: android/patches/dlls_nsiproxy.sys_ndis.c_stable_mac.patch (both legs + MARKERS): on Android a locally
  administered MAC is replaced by a fixed locally administered one = FNV-1a(WINEPREFIX + '/' + interface name);
  real MACs untouched; WINE_STABLE_MAC=0 turns it off. WINEPREFIX is inherited by every child (spawn_process execv).
  Sim apply: arm64ec 108/108, x86_64 93/93. CI 46005225921 GREEN both legs (artifact nsiproxy.so 54e3cea5).
- Device (FIT, only nsiproxy.so swapped into the installed ea-cef128 layer; old file backed up): test exe now shows
  wlan0 4e:38:d6:96:68:56 + dummy0 1e:09:a1:94:ed:14 in 32- and 64-bit processes, IP intact. After clearing the stale
  licences once: NFS MW (EA app Play 08:33 EST + SteamLite shortcut 08:49) and NFS Payback (EA app Play 08:45 +
  shortcut 08:53) all reached the title screen with automatic activation. A Wi-Fi off/on kept the same Android MAC, so
  a live network-change test was not possible; the reported MAC no longer depends on it.
- Note for release: licences created before this patch were tied to the random MAC and go stale ONCE (OOA asks for an
  EA sign-in); clearing ProgramData\Electronic Arts\EA Services\License\*.dlf lets EA fetch fresh ones.
- Not released/merged (user decision). x86_64 leg not device-tested. Other layers (GE 11-14, DD 12.8, Valve 11.0-3) not ported.

## 2026-11-27 05:42 EST — NFS Most Wanted: SteamLite launch works, EA-app Play open (ea-cef128, PAUSED by user)
- Cause of the activation error: stale OOA license (EA Services\License\71530.dlf from 11-11) -> EA thinks the license is valid and sends the 4 KB JWS token instead of a short opaque token -> OOA 4.2 reads EALaunchUserAuthToken into a 256-char buffer -> garbage token -> AUTHENTICATION_FAILED. License reset -> EA fetches an opaque token -> activation OK, game runs (59 fps) and relaunches fine.
- Also found: EASteamLauncher cuts launch env values to 1024 chars when it detects Wine. 89e2992e hides wine_get_* from EA's Steam/Epic launchers; not needed for NFS MW, keep/revert undecided.
- Open: why the license went stale (OOA machine fingerprint change); EA-app Play in a container desktop (steam:// reaches the SteamLite agent without Bannerlator's login env).

## 2026-11-27 04:35 EST — NFS Most Wanted (OOA 4.2) activation, both launch methods (staging/proton_11.14.1-DroidDeck/ea-cef128, investigating)
- Why: user order — container desktop EA Play + SteamLite shortcut must both start NFS MW 2012.
- Known: SteamLite launch -> game's Core\Activation.dll sends a 6-char garbage ea_eadmtoken -> EA GetLicense VALIDATION_FAILED/AUTHENTICATION_FAILED (also 11-11, pre-existing). Worked 09-06 on GE 11.0-6 eanet2.
- Resume: FIT container 1 on this branch's 34308f42 build; nothing running at 04:35 EST. Next: launch both methods with recording + logs.

## 2026-11-27 01:10 EST — EA app 13.836 fixed: V8 sandbox address space + Wayland other-process frames (+ NFS Heat Denuvo code write) (`staging/proton_11.14.1-DroidDeck/ea-cef128`, device-proven arm64ec on the Pocket FIT, NOT released)
- **Why (user, 11-26 ~23:20 EST):** EA Desktop opened as an empty window in Bannerlator after EA's forced update (13.822 -> 13.836.0.6349, 11-26 13:04 EST); fix it for shortcuts, container desktop on X11 and Wayland, and Steam EA games through SteamLite; branch off the 11.14.1 parent, stage for a morning test, no release.
- **Cause 1 (all backends):** EA 13.836 ships EA's own CEF 128.4.13 (Chrome 128.0.6613.138) with the V8 sandbox. `Sandbox::Initialize` does `CHECK_LT(kSandboxSize 1 TB, address_space_limit)` (libcef+0x9f6813 `cmp $0x28; jbe int3`); the limit comes from `GetSystemInfo` lpMaximumApplicationAddress, 39 bits on Android -> every renderer (`EACefSubProcess --type=renderer`) died with 0x80000003 at libcef+0x9f68cc, "CEF render process terminated", empty window. Not the Chromium sandbox: Valve's ARM `--no-sandbox` hack applied to EACefSubProcess (kernelbase test) changed nothing. Proven by skipping the check in libcef (page loads, HTTP 200).
- **Fix 1** `dlls_ntdll_unix_virtual_c_v8_sandbox_address_space.patch` (67eb63c1): NtQuerySystemInformation basic/native/emulation info reports a 2^41 user space to processes with `--type=` on the command line when the real limit is lower; Wine keeps the real limit internally, MEM_ADDRESS_REQUIREMENTS limits up to the reported value are clamped; `WINE_LARGE_ADDRESS_REPORT=1/0` forces it on/off. V8 then uses its partially reserved sandbox.
- **Cause 2 (Wayland only):** EA's browser is windowed CEF: the GPU process presents into EADesktop's child window ("Chrome Legacy Window"). winewayland created a wl_surface for that other-process window but cannot attach it (cross-client subsurfaces don't exist) -> compositor answered every present `discarded` (WAYLAND_DEBUG). X11 works because winex11 refuses other-process surfaces (`HACK: Failing surface creation for other process window`) and Chromium falls back to GDI into the shared X window.
- **Fix 2** `dlls_winewayland_drv_other_process_frames.patch` (d576450f): GameNative #122 readback now on by default (`WINE_DISABLE_VULKAN_OPWR=1` off; winex11 still refuses other-process surfaces unless =0, X11 unchanged); winewayland in the drawing process copies the read-back DIB into a section duplicated into the window owner and posts `WM_WAYLAND_XPROC_FRAME`; the owner shows it in a subsurface of its toplevel over the window's client area (empty input region, hidden with the window, freed with it). Own-process swapchains untouched. 1763102e: readback marker token without `|`.
- **Fix 3** `dlls_ntdll_unix_virtual_c_emulated_code_write_image.patch` (34308f42, arm64ec only): NFS Heat (Denuvo) patches its code in the exe's RWX `.sbss` section (0xe0000020) through a system call (4-byte write at exe+0x120100bf, 8 bytes before the crashing rip exe+0x120100c7; faulting unix pc = ntdll.so +0x10B854, a static helper between NtSetLdtEntries and NtAllocateVirtualMemoryEx). GameNative #133 only handled VirtualAlloc views; now pages of sections the image declares writable (IMAGE_SCN_MEM_WRITE, read from the mapped headers) are handled too. Read-only sections still fail as on Windows.
- **CI:** 45941530397 (67eb63c1) green both legs; 45943770734 (d576450f) green both legs, patches 105/91 applied, all new markers ok, verify-layer PASS; 45947316959 (1763102e marker fix) green; **45948075124 (34308f42) green both legs**, arm64ec 106 patches, new marker ok, verify-layer PASS.
- **Device (Pocket FIT, Bannerlator 3.2.2 test build d2591e61, container 1, layer dir 11.0-14.1-arm64ec-18 replaced by the d576450f build, released dir kept in files/layer-backup-ea/, EA libcef ORIGINAL):** EA shortcut on Wayland: 0 int3, sign-in page drawn, typing + clicks work, > 2 min stable; user's flow (Wayland container desktop, double-click EA icon): sign-in page drawn; X11 container desktop (container backend switched for the test, restored): sign-in page drawn; NFS Heat via SteamLite: EA starts in razor mode, signs in through the Steam link with no user input, cloud sync, "Successful launch"; with fix 3 (34308f42 build) the old exe+0x120100C7 crash is gone, the game runs fullscreen D3D11 with frames presented (70-300 fps) but the screen stays black for 10+ min (intro video / loading, open); Alt+F4 closed it normally. Regression runs: AIO GL avg 2416.66, DiRT Showdown title ~160 fps, Counter-Strike via SteamLite ~100 fps + normal quit; 0 crashes.
- **Staged:** `/sdcard/Download/EA-fix-20261127/` = DroidDeck-Proton-11.0-14.1-{arm64ec,x86_64}-eafix-34308f42.wcp (sha256 3b67ad700112de292a67e712f2f96f9dbfafc128c023f572f45f5debb3c14111 / 07e8130c866f55e33847fcc59797372ec50c9cbf606c74a63d2f82cbedbf384b, = run 45948075124) + SHA256SUMS.txt + README.txt; the arm64ec build is installed on the Pocket FIT.
- **Not done:** no release, no catalog change, no fast-forward of the parent; other layers (GE 11-14, Valve 11.0-3, 12.8) still lack both fixes (v19 candidate); x86_64 leg not device-tested.

## 2026-11-26 22:3x EST — DroidDeck-Proton 11.0-14.1 v18 RELEASED in the v18 release, replaces 11.0-12.8 (`proton_11.14.1-DroidDeck` @ 1a5735e7, device-proven arm64ec)
- **Why (user):** rebuild the latest DroidDeck-Proton (11-14.1) as a bionic layer for Bannerlator, v18, replacing 11.0-12.8 and updating in-app; then "let's add the new fixes to it" (GameNative option 1, skip ntsync); then "merge it and build it, v18 and add it to the v18 release to replace the previous droiddeck proton layer".
- **Branch history:** `staging/proton_11.14.1-DroidDeck/v18` d62838c7 (GE 11-14 v18 + 12 DroidDeck-only patches + 21 CachyOS tier3 picks + workflow) -> bf990de3 (+ `dlls_ntdll_process.c_cpu_area_no_trace.patch`: with Bannerlator's Wine debug on, `+seh` traced every `RtlWow64GetCurrentMachine()` since upstream 03487a0dda3; Counter-Strike 60 -> 9 fps, 1 GB log in 7 min). Parent created at bf990de3 (checkpoint ref `refs/backup/20261126/dd-proton-11.14.1-v18-checkpoint`). `staging/proton_11.14.1-DroidDeck/gn-fixes`: d53fcf05 (#139 configure.ac) + 49ecf674 (GameNative port) + 615029cb (#88 marker) + e17d6638 (#122 launcher readback, OFF unless `WINE_DISABLE_VULKAN_OPWR=0`) + 1a5735e7 (#142 dropped: needs ntsync v0.2.0 / #102; ntdll.so failed to link `ntsync_release`). Parent fast-forwarded bf990de3 -> 1a5735e7 (backup `refs/backup/20261126/proton_11.14.1-DroidDeck-before-gn-fixes`), tree == tested.
- **GameNative fixes in (credit in each patch, LGPL-2.1):** #144 #117 #63 #107 #100 #72 #88 #139 (butilly), #85 winebuild fast-forward `-import` thunks via the new `TOOLS_PATCHES` step in build-step0.sh (cache key covers tools patches) + arm64x_check_call, #86 power cache + fast NtContinue (Utkarsh Dalal; #85/#86 ntdll parts adapted), #122 native ARM64 unwind guard (arm64ec) + NLS fallback (x86_64) + launcher readback off by default (Dmage22), user32 Android clipboard (Joshua Tam), winex11 clip re-grab / grab on foreground (#82). Out: #102, #142, #122 WoW ntdll pieces (covered by dlls_ntdll_wow_forever_arm64ec.patch).
- **Finding:** the winebuild half of `dlls_ntdll_signal_arm64ec.c_arm64x_import_thunk_hotpatch.patch` never reached winebuild (applied after wine-tools are built): bf990de3 kernel32.dll had 0 hotpatch thunks; 1a5735e7 has 790 fast-forward thunks (verify-layer now checks). Patches to tools/ belong in TOOLS_PATCHES.
- **CI:** staging 45926928349 (1a5735e7) and parent 45933448325 (1a5735e7) green on both legs: arm64ec 103 patches, x86_64 89, 0 FATAL, verify-layer PASS (790 thunks, clipboard socket). Earlier: 45924360845 failed (ntsync_release), 45925300499 cancelled.
- **Device (Pocket FIT Elite, Bannerlator 3.2.2, DXVK 3.2 gplasync, FEX nightly unixlib, Banners-AIO 20261127-r2, Wayland):** 1a5735e7 arm64ec: DiRT Showdown title 414-420 fps, Counter-Strike (SteamLite, Wine debug on) 60 fps with a ~1 MB log, FlatOut 60 fps; clean exits, 0 FATAL. bf990de3: in-app update 12.8 v18 -> 14.1 v18, revert, update again (settings kept). Not tested: clipboard, launcher readback, x86_64, other devices.
- **Released (user word):** tested bytes (run 45926928349) uploaded as `DroidDeck-proton-11.0-14.1-arm64ec.wcp` (117999687 B, sha256 bf94176180e96584185835f9e80b168666f9fa752aa9583cfedb8fe493134ff6) and `DroidDeck-proton-11.0-14.1-x86_64.wcp` (64819221 B, 910bde77aea92bba86cffcd86c612a0b9ba48e08f0c26b4b8203dd14bf1be65e) to `build-bionic-layers-20261124-v18` (here) and winlator-contents `bionic-layers-20261124-v18`; public URLs 200 + sha OK; 11.0-12.8 v18 files removed from both (copies on both SD cards, `Documents/proton-layer-checkpoints/dd-proton-11.12.8-v18-release-assets/`); catalog rows 190/191 -> 11.0-14.1 (winlator-contents main 44ada4d, backup `refs/backup/20261126/main-before-dd1141-v18`); both release notes updated; tags unchanged.
- **Open:** v19 = the same GameNative set on the other layers + the trace fix on GE-Proton 11.0-14 (listed as a known issue in the v18 notes); #102 + #142 later.


## 2026-11-24 00:51 EST — v17 staging build GREEN and staged (`staging/proton_11.14-GE/v17` @ dcd87090, CI-green, NOT device-tested)
- **First builds:** parent push run 45329982897 (84da4aa9) FAILED on both legs in `dlls/wmvcore/async_reader.c`: GE video 0133 clears `active_start` / `active_duration`, members only GE's 0009 arcania patch adds (not carried). Staging run 45331633505 (ae29c527) cancelled for the same reason.
- **Fix:** parent `52e0d8cc` reverses 0133 (new commit, no force push); merged into staging as `dcd87090`.
- **CI:** staging push run **45332733791** (headSha dcd87090 asserted): build (aarch64, 28) success, build (x86_64, 28) success, release skipped (`if: false`). Logs read: arm64ec 72 patches SUCCESS, x86_64 64; every MARKER ok incl. all v17 rows; "All shipped features verified present"; GE game-fixes 15/15; verify-layer PASS on both (GE 11-14 picks: lsass.exe, WINE_DDRAW_MAXGAMETICKS, WINE_CURSOR_SCALE); lsteamclient check OK (arm64ec), absent (x86_64). Skipped steps are the arch-conditional ones only. Only warning: Node 20 deprecation. Parent rerun: push run **45332699209** (headSha 52e0d8cc asserted): aarch64 success, x86_64 success (its apt step sat on a slow mirror for ~30 min; the parent has no Azure pin, that is a v17 item), release skipped; 59 / 58 patches, markers ok, GE 15/15, verify-layer PASS on both, artifacts versionCode 16 (not staged).
- **Staged (host + device, sha256 equal on both, equal to the CI report):**
  - `/sdcard/Download/v17-layers/GE-proton-11.0-14-arm64ec-v17-dcd87090.wcp` 119947509 B, sha256 `e4bd863eb1db6d9e6c5459b7544bd4514478f264d9d3c2420086820dd0e91048`, profile Proton 11.0-14-arm64ec versionCode 17
  - `/sdcard/Download/v17-layers/GE-proton-11.0-14-x86_64-v17-dcd87090.wcp` 66351090 B, sha256 `a1036a5e350c32718ab73544a12119e17c6ffd1ec9afae04b5bcaee2efbd197a`, profile Proton 11.0-14-x86_64 versionCode 17
  - host copies in /root/src/dl/v17-layers/ (FILES.tsv rows added).
- **Not done (by design):** no release, no catalog change, no install, no device use.
- **Finding (not fixed, user decision):** with `WINENTSYNC=1` on userspace ntsync, `get_inproc_alert_fd()` stores the alert *handle* in `alert_fd`, and `pthread_exit_wrapper()` later `close()`s it as a file descriptor: every thread exit closes an unrelated fd of that number (fdsan abort risk on Android). Valve fixed the same pattern for fsync in 707bb01fbfc (CW-Bug-Id #28016). Pre-existing in every layer with our ntsync_userspace.patch.
- **RESUME:** user morning test of the staged arm64ec wcp (list in /root/src/dl/v17-draft/ge-11-14.md). Release / catalog / fast-forward decisions are the user's.

## 2026-11-24 — GE-Proton 11.0-14 layer + v17 fix set (`staging/proton_11.14-GE/v17`, built overnight, NOT device-tested)
- **Why (user, 2026-11-23 ~23:20 EST):** "build proton ge 11-14 and replace 11-9 with it ... putting 11-14 on its own parent branch like the others, no v17 release yet". Build overnight, stage for a morning test. No release, no catalog change, no install.
- **Base:** GloriousEggroll/proton-ge-custom tag GE-Proton11-14 (commit 777d72b4ae50, released 2026-11-23 05:19 EST) pins ValveSoftware/wine a47c73f52fd7 (2026-11-20), the same commit as GE-Proton11-13; a fast-forward of the 11.9-GE base 981e2f0b9007 by 125 commits.
- **Parent `proton_11.14-GE` (84da4aa9)** = `proton_11.9-GE` v16 a01a7c58 (every one of our commits kept as history) + merge Valve a47c73f52fd7 (one conflict, wg_transform.c) + android re-anchor of the sync.c / ntsync alert-fd hunks on Valve 707bb01fbfc + GE-Proton11-14 tiers, applied like the 11-9 ones (in-tree, verbatim, GE order, fuzz 2): wine-hotfixes (29 patch files + llvm-libc pair), 2 hand ports from our DroidDeck-Proton line (B8G8R8A8_UNORM first, winewayland animated cursors), video (13 standalone), ACE anti-cheat (7, in-tree because lsass is a new program), game-fixes tier +2 (ddraw max game ticks, X cursor scale, build-time). Skips and reasons are in each tier commit message. Source checks (MARKERS) + verify-layer checks for the merge and the picks; workflow `build-ge-proton-11.0-14.yml` (versionName 11.0-14, versionCode 16 on the parent).
- **v17 on top (this branch), one commit per item:** EC code map for 47-bit addresses (d4c9ffa7), ARM64EC unwind no-progress stop (fecf7376) and stack-pointer check (a188791d), DiRT 3 / Showdown telemetry sendto, winex11 offscreen check cache (opt-in WINE_OFFSCREEN_CHECK_MS), WMA cutscene audio (8956410a), GameNative #119, #128, #133 (with #131), #67 without its sechost part, DroidDeck-Proton #34, MARKERS for the in-tree fixes, Azure apt mirror pin (d48e30a1's change), versionCode 17. Not taken (user list): GameNative #122, #142, #102, ntsync v0.2.0, #124, #126, #135.
- **Local check (before push):** export of the tree + build-step PATCHES fail-hard + MARKERS (bash-evaluated) + apply-ge-patches.sh: arm64ec 72 patches / 65 markers, x86_64 64 / 50, GE game-fixes 15/15; only miss = DirectAudio (a submodule, not in the export).
- **CI:** parent push run 45329982897 (headSha 84da4aa9). Staging run: see the next entry.
- **Device:** none. Staging target: /sdcard/Download/v17-layers/GE-proton-11.0-14-<arch>-v17-<sha8>.wcp.
- **Open:** morning test (list in /root/src/dl/v17-draft/ge-11-14.md); release decision is the user's.
