// The depth-tested screen's pull toward the camera, in yards. No game state here: the offline harness includes this
// file as it is.
// Copyright (C) 2026 WarcraftXL contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <d3d9.h>

#include <algorithm>
#include <cmath>

namespace wxl_video_screen::screen_depth
{
    // How far "Characters and world appear in front" pulls the screen toward the camera: a few centimetres, enough
    // that the ground or snow it stands on can't show through at its bottom edge, and far too little to put the screen
    // in front of a character or prop that really is in front of it.
    constexpr float kDefaultPullYards = 0.05f;
    constexpr float kMaxPullYards = 0.15f;

    // The saved setting keeps the old slider's value and units ("Depth offset", -0.030..0.005, default -0.010; a
    // depth bias), so older builds read the same file. Pull in yards = -5 x that value: the old default -0.010 is the
    // new default 0.05 yd, -0.030 is 0.15 yd, zero or a positive value (which pushed the screen back) is no pull.
    //
    // Why not the old value as it was: in 318c235 the screen set it as D3DRS_DEPTHBIAS, and wxl-modern-water's
    // SetRenderState hook wrote that bias back for the whole world pass after the shadow maps (aea7d08 says how), so
    // the characters were drawn 0.010 nearer too and the offset between them and the screen came to nothing. aea7d08
    // put the same 0.010 into the screen's projection alone: with the world's near plane of 0.2 yd and depth range
    // 0..0.94, a screen D yards away then tested as if D / (1 + 0.053 D) away (10 yd -> 6.5, 18.5 yd -> 9.3), in front
    // of the character between the camera and the screen.
    constexpr float kYardsPerLegacyUnit = -5.0f;

    inline float PullYardsFromLegacy(float legacy)
    {
        if (!std::isfinite(legacy)) return kDefaultPullYards;
        return std::clamp(legacy * kYardsPerLegacyUnit, 0.0f, kMaxPullYards);
    }

    inline float LegacyFromPullYards(float yards)
    {
        if (!std::isfinite(yards)) yards = kDefaultPullYards;
        return std::clamp(yards, 0.0f, kMaxPullYards) / kYardsPerLegacyUnit;
    }

    // The scale about the eye that brings a point `distance` yards away `pull` yards nearer along its own ray (1: no
    // pull, also when the eye is almost at the screen).
    inline float PullScale(float distance, float pull)
    {
        if (!std::isfinite(distance) || !std::isfinite(pull) || !(pull > 0.0f) || !(distance > 2.0f * pull))
            return 1.0f;
        return 1.0f - pull / distance;
    }

    // View space scaled about the eye ahead of the (row-vector) projection, i.e. its first three rows times `scale`.
    // The world's projection has no translation in x, y or w (its fourth row is 0, 0, depth, 0), so every point of
    // the screen moves along its own ray: the same pixels, the same texture mapping, the same clipping, and the depth
    // of a point `scale` times as far. Not a depth bias, which other modules hook (see VideoSurface.cpp).
    inline void PullTowardEye(D3DMATRIX& projection, float scale)
    {
        if (!(scale > 0.0f) || scale == 1.0f) return;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
                projection.m[row][column] *= scale;
    }
}
