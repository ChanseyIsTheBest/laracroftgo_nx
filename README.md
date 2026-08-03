Lara Croft GO — Nintendo Switch port (Unity 2022.3 / IL2CPP wrapper)
===================================================================
 
This is a native wrapper / loader that runs the original ARM64 Android build of
Lara Croft GO on Switch homebrew. It contains no game code and no game assets —
it loads the game's own libraries and recreates, natively, the thin Android/JNI
layer the Unity engine expects.
 
Install & run
-------------
 
You need files from Lara Croft GO V.2.4.

Put the `.nro` in any folder under `sdmc:/switch/` and place your game files
next to it — the loader finds its folder at runtime, so the name is up to you:
 
```
sdmc:/switch/laracroftgo
├── lcgo_nx.nro
├── libmain.so  libunity.so  libil2cpp.so   <- from your APK: lib/arm64-v8a/
├── cursor.png                              <- optional
└── assets/                                 <- both APKs' assets/, merged
```
 
Launch via title override (hold R while starting an installed game) or a
forwarder.
 
Optionally drop a `cursor.png` (up to 64×64, transparency respected, top-left
pixel is the hotspot) in the same folder to replace the on-screen cursor with
your own.

Controls
--------
  
| Input | Action |
| --- | --- |
| **+** | Toggle the on-screen cursor |
| **–** | Toggle gyro pointing (tilt/turn the controller to aim) |
| **Left stick** | Move the cursor |
| **L** / **R** | Recenter the cursor to the middle of the screen (helps gyro aiming) |
| **A** / **ZR** / **ZL** | Tap / confirm (ZL and ZR let you play one-handed) |
| **B** | Android Back |
| **D-pad up / down** | Adjust sensitivity of whatever is driving the cursor |
 
The cursor is on by default when docked and off in handheld; **+** overrides
either way. A USB mouse works in both modes: move to control the cursor,
left-click to tap, and use the scroll wheel to change sensitivity — gyro turns
itself off while a mouse is connected. Your stick, mouse and gyro sensitivities
are remembered in `pointer.cfg` automatically after in-game adjustment.
 
Settings
--------
 
`config.txt` is written next to the `.nro` on first launch, with the options
documented inline:
 
```
handheld_res 720     # 720 or 1080
docked_res   1080    # 720 or 1080
framerate    60      # 60 or 30
```
 
Both modes accept either resolution — 1080 in handheld is supersampled down to
the panel, 720 docked is upscaled.
 
Building
--------
 
Requires devkitPro with the switch-dev group plus these portlibs:
 
```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-libpng switch-zlib
 
export DEVKITPRO=/opt/devkitpro
make                        # -> deusexgo_nx.nro
```
  
Credits
-------
 
The loader/shim infrastructure (so_util, libc_shim, jni_fake, unity_jni,
opensles, nx_pointer, diagnostics) derives from the open-source Switch
`.so`-loader lineage — Andy Nguyen, fgsfds and ChanseyIsTheBest, building on
TheOfficialFloW's Vita/Switch loader tradition — reaching this project via the
Zookeeper DX and PvZ Fusion ports, with the CloverPit and Angry Birds 2 ports as
references. All MIT-licensed. Thanks to everyone in that lineage for making this
approach possible.
