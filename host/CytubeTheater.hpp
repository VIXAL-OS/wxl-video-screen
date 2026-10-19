// CyTube theater mode for the WebView2 helper (wxl-video-screen).
// Copyright (C) 2026 WarcraftXL contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// player.html opens a CyTube channel (https://cytu.be/r/<channel>) as the page itself, since CyTube refuses to load in a
// frame. The page then shows from its top -- the site's bar, the channel's banner, chat -- and the in-world screen, a
// capture of the helper's 1280x720 view, showed the banner instead of the video. This script is run by the helper in
// every document it loads (AddScriptToExecuteOnDocumentCreated) and does nothing unless the top-level page is a channel
// (/r/<channel>) on one of kCytubeHosts. There it:
//  - finds CyTube's video area (#videowrap; without it, whatever holds #ytapiplayer) and makes it fill the view: fixed
//    at 0,0, 100vw x 100vh, black, the player inside it stretched to it (CyTube's own inline sizes lose to !important);
//    everything else is hidden (visibility), scrollbars are off, and the ancestors lose any transform, filter or
//    containment that would pin a fixed element to them instead of the view. The stylesheet is a constructed one
//    (adoptedStyleSheets): no inline <style> for a content security policy to refuse, nothing in the page's DOM;
//  - keeps it that way while CyTube swaps players between media items (YouTube iframe, video.js <video>, ...): the rules
//    go by selector, and a MutationObserver plus a once-a-second check re-mark the video area and the player;
//  - gives the panel's controls something to call (the helper calls window.wxlPlayerCommand / wxlSetVolume /
//    wxlLoadFromNative, which player.html defines for YouTube): Play / Pause / Stop go to CyTube's player object
//    (window.PLAYER: play(), pause(), setVolume(0..1), getVolume(cb)), else to a <video> in the video area, else to a
//    YouTube iframe through its postMessage API. A channel keeps its viewers in sync and resumes a paused player at its
//    next update, so Pause (and Stop, which in a synced room is the same) holds the player paused until Play. The
//    volume is the panel's (its distance-based level): it is set on the player, as CyTube's VOLUME (which a new player
//    starts with), and checked again every second. Loading a YouTube link goes back to player.html (?load=...); a
//    channel link opens that channel;
//  - window.wxlTheater(on): the helper switches the layout off while its troubleshooting window is shown (to sign in,
//    pick a playlist item, read chat) and back on when it's hidden again.
#pragma once

#include <string>

namespace wxl_video_host
{
    // The hosts player.html opens as CyTube channels (keep in step with cytubeHosts there).
    constexpr const char* kCytubeHostsJson = R"(["cytu.be"])";

    constexpr const char* kCytubeTheaterScript = R"JS((() => {
  'use strict';
  const HOSTS = __WXL_CYTUBE_HOSTS__;
  if (window.top !== window) return;
  const isChannel = url => HOSTS.includes(url.hostname.toLowerCase().replace(/^www\./, '')) && /^\/r\/[^/]+\/?$/.test(url.pathname);
  if (!isChannel(location) || window.__wxlCinemaTheater) return;
  window.__wxlCinemaTheater = true;

