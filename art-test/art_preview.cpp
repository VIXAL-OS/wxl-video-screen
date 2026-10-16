// Renders the Stonewall TV sign, writes it as stonewall-sign.bin (what the extension ships and loads), and previews it with
// glitter on the rainbow, blended as in game (premultiplied: ONE, INVSRCALPHA), over night and day: sign-preview.bmp.
// Usage: art_preview <output dir>
#include "StonewallArt.hpp"
#include <cstdio>
#include <string>

namespace art = wxl_video_screen::art;

struct Image
{
    int w, h;
    std::vector<float> rgb;
    Image(int w_, int h_) : w(w_), h(h_), rgb(size_t(w_) * h_ * 3, 0.0f) {}
    void Fill(int y0, int y1, const float c[3])
    {
        for (int y = y0; y < y1; ++y) for (int x = 0; x < w; ++x) for (int k = 0; k < 3; ++k) rgb[(size_t(y) * w + x) * 3 + k] = c[k];
    }
    void Blend(const std::vector<uint32_t>& tex, int tw, int th, int x0, int y0, int dw, int dh, float level = 1.0f)
    {
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x)
            {
                const int px = x0 + x, py = y0 + y;
                if (px < 0 || py < 0 || px >= w || py >= h) continue;
                const int tx = std::min(tw - 1, int((x + 0.5f) / dw * tw)), ty = std::min(th - 1, int((y + 0.5f) / dh * th));
                const uint32_t s = tex[size_t(ty) * tw + tx];
                const float a = (s >> 24) / 255.0f * level;
                for (int k = 0; k < 3; ++k)
                {
                    const float c = ((s >> (16 - 8 * k)) & 255) / 255.0f * level;
                    float& d = rgb[(size_t(py) * w + px) * 3 + k];
                    d = std::min(1.0f, c + d * (1.0f - a));
                }
            }
    }
    void Save(const std::string& path) const
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return;
        uint8_t header[54] = { 'B', 'M' };
        auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; ++i) header[at + i] = uint8_t(v >> (8 * i)); };
        put32(2, 54 + w * h * 4); put32(10, 54); put32(14, 40); put32(18, w); put32(22, uint32_t(-h));
        header[26] = 1; header[28] = 32;
        std::fwrite(header, 1, 54, f);
        for (size_t i = 0; i < size_t(w) * h; ++i)
        {
            const uint8_t px[4] = { uint8_t(rgb[i * 3 + 2] * 255), uint8_t(rgb[i * 3 + 1] * 255), uint8_t(rgb[i * 3] * 255), 255 };
            std::fwrite(px, 1, 4, f);
        }
        std::fclose(f);
    }
};

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    const art::Sign sign = art::MakeSign(1024, 256);
    const std::wstring bin = std::wstring(dir.begin(), dir.end()) + L"/stonewall-sign.bin";
    if (!art::SaveSign(bin.c_str(), sign)) { std::printf("could not write the sign\n"); return 1; }
    art::Sign loaded;
    if (!art::LoadSign(bin.c_str(), loaded) || loaded.pixels != sign.pixels || loaded.tv != sign.tv) { std::printf("the sign didn't read back\n"); return 2; }
    const auto sparkle = art::SparklePixels(64);
    const float night[3] = { 0.10f, 0.086f, 0.19f }, day[3] = { 0.62f, 0.76f, 0.90f };
    const int sw = 1000, sh = 250;
    Image img(sw + 40, (sh + 20) * 2);
    img.Fill(0, sh + 20, night);
    img.Fill(sh + 20, (sh + 20) * 2, day);
    for (int pass = 0; pass < 2; ++pass)
    {
        const int oy = pass * (sh + 20) + 10;
        img.Blend(sign.pixels, sign.w, sign.h, 20, oy, sw, sh);
        img.Blend(sign.tv, sign.w, sign.h, 20, oy, sw, sh);
        // Glitter on the visible arc, as the extension scatters it (a fixed sample here).
        const float samples[7][3] = { { 0.1f, 0.3f, 1.0f }, { 0.3f, 0.7f, 0.7f }, { 0.45f, 0.5f, 0.9f }, { 0.6f, 0.85f, 0.5f },
                                      { 0.75f, 0.4f, 1.0f }, { 0.9f, 0.2f, 0.6f }, { 0.97f, 0.6f, 0.8f } };
        for (const auto& s : samples)
        {
            const float r = sign.arcInner + (sign.arcOuter - sign.arcInner) * s[1];
            const float angle = sign.arcEnd + (3.14159265f - sign.arcEnd) * s[0];
            const float u = sign.arcU + std::cos(angle) * r * sign.h / sign.w, v = sign.arcV - std::sin(angle) * r;
            const int size = int(sh * 0.16f * (0.5f + 0.5f * s[2]));
            img.Blend(sparkle, 64, 64, 20 + int(u * sw) - size / 2, oy + int(v * sh) - size / 2, size, size, s[2]);
        }
    }
    img.Save(dir + "/sign-preview.bmp");
    std::printf("ok split %.3f arc (%.3f, %.3f) r %.3f..%.3f behind the S from %.2f rad, centre %.3f\n", sign.split, sign.arcU,
                sign.arcV, sign.arcInner, sign.arcOuter, sign.arcEnd, sign.centerU);
    return 0;
}
