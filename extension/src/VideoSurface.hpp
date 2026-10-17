#pragma once

#include "wxl/EventScript.hpp"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace wxl_video_screen
{
    class VideoSurface final : public wxl::ext::EventScript
    {
    public:
        static VideoSurface& Instance();

        bool Initialize(const WXL_Api* api);
        void DrawPanel();

    private:
        VideoSurface();

        using SceneClearFn = void(__cdecl*)(uint32_t flags, uint32_t colour);
        static void __cdecl SceneClearHook(uint32_t flags, uint32_t colour);

        void OnFrame(const wxl::events::FrameArgs& args);
        void OnWorldRender(const wxl::events::WorldRenderArgs& args);
        void OnWorldSceneEnd(const wxl::events::WorldSceneEndArgs& args);
        void OnUpdate(const wxl::events::UpdateArgs& args);
        void OnInput(const wxl::events::InputArgs& args);
        void OnDeviceLost(const wxl::events::DeviceResetArgs& args);
        void OnDeviceReset(const wxl::events::DeviceResetArgs& args);
        void OnWorldLeave(const wxl::events::WorldLeaveArgs& args);

        bool OpenHost(bool showWindow);
        void HideHost();
        void CloseHost();
        bool PlaceInFront();
        void Rotate(float degrees);
        void SavePlacement();
        void LoadPlacement();

        bool ConnectFrames();
        bool ConnectControl();
        bool SendCommand(LONG command);
        void SendVolume(int volume);
        void PasteUrlFromClipboard();
        void AppendUrlCharacter(wchar_t character);
        bool ValidateFrames() const;
        bool UploadLatestFrame(IDirect3DDevice9* device);
        void ReleaseTexture();
        void DrawWorldScreen(IDirect3DDevice9* device, void* sceneDepth, bool beforeWorld);
        // Stonewall TV: a neon "Stonewall TV" sign above the screen, its rainbow glittering.
        void DrawStonewall(IDirect3DDevice9* device, void* sceneDepth);
        bool EnsureStonewallTextures(IDirect3DDevice9* device);
        void ReleaseStonewallTextures();
        std::wstring ExtensionDirectory() const;

        const WXL_Api* api_ = nullptr;
        HANDLE mapping_ = nullptr;
        void* mappingView_ = nullptr;
        HANDLE controlMapping_ = nullptr;
        void* controlView_ = nullptr;
        DWORD lastConnectAttempt_ = 0;
        LONG lastFrameSequence_ = 0;
        LONG lastDrawResult_ = static_cast<LONG>(0x8000000AL);
        LONG lastStateResult_ = 0;
        uint64_t worldSceneCalls_ = 0;
        uint64_t drawAttempts_ = 0;
        uint64_t drawSuccesses_ = 0;
        DWORD lastDrawFailureLog_ = 0;
        bool loggedFirstWorldScene_ = false;
        bool preWorldDrawn_ = false;
        bool inWorldPass_ = false;             // between OnWorldRender and OnWorldSceneEnd
        const void* worldPassDepth_ = nullptr; // the depth surface bound at OnWorldRender (compared, never used)
        uint64_t worldRenderEvents_ = 0;
        unsigned gateMisses_ = 0;              // world passes in a row without the pre-world screen (depth mode)
        bool gateOff_ = false;
        bool projectedCenterValid_ = false;
        float projectedCenter_[3]{};
        float projectedClipW_ = 0.0f;
        IDirect3DDevice9* textureDevice_ = nullptr;
        IDirect3DTexture9* texture_ = nullptr;        // what the screen samples
        IDirect3DTexture9* stagingTexture_ = nullptr; // system memory, UpdateTexture'd into texture_ when it's mipmapped
        bool textureMipmapped_ = false;
        IDirect3DDevice9* stonewallDevice_ = nullptr;
        IDirect3DTexture9* signTexture_ = nullptr;   // the rainbow and "Stonewall"
        IDirect3DTexture9* tvTexture_ = nullptr;     // "TV" (its own layer: it flickers)
        IDirect3DTexture9* sparkleTexture_ = nullptr;
        float signSplit_ = 0.75f;
        float signArc_[5]{};      // the rainbow on the sign: arc centre u, v; band radii; where it goes behind the S (radians)
        float signCenter_ = 0.5f; // the design's middle (u)
        float signAspect_ = 0.25f; // the sign texture's height / width
        bool stonewallFailed_ = false;
        std::vector<uint8_t> frameScratch_;

        bool initialized_ = false;
        bool placed_ = false;
        int visible_ = 1;
        int depthTest_ = 1;
        int outline_ = 0;
        int stonewall_ = 0;
        float depthBias_ = -0.010f;
        int spatialAudio_ = 1;
        int masterVolume_ = 80;
        int effectiveVolume_ = 80;
        int lastSentVolume_ = -1;
        uint32_t lastVolumeUpdate_ = 0;
        int mapId_ = -1;
        float center_[3]{};
        float right_[3]{ 0.0f, 1.0f, 0.0f };
        float width_ = 8.0f;
        float placeDistance_ = 10.0f;
        float bottomOffset_ = 0.5f;
        float fullVolumeDistance_ = 6.0f;
        float silentDistance_ = 60.0f;
        char urlInput_[1024]{};
        bool editingUrl_ = false;
        std::string status_ = "Waiting for the cinema helper.";

        static SceneClearFn originalSceneClear_;
    };
}
