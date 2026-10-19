// Local WebView2 helper used by wxl-video-screen.
// Copyright (C) 2026 WarcraftXL contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wrl.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

#include "CytubeTheater.hpp"
#include "VideoShared.hpp"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace
{
    constexpr wchar_t kWindowClass[] = L"WXLVideoCinemaHost";
    constexpr wchar_t kWindowTitle[] = L"WXL Video Cinema - YouTube";
    constexpr wchar_t kVirtualHost[] = L"wxl-video.local";
    constexpr wchar_t kPlayerUrl[] = L"https://wxl-video.local/player.html";
    constexpr UINT_PTR kCaptureTimer = 1;
    constexpr UINT kCaptureIntervalMs = 33; // Up to 30 shared frames per second (JPEG: cheaper to encode and decode than PNG).
    constexpr UINT kShowHostMessage = WM_APP + 0x45;
    constexpr UINT kHideHostMessage = WM_APP + 0x46;

    HWND g_window = nullptr;
    ComPtr<ICoreWebView2Controller> g_controller;
    ComPtr<ICoreWebView2> g_webview;
    ComPtr<IWICImagingFactory> g_wic;
    HANDLE g_mapping = nullptr;
    void* g_mappingView = nullptr;
    HANDLE g_controlMapping = nullptr;
    void* g_controlView = nullptr;
    bool g_captureInFlight = false;
    bool g_background = false;
    bool g_pageReady = false;
    LONG g_lastCommandSequence = 0;
    LONG g_lastVolumeSequence = 0;
    HANDLE g_parentProcess = nullptr;

    DWORD ParseParentPid(const wchar_t* commandLine)
    {
        if (!commandLine) return 0;
        constexpr wchar_t option[] = L"--parent-pid";
        const wchar_t* cursor = std::wcsstr(commandLine, option);
        if (!cursor) return 0;
        cursor += std::size(option) - 1;
        while (*cursor == L' ' || *cursor == L'\t') ++cursor;
        wchar_t* end = nullptr;
        const unsigned long value = std::wcstoul(cursor, &end, 10);
        return end != cursor && value > 0 ? static_cast<DWORD>(value) : 0;
    }

    DWORD WINAPI WatchParentProcess(void*)
    {
        if (g_parentProcess && WaitForSingleObject(g_parentProcess, INFINITE) == WAIT_OBJECT_0)
        {
            if (HWND window = g_window)
                PostMessageW(window, WM_CLOSE, 0, 0);
        }
        return 0;
    }

    wxl_video_shared::FrameHeader* SharedHeader()
    {
        return static_cast<wxl_video_shared::FrameHeader*>(g_mappingView);
    }

    wxl_video_shared::ControlBlock* SharedControl()
    {
        return static_cast<wxl_video_shared::ControlBlock*>(g_controlView);
    }

    bool InitializeSharedControl()
    {
        g_controlMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(wxl_video_shared::ControlBlock)),
            wxl_video_shared::kControlMappingName);
        if (!g_controlMapping) return false;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        g_controlView = MapViewOfFile(g_controlMapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                      sizeof(wxl_video_shared::ControlBlock));
        if (!g_controlView)
        {
            CloseHandle(g_controlMapping);
            g_controlMapping = nullptr;
            return false;
        }

        auto* control = SharedControl();
        if (created)
        {
            std::memset(control, 0, sizeof(*control));
            control->magic = wxl_video_shared::kControlMagic;
            control->version = wxl_video_shared::kControlVersion;
            control->structBytes = sizeof(*control);
            control->volumePercent = 80;
        }
        if (control->magic != wxl_video_shared::kControlMagic ||
            control->version != wxl_video_shared::kControlVersion ||
            control->structBytes != sizeof(*control))
            return false;
        return true;
    }

    bool InitializeSharedFrames()
    {
        g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       static_cast<DWORD>(wxl_video_shared::kMappingBytes),
                                       wxl_video_shared::kMappingName);
        if (!g_mapping) return false;

        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        g_mappingView = MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                      wxl_video_shared::kMappingBytes);
        if (!g_mappingView)
        {
            CloseHandle(g_mapping);
            g_mapping = nullptr;
            return false;
        }

        if (created)
            std::memset(g_mappingView, 0, wxl_video_shared::kMappingBytes);

        auto* header = SharedHeader();
        header->magic = wxl_video_shared::kMagic;
        header->version = wxl_video_shared::kVersion;
        header->headerBytes = sizeof(*header);
        header->width = wxl_video_shared::kWidth;
        header->height = wxl_video_shared::kHeight;
        header->stride = wxl_video_shared::kStride;
        header->frameBytes = wxl_video_shared::kFrameBytes;
        InterlockedExchange(&header->activeIndex, 0);
        InterlockedExchange(&header->sequence, 0);
        InterlockedExchange(&header->slotSequence[0], 0);
        InterlockedExchange(&header->slotSequence[1], 0);
        InterlockedExchange(&header->hostPid, static_cast<LONG>(GetCurrentProcessId()));
        InterlockedExchange(&header->ready, 0);
        return true;
    }

    void ShutdownSharedFrames()
    {
        if (auto* header = SharedHeader())
        {
            InterlockedExchange(&header->ready, 0);
            InterlockedExchange(&header->hostPid, 0);
        }
        if (g_mappingView)
        {
            UnmapViewOfFile(g_mappingView);
            g_mappingView = nullptr;
        }
        if (g_mapping)
        {
            CloseHandle(g_mapping);
            g_mapping = nullptr;
        }
        if (g_controlView)
        {
            UnmapViewOfFile(g_controlView);
            g_controlView = nullptr;
        }
        if (g_controlMapping)
        {
            CloseHandle(g_controlMapping);
            g_controlMapping = nullptr;
        }
    }

    std::wstring JsonStringFromUtf8(const char* text, size_t length)
    {
        if (!text) return L"\"\"";
        const int wideLength = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(length), nullptr, 0);
        if (wideLength <= 0) return L"\"\"";
        std::wstring wide(static_cast<size_t>(wideLength), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(length),
                            wide.data(), wideLength);

        std::wstring quoted;
        quoted.reserve(wide.size() + 2);
        quoted.push_back(L'\"');
        constexpr wchar_t hex[] = L"0123456789ABCDEF";
        for (wchar_t character : wide)
        {
            switch (character)
            {
            case L'\\': quoted += L"\\\\"; break;
            case L'\"': quoted += L"\\\""; break;
            case L'\b': quoted += L"\\b"; break;
            case L'\f': quoted += L"\\f"; break;
            case L'\n': quoted += L"\\n"; break;
            case L'\r': quoted += L"\\r"; break;
            case L'\t': quoted += L"\\t"; break;
            default:
                if (character < 0x20)
                {
                    quoted += L"\\u00";
                    quoted.push_back(hex[(character >> 4) & 0x0F]);
                    quoted.push_back(hex[character & 0x0F]);
                }
                else quoted.push_back(character);
                break;
            }
        }
        quoted.push_back(L'\"');
        return quoted;
    }

    void ExecutePlayerScript(const std::wstring& script)
    {
        if (!g_webview || !g_pageReady || script.empty()) return;
        g_webview->ExecuteScript(
            script.c_str(),
            Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
    }

    // CyTube's video only (host/CytubeTheater.hpp) while the helper runs in the background; the whole channel page
    // while its troubleshooting window is shown.
    void ApplyTheater()
    {
        ExecutePlayerScript(g_background ? L"window.wxlTheater && window.wxlTheater(true);"
                                         : L"window.wxlTheater && window.wxlTheater(false);");
    }

    void ProcessControls()
    {
        auto* control = SharedControl();
        if (!control || !g_pageReady) return;

        const LONG commandSequence = control->commandSequence;
        if (commandSequence > 0 && commandSequence != g_lastCommandSequence)
        {
            MemoryBarrier();
            const LONG command = control->command;
            char url[wxl_video_shared::kUrlBytes]{};
            const size_t length = strnlen_s(control->url, sizeof(control->url));
            if (length < sizeof(url)) std::memcpy(url, control->url, length);
            MemoryBarrier();
            if (control->commandSequence == commandSequence)
            {
                if (command == static_cast<LONG>(wxl_video_shared::Command::LoadAndPlay))
                    ExecutePlayerScript(L"window.wxlLoadFromNative(" +
                                        JsonStringFromUtf8(url, length) + L");");
                else if (command == static_cast<LONG>(wxl_video_shared::Command::Play))
                    // With the panel's link: Play with nothing loaded yet loads it (player.html).
                    ExecutePlayerScript(L"window.wxlPlayerCommand('play', " +
                                        JsonStringFromUtf8(url, length) + L");");
                else if (command == static_cast<LONG>(wxl_video_shared::Command::Pause))
                    ExecutePlayerScript(L"window.wxlPlayerCommand('pause');");
                else if (command == static_cast<LONG>(wxl_video_shared::Command::Stop))
                    ExecutePlayerScript(L"window.wxlPlayerCommand('stop');");
                g_lastCommandSequence = commandSequence;
            }
        }

        const LONG volumeSequence = control->volumeSequence;
        if (volumeSequence > 0 && volumeSequence != g_lastVolumeSequence)
        {
            MemoryBarrier();
            LONG volume = control->volumePercent;
            volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
            if (control->volumeSequence == volumeSequence)
            {
                ExecutePlayerScript(L"window.wxlSetVolume(" + std::to_wstring(volume) + L");");
                g_lastVolumeSequence = volumeSequence;
            }
        }
    }

    bool PublishPreview(IStream* stream)
    {
        if (!stream || !g_wic || !g_mappingView) return false;

        LARGE_INTEGER start{};
        if (FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) return false;

        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(g_wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad,
                                                   &decoder)) || !decoder)
            return false;

        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame)) || !frame) return false;

        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(g_wic->CreateBitmapScaler(&scaler)) || !scaler ||
            FAILED(scaler->Initialize(frame.Get(), wxl_video_shared::kWidth,
                                      wxl_video_shared::kHeight,
                                      WICBitmapInterpolationModeFant)))
            return false;

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(g_wic->CreateFormatConverter(&converter)) || !converter ||
            FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
            return false;

        std::vector<uint8_t> pixels(wxl_video_shared::kFrameBytes);
        if (FAILED(converter->CopyPixels(nullptr, wxl_video_shared::kStride,
                                         wxl_video_shared::kFrameBytes, pixels.data())))
            return false;

        auto* header = SharedHeader();
        LONG nextSequence = header->sequence + 1;
        if (nextSequence <= 0) nextSequence = 1;
        const unsigned target = static_cast<unsigned>((header->activeIndex ^ 1) & 1);

        InterlockedExchange(&header->slotSequence[target], -nextSequence);
        std::memcpy(wxl_video_shared::Pixels(g_mappingView, target), pixels.data(), pixels.size());
        MemoryBarrier();
        InterlockedExchange(&header->slotSequence[target], nextSequence);
        InterlockedExchange(&header->activeIndex, static_cast<LONG>(target));
        InterlockedExchange(&header->sequence, nextSequence);
        InterlockedExchange(&header->ready, 1);
        return true;
    }

    void CapturePreview()
    {
        if (!g_webview || g_captureInFlight || !g_mappingView) return;

        ComPtr<IStream> stream;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream) return;

        g_captureInFlight = true;
        const HRESULT result = g_webview->CapturePreview(
            COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_JPEG, stream.Get(),
            Callback<ICoreWebView2CapturePreviewCompletedHandler>(
                [stream](HRESULT captureResult) -> HRESULT {
                    if (SUCCEEDED(captureResult))
                        PublishPreview(stream.Get());
                    g_captureInFlight = false;
                    return S_OK;
                }).Get());
        if (FAILED(result))
            g_captureInFlight = false;
    }

    std::wstring ModuleDirectory()
    {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        return length && length < MAX_PATH
            ? std::filesystem::path(modulePath).parent_path().wstring()
            : std::wstring{};
    }

    std::wstring LocalDataDirectory()
    {
        wchar_t localAppData[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT,
                                    localAppData)))
            return {};

        const std::filesystem::path path = std::filesystem::path(localAppData) /
                                           L"WarcraftXL" / L"video-cinema";
        std::error_code error;
        std::filesystem::create_directories(path, error);
        return error ? std::wstring{} : path.wstring();
    }

    void ResizeController()
    {
        if (!g_controller || !g_window) return;
        RECT bounds{};
        GetClientRect(g_window, &bounds);
        g_controller->put_Bounds(bounds);
    }

    void ShowHostWindow()
    {
        if (!g_window) return;
        g_background = false;
        LONG_PTR extended = GetWindowLongPtrW(g_window, GWL_EXSTYLE);
        extended &= ~(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
        extended |= WS_EX_APPWINDOW;
        SetWindowLongPtrW(g_window, GWL_EXSTYLE, extended);

        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        RECT bounds{};
        GetWindowRect(g_window, &bounds);
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const int workWidth = static_cast<int>(work.right - work.left);
        const int workHeight = static_cast<int>(work.bottom - work.top);
        const int x = static_cast<int>(work.left) + std::max(0, (workWidth - width) / 2);
        const int y = static_cast<int>(work.top) + std::max(0, (workHeight - height) / 2);
        ShowWindow(g_window, SW_RESTORE);
        SetWindowPos(g_window, HWND_NOTOPMOST, x, y, width, height,
                     SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        SetForegroundWindow(g_window);
        ApplyTheater();
    }

    void HideHostWindow()
    {
        if (!g_window) return;
        g_background = true;
        LONG_PTR extended = GetWindowLongPtrW(g_window, GWL_EXSTYLE);
        extended &= ~WS_EX_APPWINDOW;
        extended |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        SetWindowLongPtrW(g_window, GWL_EXSTYLE, extended);
        // Keep the compositor live for CapturePreview and audio, but move the tool window entirely
        // outside the virtual desktop so normal play has no visible companion window or taskbar item.
        SetWindowPos(g_window, HWND_BOTTOM, -32000, -32000, 1280, 720,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        ApplyTheater();
    }

    void CreateWebView()
    {
        const std::wstring dataDirectory = LocalDataDirectory();
        if (dataDirectory.empty())
        {
            MessageBoxW(g_window, L"Could not create the local WebView2 data folder.",
                        kWindowTitle, MB_ICONERROR | MB_OK);
            return;
        }

        auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        if (options)
            options->put_AdditionalBrowserArguments(
                L"--disable-features=CalculateNativeWinOcclusion "
                L"--disable-background-timer-throttling "
                L"--disable-renderer-backgrounding "
                L"--autoplay-policy=no-user-gesture-required");

        const HRESULT createResult = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, dataDirectory.c_str(), options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                    if (FAILED(result) || !environment)
                    {
                        MessageBoxW(g_window,
                                    L"Microsoft Edge WebView2 Runtime is required to open the YouTube cinema.",
                                    kWindowTitle, MB_ICONERROR | MB_OK);
                        return result;
                    }

                    return environment->CreateCoreWebView2Controller(
                        g_window,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                                if (FAILED(controllerResult) || !controller)
                                {
                                    MessageBoxW(g_window, L"Could not create the YouTube cinema player.",
                                                kWindowTitle, MB_ICONERROR | MB_OK);
                                    return controllerResult;
                                }

                                g_controller = controller;
                                g_controller->get_CoreWebView2(&g_webview);
                                g_pageReady = false;

                                ComPtr<ICoreWebView2Settings> settings;
                                if (g_webview && SUCCEEDED(g_webview->get_Settings(&settings)) && settings)
                                {
                                    settings->put_IsStatusBarEnabled(FALSE);
                                    settings->put_AreDevToolsEnabled(FALSE);
                                    settings->put_AreDefaultContextMenusEnabled(TRUE);
                                }

                                // A file:// page does not send the HTTP Referer identity that current
                                // YouTube embeds require (Error 153).  WebView2's private virtual host
                                // gives the bundled page a normal HTTPS origin without running a server
                                // or exposing any local files outside the extension directory.
                                ComPtr<ICoreWebView2_3> webview3;
                                const std::wstring moduleDirectory = ModuleDirectory();
                                const HRESULT interfaceResult = g_webview
                                    ? g_webview.As(&webview3) : E_NOINTERFACE;
                                const HRESULT mappingResult =
                                    SUCCEEDED(interfaceResult) && webview3 && !moduleDirectory.empty()
                                    ? webview3->SetVirtualHostNameToFolderMapping(
                                          kVirtualHost, moduleDirectory.c_str(),
                                          COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS)
                                    : E_FAIL;

                                ResizeController();
                                EventRegistrationToken navigationToken{};
                                if (g_webview)
                                {
                                    g_webview->add_NavigationCompleted(
                                        Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args)
                                                -> HRESULT {
                                                BOOL succeeded = FALSE;
                                                if (args) args->get_IsSuccess(&succeeded);
                                                g_pageReady = succeeded != FALSE;
                                                if (g_pageReady)
                                                {
                                                    // A new page (a CyTube channel, or player.html back from one)
                                                    // gets the panel's volume again and the right layout.
                                                    g_lastVolumeSequence = 0;
                                                    ApplyTheater();
                                                }
                                                return S_OK;
                                            }).Get(),
                                        &navigationToken);
                                }
                                // CyTube channels show only their video (host/CytubeTheater.hpp; the script
                                // checks the page's host itself and does nothing anywhere else).
                                if (g_webview)
                                    g_webview->AddScriptToExecuteOnDocumentCreated(
                                        wxl_video_host::CytubeTheaterScript().c_str(),
                                        Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                                            [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
                                if (FAILED(mappingResult) || !g_webview ||
                                    FAILED(g_webview->Navigate(kPlayerUrl)))
                                {
                                    MessageBoxW(g_window, L"The local cinema page could not be loaded.",
                                                kWindowTitle, MB_ICONERROR | MB_OK);
                                }
                                SetTimer(g_window, kCaptureTimer, kCaptureIntervalMs, nullptr);
                                return S_OK;
                            })
                            .Get());
                })
                .Get());

        if (FAILED(createResult))
        {
            MessageBoxW(g_window,
                        L"Could not start Microsoft Edge WebView2. Install or repair the Evergreen Runtime.",
                        kWindowTitle, MB_ICONERROR | MB_OK);
        }
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            g_window = window;
            CreateWebView();
            return 0;
        case WM_SIZE:
            ResizeController();
            return 0;
        case WM_TIMER:
            if (wParam == kCaptureTimer)
            {
                ProcessControls();
                CapturePreview();
                return 0;
            }
            break;
        case kShowHostMessage:
            ShowHostWindow();
            return 0;
        case kHideHostMessage:
            HideHostWindow();
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto* minimum = reinterpret_cast<MINMAXINFO*>(lParam);
            minimum->ptMinTrackSize.x = 640;
            minimum->ptMinTrackSize.y = 420;
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, kCaptureTimer);
            g_webview.Reset();
            g_controller.Reset();
            g_pageReady = false;
            g_wic.Reset();
            ShutdownSharedFrames();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    g_background = commandLine && std::wcsstr(commandLine, L"--background") != nullptr;
    const DWORD parentPid = ParseParentPid(commandLine);
    if (parentPid)
    {
        g_parentProcess = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
        // Never leave playback running when the WarcraftXL parent is already gone.
        if (!g_parentProcess) return 0;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 1;

    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&g_wic))) || !g_wic || !InitializeSharedFrames() ||
        !InitializeSharedControl())
    {
        MessageBoxW(nullptr, L"Could not create the shared video-frame transport.",
                    kWindowTitle, MB_ICONERROR | MB_OK);
        g_wic.Reset();
        ShutdownSharedFrames();
        CoUninitialize();
        return 1;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        g_wic.Reset();
        ShutdownSharedFrames();
        CoUninitialize();
        return 1;
    }

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT windowBounds{ 0, 0, 1280, 720 };
    AdjustWindowRectEx(&windowBounds, style, FALSE, WS_EX_APPWINDOW);
    const DWORD extendedStyle = g_background
        ? (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) : WS_EX_APPWINDOW;
    HWND window = CreateWindowExW(extendedStyle, kWindowClass, kWindowTitle, style,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  windowBounds.right - windowBounds.left,
                                  windowBounds.bottom - windowBounds.top,
                                  nullptr, nullptr, instance, nullptr);
    if (!window)
    {
        g_wic.Reset();
        ShutdownSharedFrames();
        CoUninitialize();
        return 1;
    }

    if (g_parentProcess)
    {
        if (HANDLE watcher = CreateThread(nullptr, 0, &WatchParentProcess, nullptr, 0, nullptr))
            CloseHandle(watcher);
        else
        {
            DestroyWindow(window);
            CoUninitialize();
            return 1;
        }
    }

    UpdateWindow(window);
    if (g_background)
        HideHostWindow();
    else
    {
        ShowWindow(window, showCommand ? showCommand : SW_SHOW);
        ShowHostWindow();
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    return static_cast<int>(message.wParam);
}
