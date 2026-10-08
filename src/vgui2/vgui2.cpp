#include "automation.h"
#include "vgui2.h"
#include "core/plugins.h"
#include "input/engine_input.h"
#include "input/input_state.h"
#include "rcon/rcon_server.h"
#include "util/vgui2_extension.h"
#include <metahook.h>
#include <Interface/VGUI/IPanel.h>
#include <Interface/VGUI/IInput.h>
#include <Interface/VGUI/IVGui.h>
#include <Interface/VGUI/ISurface.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace VGUI2
{
namespace
{
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
class NativeBackend final : public Backend
{
    HMODULE module=nullptr;
    vgui::IVGui* gui=nullptr;
    vgui::IPanel* panels=nullptr;
    vgui::IInput* input=nullptr;
    vgui::ISurface* surface=nullptr;
    vgui::ISurface_HL25* surface25=nullptr;
    bool stopped=false;
    // HPanel zero is valid; encode h+1 to reserve our zero for "no panel".
    Handle HandleOf(vgui::VPANEL p) { return p ? static_cast<Handle>(gui->PanelToHandle(p)+1) : 0; }
    vgui::VPANEL PanelOf(Handle h) { return gui && h ? gui->HandleToPanel(h-1) : 0; }
public:
    void Clear() { module=nullptr; gui=nullptr; panels=nullptr; input=nullptr; surface=nullptr; surface25=nullptr; }
    void Stop() { Clear(); stopped=true; }
    void Start() { Clear(); stopped=false; }
    Handle Root() override {
        if(stopped) return 0;
        auto loaded=GetModuleHandleA("vgui2.dll");
        if(loaded!=module) { Clear(); module=loaded; }
        if(!module) return 0;
        if(!gui || !panels || !input || (!surface && !surface25)) {
            auto factory=Sys_GetFactory(reinterpret_cast<HINTERFACEMODULE>(module));
            auto engine=g_pMetaHookAPI->GetEngineFactory();
            if(!factory || !engine) return 0;
            gui=static_cast<vgui::IVGui*>(factory(VGUI_IVGUI_INTERFACE_VERSION,nullptr));
            panels=static_cast<vgui::IPanel*>(factory(VGUI_PANEL_INTERFACE_VERSION,nullptr));
            input=static_cast<vgui::IInput*>(factory(VGUI_INPUT_INTERFACE_VERSION,nullptr));
            if(g_iEngineType==ENGINE_GOLDSRC_HL25) surface25=static_cast<vgui::ISurface_HL25*>(engine(VGUI_SURFACE_INTERFACE_VERSION,nullptr));
            else surface=static_cast<vgui::ISurface*>(engine(VGUI_SURFACE_INTERFACE_VERSION,nullptr));
        }
        if(!gui || !panels || !input || (!surface && !surface25) || !gui->IsRunning()) return 0;
        return HandleOf(surface25 ? surface25->GetEmbeddedPanel() : surface->GetEmbeddedPanel());
    }
    bool Read(Handle h,Panel& out) override {
        auto p=PanelOf(h); if(!p) return false;
        out.parent=HandleOf(panels->GetParent(p));
        auto name=panels->GetName(p), type=panels->GetClassName(p);
        out.name=name?name:""; out.className=type?type:"";
        panels->GetPos(p,out.local.x,out.local.y); panels->GetSize(p,out.local.w,out.local.h);
        out.bounds=out.local; panels->GetAbsPos(p,out.bounds.x,out.bounds.y);
        int right,bottom; panels->GetClipRect(p,out.clip.x,out.clip.y,right,bottom);
        out.clip.w=(std::max)(0,right-out.clip.x); out.clip.h=(std::max)(0,bottom-out.clip.y);
        out.visible=panels->IsVisible(p); out.enabled=panels->IsEnabled(p);
        out.mouse=panels->IsMouseInputEnabled(p); out.keyboard=panels->IsKeyBoardInputEnabled(p); out.popup=panels->IsPopup(p);
        return true;
    }
    std::vector<Handle> Children(Handle h) override {
        auto p=PanelOf(h); std::vector<Handle> out; if(!p) return out;
        int count=panels->GetChildCount(p);
        if(count<0 || count>MaxPanelCount) throw std::runtime_error("VGUI2 child count exceeds safety limit");
        out.reserve(count); for(int i=0;i<count;++i) out.push_back(HandleOf(panels->GetChild(p,i))); return out;
    }
    Geometry GetGeometry() override {
        InputState::MouseGeometry g;
        if(!EngineInput::GetMouseGeometry(g)) throw std::runtime_error("VGUI2 window geometry is unavailable");
        return {g.windowWidth,g.windowHeight,g.imageWidth,g.imageHeight};
    }
    Handle Focus() override { return HandleOf(input->GetFocus()); }
    Handle Hover() override { return HandleOf(input->GetMouseOver()); }
    Handle AppModal() override { return HandleOf(input->GetAppModalSurface()); }
    Handle SurfaceModal() override { return HandleOf(surface25?surface25->GetModalPanel():surface->GetModalPanel()); }
    Handle Hit(int x,int y) override {
        int count=surface25?surface25->GetPopupCount():surface->GetPopupCount();
        if(count<0 || count>MaxPanelCount) throw std::runtime_error("VGUI2 popup count exceeds safety limit");
        for(int i=count-1;i>=0;--i) {
            auto p=surface25?surface25->GetPopup(i):surface->GetPopup(i);
            bool visible=true; auto ancestor=p;
            for(int depth=0;ancestor && depth<MaxPanelCount;++depth) {
                if(!panels->IsVisible(ancestor)) { visible=false; break; } ancestor=panels->GetParent(ancestor);
            }
            if(!visible || ancestor || !panels->IsMouseInputEnabled(p)) continue;
            if(auto hit=panels->IsWithinTraverse(p,x,y,false)) return HandleOf(hit);
        }
        auto root=surface25?surface25->GetEmbeddedPanel():surface->GetEmbeddedPanel();
        return HandleOf(panels->IsWithinTraverse(root,x,y,false));
    }
    void RequestFocus(Handle h) override { if(auto p=PanelOf(h)) panels->RequestFocus(p); }
    int HeldButtons() override { return EngineInput::GetHeldMouseButtons(); }
    bool PhysicalButtonsHeld() override {
        return input->IsMouseDown(vgui::MOUSE_LEFT) || input->IsMouseDown(vgui::MOUSE_RIGHT) || input->IsMouseDown(vgui::MOUSE_MIDDLE);
    }
    bool Move(int x,int y) override { return EngineInput::MoveMouse(x,y,false); }
    bool Cursor(int& x,int& y) override { return EngineInput::GetVirtualMousePosition(x,y); }
    bool Mouse(int mask) override { return EngineInput::SendMouse(mask,mask!=0); }
    const char* InputError() override { return EngineInput::EngineHooksError(); }
};
NativeBackend backend;
Automation& State() { static Automation state(backend); return state; }
IVGUI2Extension* extension=nullptr;
class Lifecycle final : public IVGUI2Extension_BaseUICallbacks
{
public:
    int GetAltitude() const override { return 1000; }
    void Initialize(CreateInterfaceFn*,int) override { State().Shutdown(); backend.Start(); }
    void Shutdown() override { State().Shutdown(); backend.Stop(); }
    void Start(cl_enginefuncs_s*,int) override {}
    void Key_Event(int&,int&,const char*&,VGUI2Extension_CallbackContext*) override {}
    void CallEngineSurfaceAppProc(void*&,void*&,VGUI2Extension_CallbackContext*) override {}
    void CallEngineSurfaceWndProc(void*&,unsigned int&,unsigned int&,long&,VGUI2Extension_CallbackContext*) override {}
    void Paint(int&,int&,int&,int&,VGUI2Extension_CallbackContext*) override {}
    void HideGameUI(VGUI2Extension_CallbackContext*) override {}
    void ActivateGameUI(VGUI2Extension_CallbackContext*) override {}
    void HideConsole(VGUI2Extension_CallbackContext*) override {}
    void ShowConsole(VGUI2Extension_CallbackContext*) override {}
} lifecycle;
void ObserveLifecycle()
{
    if(!extension) {
        extension=FindVGUI2Extension();
        if(extension) extension->RegisterBaseUICallbacks(&lifecycle);
    }
}
int Number(const char* text) {
    size_t used=0; std::string s=text; int value=std::stoi(s,&used);
    if(used!=s.size()) throw std::runtime_error("invalid integer argument"); return value;
}
void Print(const std::string& text) {
    // Each framed line fits the engine's print buffer and UDP datagram budget.
    size_t start=0;
    while(start<text.size()) {
        auto end=text.find('\n',start); if(end==std::string::npos) end=text.size(); else ++end;
        for(size_t pos=start;pos<end;pos+=768) gEngfuncs.Con_Printf("%s",text.substr(pos,(std::min)(size_t(768),end-pos)).c_str());
        start=end;
    }
}
}
void Command()
{
    std::string request;
    try {
        int argc=gEngfuncs.Cmd_Argc(); auto argv=gEngfuncs.Cmd_Argv;
        std::string command=argc>1?argv(1):"tree"; Json args; args.SetObject();
        if(argc>2 && std::string(argv(2))=="--request") {
            if(argc!=5) throw std::runtime_error("expected --request <id> <hex-json>");
            request=argv(3);
            if(request.size()!=32 || request.find_first_not_of("0123456789abcdef")!=std::string::npos) {
                request.clear(); throw std::runtime_error("invalid VGUI2 request ID");
            }
            args=Parse(Unhex(argv(4)));
        } else if(command=="tree") {
            for(int i=2;i<argc;++i) {
                std::string arg=argv(i);
                if(arg=="--all") Put(args,args,"include_hidden",true);
                else if((arg=="--root" || arg=="--cursor" || arg=="--depth") && i+1<argc) {
                    const char* value=argv(++i);
                    if(arg=="--depth") Put(args,args,"max_depth",Number(value));
                    else Put(args,args,arg=="--root"?"root":"cursor",value);
                } else throw std::runtime_error("tree [--all] [--root ref] [--cursor cursor] [--depth n]");
            }
        } else {
            if(argc<3) throw std::runtime_error("expected a VGUI2 reference or operation ID");
            Put(args,args,command=="result"?"operation":"ref",argv(2));
            if(command=="inspect") {
                if(argc>4) throw std::runtime_error("inspect <ref> [text_offset]");
                if(argc==4) Put(args,args,"text_offset",Number(argv(3)));
            } else if(command=="set_text") {
                if(argc!=4) throw std::runtime_error("set_text <ref> <UTF-8-hex>");
                Put(args,args,"text",Unhex(argv(3)));
            } else if(command=="click") {
                if(argc==6 || argc>7) throw std::runtime_error("click <ref> [left|right] [1|2] [x y]");
                if(argc>3) Put(args,args,"button",argv(3)); if(argc>4) Put(args,args,"click_count",Number(argv(4)));
                if(argc==7) { Put(args,args,"x",Number(argv(5))); Put(args,args,"y",Number(argv(6))); }
            } else if(argc!=3) throw std::runtime_error("unexpected VGUI2 arguments");
        }
        size_t budget=std::string(RconServer::Protocol())=="goldsrc-udp"?UdpBudget:TcpBudget;
        auto result=State().Execute(command,args,Now(),budget);
        if(!request.empty()) Print(FrameReply(result,request));
        else if(result.HasMember("nodes")) {
            for(auto& node:result["nodes"].GetArray()) {
                auto depth=Integer(node,"depth",0,0,20000);
                Print(std::string((std::min)(depth,64)*2,' ')+String(node,"ref")+" "+String(node,"class")+" "+Dump(node["name"])+
                    " bounds="+Dump(node["bounds"])+" visible="+Dump(node["visible"])+" enabled="+Dump(node["enabled"])+"\n");
            }
            if(!result["next_cursor"].IsNull()) Print("cli.vgui2: next_cursor="+String(result,"next_cursor")+"\n");
        } else Print(Dump(result)+"\n");
    } catch(const std::exception& error) {
        Json result; result.SetObject(); Put(result,result,"error",error.what());
        Print(request.empty()?Dump(result)+"\n":FrameReply(result,request));
    }
}
void Frame() { ObserveLifecycle(); State().Frame(Now()); }
void Shutdown()
{
    State().Shutdown();
    if(extension && FindVGUI2Extension()==extension) extension->UnregisterBaseUICallbacks(&lifecycle);
    extension=nullptr;
    backend.Start();
}
}