  const CSS = `
html.wxl-theater, html.wxl-theater body { overflow: hidden !important; background: #000 !important; }
html.wxl-theater { transform: none !important; filter: none !important; contain: none !important; }
html.wxl-theater body * { visibility: hidden !important; }
html.wxl-theater .wxl-theater-target, html.wxl-theater .wxl-theater-target * { visibility: visible !important; }
html.wxl-theater .wxl-theater-chain { transform: none !important; filter: none !important; backdrop-filter: none !important;
  perspective: none !important; contain: none !important; container-type: normal !important; will-change: auto !important; }
html.wxl-theater .wxl-theater-target { position: fixed !important; left: 0 !important; top: 0 !important; right: auto !important;
  bottom: auto !important; width: 100vw !important; height: 100vh !important; min-width: 0 !important; min-height: 0 !important;
  max-width: none !important; max-height: none !important; margin: 0 !important; padding: 0 !important; border: 0 !important;
  float: none !important; transform: none !important; z-index: 2147483647 !important; background: #000 !important; overflow: hidden !important; }
html.wxl-theater .wxl-theater-target > :not(.wxl-theater-keep) { display: none !important; }
html.wxl-theater .wxl-theater-target .wxl-theater-keep, html.wxl-theater .wxl-theater-target .wxl-theater-player,
html.wxl-theater .wxl-theater-target #ytapiplayer, html.wxl-theater .wxl-theater-target .embed-responsive-item,
html.wxl-theater .wxl-theater-target iframe, html.wxl-theater .wxl-theater-target .video-js,
html.wxl-theater .wxl-theater-target object, html.wxl-theater .wxl-theater-target embed {
  position: absolute !important; left: 0 !important; top: 0 !important; width: 100% !important; height: 100% !important;
  min-width: 0 !important; min-height: 0 !important; max-width: none !important; max-height: none !important;
  margin: 0 !important; padding: 0 !important; border: 0 !important; transform: none !important; }
html.wxl-theater .wxl-theater-target video { width: 100% !important; height: 100% !important; max-width: none !important;
  max-height: none !important; object-fit: contain !important; background: #000 !important; }
html.wxl-theater ::-webkit-scrollbar { display: none !important; width: 0 !important; height: 0 !important; }
`;

  let enabled = true;
  let sheet = null, styleElement = null;
  function ensureSheet() {
    try {
      if (!sheet) { sheet = new CSSStyleSheet(); sheet.replaceSync(CSS); }
      if (!document.adoptedStyleSheets.includes(sheet)) document.adoptedStyleSheets = [...document.adoptedStyleSheets, sheet];
      return;
    } catch (_) { sheet = null; }
    const parent = document.head || document.documentElement;
    if (!parent) return;
    if (!styleElement) { styleElement = document.createElement('style'); styleElement.textContent = CSS; }
    if (styleElement.parentNode !== parent) parent.appendChild(styleElement);
  }

  // The video area: CyTube's #videowrap; without it (a channel's own layout), what holds the player.
  function findTarget() {
    const wrap = document.getElementById('videowrap');
    if (wrap) return wrap;
    const player = document.getElementById('ytapiplayer');
    if (!player) return null;
    return player.closest('.embed-responsive') || player.parentElement;
  }
  // The child of the video area the player is in (kept; the header and footer around it are hidden).
  function playerBranch(target) {
    const player = target.querySelector('#ytapiplayer, iframe, video, .video-js, object, embed');
    if (!player) return null;
    let branch = player;
    while (branch.parentElement && branch.parentElement !== target) branch = branch.parentElement;
    return branch.parentElement === target ? branch : null;
  }
  const mark = (element, name, on) => { if (element.classList.contains(name) !== on) element.classList.toggle(name, on); };
  let target = null, keep = null;
  function apply() {
    const root = document.documentElement;
    if (!root) return;
    ensureSheet();
    const nextTarget = findTarget();
    if (nextTarget !== target) {
      document.querySelectorAll('.wxl-theater-target').forEach(e => mark(e, 'wxl-theater-target', false));
      document.querySelectorAll('.wxl-theater-chain').forEach(e => mark(e, 'wxl-theater-chain', false));
      target = nextTarget;
    }
    if (target) {
      mark(target, 'wxl-theater-target', true);
      for (let e = target.parentElement; e && e !== root; e = e.parentElement) mark(e, 'wxl-theater-chain', true);
      const nextKeep = playerBranch(target);
      if (nextKeep !== keep) { if (keep) mark(keep, 'wxl-theater-keep', false); keep = nextKeep; }
      if (keep) mark(keep, 'wxl-theater-keep', true);
    }
    const on = enabled && !!target && !!keep;
    mark(root, 'wxl-theater', on);
    if (on && (window.scrollX || window.scrollY)) window.scrollTo(0, 0);
  }

