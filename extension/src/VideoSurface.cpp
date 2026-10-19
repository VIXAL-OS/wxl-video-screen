// Placeable world-space WebView video surface for WarcraftXL build 12340.
// Copyright (C) 2026 WarcraftXL contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "VideoSurface.hpp"

#include "ScreenDepth.hpp"
#include "StonewallArt.hpp"
#include "VideoShared.hpp"

#include "game/Camera.hpp"
#include "game/Gfx.hpp"
#include "game/World.hpp"

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>

namespace wxl_video_screen
{
    VideoSurface::SceneClearFn VideoSurface::originalSceneClear_ = nullptr;

    namespace
    {
        constexpr char kTag[] = "wxl-video-screen";
        constexpr wchar_t kHostWindowClass[] = L"WXLVideoCinemaHost";
        constexpr UINT kShowHostMessage = WM_APP + 0x45;
        constexpr UINT kHideHostMessage = WM_APP + 0x46;
        constexpr float kPi = 3.14159265358979323846f;

        void ModuleAddressMarker() {}

        bool Finite3(const float value[3])
        {
            return std::isfinite(value[0]) && std::isfinite(value[1]) &&
                   std::isfinite(value[2]) && std::abs(value[0]) <= 65536.0f &&
                   std::abs(value[1]) <= 65536.0f && std::abs(value[2]) <= 65536.0f;
        }

        struct Vertex
        {
            float x, y, z;
            D3DCOLOR color;
            float u, v;
        };

        // The world pass's own projection and depth range (Living Azeroth verified both in the client, 2026-10-02; the
        // disassembly agrees):
        //  - the graphics device keeps the projection it was GIVEN at +0xF88 (what gfx::SceneMatrices returns) and the
        //    one it RENDERS with at +0xFC8: its set-projection routine 0x6A9B40 recomputes the depth terms (stored at
        //    0x6A9CED), and the shader system (0x872C10) hands the world that one. The same x and y, so the same place
        //    on screen; but a depth that isn't the world's, so the screen and the characters in front of it were
        //    compared by two different measures.
        //  - the world render (0x4F8EA0) sets viewport z = 0..[0xADEEE4] (a static 0.94) at 0x4F905A, before the scene
        //    render's own clear (0x79AB9A, where the pre-world screen is drawn), and puts 0..1 back before
        //    OnWorldSceneEnd. With 0..1 at the end of the scene everything is ~6% further than the world drawn there.
        constexpr size_t kDeviceRenderProjection = 0xFC8;
        constexpr uintptr_t kWorldPassMaxZ = 0x00ADEEE4;

        bool RenderProjection(float projection[16])
        {
            const void* graphics = wxl::game::gx::RawGraphicsDevice();
            if (!graphics) return false;
            std::memcpy(projection, static_cast<const uint8_t*>(graphics) + kDeviceRenderProjection, 16 * sizeof(float));
            for (int i = 0; i < 16; ++i)
                if (!std::isfinite(projection[i])) return false;
            return projection[11] != 0.0f; // a perspective (w = view z), as the world's always is
        }

        // A state block holding everything as it is now. CreateStateBlock(D3DSBT_ALL) alone records the vertex
        // shader's float constants c128..c255 and the pixel shader's c128..c223 wrong: Apply() then writes copies
        // of c0..c127 / c0..c95 there (this machine's D3D9 runtime, on the GeForce and the Radeon alike: scratchpad
        // vc-statetest, 2026-10-17; every other state comes back intact). Capture() right after records them
        // properly -- Dear ImGui's DX9 backend does the same (its issue #3857). Drawn before the world, the screen
        // left those registers wrong for the world pass: whatever was set there before it and not set again (the
        // client's model shaders read c224..c235, for one) read copies of the low registers instead.
        HRESULT CaptureState(IDirect3DDevice9* device, IDirect3DStateBlock9** state)
        {
            *state = nullptr;
            HRESULT result = device->CreateStateBlock(D3DSBT_ALL, state);
            if (SUCCEEDED(result) && *state && FAILED(result = (*state)->Capture()))
            {
                (*state)->Release();
                *state = nullptr;
            }
            return result;
        }

        float WorldPassMaxZ()
        {
            const float z = *reinterpret_cast<const volatile float*>(kWorldPassMaxZ);
            return std::isfinite(z) && z > 0.5f && z <= 1.0f ? z : 1.0f;
        }

        // Hooked calls.
        // What a state block can't put back for everyone else. wxl-modern-water patches the live device's SetRenderState
        // and SetDepthStencilSurface (its d3d9_wrap.cpp, "device adoption", for its shadow-map bias): it takes every
        // D3DRS_DEPTHBIAS and D3DRS_SLOPESCALEDEPTHBIAS that goes through SetRenderState for the client's own, raises the
        // bias while a shadow map is the depth target, and when another depth is bound writes "the client's" back. Apply()
        // restores states inside the runtime, not through the device's functions, so that hook never sees the restore.
        // The screen's -0.010 therefore came back for the world: drawn at the world pass's clear, ahead of the shadow maps
        // (CWorldScene::Render 0x79A870: the clear, CShadowQuery::Update, then the terrain), it was written back when the
        // world's depth was bound again after them, and the client (which re-sends nothing it believes is still set) drew
        // the terrain 0.01 nearer than it is -- Living Azeroth's snow cover, a few centimetres above the terrain with no
        // bias of its own, lost the depth test everywhere, and the shadow maps themselves were drawn 0.01 toward the light
        // (shadowed ground). At the end of the scene the Stonewall sign's own bias of 0 happened to write over it.
        // So nothing here sets a depth bias: the screen is pulled toward the camera in its projection, by yards
        // (ScreenDepth.hpp: the leaked bias had moved the characters with the screen, so the same 0.010 in the
        // projection alone put the screen in front of them), and depth writes are switched on only where the depth
        // test is (with depth buffering off nothing is written).

        IDirect3DTexture9* MakeTexture(IDirect3DDevice9* device, int w, int h, const std::vector<uint32_t>& pixels)
        {
            IDirect3DTexture9* texture = nullptr;
            if (FAILED(device->CreateTexture(w, h, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)) &&
                FAILED(device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
                return nullptr;
            D3DLOCKED_RECT locked{};
            if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) { texture->Release(); return nullptr; }
            for (int y = 0; y < h; ++y)
                std::memcpy(static_cast<uint8_t*>(locked.pBits) + size_t(y) * locked.Pitch, pixels.data() + size_t(y) * w, size_t(w) * 4);
            texture->UnlockRect(0);
            return texture;
        }
    }

    VideoSurface& VideoSurface::Instance()
    {
        static VideoSurface instance;
        return instance;
    }

    VideoSurface::VideoSurface()
    {
        on<&VideoSurface::OnFrame>(wxl::events::Event::OnFrame);
        on<&VideoSurface::OnWorldRender>(wxl::events::Event::OnWorldRender);
        on<&VideoSurface::OnWorldSceneEnd>(wxl::events::Event::OnWorldSceneEnd);
        on<&VideoSurface::OnUpdate>(wxl::events::Event::OnUpdate);
        on<&VideoSurface::OnInput>(wxl::events::Event::OnInput);
        on<&VideoSurface::OnDeviceLost>(wxl::events::Event::OnDeviceLost);
        on<&VideoSurface::OnDeviceReset>(wxl::events::Event::OnDeviceReset);
        on<&VideoSurface::OnWorldLeave>(wxl::events::Event::OnWorldLeave);
    }

