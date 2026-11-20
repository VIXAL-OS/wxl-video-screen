#include "FilmTransport.h"
#include <algorithm>
#include <cstring>

namespace WxlFilm {
namespace P = WxlFilmPlex;
namespace C = WxlFilmCinema;
static_assert(sizeof(P::FrameHeader) == sizeof(C::FrameHeader));
void View::reset() {
    if (view_) UnmapViewOfFile(view_);
    view_ = nullptr;
}
void* View::open(DWORD pid, const wchar_t* part, DWORD access, size_t bytes) {
    if (view_) return view_;
    std::wstring name = L"Local\\WxlFilm-" + std::to_wstring(pid) + L"-" + part;
    HANDLE mapping = OpenFileMappingW(access, FALSE, name.c_str());
    if (mapping) {
        view_ = MapViewOfFile(mapping, access, 0, 0, bytes);
        CloseHandle(mapping);
    }
    return view_;
}
void Session::close() {
    if (lifetime_) { SetEvent(lifetime_); CloseHandle(lifetime_); }
    if (process_) CloseHandle(process_);
    lifetime_ = process_ = nullptr;
    pid_ = 0;
    sequence_ = 0;
    frames_.reset(); controls_.reset(); ui_.reset();
    std::vector<unsigned char>().swap(pixels);
    std::vector<unsigned char>().swap(scratch_);
    status = "Player closed.";
}
bool Session::live() {
    if (!process_) return false;
    if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) return true;
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    close();
    status = "Helper exited (code " + std::to_string(code) + ").";
    return false;
}
bool Session::start(Player next, const std::filesystem::path& dir) {
    if (live() && next == player) return true;
    auto folder = dir / L"Media";
    auto path = folder / L"wxl-video-host.exe";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        status = "The mod's Media folder is missing its helper.";
        return false;
    }
    // Open the successor before releasing the previous session.
    static unsigned serial = 0;
    // The two independently loaded provider DLLs each own a serial counter.
    auto suffix = std::to_wstring(GetCurrentProcessId()) + (next==Player::Plex ? L"-1-" : L"-2-") + std::to_wstring(++serial);
    HANDLE life = CreateEventW(nullptr, TRUE, FALSE, (L"Local\\WxlFilm-Life-" + suffix).c_str());
    if (!life || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (life) CloseHandle(life);
        status = "Could not reserve a playback session."; return false;
    }
    std::wstring args = L"\"" + path.wstring() + L"\" --background --parent-pid " +
        std::to_wstring(GetCurrentProcessId()) + L" --lifetime " + suffix;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(path.c_str(), args.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        folder.c_str(), &startup, &pi)) {
        DWORD error = GetLastError();
        CloseHandle(life); status = "Helper launch failed (" + std::to_string(error) + ").";
        return false;
    }
    CloseHandle(pi.hThread);
    close();
    lifetime_ = life; process_ = pi.hProcess; pid_ = pi.dwProcessId; player = next;
    status = "Starting player.";
    return true;
}
bool Session::attach() {
    if (!live()) return false;
    const size_t frameBytes = size_t(width()) * height() * 4;
    auto* h = static_cast<const C::FrameHeader*>(frames_.open(pid_, L"Frames", FILE_MAP_READ,
        sizeof(C::FrameHeader) + 2 * frameBytes));
    if (!h || h->hostPid != static_cast<LONG>(pid_)) return false;
    if (h->magic != C::kMagic || h->version != C::kVersion || h->headerBytes != sizeof(*h) ||
        h->width != width() || h->height != height() || h->stride != width()*4 || h->frameBytes != frameBytes) {
        status = "Incompatible helper frame layout."; return false;
    }
    if (status == "Starting player.") status = "Player connected.";
    return true;
}
bool Session::frame() {
    if (!attach()) return false;
    size_t bytes = size_t(width()) * height() * 4;
    auto* h = static_cast<const C::FrameHeader*>(frames_.open(pid_, L"Frames", FILE_MAP_READ, sizeof(C::FrameHeader) + 2*bytes));
    LONG next = h->sequence;
    MemoryBarrier();
    LONG slot = h->activeIndex;
    if (!h->ready || next <= 0 || next == sequence_ || slot < 0 || slot > 1 || h->slotSequence[slot] != next) return false;
    MemoryBarrier();
    scratch_.resize(bytes);
    memcpy(scratch_.data(), reinterpret_cast<const unsigned char*>(h) + sizeof(*h) + bytes*slot, bytes);
    MemoryBarrier();
    if (!h->ready || h->slotSequence[slot] != next || h->hostPid != static_cast<LONG>(pid_)) return false;
    pixels.swap(scratch_);
    sequence_ = next;
    return true;
}
bool Session::snapshot(P::UiSnapshot& snap) {
    if (player != Player::Plex || !attach()) return false;
    auto* ui = static_cast<const P::UiBridge*>(ui_.open(pid_, L"Ui", FILE_MAP_READ | FILE_MAP_WRITE, sizeof(P::UiBridge)));
    return P::ReadUiSnapshot(ui, snap);
}

