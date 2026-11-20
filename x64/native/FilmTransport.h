#pragma once
#include "CinemaProtocol.h"
#include "PlexProtocol.h"
#include <filesystem>
#include <string>
#include <vector>

namespace WxlFilm {
enum class Player { Cinema, Plex };
class View {
    void* view_ = nullptr;
public:
    ~View() { reset(); }
    void reset();
    void* open(DWORD pid, const wchar_t* name, DWORD access, size_t bytes);
};
class Session {
    View frames_, controls_, ui_;
    HANDLE process_ = nullptr;
    HANDLE lifetime_ = nullptr;
    DWORD pid_ = 0;
    LONG sequence_ = 0;
    std::vector<unsigned char> scratch_;
    bool attach();
public:
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() { close(); }
    Player player = Player::Cinema;
    std::string status = "Player closed.";
    std::vector<unsigned char> pixels;
    unsigned width() const { return player == Player::Plex ? WxlFilmPlex::kWidth : WxlFilmCinema::kWidth; }
    unsigned height() const { return player == Player::Plex ? WxlFilmPlex::kHeight : WxlFilmCinema::kHeight; }
    bool opened() const { return process_ != nullptr; }
    bool start(Player next, const std::filesystem::path& clientDirectory);
    bool live();
    void close();
    bool frame();
    bool control(const std::string& command, const std::string& text, int value);
    bool snapshot(WxlFilmPlex::UiSnapshot& snap);
};
}
