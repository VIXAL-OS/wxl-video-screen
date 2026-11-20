# WarcraftXL x64 Film Club

This optional build targets the WarcraftXL-Labs Windows x64 mod ABI 0.1.46. The release archive supplies a native mod under `Mods` and the shared `Interface/AddOns/WXLFilmClub` panel. Extract those folders beside the installed WarcraftXL executable while the game is closed. The native world screen uses Direct3D 9 or 9Ex.

In the world, type **/film**, select the installed player and click **Place screen**. The screen stays twelve yards ahead of the camera at the moment of placement. Hiding the panel leaves playback running. Ending the session or leaving the world closes the helper. A participant places their own screen in each client. Both providers can share the same copy of the addon.

In an x64 Visual Studio developer shell with Git, CMake and Ninja on PATH, run the command for this fork with absolute paths and an empty build directory.

```powershell
python x64/build.py --sdk C:/src/WarcraftXL-sdk --build C:/build/film-cinema --webview2 C:/sdk/Microsoft.Web.WebView2.1.0.4129.50
python x64/build.py --sdk C:/src/WarcraftXL-sdk --build C:/build/film-plex --mpv C:/sdk/mpv-dev-x86_64-20261014
```

Use the SDK revision `2920b6e3143169a4244dde6e9d832cd7721f2489`. For a distributable Plex archive, include the pinned yt-dlp executable and third-party license files from the fork's existing release alongside libmpv. For Cinema include the WebView2 notices and install Microsoft's WebView2 Evergreen runtime. Source builds apply the worker changes in the build directory. The existing 32-bit extension and its release process continue to use the original host source.

Worker profiles live under `%LOCALAPPDATA%\WarcraftXLFilm`. Plex uses the `plex-player` folder and Cinema uses `video-cinema`. For a watch party, set `relayUrl` in `plex-player\party-settings.json` to a relay reachable by all participants before starting the Plex player. The default loopback address reaches only the same machine.

