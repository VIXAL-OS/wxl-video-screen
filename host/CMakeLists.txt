cmake_minimum_required(VERSION 3.20)
project(wxl_video_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(WEBVIEW2_ROOT "${CMAKE_CURRENT_LIST_DIR}/third_party/webview2/Microsoft.Web.WebView2.1.0.4129.50"
    CACHE PATH "Root of the Microsoft.Web.WebView2 SDK")

add_executable(wxl-video-host WIN32 VideoHost.cpp)
target_include_directories(wxl-video-host PRIVATE "${WEBVIEW2_ROOT}/build/native/include")
target_compile_definitions(wxl-video-host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(wxl-video-host PRIVATE
    "${WEBVIEW2_ROOT}/build/native/x86/WebView2LoaderStatic.lib"
    user32 ole32 oleaut32 shell32 windowscodecs)