    bool VideoSurface::Initialize(const WXL_Api* api)
    {
        if (!api || initialized_) return initialized_;
        api_ = api;
        frameScratch_.resize(wxl_video_shared::kFrameBytes);
        ConnectControl();
        LoadPlacement();
        if (!api_->HookAttach || !api_->HookAttach(
                "wxl-video-screen.scene-clear",
                wxl::game::gx::kSceneClearSeam,
                reinterpret_cast<void*>(&VideoSurface::SceneClearHook),
                reinterpret_cast<void**>(&originalSceneClear_),
                WXL_HOOK_DEFAULT_PRIORITY))
        {
            status_ = "Could not attach the pre-world screen renderer.";
            api_->Log(WXL_LOG_ERROR, kTag, "%s", status_.c_str());
            return false;
        }
        initialized_ = true;
        return true;
    }

    void __cdecl VideoSurface::SceneClearHook(uint32_t flags, uint32_t colour)
    {
        if (originalSceneClear_) originalSceneClear_(flags, colour);

        auto& self = Instance();
        // Only the world pass's own clear (some twenty callers clear colour and depth, shadow maps and portraits among
        // them): after OnWorldRender, into the depth surface the world is drawn with. Without that event (an older core)
        // the first such clear of the frame, as before.
        if (self.preWorldDrawn_ || !self.initialized_ || !self.depthTest_ || !self.visible_ ||
            !self.placed_ || (flags & (wxl::game::gx::clear::kColor |
                                      wxl::game::gx::clear::kDepth)) !=
                             (wxl::game::gx::clear::kColor |
                              wxl::game::gx::clear::kDepth) ||
            (self.worldRenderEvents_ && !self.gateOff_ && !self.inWorldPass_) ||
            wxl::game::world::CurrentMapId() != self.mapId_)
            return;

        auto* device = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        if (!device) return;

        IDirect3DSurface9* depth = nullptr;
        device->GetDepthStencilSurface(&depth);
        if (!self.worldRenderEvents_ || self.gateOff_ || (depth && depth == self.worldPassDepth_))
        {
            self.DrawWorldScreen(device, depth, true);
            self.preWorldDrawn_ = true;
        }
        if (depth) depth->Release();
    }

    void VideoSurface::OnFrame(const wxl::events::FrameArgs&)
    {
        preWorldDrawn_ = false;
    }

    void VideoSurface::OnWorldRender(const wxl::events::WorldRenderArgs& args)
    {
        ++worldRenderEvents_;
        inWorldPass_ = true;
        worldPassDepth_ = nullptr;
        if (auto* device = static_cast<IDirect3DDevice9*>(args.device))
        {
            IDirect3DSurface9* depth = nullptr;
            if (SUCCEEDED(device->GetDepthStencilSurface(&depth)) && depth)
            {
                worldPassDepth_ = depth; // compared by address only, never used
                depth->Release();
            }
        }
    }

