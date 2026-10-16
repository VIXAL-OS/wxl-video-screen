// Stonewall TV's art, drawn on the CPU as premultiplied BGRA (for ONE, INVSRCALPHA blending: where the colour exceeds the
// alpha, the light adds, which is what makes it glow). Its own header so a tool can render it outside the game.
// - the sign: a glowing rainbow arc that passes behind the S of "Stonewall" (hollow neon tubes,
//   Harlow Solid Italic, pink), then "TV" (neon tubes, Mistral, cyan). Both fonts come with Microsoft Office, not with
//   Windows, so the sign is rendered once and shipped as stonewall-sign.bin (SaveSign/LoadSign); rendered at run time
//   only when that file is missing (then with whatever fonts Windows substitutes);
// - a glitter sparkle (pure light: alpha 0), scattered over the rainbow's band in game.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace wxl_video_screen::art
{
    inline float Smooth(float edge0, float edge1, float x)
    {
        const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    inline uint32_t Premultiplied(float r, float g, float b, float a)
    {
        const auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return (byte(a) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
    }

    // Three box blurs (about a Gaussian), radius r, on a w x h mask, in place.
    inline void Blur(std::vector<float>& m, int w, int h, int r)
    {
        if (r <= 0) return;
        std::vector<float> t(m.size());
        for (int pass = 0; pass < 3; ++pass)
        {
            for (int y = 0; y < h; ++y)
            {
                float sum = 0.0f;
                for (int x = -r; x <= r; ++x) sum += m[size_t(y) * w + std::clamp(x, 0, w - 1)];
                for (int x = 0; x < w; ++x)
                {
                    t[size_t(y) * w + x] = sum / (2 * r + 1);
                    sum += m[size_t(y) * w + std::min(x + r + 1, w - 1)] - m[size_t(y) * w + std::max(x - r, 0)];
                }
            }
            for (int x = 0; x < w; ++x)
            {
                float sum = 0.0f;
                for (int y = -r; y <= r; ++y) sum += t[size_t(std::clamp(y, 0, h - 1)) * w + x];
                for (int y = 0; y < h; ++y)
                {
                    m[size_t(y) * w + x] = sum / (2 * r + 1);
                    sum += t[size_t(std::min(y + r + 1, h - 1)) * w + x] - t[size_t(std::max(y - r, 0)) * w + x];
                }
            }
        }
    }

    // A GDI canvas k times the target size (GDI's shapes aren't antialiased: drawn big, then averaged down).
    class Canvas
    {
    public:
        Canvas(int w, int h) : w_(w), h_(h)
        {
            dc_ = CreateCompatibleDC(nullptr);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = w;
            info.bmiHeader.biHeight = -h; // top-down
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            bitmap_ = dc_ ? CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0) : nullptr;
            if (bitmap_) old_ = SelectObject(dc_, bitmap_);
            Clear();
        }
        ~Canvas()
        {
            if (old_) SelectObject(dc_, old_);
            if (bitmap_) DeleteObject(bitmap_);
            if (dc_) DeleteDC(dc_);
        }
        Canvas(const Canvas&) = delete;
        Canvas& operator=(const Canvas&) = delete;
        bool Ok() const { return dc_ && bitmap_ && bits_; }
        HDC Dc() const { return dc_; }
        void Clear() { if (bits_) std::memset(bits_, 0, size_t(w_) * h_ * 4); }
        // The drawing (white on black) averaged down k x k into a w/k x h/k coverage mask.
        std::vector<float> Mask(int k) const
        {
            GdiFlush();
            const int w = w_ / k, h = h_ / k;
            std::vector<float> m(size_t(w) * h, 0.0f);
            if (!bits_) return m;
            const auto* p = static_cast<const uint8_t*>(bits_);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    float sum = 0.0f;
                    for (int dy = 0; dy < k; ++dy)
                        for (int dx = 0; dx < k; ++dx) sum += p[(size_t(y * k + dy) * w_ + x * k + dx) * 4 + 1]; // green
                    m[size_t(y) * w + x] = sum / (255.0f * k * k);
                }
            return m;
        }

    private:
        int w_, h_;
        HDC dc_ = nullptr;
        HBITMAP bitmap_ = nullptr;
        HGDIOBJ old_ = nullptr;
        void* bits_ = nullptr;
    };

    // One element's light: a tube (pale along its middle, its colour at its edges) and its glow, added onto `rgb`; its
    // coverage onto `alpha` (less than its light: the glow adds).
    inline void AddTube(const std::vector<float>& coverage, int w, int h, const float tint[3], std::vector<float>& rgb,
                        std::vector<float>& alpha)
    {
        std::vector<float> core = coverage, glow = coverage, halo = coverage;
        Blur(core, w, h, 1);
        Blur(glow, w, h, 4);
        Blur(halo, w, h, 14);
        for (size_t i = 0; i < coverage.size(); ++i)
        {
            const float c = coverage[i], hot = Smooth(0.55f, 0.95f, core[i]);
            const float g = std::min(1.0f, glow[i] * 1.7f), o = std::min(1.0f, halo[i] * 2.4f);
            for (int k = 0; k < 3; ++k)
                rgb[i * 3 + k] += tint[k] * (c * 1.05f + g * 0.85f + o * 0.6f) + hot * (0.26f - 0.18f * tint[k]);
            alpha[i] = std::max(alpha[i], std::min(1.0f, c * 0.85f + g * 0.3f + o * 0.12f));
        }
    }

    struct Sign
    {
        int w = 0, h = 0;
        std::vector<uint32_t> pixels;     // w x h, premultiplied BGRA: the rainbow and "Stonewall"
        std::vector<uint32_t> tv;         // w x h: "TV", on its own layer (it flickers on its own; its T overlaps the l)
        float split = 0.75f;              // (unused since v3: "TV" has its own layer)
        float arcU = 0.0f, arcV = 0.0f;   // the rainbow arc's centre (u, v)
        float arcInner = 0.0f, arcOuter = 0.0f; // its band's radii, in texture heights (pixels are square)
        float arcEnd = 1.5708f;           // the angle (radians; pi = its left foot) where it goes behind the S
        float centerU = 0.5f;             // the middle of the whole design (rainbow to "TV"), u
    };

    // The sign, w x h (4:1).
    inline Sign MakeSign(int w, int h)
    {
        constexpr int kSuper = 3;
        static const float kPink[3] = { 1.00f, 0.16f, 0.62f }, kCyan[3] = { 0.12f, 0.86f, 1.00f };
        static const float kStripes[6][3] = {
            { 0.894f, 0.012f, 0.012f }, { 1.000f, 0.549f, 0.000f }, { 1.000f, 0.929f, 0.000f },
            { 0.000f, 0.502f, 0.149f }, { 0.000f, 0.302f, 1.000f }, { 0.459f, 0.027f, 0.529f } };
        Sign sign;
        sign.w = w; sign.h = h;
        sign.pixels.assign(size_t(w) * h, 0);
        Canvas canvas(w * kSuper, h * kSuper);
        if (!canvas.Ok()) return sign;
        HDC dc = canvas.Dc();
        const int W = w * kSuper, H = h * kSuper;
        const wchar_t kWord[] = L"Stonewall", kGap[] = L" ", kTv[] = L"TV";

        // The two fonts on one baseline; together with room for the rainbow (about half the height) as wide as fits
        // (86% of the width).
        float scale = 1.0f;
        const auto harlow = [&] { return CreateFontW(-static_cast<int>(H * 0.58f * scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                                     DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                                     VARIABLE_PITCH | FF_DECORATIVE, L"Harlow Solid Italic"); };
        const auto mistral = [&] { return CreateFontW(-static_cast<int>(H * 0.7f * scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                                      DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                                      VARIABLE_PITCH | FF_SCRIPT, L"Mistral"); };
        SIZE word{}, gap{}, tv{};
        const auto measure = [&] {
            HFONT a = harlow(), b = mistral();
            HGDIOBJ old = SelectObject(dc, a);
            GetTextExtentPoint32W(dc, kWord, lstrlenW(kWord), &word);
            GetTextExtentPoint32W(dc, kGap, lstrlenW(kGap), &gap);
            SelectObject(dc, b);
            GetTextExtentPoint32W(dc, kTv, lstrlenW(kTv), &tv);
            SelectObject(dc, old);
            DeleteObject(a);
            DeleteObject(b);
        };
        measure();
        const auto total = [&] { return static_cast<float>(word.cx + gap.cx + tv.cx) + H * 0.5f * scale; };
        if (total() > W * 0.86f)
        {
            scale *= W * 0.86f / total();
            measure();
        }
        const int x0 = static_cast<int>((W - total()) * 0.5f + H * 0.5f * scale), baseline = static_cast<int>(H * 0.72f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        SetTextAlign(dc, TA_BASELINE | TA_LEFT);
        const auto inkColumns = [&](const std::vector<float>& m, int& first, int& last) {
            first = w; last = -1;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (m[size_t(y) * w + x] > 0.3f) { first = std::min(first, x); last = std::max(last, x); }
        };

        // "Stonewall": hollow tubes along the letters' outlines (a stroked path; round joins).
        HFONT font = harlow();
        HGDIOBJ oldFont = SelectObject(dc, font);
        LOGBRUSH brush{ BS_SOLID, RGB(255, 255, 255), 0 };
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                                std::max(2, static_cast<int>(H * 0.58f * scale * 0.04f)), &brush, 0, nullptr);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        BeginPath(dc);
        TextOutW(dc, x0, baseline, kWord, lstrlenW(kWord));
        EndPath(dc);
        StrokePath(dc);
        const std::vector<float> word1 = canvas.Mask(kSuper);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        // The S's body, filled (and grown a little, under its tube): what the rainbow passes behind.
        canvas.Clear();
        TextOutW(dc, x0, baseline, L"S", 1);
        std::vector<float> sBody = canvas.Mask(kSuper);
        Blur(sBody, w, h, 2);
        for (float& v : sBody) v = Smooth(0.05f, 0.3f, v);
        // Its holes too (its top loop): whatever the outside can't reach is the S (flood fill from the edges).
        {
            std::vector<uint8_t> outside(size_t(w) * h, 0);
            std::vector<int> stack;
            const auto push = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= w || y >= h) return;
                const size_t q = size_t(y) * w + x;
                if (outside[q] || sBody[q] >= 0.5f) return;
                outside[q] = 1;
                stack.push_back(static_cast<int>(q));
            };
            for (int x = 0; x < w; ++x) { push(x, 0); push(x, h - 1); }
            for (int y = 0; y < h; ++y) { push(0, y); push(w - 1, y); }
            while (!stack.empty())
            {
                const int q = stack.back();
                stack.pop_back();
                const int x = q % w, y = q / w;
                push(x + 1, y); push(x - 1, y); push(x, y + 1); push(x, y - 1);
            }
            for (size_t q = 0; q < sBody.size(); ++q)
                if (!outside[q]) sBody[q] = 1.0f;
        }
        SelectObject(dc, oldFont);
        DeleteObject(font);

        // "TV" (filled tubes: Mistral's strokes are already tube-thin), placed so the top of the last l nestles in the T's
        // left joint: just left of the T's stem, under its crossbar.
        font = mistral();
        oldFont = SelectObject(dc, font);
        int wordFirst, wordLast, tvFirst, tvLast;
        inkColumns(word1, wordFirst, wordLast);
        int tvX = x0 + static_cast<int>(word.cx + gap.cx);
        canvas.Clear();
        TextOutW(dc, tvX, baseline, kTv, lstrlenW(kTv));
        inkColumns(canvas.Mask(kSuper), tvFirst, tvLast);
        {
            const std::vector<float> trial = canvas.Mask(kSuper);
            // The last l's top: the highest ink in the rightmost 8% of "Stonewall".
            int lx = -1, ly = h;
            for (int y = 0; y < h && lx < 0; ++y)
                for (int x = wordLast; x >= wordLast - (wordLast - wordFirst) * 8 / 100 && x >= 0; --x)
                    if (word1[size_t(y) * w + x] > 0.3f) { lx = x; ly = y; break; }
            // The T's stem: below its crossbar (35% down the letters), the leftmost ink in TV's left half.
            int tvTop = h, tvBottom = -1;
            for (int y = 0; y < h; ++y)
                for (int x = tvFirst; x <= tvLast; ++x)
                    if (trial[size_t(y) * w + x] > 0.3f) { tvTop = std::min(tvTop, y); tvBottom = std::max(tvBottom, y); }
            int stem = -1;
            if (tvBottom > tvTop)
            {
                const int row = tvTop + (tvBottom - tvTop) * 35 / 100;
                for (int y = std::max(0, row - 2); y <= std::min(h - 1, row + 2); ++y)
                    for (int x = tvFirst; x <= (tvFirst + tvLast) / 2; ++x)
                        if (trial[size_t(y) * w + x] > 0.3f) { stem = stem < 0 ? x : std::min(stem, x); break; }
            }
            const int margin = std::max(1, h * 12 / 1000) + std::max(1, h * 20 / 1000); // the whole l shows ("like 5 pixels to the right", the user)
            if (lx >= 0 && stem >= 0) tvX += ((lx + margin) - stem) * kSuper;
            else if (tvLast >= tvFirst && wordLast >= wordFirst) tvX += (wordLast + 4 - tvFirst) * kSuper;
            (void)ly;
        }
        canvas.Clear();
        TextOutW(dc, tvX, baseline, kTv, lstrlenW(kTv));
        const std::vector<float> word2 = canvas.Mask(kSuper);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        inkColumns(word2, tvFirst, tvLast);
        sign.split = (wordLast + tvFirst) * 0.5f / w;

        // Where the S is: the first column with ink, and the word's ink rows.
        int sLeft = w, inkTop = h, inkBottom = -1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (word1[size_t(y) * w + x] > 0.3f)
                {
                    sLeft = std::min(sLeft, x);
                    inkTop = std::min(inkTop, y);
                    inkBottom = std::max(inkBottom, y);
                }
        std::vector<float> rainbowRgb(size_t(w) * h * 3, 0.0f), rainbowA(size_t(w) * h, 0.0f);
        int rainbowLeft = sLeft;
        if (inkBottom > inkTop)
        {
            // The rainbow: an arc centred on the baseline a little right of the S's left edge, its top at the S's top. It
            // rises from its left foot, over its top, and on down behind the S, whose body hides it; it ends (softly)
            // inside the S at 45 degrees.
            const float outer = (baseline / static_cast<float>(kSuper) - inkTop) * 1.02f, inner = outer * 0.48f;
            const float cy = baseline / static_cast<float>(kSuper), cx = sLeft + outer * 0.1f;
            constexpr float kEnd = 0.7854f;
            sign.arcU = cx / w; sign.arcV = cy / h;
            sign.arcInner = inner / h; sign.arcOuter = outer / h;
            // Where it goes behind the S (the band's middle meets the S's left edge): glitter stays on the visible part.
            const float middle = (inner + outer) * 0.5f;
            sign.arcEnd = std::acos(std::clamp((sLeft - cx) / middle, -1.0f, 1.0f));
            rainbowLeft = static_cast<int>(cx - outer);
            std::vector<float> band(size_t(w) * h * 3, 0.0f), cover(size_t(w) * h, 0.0f);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const float px = x + 0.5f, py = y + 0.5f;
                    if (py > cy) continue;
                    const float r = std::sqrt((cx - px) * (cx - px) + (cy - py) * (cy - py));
                    const float angle = std::atan2(cy - py, px - cx); // 0 right, pi left
                    const size_t q = size_t(y) * w + x;
                    // Soft edges, a soft foot at the baseline and a soft end; hidden where the S's body is.
                    const float c = Smooth(inner - 0.8f, inner + 0.8f, r) * (1.0f - Smooth(outer - 0.8f, outer + 0.8f, r)) *
                                    Smooth(0.0f, outer * 0.08f, cy - py) * Smooth(kEnd, kEnd + 0.12f, angle) * (1.0f - sBody[q]);
                    if (c <= 0.0f) continue;
                    const float stripe = std::clamp((outer - r) / (outer - inner) * 6.0f, 0.0f, 5.999f);
                    const int i = static_cast<int>(stripe), j = std::min(i + 1, 5);
                    const float seam = Smooth(0.88f, 1.0f, stripe - i);
                    for (int k = 0; k < 3; ++k) band[q * 3 + k] = (kStripes[i][k] + (kStripes[j][k] - kStripes[i][k]) * seam) * c;
                    cover[q] = c;
                }
            // Its glow: each colour channel blurred, so every stripe glows in its own colour.
            const int radius = std::max(1, h / 40);
            std::vector<float> glow[3];
            for (int k = 0; k < 3; ++k)
            {
                glow[k].resize(cover.size());
                for (size_t q = 0; q < cover.size(); ++q) glow[k][q] = band[q * 3 + k];
                Blur(glow[k], w, h, radius);
            }
            std::vector<float> glowCover = cover;
            Blur(glowCover, w, h, radius);
            for (size_t q = 0; q < cover.size(); ++q)
            {
                for (int k = 0; k < 3; ++k) rainbowRgb[q * 3 + k] = std::min(1.0f, band[q * 3 + k] * 0.95f + glow[k][q] * 0.85f);
                rainbowA[q] = std::min(1.0f, cover[q] * 0.9f + glowCover[q] * 0.2f);
            }
        }
        sign.centerU = (rainbowLeft + (tvLast >= tvFirst ? tvLast : wordLast)) * 0.5f / w;

        // "TV" on its own layer; "Stonewall" over the rainbow (premultiplied "over").
        {
            std::vector<float> rgb(size_t(w) * h * 3, 0.0f), alpha(size_t(w) * h, 0.0f);
            AddTube(word2, w, h, kCyan, rgb, alpha);
            sign.tv.resize(size_t(w) * h);
            for (size_t q = 0; q < alpha.size(); ++q)
                sign.tv[q] = Premultiplied(rgb[q * 3], rgb[q * 3 + 1], rgb[q * 3 + 2], alpha[q]);
        }
        std::vector<float> rgb(size_t(w) * h * 3, 0.0f), alpha(size_t(w) * h, 0.0f);
        AddTube(word1, w, h, kPink, rgb, alpha);
        for (size_t q = 0; q < alpha.size(); ++q)
        {
            const float keep = 1.0f - alpha[q];
            sign.pixels[q] = Premultiplied(rgb[q * 3] + rainbowRgb[q * 3] * keep, rgb[q * 3 + 1] + rainbowRgb[q * 3 + 1] * keep,
                                           rgb[q * 3 + 2] + rainbowRgb[q * 3 + 2] * keep, alpha[q] + rainbowA[q] * keep);
        }
        return sign;
    }

    // stonewall-sign.bin: "SWTV", version 3, w, h, the Sign's seven floats, then two w x h premultiplied BGRA layers
    // (the rainbow and "Stonewall"; "TV").
    constexpr uint32_t kSignMagic = 0x56545753u, kSignVersion = 3u;
    inline bool SaveSign(const wchar_t* path, const Sign& sign)
    {
        FILE* f = _wfopen(path, L"wb");
        if (!f) return false;
        const uint32_t head[4] = { kSignMagic, kSignVersion, static_cast<uint32_t>(sign.w), static_cast<uint32_t>(sign.h) };
        const float values[7] = { sign.split, sign.arcU, sign.arcV, sign.arcInner, sign.arcOuter, sign.arcEnd, sign.centerU };
        const bool ok = std::fwrite(head, sizeof(head), 1, f) == 1 && std::fwrite(values, sizeof(values), 1, f) == 1 &&
                        std::fwrite(sign.pixels.data(), 4, sign.pixels.size(), f) == sign.pixels.size() &&
                        sign.tv.size() == sign.pixels.size() && std::fwrite(sign.tv.data(), 4, sign.tv.size(), f) == sign.tv.size();
        std::fclose(f);
        return ok;
    }
    inline bool LoadSign(const wchar_t* path, Sign& sign)
    {
        FILE* f = _wfopen(path, L"rb");
        if (!f) return false;
        uint32_t head[4]{};
        float values[7]{};
        bool ok = std::fread(head, sizeof(head), 1, f) == 1 && head[0] == kSignMagic && head[1] == kSignVersion &&
                  head[2] > 0 && head[2] <= 4096 && head[3] > 0 && head[3] <= 4096 && std::fread(values, sizeof(values), 1, f) == 1;
        if (ok)
        {
            sign.w = static_cast<int>(head[2]); sign.h = static_cast<int>(head[3]);
            sign.split = values[0]; sign.arcU = values[1]; sign.arcV = values[2];
            sign.arcInner = values[3]; sign.arcOuter = values[4]; sign.arcEnd = values[5]; sign.centerU = values[6];
            sign.pixels.resize(size_t(sign.w) * sign.h);
            sign.tv.resize(sign.pixels.size());
            ok = std::fread(sign.pixels.data(), 4, sign.pixels.size(), f) == sign.pixels.size() &&
                 std::fread(sign.tv.data(), 4, sign.tv.size(), f) == sign.tv.size();
        }
        std::fclose(f);
        return ok;
    }

    // A four-pointed glitter sparkle, s x s: pure light (alpha 0: it only adds).
    inline std::vector<uint32_t> SparklePixels(int s)
    {
        std::vector<uint32_t> out(size_t(s) * s);
        for (int y = 0; y < s; ++y)
            for (int x = 0; x < s; ++x)
            {
                const float dx = std::abs((x + 0.5f) / s * 2.0f - 1.0f), dy = std::abs((y + 0.5f) / s * 2.0f - 1.0f);
                const float rays = std::exp(-dx * 9.0f) * std::exp(-dy * 120.0f) + std::exp(-dy * 9.0f) * std::exp(-dx * 120.0f);
                const float core = std::exp(-(dx * dx + dy * dy) * 40.0f);
                const float edge = 1.0f - Smooth(0.85f, 1.0f, std::max(dx, dy));
                const float l = std::min(1.0f, (rays * 0.9f + core) * edge);
                out[size_t(y) * s + x] = Premultiplied(l, l * 0.97f, l * 0.92f, 0.0f);
            }
        return out;
    }
}
