# Recomp Launcher catalog submission (draft)

Form: https://recomplauncher.com/submit?platform=pc. Sign in with the GitHub account that owns the repo (Fulixts);
a free developer account (https://recomplauncher.com/dev) makes the review faster.

## Step 1: your project

| Field | Value |
| --- | --- |
| Repo | https://github.com/Fulixts/OpenJamboree |
| Kind | Game port |
| Name | OpenJamboree |
| Made by | Fulixts (fork of drewkellar/OpenJamboree, built on SwitchRecomp by MZ Labs) |
| Original console | Nintendo Switch |
| Copy players need | Super Mario Party Jamboree base game (.nsp, 0100965017338000) + update 2.3.0 (.nsp, v458752) + prod.keys, all dumped from their own Switch |
| Windows build | `OpenJamboree-v*-Windows-x64.zip` |
| Linux build | `OpenJamboree-v*-Linux-x64.tar.xz` |
| macOS build | none |

One-line summary: Super Mario Party Jamboree 2.3.0 recompiled to run natively on PC, built from your own Switch dump.

## Step 2: Easy Setup

- Quick install: "It needs to be built or set up first". Steps:
  1. Pick your base game .nsp, the 2.3.0 update .nsp and your prod.keys.
  2. Easy Setup checks the copy, unpacks it and recompiles the game on your PC (20 to 40 minutes the first time).
  3. Play. Keep both .nsp files: new releases build again from them.
- Settings in the launcher: not yet (the game has its own in-game menu on Esc); planned.
- "Check my repo" should find `recomplauncher.json` in the latest release.

## Step 3: details

Features: in-game settings menu (Esc / Share+Options), 1080p TV mode and internal resolution up to 400%, skip the
3 minute opening, native PlayStation / Xbox / Switch Pro controllers (SDL3), Brazilian Portuguese, 60 FPS lock,
texture cache for shorter loads. Coming: LAN play, mods.

Page text: OpenJamboree is a static recompilation of Super Mario Party Jamboree (update 2.3.0): the game's ARM64 code
is translated to C and compiled into a native program on your own PC, and a runtime provides the Switch services, the
GPU (OpenGL), audio and video. No game files, keys or firmware are included or downloaded: Easy Setup builds
everything from the copy you dumped from your own Switch.

AI: Yes, Code (the launcher integration, in-game menu, controller support, setup and CI were written with AI help).

Credits: SwitchRecomp by MZ Labs (AGPL v3); original OpenJamboree port by drewkellar; Dear ImGui; SDL3; LibHac; OpenH264; Opus.

## Pictures (still to make)

- Icon: square, 512x512 or bigger (required). Our own art, not Nintendo's.
- Screenshots: 1 to 4, of the game running in the recomp, in a match (not a menu). F3 in game saves one to `captures/`.
- Backdrop 16:9 and logo: optional, the store can find art.