    std::wstring VideoSurface::ExtensionDirectory() const
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&ModuleAddressMarker), &module))
            return {};

        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        if (!length || length >= MAX_PATH) return {};
        return std::filesystem::path(path).parent_path().wstring();
    }

    bool VideoSurface::OpenHost(bool showWindow)
    {
        if (HWND host = FindWindowW(kHostWindowClass, nullptr))
        {
            PostMessageW(host, showWindow ? kShowHostMessage : kHideHostMessage, 0, 0);
            return true;
        }

        const std::wstring directory = ExtensionDirectory();
        const std::wstring hostPath = directory.empty()
            ? std::wstring{} : (std::filesystem::path(directory) / L"wxl-video-host.exe").wstring();
        if (hostPath.empty() || GetFileAttributesW(hostPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            status_ = "Cinema helper is missing from the extension folder.";
            if (api_) api_->Log(WXL_LOG_ERROR, kTag, "%s", status_.c_str());
            return false;
        }

        std::wstring commandLine = L"\"" + hostPath + L"\" --parent-pid " +
                                   std::to_wstring(GetCurrentProcessId());
        if (!showWindow) commandLine += L" --background";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(hostPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                            CREATE_NEW_PROCESS_GROUP, nullptr, directory.c_str(), &startup, &process))
        {
            char error[128]{};
            std::snprintf(error, sizeof(error), "Could not start helper (Win32 error %lu).",
                          static_cast<unsigned long>(GetLastError()));
            status_ = error;
            if (api_) api_->Log(WXL_LOG_ERROR, kTag, "%s", status_.c_str());
            return false;
        }

        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        status_ = showWindow
            ? "Cinema troubleshooting window opened."
            : "Background cinema player started.";
        return true;
    }

    void VideoSurface::HideHost()
    {
        if (HWND host = FindWindowW(kHostWindowClass, nullptr))
        {
            PostMessageW(host, kHideHostMessage, 0, 0);
            status_ = "Cinema player is running in the background.";
        }
    }

    void VideoSurface::CloseHost()
    {
        if (HWND host = FindWindowW(kHostWindowClass, nullptr))
            PostMessageW(host, WM_CLOSE, 0, 0);
    }

    bool VideoSurface::ConnectControl()
    {
        if (controlView_)
        {
            const auto* control = static_cast<const wxl_video_shared::ControlBlock*>(controlView_);
            return control->magic == wxl_video_shared::kControlMagic &&
                   control->version == wxl_video_shared::kControlVersion &&
                   control->structBytes == sizeof(*control);
        }

        controlMapping_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(wxl_video_shared::ControlBlock)),
            wxl_video_shared::kControlMappingName);
        if (!controlMapping_) return false;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        controlView_ = MapViewOfFile(controlMapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                                     sizeof(wxl_video_shared::ControlBlock));
        if (!controlView_)
        {
            CloseHandle(controlMapping_);
            controlMapping_ = nullptr;
            return false;
        }

        auto* control = static_cast<wxl_video_shared::ControlBlock*>(controlView_);
        if (created)
        {
            std::memset(control, 0, sizeof(*control));
            control->magic = wxl_video_shared::kControlMagic;
            control->version = wxl_video_shared::kControlVersion;
            control->structBytes = sizeof(*control);
            control->volumePercent = masterVolume_;
        }
        if (control->magic != wxl_video_shared::kControlMagic ||
            control->version != wxl_video_shared::kControlVersion ||
            control->structBytes != sizeof(*control))
        {
            UnmapViewOfFile(controlView_);
            controlView_ = nullptr;
            CloseHandle(controlMapping_);
            controlMapping_ = nullptr;
            return false;
        }
        return true;
    }

    bool VideoSurface::SendCommand(LONG command)
    {
        if (!ConnectControl() || !OpenHost(false)) return false;
        auto* control = static_cast<wxl_video_shared::ControlBlock*>(controlView_);
        if (command == static_cast<LONG>(wxl_video_shared::Command::LoadAndPlay))
        {
            if (!urlInput_[0])
            {
                status_ = "Paste a YouTube URL or video ID first.";
                return false;
            }
            strncpy_s(control->url, sizeof(control->url), urlInput_, _TRUNCATE);
        }
        else if (command == static_cast<LONG>(wxl_video_shared::Command::Play))
        {
            // The link goes along: Play with nothing loaded yet loads it (the helper's player.html); an older helper
            // ignores it.
            strncpy_s(control->url, sizeof(control->url), urlInput_, _TRUNCATE);
        }
        InterlockedExchange(&control->command, command);
        MemoryBarrier();
        LONG sequence = control->commandSequence + 1;
        if (sequence <= 0) sequence = 1;
        InterlockedExchange(&control->commandSequence, sequence);
        status_ = "Command sent to the background cinema player.";
        return true;
    }

    void VideoSurface::SendVolume(int volume)
    {
        volume = std::clamp(volume, 0, 100);
        effectiveVolume_ = volume;
        if (!ConnectControl() || volume == lastSentVolume_) return;
        auto* control = static_cast<wxl_video_shared::ControlBlock*>(controlView_);
        InterlockedExchange(&control->volumePercent, volume);
        MemoryBarrier();
        LONG sequence = control->volumeSequence + 1;
        if (sequence <= 0) sequence = 1;
        InterlockedExchange(&control->volumeSequence, sequence);
        lastSentVolume_ = volume;
    }

    void VideoSurface::PasteUrlFromClipboard()
    {
        if (!OpenClipboard(nullptr))
        {
            status_ = "The Windows clipboard is busy; try Paste again.";
            return;
        }
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        const auto* wide = data ? static_cast<const wchar_t*>(GlobalLock(data)) : nullptr;
        if (!wide)
        {
            CloseClipboard();
            status_ = "Clipboard does not contain text.";
            return;
        }

        const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
                                                  nullptr, 0, nullptr, nullptr);
        if (required > 1 && required <= static_cast<int>(sizeof(urlInput_)))
        {
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, urlInput_,
                                static_cast<int>(sizeof(urlInput_)), nullptr, nullptr);
            size_t length = std::strlen(urlInput_);
            while (length && static_cast<unsigned char>(urlInput_[length - 1]) <= 0x20)
                urlInput_[--length] = '\0';
            status_ = "YouTube URL pasted. Select Load URL and play.";
        }
        else
        {
            status_ = "Clipboard text is empty, invalid, or too long.";
        }
        GlobalUnlock(data);
        CloseClipboard();
    }

    void VideoSurface::AppendUrlCharacter(wchar_t character)
    {
        const size_t length = std::strlen(urlInput_);
        if (character == L'\b')
        {
            if (!length) return;
            size_t start = length - 1;
            while (start && (static_cast<unsigned char>(urlInput_[start]) & 0xC0u) == 0x80u)
                --start;
            urlInput_[start] = '\0';
            return;
        }
        if (character < 0x20 || character == 0x7F) return;

        char encoded[5]{};
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, &character, 1,
                                               encoded, 4, nullptr, nullptr);
        if (bytes <= 0 || length + static_cast<size_t>(bytes) >= sizeof(urlInput_)) return;
        std::memcpy(urlInput_ + length, encoded, static_cast<size_t>(bytes));
        urlInput_[length + static_cast<size_t>(bytes)] = '\0';
    }

    bool VideoSurface::PlaceInFront()
    {
        namespace camera = wxl::game::camera;
        namespace world = wxl::game::world;

        const int map = world::CurrentMapId();
        const unsigned long long guid = world::ActivePlayerGuid();
        void* player = guid ? world::ResolveObject(guid, world::kTypeMaskPlayer) : nullptr;
        if (map < 0 || !player)
        {
            status_ = "Enter the world before placing the screen.";
            return false;
        }

        float playerPosition[3]{};
        float cameraPosition[3]{};
        world::UnitPosition(player, playerPosition);
        camera::GetPosition(cameraPosition);
        if (!Finite3(playerPosition) || !Finite3(cameraPosition))
        {
            status_ = "Player or camera position is unavailable.";
            return false;
        }

        // Use the engine's actual view direction.  Deriving it from camera -> player works only while
        // the stock third-person camera sits behind the character; orbit/front/free-camera views can
        // otherwise place the screen behind the visible scene.
        const float* view = camera::GetView();
        float forwardX = view ? view[2] : playerPosition[0] - cameraPosition[0];
        float forwardY = view ? view[6] : playerPosition[1] - cameraPosition[1];
        float length = std::sqrt(forwardX * forwardX + forwardY * forwardY);
        if (!(length > 0.001f) || !std::isfinite(length))
        {
            forwardX = playerPosition[0] - cameraPosition[0];
            forwardY = playerPosition[1] - cameraPosition[1];
            length = std::sqrt(forwardX * forwardX + forwardY * forwardY);
        }
        if (!(length > 0.001f) || !std::isfinite(length))
        {
            forwardX = 1.0f;
            forwardY = 0.0f;
            length = 1.0f;
        }
        forwardX /= length;
        forwardY /= length;

        right_[0] = -forwardY;
        right_[1] = forwardX;
        right_[2] = 0.0f;
        const float height = width_ * 9.0f / 16.0f;
        center_[0] = playerPosition[0] + forwardX * placeDistance_;
        center_[1] = playerPosition[1] + forwardY * placeDistance_;
        center_[2] = playerPosition[2] + bottomOffset_ + height * 0.5f;
        mapId_ = map;
        placed_ = true;
        visible_ = 1;
        SavePlacement();
        status_ = "World screen placed in front of your character.";
        if (api_)
            api_->Log(WXL_LOG_INFO, kTag, "placed world screen map=%d xyz=%.3f %.3f %.3f width=%.2f",
                      mapId_, center_[0], center_[1], center_[2], width_);
        return true;
    }

    void VideoSurface::Rotate(float degrees)
    {
        if (!placed_) return;
        const float radians = degrees * kPi / 180.0f;
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        const float x = right_[0] * cosine - right_[1] * sine;
        const float y = right_[0] * sine + right_[1] * cosine;
        right_[0] = x;
        right_[1] = y;
        SavePlacement();
    }

    void VideoSurface::SavePlacement()
    {
        if (!placed_) return;
        const std::wstring directory = ExtensionDirectory();
        if (directory.empty()) return;

        const std::filesystem::path target = std::filesystem::path(directory) / L"world-screen.tsv";
        const std::filesystem::path temporary = target.wstring() + L".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return;
        output.imbue(std::locale::classic());
        output << "wxl-video-screen-v1\t" << mapId_ << '\t'
               << std::setprecision(9) << center_[0] << '\t' << center_[1] << '\t' << center_[2] << '\t'
               << right_[0] << '\t' << right_[1] << '\t' << right_[2] << '\t'
               << width_ << '\t' << visible_ << '\t' << depthTest_ << '\t'
               << screen_depth::LegacyFromPullYards(depthPull_) << '\t' // the old field, its old units
               << stonewall_ << '\n';
        output.close();
        if (!output) return;
        MoveFileExW(temporary.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    void VideoSurface::LoadPlacement()
    {
        const std::wstring directory = ExtensionDirectory();
        if (directory.empty()) return;
        std::ifstream input(std::filesystem::path(directory) / L"world-screen.tsv");
        if (!input) return;
        input.imbue(std::locale::classic());

        std::string version;
        int map = -1;
        int visible = 1;
        int depth = 1;
        float center[3]{};
        float right[3]{};
        float width = 0.0f;
        // The old "Depth offset" field and units, read as a pull in yards (ScreenDepth.hpp): the old default -0.010 is
        // 0.05 yd.
        float legacyDepthOffset = screen_depth::LegacyFromPullYards(screen_depth::kDefaultPullYards);
        int stonewall = 0;
        if (!std::getline(input, version, '\t') || version != "wxl-video-screen-v1" ||
            !(input >> map) || input.get() != '\t' ||
            !(input >> center[0]) || input.get() != '\t' ||
            !(input >> center[1]) || input.get() != '\t' ||
            !(input >> center[2]) || input.get() != '\t' ||
            !(input >> right[0]) || input.get() != '\t' ||
            !(input >> right[1]) || input.get() != '\t' ||
            !(input >> right[2]) || input.get() != '\t' ||
            !(input >> width) || input.get() != '\t' ||
            !(input >> visible) || input.get() != '\t' || !(input >> depth))
            return;
        if (input.peek() == '\t')
        {
            input.get();
            if (!(input >> legacyDepthOffset))
                legacyDepthOffset = screen_depth::LegacyFromPullYards(screen_depth::kDefaultPullYards);
            else if (input.peek() == '\t')
            {
                input.get();
                if (!(input >> stonewall)) stonewall = 0;
            }
        }

        const float rightLength = std::sqrt(right[0] * right[0] + right[1] * right[1]);
        if (map < 0 || !Finite3(center) || !(rightLength > 0.9f && rightLength < 1.1f) ||
            !std::isfinite(width) || width < 2.0f || width > 30.0f)
            return;

        mapId_ = map;
        std::memcpy(center_, center, sizeof(center_));
        right_[0] = right[0] / rightLength;
        right_[1] = right[1] / rightLength;
        right_[2] = 0.0f;
        width_ = width;
        visible_ = visible ? 1 : 0;
        depthTest_ = depth ? 1 : 0;
        depthPull_ = screen_depth::PullYardsFromLegacy(legacyDepthOffset);
        stonewall_ = stonewall ? 1 : 0;
        placed_ = true;
        status_ = "Restored the saved world-screen placement.";
    }

    bool VideoSurface::ValidateFrames() const
    {
        if (!mappingView_) return false;
        const auto* header = static_cast<const wxl_video_shared::FrameHeader*>(mappingView_);
        return header->magic == wxl_video_shared::kMagic &&
               header->version == wxl_video_shared::kVersion &&
               header->headerBytes == sizeof(*header) &&
               header->width == wxl_video_shared::kWidth &&
               header->height == wxl_video_shared::kHeight &&
               header->stride == wxl_video_shared::kStride &&
               header->frameBytes == wxl_video_shared::kFrameBytes;
    }

    bool VideoSurface::ConnectFrames()
    {
        if (mappingView_) return ValidateFrames();
        const DWORD now = GetTickCount();
        if (now - lastConnectAttempt_ < 1000) return false;
        lastConnectAttempt_ = now;

        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, wxl_video_shared::kMappingName);
        if (!mapping_) return false;
        mappingView_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, wxl_video_shared::kMappingBytes);
        if (!mappingView_ || !ValidateFrames())
        {
            if (mappingView_) UnmapViewOfFile(mappingView_);
            mappingView_ = nullptr;
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }
        return true;
    }

    void VideoSurface::ReleaseTexture()
    {
        if (texture_)
        {
            texture_->Release();
            texture_ = nullptr;
        }
        if (stagingTexture_)
        {
            stagingTexture_->Release();
            stagingTexture_ = nullptr;
        }
        textureDevice_ = nullptr;
        textureMipmapped_ = false;
        lastFrameSequence_ = 0;
    }

    bool VideoSurface::UploadLatestFrame(IDirect3DDevice9* device)
    {
        if (!device || !ConnectFrames() || !ValidateFrames()) return false;
        const auto* header = static_cast<const wxl_video_shared::FrameHeader*>(mappingView_);
        if (!header->ready) return false;

        const unsigned active = static_cast<unsigned>(header->activeIndex) & 1u;
        const LONG before = header->slotSequence[active];
        if (before <= 0 || before == lastFrameSequence_) return texture_ != nullptr;
        std::memcpy(frameScratch_.data(), wxl_video_shared::Pixels(mappingView_, active),
                    frameScratch_.size());
        MemoryBarrier();
        if (header->slotSequence[active] != before) return texture_ != nullptr;

        if (textureDevice_ != device)
            ReleaseTexture();
        if (!texture_)
        {
            // Mipmapped where the device can (a 1280-wide picture shimmers from afar without): each frame goes into a
            // system-memory texture and UpdateTexture hands it to a default-pool AUTOGENMIPMAP one, which makes its levels
            // on the GPU. Not a DYNAMIC | AUTOGENMIPMAP texture locked with DISCARD (the Stonewall build until now): on
            // this machine's GeForce GTX 1660 SUPER and Radeon both, its levels are made once, from the first frame,
            // and never again (scratchpad vc-miptest, 2026-10-17) -- and the first frame is the helper's dark page, so
            // the screen showed black wherever it was smaller than 1280 pixels across, i.e. nearly always.
            const UINT width = wxl_video_shared::kWidth, height = wxl_video_shared::kHeight;
            HRESULT made = device->CreateTexture(width, height, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8,
                                                 D3DPOOL_DEFAULT, &texture_, nullptr);
            if (SUCCEEDED(made) && made != D3DOK_NOAUTOGEN && texture_)
                made = device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                             &stagingTexture_, nullptr);
            textureMipmapped_ = SUCCEEDED(made) && made != D3DOK_NOAUTOGEN && texture_ && stagingTexture_;
            if (!textureMipmapped_)
            {
                // One level, no mipmaps, locked directly (stock Video Cinema and Azeroth Plex).
                ReleaseTexture();
                if (FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                                 D3DPOOL_DEFAULT, &texture_, nullptr)) || !texture_)
                {
                    texture_ = nullptr;
                    status_ = "D3D9 could not create the world-screen texture.";
                    return false;
                }
            }
            textureDevice_ = device;
        }

        IDirect3DTexture9* const target = textureMipmapped_ ? stagingTexture_ : texture_;
        D3DLOCKED_RECT locked{};
        if (FAILED(target->LockRect(0, &locked, nullptr, textureMipmapped_ ? 0 : D3DLOCK_DISCARD)))
            return false;
        const auto* source = frameScratch_.data();
        auto* destination = static_cast<uint8_t*>(locked.pBits);
        for (uint32_t row = 0; row < wxl_video_shared::kHeight; ++row)
            std::memcpy(destination + size_t(row) * locked.Pitch,
                        source + size_t(row) * wxl_video_shared::kStride,
                        wxl_video_shared::kStride);
        target->UnlockRect(0);
        if (textureMipmapped_)
        {
            if (FAILED(device->UpdateTexture(stagingTexture_, texture_))) return false;
            texture_->GenerateMipSubLevels();
        }
        lastFrameSequence_ = before;

        char message[128]{};
        std::snprintf(message, sizeof(message), "Live video frame %ld connected (%ux%u).", before,
                      wxl_video_shared::kWidth, wxl_video_shared::kHeight);
        status_ = message;
        return true;
    }

    void VideoSurface::DrawWorldScreen(IDirect3DDevice9* device, void* sceneDepthRaw,
                                       bool beforeWorld)
    {
        if (!device || !placed_ || !visible_ || wxl::game::world::CurrentMapId() != mapId_)
            return;

        ++drawAttempts_;
        // Before the world only the draw itself: the frame is uploaded at the end of the scene (OnWorldSceneEnd).
        if (!beforeWorld) UploadLatestFrame(device);

        const float halfWidth = width_ * 0.5f;
        const float halfHeight = width_ * 9.0f / 32.0f;
        // A missing helper/video must still produce an unmistakable world-space test card.  The
        // previous nearly-black placeholder was effectively invisible in dark zones and, because it
        // also honored world depth, gave no way to distinguish placement from playback failures.
        const D3DCOLOR topLeft = texture_ ? 0xFFFFFFFFu : 0xFFFF2D55u;
        const D3DCOLOR topRight = texture_ ? 0xFFFFFFFFu : 0xFFFFD60Au;
        const D3DCOLOR bottomLeft = texture_ ? 0xFFFFFFFFu : 0xFF00E5FFu;
        const D3DCOLOR bottomRight = texture_ ? 0xFFFFFFFFu : 0xFF7C4DFFu;
        const Vertex vertices[4] = {
            { center_[0] - right_[0] * halfWidth, center_[1] - right_[1] * halfWidth,
              center_[2] + halfHeight, topLeft, 1.0f, 0.0f },
            { center_[0] + right_[0] * halfWidth, center_[1] + right_[1] * halfWidth,
              center_[2] + halfHeight, topRight, 0.0f, 0.0f },
            { center_[0] - right_[0] * halfWidth, center_[1] - right_[1] * halfWidth,
              center_[2] - halfHeight, bottomLeft, 1.0f, 1.0f },
            { center_[0] + right_[0] * halfWidth, center_[1] + right_[1] * halfWidth,
              center_[2] - halfHeight, bottomRight, 0.0f, 1.0f },
        };
        const Vertex border[5] = {
            { vertices[0].x, vertices[0].y, vertices[0].z, 0xFFFFFF00u, 0.0f, 0.0f },
            { vertices[1].x, vertices[1].y, vertices[1].z, 0xFFFFFF00u, 0.0f, 0.0f },
            { vertices[3].x, vertices[3].y, vertices[3].z, 0xFFFFFF00u, 0.0f, 0.0f },
            { vertices[2].x, vertices[2].y, vertices[2].z, 0xFFFFFF00u, 0.0f, 0.0f },
            { vertices[0].x, vertices[0].y, vertices[0].z, 0xFFFFFF00u, 0.0f, 0.0f },
        };

        IDirect3DStateBlock9* state = nullptr;
        const HRESULT stateResult = CaptureState(device, &state);
        lastStateResult_ = stateResult;
        if (FAILED(stateResult) || !state)
        {
            const DWORD now = GetTickCount();
            if (api_ && now - lastDrawFailureLog_ >= 1000)
            {
                api_->Log(WXL_LOG_ERROR, kTag, "world screen state capture failed hr=0x%08lX",
                          static_cast<unsigned long>(stateResult));
                lastDrawFailureLog_ = now;
            }
            return;
        }
        // Depth-tested only before the world: the freshly cleared depth is seeded with the screen, and nearer characters
        // and props then draw over it. At the end of the scene (only the through-world mode draws there) it isn't, and
        // the depth surface is left alone (Azeroth Plex 754023c: the world text batch is drawn straight after).
        auto* sceneDepth = static_cast<IDirect3DSurface9*>(sceneDepthRaw);
        const bool depthTested = sceneDepth && beforeWorld;
        IDirect3DSurface9* oldDepth = nullptr;
        bool swapDepth = false;
        if (depthTested)
        {
            // The pre-world screen draws into the depth already bound (the hook passes it): no SetDepthStencilSurface
            // then, which other modules hook (see "Hooked calls").
            device->GetDepthStencilSurface(&oldDepth);
            swapDepth = oldDepth != sceneDepth;
            if (swapDepth) device->SetDepthStencilSurface(sceneDepth);
        }

        // Build 12340 creates a pure D3D9 device, so GetTransform is unavailable.  Use WarcraftXL's
        // exact scene-matrix bank while the world-scene-end event still owns it.  Camera globals are
        // close enough to place the quad visually, but not guaranteed to use the identical projection
        // that populated this depth surface; that mismatch makes a live video texture appear in broken
        // patches even though the depth-free diagnostic card is complete.
        float eye[3]{};
        wxl::game::camera::GetPosition(eye);
        D3DMATRIX world{};
        world._11 = world._22 = world._33 = world._44 = 1.0f;
        world._41 = -eye[0];
        world._42 = -eye[1];
        world._43 = -eye[2];
        D3DMATRIX view{};
        D3DMATRIX projection{};
        if (!wxl::game::gfx::SceneMatrices(&view._11, &projection._11))
        {
            if (swapDepth) device->SetDepthStencilSurface(oldDepth);
            if (oldDepth) oldDepth->Release();
            state->Apply();
            state->Release();
            return;
        }
        // The projection the world is rendered with, not the one it was given (see RenderProjection): the same place
        // on screen either way, but only this one gives the world's depth.
        float rendered[16];
        if (RenderProjection(rendered)) std::memcpy(&projection._11, rendered, sizeof(rendered));
        if (depthTested)
        {
            D3DVIEWPORT9 viewport{};
            if (SUCCEEDED(device->GetViewport(&viewport)))
            {
                viewport.MinZ = 0.0f;
                viewport.MaxZ = WorldPassMaxZ();
                device->SetViewport(&viewport);
            }
        }
        const float* sceneView = &view._11;
        const float* sceneProjection = &projection._11;

        const float relativeCenter[4] = {
            center_[0] - eye[0], center_[1] - eye[1], center_[2] - eye[2], 1.0f
        };
        // The whole view-space point first, then its projection (each clip column needs all four view components).
        float viewCenter[4]{};
        float clipCenter[4]{};
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                viewCenter[column] += relativeCenter[row] * sceneView[row * 4 + column];
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                clipCenter[column] += viewCenter[row] * sceneProjection[row * 4 + column];
        projectedClipW_ = clipCenter[3];
        projectedCenterValid_ = std::isfinite(projectedClipW_) &&
                                std::abs(projectedClipW_) > 0.0001f;
        if (projectedCenterValid_)
        {
            projectedCenter_[0] = clipCenter[0] / projectedClipW_;
            projectedCenter_[1] = clipCenter[1] / projectedClipW_;
            projectedCenter_[2] = clipCenter[2] / projectedClipW_;
            projectedCenterValid_ = Finite3(projectedCenter_);
        }

        // The panel's "Depth pull": the screen tested as if it stood that many yards nearer (at its centre, along the
        // ray from the eye; ScreenDepth.hpp), in its projection, not as D3DRS_DEPTHBIAS ("Hooked calls"). It only
        // matters where the screen is depth-tested. The world's view is a rotation about the camera (no translation), so
        // the scale is about the eye, and the centre's distance from the camera is the one to pull it along.
        D3DMATRIX drawProjection = projection;
        if (depthTested)
        {
            const float distance = std::sqrt(relativeCenter[0] * relativeCenter[0] +
                                             relativeCenter[1] * relativeCenter[1] +
                                             relativeCenter[2] * relativeCenter[2]);
            screen_depth::PullTowardEye(drawProjection, screen_depth::PullScale(distance, depthPull_));
        }
        device->SetTransform(D3DTS_WORLD, &world);
        device->SetTransform(D3DTS_VIEW, &view);
        device->SetTransform(D3DTS_PROJECTION, &drawProjection);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        device->SetTexture(0, texture_);
        if (texture_)
        {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        }
        else
        {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, textureMipmapped_ ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPING, TRUE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0Fu);
        device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
        device->SetRenderState(D3DRS_ZENABLE, depthTested ? D3DZB_TRUE : D3DZB_FALSE);
        // Depth writes only with the depth test (the pre-world screen seeds the world's depth); otherwise depth
        // buffering is off and nothing is written, so ZWRITEENABLE, which other modules hook, is left alone.
        if (depthTested) device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        // Build 12340's projection maps near-to-far depth in increasing order.  The live test
        // positions proved the character was between the camera and this quad while GREATER_EQUAL
        // still painted over it: the comparison was reversed.  Match the world's LESS_EQUAL test so
        // a character, doodad, or terrain pixel with smaller depth remains in front of the screen.
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        // No D3DRS_DEPTHBIAS: the pull is in drawProjection ("Hooked calls" says why).

        const HRESULT drawResult = device->DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));

        HRESULT borderResult = S_OK;
        if (outline_)
        {
            device->SetTexture(0, nullptr);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            borderResult = device->DrawPrimitiveUP(
                D3DPT_LINESTRIP, 4, border, sizeof(Vertex));
        }
        lastDrawResult_ = FAILED(drawResult) ? drawResult : borderResult;
        if (SUCCEEDED(drawResult) && SUCCEEDED(borderResult))
        {
            ++drawSuccesses_;
        }
        else
        {
            const DWORD now = GetTickCount();
            if (api_ && now - lastDrawFailureLog_ >= 1000)
            {
                api_->Log(WXL_LOG_ERROR, kTag, "world screen draw failed fill=0x%08lX border=0x%08lX",
                          static_cast<unsigned long>(drawResult),
                          static_cast<unsigned long>(borderResult));
                lastDrawFailureLog_ = now;
            }
        }

        if (swapDepth) device->SetDepthStencilSurface(oldDepth);
        if (oldDepth) oldDepth->Release();
        state->Apply(); // includes the viewport
        state->Release();
    }

    void VideoSurface::OnWorldSceneEnd(const wxl::events::WorldSceneEndArgs& args)
    {
        ++worldSceneCalls_;
        inWorldPass_ = false;
        // The gate above should match every world pass; if it never does (another module drawing the world into a
        // depth surface of its own, say), fall back to the first colour + depth clear of the frame rather than lose
        // the screen.
        if (depthTest_ && visible_ && placed_ && !gateOff_ && worldRenderEvents_ && wxl::game::world::CurrentMapId() == mapId_)
        {
            gateMisses_ = preWorldDrawn_ ? 0u : gateMisses_ + 1u;
            if (gateMisses_ >= 300u)
            {
                gateOff_ = true;
                if (api_) api_->Log(WXL_LOG_WARN, kTag, "pre-world screen: the world pass's clear never matched its depth surface; drawing at the first colour and depth clear of the frame instead");
            }
        }
        if (!loggedFirstWorldScene_ && api_)
        {
            api_->Log(WXL_LOG_INFO, kTag,
                      "first world scene device=%p depth=%p currentMap=%d placed=%d savedMap=%d",
                      args.device, args.sceneDepth, wxl::game::world::CurrentMapId(),
                      placed_ ? 1 : 0, mapId_);
            loggedFirstWorldScene_ = true;
        }
        // With physical occlusion enabled, the scene-clear hook already placed the screen into the
        // freshly cleared colour/depth targets. WoW's own world draw then naturally paints nearer
        // characters and props over it. The end-of-scene path is retained for the intentional
        // through-world mode.
        auto* device = static_cast<IDirect3DDevice9*>(args.device);
        if (!depthTest_)
            DrawWorldScreen(device, args.sceneDepth, false);
        else if (device && placed_ && visible_ && wxl::game::world::CurrentMapId() == mapId_)
            UploadLatestFrame(device); // for the next frame's pre-world screen
        DrawStonewall(device, args.sceneDepth);
    }

    void VideoSurface::OnUpdate(const wxl::events::UpdateArgs& args)
    {
        if (args.timeMs - lastVolumeUpdate_ < 200) return;
        lastVolumeUpdate_ = args.timeMs;

        int targetVolume = masterVolume_;
        if (spatialAudio_ && placed_)
        {
            namespace world = wxl::game::world;
            if (world::CurrentMapId() != mapId_)
            {
                targetVolume = 0;
            }
            else
            {
                const unsigned long long guid = world::ActivePlayerGuid();
                void* player = guid ? world::ResolveObject(guid, world::kTypeMaskPlayer) : nullptr;
                float position[3]{};
                if (!player)
                {
                    targetVolume = 0;
                }
                else
                {
                    world::UnitPosition(player, position);
                    const float dx = position[0] - center_[0];
                    const float dy = position[1] - center_[1];
                    const float dz = position[2] - center_[2];
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (!std::isfinite(distance) || distance >= silentDistance_)
                        targetVolume = 0;
                    else if (distance > fullVolumeDistance_)
                    {
                        const float range = std::max(1.0f, silentDistance_ - fullVolumeDistance_);
                        const float gain = 1.0f - (distance - fullVolumeDistance_) / range;
                        targetVolume = static_cast<int>(std::lround(masterVolume_ * gain));
                    }
                }
            }
        }
        SendVolume(targetVolume);
    }

    void VideoSurface::OnInput(const wxl::events::InputArgs& args)
    {
        if (!editingUrl_ || !args.handled) return;

        if (args.message == WM_CHAR)
        {
            const wchar_t character = static_cast<wchar_t>(args.wparam);
            if (character == L'\r')
            {
                editingUrl_ = false;
                SendCommand(static_cast<LONG>(wxl_video_shared::Command::LoadAndPlay));
            }
            else
            {
                AppendUrlCharacter(character);
            }
            *args.handled = true;
            return;
        }

        if (args.message == WM_KEYDOWN || args.message == WM_SYSKEYDOWN)
        {
            const UINT key = static_cast<UINT>(args.wparam);
            if (key == VK_ESCAPE)
            {
                editingUrl_ = false;
                status_ = "URL typing cancelled.";
            }
            else if (key == 'V' && (GetKeyState(VK_CONTROL) & 0x8000))
            {
                PasteUrlFromClipboard();
            }
            *args.handled = true;
            return;
        }

        if (args.message == WM_KEYUP || args.message == WM_SYSKEYUP)
            *args.handled = true;
    }

    void VideoSurface::OnDeviceLost(const wxl::events::DeviceResetArgs&)
    {
        ReleaseTexture();
    }

    void VideoSurface::ReleaseStonewallTextures()
    {
        for (IDirect3DTexture9** texture : { &signTexture_, &tvTexture_, &sparkleTexture_ })
            if (*texture) { (*texture)->Release(); *texture = nullptr; }
        stonewallDevice_ = nullptr;
    }

    bool VideoSurface::EnsureStonewallTextures(IDirect3DDevice9* device)
    {
        if (stonewallDevice_ != device) ReleaseStonewallTextures();
        if (signTexture_ && tvTexture_ && sparkleTexture_) return true;
        if (stonewallFailed_) return false;
        stonewallDevice_ = device;
        // The sign as shipped (its fonts come with Office, not Windows); rendered here only if the file is missing.
        art::Sign sign;
        const std::wstring directory = ExtensionDirectory();
        const std::wstring path = directory.empty() ? std::wstring{} : (std::filesystem::path(directory) / L"stonewall-sign.bin").wstring();
        if (path.empty() || !art::LoadSign(path.c_str(), sign))
        {
            sign = art::MakeSign(1024, 256);
            if (api_) api_->Log(WXL_LOG_WARN, kTag, "Stonewall TV: stonewall-sign.bin missing or unreadable; rendered the sign here.");
        }
        signSplit_ = sign.split;
        signArc_[0] = sign.arcU; signArc_[1] = sign.arcV; signArc_[2] = sign.arcInner; signArc_[3] = sign.arcOuter;
        signArc_[4] = sign.arcEnd;
        signCenter_ = sign.centerU;
        signAspect_ = sign.w > 0 ? static_cast<float>(sign.h) / sign.w : 0.25f;
        signTexture_ = MakeTexture(device, sign.w, sign.h, sign.pixels);
        tvTexture_ = MakeTexture(device, sign.w, sign.h, sign.tv);
        sparkleTexture_ = MakeTexture(device, 64, 64, art::SparklePixels(64));
        if (!signTexture_ || !tvTexture_ || !sparkleTexture_)
        {
            stonewallFailed_ = true;
            ReleaseStonewallTextures();
            if (api_) api_->Log(WXL_LOG_ERROR, kTag, "Stonewall TV: could not create its textures.");
            return false;
        }
        return true;
    }

    namespace
    {
        uint32_t Hash(uint32_t x)
        {
            x ^= x >> 16; x *= 0x7FEB352Du;
            x ^= x >> 15; x *= 0x846CA68Bu;
            return x ^ (x >> 16);
        }
        float Hash01(uint32_t x) { return (Hash(x) & 0xFFFFFFu) / 16777216.0f; }
        D3DCOLOR Grey(float level)
        {
            const uint32_t c = static_cast<uint32_t>(std::clamp(level, 0.0f, 1.0f) * 255.0f + 0.5f);
            return (c << 24) | (c << 16) | (c << 8) | c;
        }
    }

    // After the world, so it blends over what's behind it; no depth test, no depth writes.
    void VideoSurface::DrawStonewall(IDirect3DDevice9* device, void* sceneDepthRaw)
    {
        if (!device || !placed_ || !visible_ || !stonewall_ || wxl::game::world::CurrentMapId() != mapId_) return;
        if (!EnsureStonewallTextures(device)) return;

        float eye[3]{};
        wxl::game::camera::GetPosition(eye);
        D3DMATRIX world{};
        world._11 = world._22 = world._33 = world._44 = 1.0f;
        world._41 = -eye[0];
        world._42 = -eye[1];
        world._43 = -eye[2];
        D3DMATRIX view{}, projection{};
        if (!wxl::game::gfx::SceneMatrices(&view._11, &projection._11)) return;
        float rendered[16];
        if (RenderProjection(rendered)) std::memcpy(&projection._11, rendered, sizeof(rendered));

        IDirect3DStateBlock9* state = nullptr;
        if (FAILED(CaptureState(device, &state)) || !state) return;
        // Not depth-tested (Azeroth Plex 754023c, the same sign): here, at the end of the world pass, the viewport's depth
        // range is back to 0..1 (the world drew with 0..0.94) and the sign was drawn with SceneMatrices' projection, not
        // the world's, so it lost the depth test to the terrain behind it (Living Azeroth found the same at this point;
        // in Azeroth Plex, in game on 2026-10-14, with "Characters and world appear in front" on, the sign and the glow
        // never showed). Nor is the depth surface swapped: the world text batch (the player names) is drawn straight
        // after this event.
        (void)sceneDepthRaw;

        device->SetTransform(D3DTS_WORLD, &world);
        device->SetTransform(D3DTS_VIEW, &view);
        device->SetTransform(D3DTS_PROJECTION, &projection);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPING, TRUE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x07u);
        // Depth buffering off: no depth test, nothing written. Neither ZWRITEENABLE nor DEPTHBIAS is set (other modules
        // hook them and take what they see for the client's: "Hooked calls").
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);

        // Positions in the screen's plane: h yards toward the viewer's right of its centre (the screen's u runs that way:
        // its viewer-right edge is center - right * half), z yards up, `lift` yards toward the camera.
        float toward[3] = { right_[1], -right_[0], 0.0f };
        if ((eye[0] - center_[0]) * toward[0] + (eye[1] - center_[1]) * toward[1] < 0.0f) { toward[0] = -toward[0]; toward[1] = -toward[1]; }
        const auto quad = [&](IDirect3DTexture9* texture, float h0, float h1, float z0, float z1, float u0, float u1,
                              float lift, D3DCOLOR colour) {
            const auto at = [&](float h, float z, float u, float v) {
                return Vertex{ center_[0] - right_[0] * h + toward[0] * lift, center_[1] - right_[1] * h + toward[1] * lift,
                               center_[2] + z, colour, u, v };
            };
            const Vertex v[4] = { at(h0, z1, u0, 0.0f), at(h1, z1, u1, 0.0f), at(h0, z0, u0, 1.0f), at(h1, z0, u1, 1.0f) };
            device->SetTexture(0, texture);
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
        };

        const float W = width_, top = W * 9.0f / 32.0f;
        const float t = static_cast<float>(GetTickCount() % 3600000u) / 1000.0f;

        // The sign, centred above the screen. The rainbow and "Stonewall" hum; "TV" is a tube on its way out: every few
        // seconds it stutters (60 ms on/off for about half a second).
        const float signW = W * 0.78f, signH = signW * signAspect_, signZ = top + W * 0.01f;
        const float left = -signW * signCenter_; // the design (not the texture) centred
        const float hum = 0.93f + 0.07f * std::sin(t * 1.3f);
        quad(signTexture_, left, left + signW, signZ, signZ + signH, 0.0f, 1.0f, 0.0f, Grey(hum));
        const uint32_t epoch = static_cast<uint32_t>(t / 7.0f);
        const float burst = epoch * 7.0f + Hash01(epoch * 3u + 1u) * 5.5f;
        float tv = 0.96f;
        if (t >= burst && t < burst + 0.55f) tv = (Hash(static_cast<uint32_t>(t / 0.06f)) & 3u) ? 1.0f : 0.12f;
        quad(tvTexture_, left, left + signW, signZ, signZ + signH, 0.0f, 1.0f, 0.0f, Grey(tv * hum));

        // Glitter on the rainbow: sparkles that twinkle along its visible arc (up to where it passes behind the S), each
        // somewhere new every time it flashes.
        const float arcU = signArc_[0], arcV = signArc_[1], inner = signArc_[2], outer = signArc_[3], arcEnd = signArc_[4];
        for (uint32_t i = 0; i < 16u; ++i)
        {
            const float period = 1.1f + Hash01(i * 7u + 3u) * 1.6f, phase = Hash01(i * 11u + 5u) * period;
            const float cycles = (t + phase) / period;
            const uint32_t cycle = static_cast<uint32_t>(cycles);
            const float flash = std::pow(std::sin((cycles - static_cast<float>(cycle)) * 3.14159265f), 6.0f);
            if (flash < 0.02f) continue;
            const uint32_t seed = i * 977u + cycle * 131u;
            const float r = inner + (outer - inner) * (0.15f + 0.7f * Hash01(seed + 1u)); // in texture heights
            const float angle = arcEnd + (3.14159265f - arcEnd) * (0.04f + 0.92f * Hash01(seed));
            const float u = arcU + std::cos(angle) * r * signAspect_, v = arcV - std::sin(angle) * r;
            const float sh = left + u * signW, sz = signZ + (1.0f - v) * signH, size = signH * 0.16f * (0.5f + 0.5f * flash);
            quad(sparkleTexture_, sh - size * 0.5f, sh + size * 0.5f, sz - size * 0.5f, sz + size * 0.5f, 0.0f, 1.0f, 0.005f,
                 Grey(flash));
        }
        device->SetTexture(0, nullptr);

        state->Apply();
        state->Release();
    }

    void VideoSurface::OnDeviceReset(const wxl::events::DeviceResetArgs&)
    {
        ReleaseTexture();
    }

    void VideoSurface::OnWorldLeave(const wxl::events::WorldLeaveArgs&)
    {
        ReleaseTexture();
    }


    void VideoSurface::DrawPanel()
    {
        if (!api_) return;
        api_->UiText("Physical world-space screen for YouTube and CyTube channels (cytu.be/r/<channel>).");
        if (editingUrl_)
        {
            api_->UiText("TYPE URL NOW: Enter loads it, Backspace edits, Ctrl+V pastes, Esc cancels.");
        }
        else if (api_->UiButton("Type YouTube URL in game"))
        {
            editingUrl_ = true;
            status_ = "URL typing active. Enter loads and plays it.";
        }
        if (api_->UiButton("Paste YouTube URL from clipboard")) PasteUrlFromClipboard();
        char urlDisplay[1120]{};
        std::snprintf(urlDisplay, sizeof(urlDisplay), "URL: %s", urlInput_[0] ? urlInput_ : "(none)");
        api_->UiText(urlDisplay);
        if (api_->UiButton("Load URL and play"))
            SendCommand(static_cast<LONG>(wxl_video_shared::Command::LoadAndPlay));
        if (api_->UiButton("Play"))
            SendCommand(static_cast<LONG>(wxl_video_shared::Command::Play));
        if (api_->UiButton("Pause"))
            SendCommand(static_cast<LONG>(wxl_video_shared::Command::Pause));
        if (api_->UiButton("Stop"))
            SendCommand(static_cast<LONG>(wxl_video_shared::Command::Stop));
        api_->UiSeparator();

        if (api_->UiSliderInt("Master volume", &masterVolume_, 0, 100))
            SendVolume(masterVolume_);
        api_->UiCheckbox("Distance-based audio", &spatialAudio_);
        if (spatialAudio_)
        {
            api_->UiSliderFloat("Full volume distance (yards)", &fullVolumeDistance_, 1.0f, 30.0f);
            api_->UiSliderFloat("Silent distance (yards)", &silentDistance_, 10.0f, 120.0f);
            if (silentDistance_ < fullVolumeDistance_ + 1.0f)
                silentDistance_ = fullVolumeDistance_ + 1.0f;
        }
        char volumeStatus[96]{};
        std::snprintf(volumeStatus, sizeof(volumeStatus), "Current screen audio: %d%%", effectiveVolume_);
        api_->UiText(volumeStatus);
        if (api_->UiButton("Show browser troubleshooting window")) OpenHost(true);
        if (api_->UiButton("Hide browser in background")) HideHost();
        if (api_->UiButton("Close background player")) CloseHost();
        api_->UiSeparator();

        if (api_->UiSliderFloat("Screen width (yards)", &width_, 2.0f, 30.0f) && placed_)
            SavePlacement();
        api_->UiSliderFloat("Place distance (yards)", &placeDistance_, 4.0f, 30.0f);
        api_->UiSliderFloat("Bottom height (yards)", &bottomOffset_, -3.0f, 10.0f);
        if (api_->UiButton("Place / move screen in front of me")) PlaceInFront();

        if (placed_)
        {
            if (api_->UiCheckbox("Show physical screen", &visible_)) SavePlacement();
            if (api_->UiCheckbox("Characters and world appear in front", &depthTest_)) SavePlacement();
            if (api_->UiSliderFloat("Depth pull (yards toward the camera)", &depthPull_, 0.0f,
                                    screen_depth::kMaxPullYards))
                SavePlacement();
            api_->UiCheckbox("Show yellow placement outline", &outline_);
            if (api_->UiCheckbox("Stonewall TV: neon sign with a glittery rainbow", &stonewall_)) SavePlacement();
            if (api_->UiButton("Rotate screen left 5 degrees")) Rotate(5.0f);
            if (api_->UiButton("Rotate screen right 5 degrees")) Rotate(-5.0f);
            if (api_->UiButton("Raise screen 0.5 yard")) { center_[2] += 0.5f; SavePlacement(); }
            if (api_->UiButton("Lower screen 0.5 yard")) { center_[2] -= 0.5f; SavePlacement(); }

            char placement[192]{};
            std::snprintf(placement, sizeof(placement),
                          "Saved screen: map %d at %.2f, %.2f, %.2f (%.2f x %.2f yards)",
                          mapId_, center_[0], center_[1], center_[2], width_, width_ * 9.0f / 16.0f);
            api_->UiText(placement);
        }
        else
        {
            api_->UiText("No physical screen has been placed yet.");
        }
        char diagnostics[256]{};
        std::snprintf(diagnostics, sizeof(diagnostics),
                      "World draw: scenes %llu | attempts %llu | success %llu | hr 0x%08lX | frame %ld | helper %s | %s",
                      static_cast<unsigned long long>(worldSceneCalls_),
                      static_cast<unsigned long long>(drawAttempts_),
                      static_cast<unsigned long long>(drawSuccesses_),
                      static_cast<unsigned long>(lastDrawResult_), lastFrameSequence_,
                      FindWindowW(kHostWindowClass, nullptr) ? "open" : "closed",
                      !texture_ ? "no texture" : textureMipmapped_ ? "mipmapped" : "no mipmaps");
        api_->UiText(diagnostics);
        char projection[192]{};
        if (projectedCenterValid_)
            std::snprintf(projection, sizeof(projection),
                          "Projected center NDC: %.3f, %.3f, %.3f | clip W %.3f (visible XY is -1 to +1)",
                          projectedCenter_[0], projectedCenter_[1], projectedCenter_[2], projectedClipW_);
        else
            std::snprintf(projection, sizeof(projection),
                          "Projected center unavailable | clip W %.3f", projectedClipW_);
        api_->UiText(projection);
        if (!texture_)
            api_->UiText("No video frame yet: a bright four-color test card should still be visible in the world.");
        api_->UiText(status_.c_str());
    }
}
