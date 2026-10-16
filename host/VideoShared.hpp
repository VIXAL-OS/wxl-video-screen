#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace wxl_video_shared
{
    // Stonewall TV build: 1280x720 frames (stock: 640x360), under names of its own. The stock names are shared with
    // AzerothPlex (its frames and control blocks are named WXLVideoCinema*-v1 too), so with both running each read
    // the other's; and an old helper with this extension (or the other way round) never opens a mapping of another size.
    constexpr wchar_t kMappingName[] = L"Local\\WXLStonewallTvFrames-v1";
    constexpr wchar_t kControlMappingName[] = L"Local\\WXLStonewallTvControl-v1";
    constexpr uint32_t kMagic = 0x46565857u; // "WXVF"
    constexpr uint32_t kVersion = 2;
    constexpr uint32_t kWidth = 1280;
    constexpr uint32_t kHeight = 720;
    constexpr uint32_t kStride = kWidth * 4;
    constexpr uint32_t kFrameBytes = kStride * kHeight;
    constexpr uint32_t kControlMagic = 0x43565857u; // "WXVC"
    constexpr uint32_t kControlVersion = 1;
    constexpr uint32_t kUrlBytes = 1024;

    enum class Command : LONG
    {
        None = 0,
        LoadAndPlay = 1,
        Play = 2,
        Pause = 3,
        Stop = 4,
    };

#pragma pack(push, 1)
    struct FrameHeader
    {
        uint32_t magic;
        uint32_t version;
        uint32_t headerBytes;
        uint32_t width;
        uint32_t height;
        uint32_t stride;
        uint32_t frameBytes;
        volatile LONG activeIndex;
        volatile LONG sequence;
        volatile LONG slotSequence[2];
        volatile LONG hostPid;
        volatile LONG ready;
        uint32_t reserved[3];
    };
#pragma pack(pop)

    static_assert(sizeof(FrameHeader) == 64, "shared video header ABI changed");

    struct ControlBlock
    {
        uint32_t magic;
        uint32_t version;
        uint32_t structBytes;
        volatile LONG commandSequence;
        volatile LONG command;
        volatile LONG volumeSequence;
        volatile LONG volumePercent;
        char url[kUrlBytes];
    };

    static_assert(sizeof(ControlBlock) == 1052, "shared video control ABI changed");

    constexpr size_t kMappingBytes = sizeof(FrameHeader) + size_t(kFrameBytes) * 2u;

    inline uint8_t* Pixels(void* mapping, unsigned index)
    {
        return static_cast<uint8_t*>(mapping) + sizeof(FrameHeader) +
               size_t(index & 1u) * kFrameBytes;
    }

    inline const uint8_t* Pixels(const void* mapping, unsigned index)
    {
        return static_cast<const uint8_t*>(mapping) + sizeof(FrameHeader) +
               size_t(index & 1u) * kFrameBytes;
    }
}