template<class Block>
static bool valid(const Block* b) {
    return b && b->magic == C::kControlMagic && b->version == C::kControlVersion && b->structBytes == sizeof(*b);
}
bool Session::control(const std::string& cmd, const std::string& text, int value) {
    if (!attach()) return false;
    constexpr DWORD rw = FILE_MAP_READ | FILE_MAP_WRITE;
    if (cmd == "volume") {
        if (value < 0 || value > 100) return false;
        auto volume = [value](auto* b) {
            if (!valid(b)) return false;
            InterlockedExchange(&b->volumePercent, value);
            InterlockedIncrement(&b->volumeSequence);
            return true;
        };
        if (player == Player::Cinema)
            return volume(static_cast<C::ControlBlock*>(controls_.open(pid_, L"Control", rw, sizeof(C::ControlBlock))));
        return volume(static_cast<P::ControlBlock*>(controls_.open(pid_, L"Control", rw, sizeof(P::ControlBlock))));
    }
    if (player == Player::Cinema) {
        auto* b = static_cast<C::ControlBlock*>(controls_.open(pid_, L"Control", rw, sizeof(C::ControlBlock)));
        if (!valid(b) || InterlockedCompareExchange(&b->command, 0, 0) != 0) return false;
        C::Command op;
        if (cmd == "load") {
            if (text.empty() || text.size() >= sizeof(b->url) || text.find('\0') != text.npos) return false;
            op = C::Command::LoadAndPlay;
        } else if (cmd == "play") op = C::Command::Play;
        else if (cmd == "pause") op = C::Command::Pause;
        else if (cmd == "stop") op = C::Command::Stop;
        else return false;
        if (cmd == "load") memcpy(b->url, text.c_str(), text.size()+1);
        InterlockedExchange(&b->command, static_cast<LONG>(op));
        InterlockedIncrement(&b->commandSequence);
        return true;
    }
    auto* ui = static_cast<P::UiBridge*>(ui_.open(pid_, L"Ui", rw, sizeof(P::UiBridge)));
    if (!ui || ui->magic != P::kUiMagic || ui->version != P::kUiVersion || ui->structBytes != sizeof(*ui)) return false;
    if (InterlockedCompareExchange(&ui->command.command, 0, 0) != 0) return false;
    struct Op { const char* name; P::UiCommand id; };
    static const Op ops[] = {
        {"login",P::UiCommand::StartLogin}, {"server",P::UiCommand::SelectServer}, {"library",P::UiCommand::SelectSection},
        {"item",P::UiCommand::SelectItem}, {"search",P::UiCommand::Search}, {"play",P::UiCommand::Play},
        {"pause",P::UiCommand::Pause}, {"stop",P::UiCommand::Stop}, {"next",P::UiCommand::NextPage},
        {"prev",P::UiCommand::PreviousPage}, {"back",P::UiCommand::NavigateBack}, {"create",P::UiCommand::CreateParty},
        {"join",P::UiCommand::JoinParty}, {"leave",P::UiCommand::LeaveParty}, {"channel",P::UiCommand::JoinCytube},
        {"leavechannel",P::UiCommand::LeaveCytube}
    };
    for (auto op : ops) if (cmd == op.name) {
        P::UiCommandPacket packet{};
        if (text.size() >= sizeof(packet.text) || text.find('\0') != text.npos) return false;
        packet.command = static_cast<LONG>(op.id);
        packet.index = value - 1; // Panel rows start at one.
        memcpy(packet.text, text.c_str(), text.size()+1);
        ui->command = packet;
        MemoryBarrier();
        InterlockedIncrement(&ui->commandSequence);
        return true;
    }
    return false;
}
}
