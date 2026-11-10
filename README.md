# NFSU2 Fixed Hood Cam

Rigidly attaches the hood camera to the car, with an optional horizon lock.

Targets the US v1.2 `SPEED2.EXE`
(`sha256 bc34932b36bb19194000690ec26edf964176856c31fb0978e01e61ee814eee9d`).

## What it does

The plugin hooks the look-at call in `TrackCarCameraMover::Update`
(`0x453B44` → `FUN_005BA9A0`). It captures the hood-camera mount once and then
rebuilds the camera position and direction from the car transform every frame.
The game's positional and rotational inertia is removed.

`HorizonLock` selects the up vector:

- `1` = world up; the horizon stays level even when the car body rolls.
- `0` = car up; the camera is fixed tightly to the car and rolls with it.
  This is the default.

It also centers the player car's audio pan by hooking the pan update and
forcing player index 0 to pan `0`; AI car audio is unchanged. The centering
respects `OnlyHood`: with `OnlyHood=1` it applies only in hood view, with
`OnlyHood=0` it applies in every view.

The EXE is patched in memory only. No game files are modified on disk.

## Install

1. Get `NFSU2FixedHoodCam.asi` from the Releases page, or build it yourself
   with `build.ps1` (see below).
2. Copy `NFSU2FixedHoodCam.asi` into the game's `scripts\` folder.
3. Start the game normally (an ASI loader is required, as with any other
   `.asi` mod).
4. Optionally copy `NFSU2FixedHoodCam.ini` next to the `.asi`. If it is
   missing, the plugin creates one with default values on first run.

## Config

Config is read once at startup. Restart the game after changing values.

```ini
[General]
Enabled=1       ; 1 = plugin active, 0 = vanilla
OnlyHood=1      ; 1 = only POV type 4 (hood), 0 = every camera mover
DebugLog=0      ; 1 = append NFSU2FixedHoodCam.log

[Camera]
RemoveMovement=1
HorizonLock=0

[Audio]
CenterPlayer=1
```

### Tightly fixed to the car, including roll (default)

```ini
[Camera]
RemoveMovement=1
HorizonLock=0
```

### Fixed mount, but horizon stays level

```ini
[Camera]
RemoveMovement=1
HorizonLock=1
```

### Vanilla camera

```ini
[Camera]
RemoveMovement=0
```

### Centered player car audio

`[Audio] CenterPlayer=1` forces the player car's audio pan to `0` while
leaving AI car positioning unchanged. The pan value is hardcoded; there is no
config knob for it. The override honors `OnlyHood`: centered only in hood view
when `OnlyHood=1`, centered in every view when `OnlyHood=0`.

## Build

Requires a 32-bit MinGW-w64 `g++`. If it is on `PATH`:

```powershell
.\build.ps1
```

Otherwise pass the compiler explicitly:

```powershell
.\build.ps1 -Gxx "C:\path\to\mingw32\bin\g++.exe"
```

The output is `NFSU2FixedHoodCam.asi` in this folder. Build outputs are ignored
by git; attach the ASI to a GitHub release instead of committing it.

## Notes / limitations

- The hook is at `0x453B44` (`CALL FUN_005BA9A0`). The stub is allocated near
  the EXE and preserves x87 state and all general-purpose registers around its
  C helper.
- The original `CALL` is replaced with another `CALL`, so the CPU pushes the
  original return address. Replacing it with a `JMP` would shift the stack by
  one argument and crash.
- With `OnlyHood=1`, the helper checks the active mover's POV type at
  `mover+0x4C` (`in_ECX[0x13]` in the game code; 4 = hood).
- Other EXE versions/regions are not supported; the call-site byte checks and
  target checks fail closed and the plugin does nothing.
- The audio hook centers only player audio-object index 0; AI car panning is
  left alone.

## License

The Unlicense (public domain dedication). See `LICENSE`.