  // The panel's controls.
  const control = { held: false, volume: null, player: null };
  const cyPlayer = () => { try { const p = window.PLAYER; return p && typeof p === 'object' ? p : null; } catch (_) { return null; } };
  const media = () => target ? Array.from(target.querySelectorAll('video, audio')) : [];
  const youtubeFrames = () => target ? Array.from(target.querySelectorAll('iframe')).filter(f => /youtube(-nocookie)?\.com\/embed\//.test(f.src || '')) : [];
  const ytCommand = (func, args) => youtubeFrames().forEach(f => {
    try { f.contentWindow.postMessage(JSON.stringify({ event: 'command', func, args: args || [] }), '*'); } catch (_) {}
  });
  const call = (p, name, ...args) => { try { if (p && typeof p[name] === 'function') { p[name](...args); return true; } } catch (_) {} return false; };

  function holdable(p) {
    if (!p || p.__wxlHoldable) return;
    const play = p.play;
    if (typeof play === 'function') p.play = function () { if (control.held) return undefined; return play.apply(this, arguments); };
    try { p.__wxlHoldable = true; } catch (_) {}
  }
  function enforce() {
    const p = cyPlayer();
    if (p !== control.player) {
      control.player = p;
      holdable(p);
      if (p && control.held) call(p, 'pause');
      if (p && control.volume !== null) call(p, 'setVolume', control.volume);
    }
    if (control.held) media().forEach(m => { if (!m.paused) m.pause(); });
    if (control.volume !== null) {
      const v = control.volume;
      media().forEach(m => { if (Math.abs(m.volume - v) > 0.005) m.volume = v; });
      if (p && typeof p.getVolume === 'function')
        call(p, 'getVolume', got => { if (typeof got === 'number' && Math.abs(got - v) > 0.01) call(p, 'setVolume', v); });
    }
  }

  window.wxlPlayerCommand = command => {
    const p = cyPlayer();
    if (command === 'play') {
      control.held = false;
      if (!call(p, 'play')) { media().forEach(m => { const r = m.play(); if (r && r.catch) r.catch(() => {}); }); ytCommand('playVideo'); }
      return true;
    }
    if (command === 'pause' || command === 'stop') {
      control.held = true;
      if (!call(p, 'pause')) { media().forEach(m => m.pause()); ytCommand('pauseVideo'); }
      return true;
    }
    return false;
  };
  window.wxlSetVolume = value => {
    const percent = Math.max(0, Math.min(100, Number(value) || 0));
    const v = percent / 100;
    control.volume = v;
    try { window.VOLUME = v; } catch (_) {}
    const p = cyPlayer();
    if (!call(p, 'setVolume', v)) ytCommand('setVolume', [percent]);
    media().forEach(m => { m.volume = v; });
    return percent;
  };
  window.wxlLoadFromNative = raw => {
    const text = String(raw || '').trim();
    let url = null;
    try { url = new URL(text); } catch (_) {}
    if (url && (url.protocol === 'https:' || url.protocol === 'http:') && isChannel(url)) {
      if (!url.hash) url.hash = 'wxl-cinema';
      location.assign(url.href);
    } else {
      location.assign('https://wxl-video.local/player.html?load=' + encodeURIComponent(text));
    }
    return true;
  };
  window.wxlTheater = on => { enabled = !!on; apply(); return enabled; };

  let pending = false;
  const schedule = () => { if (pending) return; pending = true; setTimeout(() => { pending = false; apply(); enforce(); }, 50); };
  new MutationObserver(schedule).observe(document, { childList: true, subtree: true });
  setInterval(() => { apply(); enforce(); }, 1000);
  schedule();
})();)JS";

    /// The script with the hosts filled in, as WebView2 wants it.
    inline std::wstring CytubeTheaterScript()
    {
        std::string script = kCytubeTheaterScript;
        const std::string marker = "__WXL_CYTUBE_HOSTS__";
        const size_t at = script.find(marker);
        if (at != std::string::npos) script.replace(at, marker.size(), kCytubeHostsJson);
        return std::wstring(script.begin(), script.end()); // ASCII
    }
}
