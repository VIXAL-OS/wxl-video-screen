# Modifications

This is a modified version of [wxl-video-screen](https://github.com/sogyboi/wxl-video-screen) (Video Cinema) by
sogyboi, at commit `1e96963`, licensed under GPL-3.0 (see [LICENSE](LICENSE)). The modifications are released under
the same license.

- **Who:** VIXAL-OS, with AI coding assistants (Claude and Codex).
- **When:** 13–17 October 2026.
- **Target:** the 3.3.5a client (build 12340) with WarcraftXL, played on ChromieCraft.

## What changed
- **Smoother, sharper picture:** the helper captures the page every 33 ms instead of 67 ms, as JPEG instead of PNG,
  and the shared frames are 1280x720 instead of 640x360 (`VideoShared.hpp` version 2, in both the helper and the
  extension). The screen's texture has mipmaps the GPU makes from each frame (a system-memory texture copied into an
  auto-mipmapped one with `UpdateTexture`), falling back to one level when that isn't available.
- **Own shared-memory names** (`Local\WXLStonewallTvFrames-v1`, `Local\WXLStonewallTvControl-v1`), so this build
  and AzerothPlex, which uses the stock Video Cinema names, can run side by side.
- **Stonewall TV:** an optional neon sign over the screen ("Stonewall" in hollow tubes with a rainbow arc, "TV"
  stuttering), with a glow and sparkles, drawn after the world without a depth test, so it shows with "Characters
  and world appear in front" on (`extension/src/StonewallArt.hpp`, `DrawStonewall` in `VideoSurface.cpp`). Switched by a "Stonewall TV" checkbox,
  saved as an optional last field of the screen's settings. The sign is baked once into `stonewall-sign.bin` beside
  the DLL, because its fonts come with Microsoft Office; without the file it's drawn at runtime as a fallback.
- **Depth that matches the world's:** with "Characters and world appear in front" on, the screen is drawn before
  the world with the projection the world is rendered with (the graphics device's second matrix) and the world
  pass's depth range, instead of the projection the device was given and the viewport's range. At the end of the
  scene, nothing depth-tests or swaps the depth surface.
- **State put back whole, and only the world pass touched:** every state block is captured right after it is created (`CreateStateBlock` alone gives the shaders' float constants above c127 back wrong on this machine's D3D9 runtime). The screen drawn before the world goes only into the world pass's own clear, into the depth surface the world is drawn with, and the frame is uploaded at the end of the scene, not inside the pass.
- **CyTube channels:** `player.html` opens a CyTube channel link (`https://cytu.be/r/<channel>`) as the page itself,
  since CyTube refuses to load inside a frame; the channel's own sync then drives the video.

## Build files added
- `ext-build/CMakeLists.txt` builds the extension DLL against wxl-core's sources.
- `art-test/` renders previews of the sign for checking the art without the game.
