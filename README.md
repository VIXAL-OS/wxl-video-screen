# WXL Video Cinema

Video Cinema is an optional WarcraftXL ABI 1.1 extension for the 32-bit WoW
3.3.5a build-12340 client. It places a client-local, world-space screen and
plays an official YouTube embed through a small local WebView2 helper process.

The Hub release is one ZIP containing `wxl-video-screen.dll`,
`wxl-video-host.exe`, `player.html`, `wxl.json`, and license documentation.
It contains no WoW client files, test client, saved placement, browser profile,
or video content.

## Install from WXL Hub

1. Use a WarcraftXL-enabled build-12340 client.
2. Install **Video Cinema** from WXL Hub and restart the client when prompted.
3. Ensure the Microsoft Edge WebView2 Evergreen Runtime is installed.
4. In game, press **Insert**, open **Video Cinema**, enter a supported YouTube
   URL, and choose **Place / move screen in front of me**.

The extension saves local placement in
`Extensions/wxl-video-screen/world-screen.tsv`. That per-client file is
intentionally excluded from source control and release archives.

## Requirements and boundaries

- WarcraftXL ABI 1.1 and a 32-bit build-12340 WoW client.
- Microsoft Edge WebView2 Evergreen Runtime.
- Internet access and normal YouTube availability for the selected video.
- The screen is client-local; it does not create a server GameObject, collision,
  or multiplayer-synchronized playback.

YouTube embedding, ads, sign-in, age/region/DRM restrictions, and owner policy
remain authoritative. The extension does not download, extract, transcode,
cache, or redistribute YouTube media.

## Build

The GitHub Actions workflow builds the extension against `WarcraftXL/wxl-core`
tag `v1.1`, downloads the pinned WebView2 SDK only for the helper build, and
publishes a versioned ZIP release. The WebView2 runtime itself is not bundled.

For a local helper build, pass an existing SDK location if it is not beneath
`host/third_party/webview2/`:

```powershell
cmake -S host -B host/build -A Win32 `
  -DWEBVIEW2_ROOT='C:/path/to/Microsoft.Web.WebView2.1.0.4129.50'
cmake --build host/build --config Release
```

The native extension source is in `extension/src/`; copy only that directory
into `wxl-core/extensions/wxl-video-screen/` when building it manually.

## Privacy

WebView2 browser-local data is stored under
`%LOCALAPPDATA%\\WarcraftXL\\video-cinema`. Account credentials are not passed
through WarcraftXL.

License: GPL-3.0-or-later. See [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
