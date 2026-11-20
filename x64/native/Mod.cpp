#include "FilmTransport.h"
#include "Screen.h"
#include <wxl/mod.h>
#include <wxl/services/cvar.h>
#include <wxl/services/gx.h>
#include <wxl/services/script.h>
#include <algorithm>
#include <cmath>
#include <string_view>

namespace {
using namespace WxlFilm;
namespace P = WxlFilmPlex;
#ifdef FILM_PLEX
constexpr auto player=Player::Plex;
constexpr char id[]="vixal.azerothplex", title[]="Azeroth Plex", actionName[]="WXLFilmPlexAction", stateName[]="WXLFilmPlexState";
#else
constexpr auto player=Player::Cinema;
constexpr char id[]="vixal.videocinema", title[]="Video Cinema", actionName[]="WXLFilmCinemaAction", stateName[]="WXLFilmCinemaState";
#endif
const wxl_host* host;
const wxl_script_api* script;
const wxl_gx_api* gx;
const wxl_camera_api* camera;
const wxl_cvar_api* cvar;
std::filesystem::path folder;
Session session;
Screen screen;
P::UiSnapshot snapshot;
float aspect=16.f/9, farClip=777;
unsigned mapId=0;
bool inWorld=false;
ULONGLONG refreshAt=0;
std::string screenStatus;
wxl_str str(std::string_view s) { return {s.data(),static_cast<uint32_t>(s.size())}; }
void push(wxl_script_call* call,std::string_view s) { script->push_string(call,str(s)); }
bool readCamera(CameraView& v) {
    return inWorld && !camera->cinematic() && camera->position(&v.eye)==WXL_OK &&
        camera->forward(&v.forward)==WXL_OK && camera->up(&v.up)==WXL_OK && camera->fov(&v.fov)==WXL_OK;
}
IDirect3DDevice9* device() {
    auto api=gx->api();
    if (api!=WXL_GX_BACKEND_D3D9 && api!=WXL_GX_BACKEND_D3D9EX) return nullptr;
    return static_cast<IDirect3DDevice9*>(gx->d3d9_device());
}
void closeSession() {
    screen.hide(); session.close(); screenStatus.clear(); aspect=16.f/9; refreshAt=0;
}
bool getString(wxl_script_call* call,unsigned index,size_t limit,std::string& value) {
    if (script->arg_count(call)<=index) return true;
    wxl_str input{};
    if (!script->arg_string(call,index,&input) || input.size>limit || (input.size && !input.data)) return false;
    value.assign(input.data?input.data:"",input.size);
    return value.find('\0')==std::string::npos;
}
void WXL_CALL action(wxl_script_call* call,void*) {
    std::string cmd,text,message;
    bool ok=false;
    if (!getString(call,0,32,cmd) || !getString(call,1,1023,text)) {
        message="Invalid or oversized film command.";
    } else if (cmd=="close") {
        closeSession(); ok=true;
    } else if (!inWorld) {
        message="Enter the world to start a film.";
    } else if (cmd=="open") {
        ok=session.start(player,folder); message=session.status;
    } else if (cmd=="hide") {
        screen.hide(); screenStatus.clear(); ok=true;
    } else if (cmd=="place") {
        CameraView view{};
        if (!session.live()) message="Choose a player first.";
        else if (!device()) message="This world screen requires Direct3D 9 or 9Ex.";
        else if (readCamera(view) && screen.place(view)) { ok=true; screenStatus.clear(); }
        else message="Place the screen in a world view while looking toward the horizon.";
    } else {
        double n=0;
        int value=script->arg_number(call,2,&n) && std::isfinite(n) && n>=0 && n<=100 ? int(n) : -1;
        ok=session.control(cmd,text,value);
        if (!ok) message=session.opened() ? "The player is starting, busy, or the input is unavailable." : session.status;
    }
    script->push_bool(call,ok); push(call,message);
}
template<size_t N> std::string_view field(const char (&s)[N]) { return {s,strnlen_s(s,N)}; }
void WXL_CALL state(wxl_script_call* call,void*) {
    std::string kind;
    double offset=0;
    getString(call,0,12,kind); script->arg_number(call,1,&offset);
    unsigned first=std::isfinite(offset) && offset>=0 && offset<=64 ? unsigned(offset) : 0;
    bool alive=session.live();
    if (!alive && screen.placed()) screen.hide();
    const bool got=player==Player::Plex && alive && session.snapshot(snapshot);
    std::string_view status=session.status;
    if (got && snapshot.status[0]) status=field(snapshot.status);
    if (got && snapshot.cytube.active && snapshot.cytube.status[0]) status=field(snapshot.cytube.status);
    script->push_bool(call,alive); script->push_bool(call,screen.placed()); push(call,status); push(call,screenStatus);
    push(call,got?field(snapshot.error):""); push(call,got?field(snapshot.partyError):""); push(call,got?field(snapshot.partyRoomCode):"");
    unsigned count=0;
    if (got && kind=="server") count=std::min<size_t>(snapshot.serverCount,std::size(snapshot.servers));
    if (got && kind=="library") count=std::min<size_t>(snapshot.sectionCount,std::size(snapshot.sections));
    if (got && kind=="item") count=std::min<size_t>(snapshot.itemCount,std::size(snapshot.items));
    script->push_number(call,count);
    for (unsigned i=first; i<count && i-first<5; ++i) {
        if (kind=="server") push(call,field(snapshot.servers[i].name));
        else if (kind=="library") push(call,field(snapshot.sections[i].title));
        else push(call,field(snapshot.items[i].title));
    }
}
void WXL_CALL scriptReady(void*,const void* raw) {
    if (static_cast<const wxl_script_vm_args*>(raw)->context!=WXL_SCRIPT_CONTEXT_GAME) return;
    if (script->register_function(actionName,action,nullptr)!=WXL_OK ||
        script->register_function(stateName,state,nullptr)!=WXL_OK)
        host->disable(str("Could not register the Film Club game functions."));
}
void WXL_CALL enter(void*,const void* raw) {
    mapId=static_cast<const wxl_world_args*>(raw)->map_id; inWorld=true;
}
void WXL_CALL leave(void*,const void*) { inWorld=false; closeSession(); }
void WXL_CALL vmClosing(void*,const void* raw) {
    if (static_cast<const wxl_script_vm_args*>(raw)->context==WXL_SCRIPT_CONTEXT_GAME) closeSession();
}
void WXL_CALL lost(void*,const void*) { screen.releaseDevice(); }
void WXL_CALL draw(void*,const void* raw) {
    if (!screen.placed() || static_cast<const wxl_render_point_args*>(raw)->point!=WXL_RENDER_POINT_AFTER_OPAQUE_WORLD) return;
    if (!session.live()) { screen.hide(); return; }
    CameraView view{};
    if (!readCamera(view)) return;
    auto d=device();
    if (!d) { screen.releaseDevice(); return; }
    auto now=GetTickCount64();
    if (now>=refreshAt) {
        refreshAt=now+500;
        double clip=777,overrideClip=0;
        cvar->get_number(str("farclip"),&clip); cvar->get_number(str("farclipoverride"),&overrideClip);
        bool limited=(mapId<530 || mapId==575 || mapId==543) && overrideClip<1;
        farClip=float(std::clamp(clip,550./3,(limited?2375.:4750.)/3));
        if (player==Player::Plex && session.snapshot(snapshot) && snapshot.videoAspectRatio>=0.25f && snapshot.videoAspectRatio<=5.f)
            aspect=snapshot.videoAspectRatio;
    }
    bool changed=session.frame();
    if (session.pixels.empty()) { screenStatus="Waiting for video."; return; }
    screenStatus=screen.draw(d,view,farClip,session.pixels,session.width(),session.height(),changed,aspect);
    gx->invalidate_state();
}
struct Event { wxl_event type; wxl_event_fn fn; wxl_subscription token=0; };
Event events[]={{WXL_EV_ScriptVmReady,scriptReady},{WXL_EV_ScriptVmClosing,vmClosing},{WXL_EV_WorldEnter,enter},
                {WXL_EV_WorldLeave,leave},{WXL_EV_SwapchainLost,lost},{WXL_EV_RenderPoint,draw}};
void unsubscribe() {
    for (auto& e:events) if(e.token) { host->unsubscribe(e.token); e.token=0; }
}
}
extern "C" const wxl_mod_info* WXL_CALL wxl_mod_query() {
    static const wxl_mod_info info{sizeof(info),WXL_ABI_VERSION_MAJOR,WXL_ABI_VERSION_MINOR,id,title,0,WXL_ABI_VERSION_FEATURE};
    return &info;
}
extern "C" wxl_result WXL_CALL wxl_mod_load(const wxl_host* h) {
    try {
        host=h;
        if (!h) return WXL_E_VERSION;
        script=static_cast<const wxl_script_api*>(h->get_service(WXL_SCRIPT_SERVICE,WXL_SCRIPT_SERVICE_VERSION));
        gx=static_cast<const wxl_gx_api*>(h->get_service(WXL_GX_SERVICE,WXL_GX_SERVICE_VERSION));
        cvar=static_cast<const wxl_cvar_api*>(h->get_service(WXL_CVAR_SERVICE,WXL_CVAR_SERVICE_VERSION));
        camera=static_cast<const wxl_camera_api*>(h->get_service(WXL_CAMERA_SERVICE,WXL_CAMERA_SERVICE_VERSION));
        if (!script || !gx || !cvar || !camera) return WXL_E_UNSUPPORTED;
        // Resolve the provider's own bin/x64 DLL. The user may rename its mod directory.
        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&wxl_mod_query),&self)) return WXL_E_FAILED;
        wchar_t path[32768];
        auto n=GetModuleFileNameW(self,path,static_cast<DWORD>(std::size(path)));
        if (!n || n>=std::size(path)) return WXL_E_FAILED;
        folder=std::filesystem::path(path).parent_path().parent_path().parent_path();
        for (auto& e:events) {
            e.token=h->subscribe(e.type,e.fn,nullptr,WXL_PRIORITY_DEFAULT);
            if (!e.token) { unsubscribe(); return WXL_E_UNSUPPORTED; }
        }
        return WXL_OK;
    } catch (...) { if (host) unsubscribe(); return WXL_E_FAILED; }
}
extern "C" void WXL_CALL wxl_mod_stage(wxl_stage stage) { if (stage==WXL_STAGE_SHUTDOWN) leave(nullptr,nullptr); }
extern "C" void WXL_CALL wxl_mod_unload() { unsubscribe(); closeSession(); host=nullptr; }

