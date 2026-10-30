# GePatch + Adrenaline

[Русский](README.md) | [English](README_EN.md)

GePatch and Adrenaline for running PSP games at **960×544** on PS Vita.

This is an independent experimental modification. Original projects:
[TheOfficialFloW/GePatch](https://github.com/TheOfficialFloW/GePatch) and
[isage/Adrenaline](https://github.com/isage/Adrenaline), based on TheFloW's Adrenaline.

## Release 2026.10.30

The release includes two modules:

| File | SHA256 prefix | Purpose |
|---|---|---|
| `ge_patch.prx` | `091579af` | PSP kernel plugin |
| `adrenaline_user.suprx` | `476689be` | Vita-side user module |

Full checksums: [release/SHA256SUMS](release/SHA256SUMS).

I fixed screen tearing during fades and transitions and improved graphics
processing and frame presentation. Tested on my Vita in Tekken 6.

## Adrenaline compatibility

GePatch includes support for standard Adrenaline, so updating Adrenaline is
optional when using the plugin on its own. Fixes for stencil-based shadows and
screen tearing through double buffering require the modified
`adrenaline_user.suprx` included in the release. This module is intended for
**Adrenaline 8.0.2**.

Other Adrenaline versions and forks, as well as ABM compatibility, have not been tested yet.

## Installing the update

Installing both modules requires **Adrenaline 8.0.2**. This package updates two
modules; it does not include a full installation VPK or PSP firmware.

1. Close Adrenaline completely.
2. Back up your existing `ge_patch.prx`, `adrenaline_user.suprx` and
   `ux0:/pspemu/seplugins/game.txt` so you can restore them.
3. Copy `ge_patch.prx` to `ux0:/pspemu/seplugins/ge_patch.prx`.
4. Copy `adrenaline_user.suprx` to
   `ux0:/app/PSPEMUCFW/sce_module/adrenaline_user.suprx`.
5. Add or enable `ms0:/seplugins/ge_patch.prx 1` in
   `ux0:/pspemu/seplugins/game.txt`. Keep your existing entries.
6. Restart Adrenaline completely. To roll back, close it and restore your
   backed-up modules and configuration.

If you keep standard Adrenaline, skip replacing `adrenaline_user.suprx`.
For the full set of fixes, install both modules from the same release.
Verify the transferred files against their SHA256 checksums.

## Changes

- Rendering at 960×544.
- Two display buffers, with GE commands publishing the completed buffer index.
- Long display lists are processed in place. Short lists are copied and released
  to the GE incrementally, holding back inset commands that may still change.
- Saving and restoring modified vertices and commands, UV inset correction,
  and stencil/framebuffer format handling.
- VBlank waiting does not hold up a game that is behind schedule.

## Known issues

Some games may have graphical glitches or performance issues.
I'm continuing to work on compatibility.

## Building and testing

Instructions: [BUILD.md](BUILD.md) (in Russian). Tests use synthetic GE lists
and do not launch games. Visual output and speed are checked on Vita;
Tekken 6 speed is measured using the round timer rather than rendered frame counts.

## Licenses and provenance

Original notices and licenses are preserved:
[GePatch](gepatch/LICENSE.TXT), [Adrenaline GPLv3](adrenaline/src/LICENSE),
and the licenses of bundled libraries. Source and binary details:
[release/manifest.json](release/manifest.json),
[GePatch upstream manifest](gepatch/UPSTREAM_MANIFEST.json).
Adrenaline upstream commit: `6c2ab8c79679b021de230f28da8278518c410bea`.
Games, disc images, saves and Sony firmware are not included in this repository.
