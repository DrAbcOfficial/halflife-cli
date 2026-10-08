#pragma once

#include <rapidjson/document.h>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

// Engine-independent state machine. Only Backend touches live VGUI interfaces;
// all calls, including Execute/Frame/Shutdown, belong to the game thread.
namespace VGUI2
{
using Json = rapidjson::Document;
using Value = rapidjson::Value;
using Handle = uint32_t;
constexpr Handle Invalid = 0;
constexpr size_t UdpBudget = 12000, TcpBudget = 1600, ChunkBytes = 384;
constexpr int MaxPanelCount = 20000;
struct Rect { int x=0, y=0, w=0, h=0; };
struct Geometry { int windowWidth=0, windowHeight=0, imageWidth=0, imageHeight=0; };
struct Panel
{
    Handle parent=0;
    std::string name, className;
    Rect local, bounds, clip;
    bool visible=true, enabled=true, mouse=true, keyboard=true, popup=false;
};
struct Backend
{
    virtual ~Backend() = default;
    virtual Handle Root()=0;
    virtual bool Read(Handle, Panel&)=0;
    virtual std::vector<Handle> Children(Handle)=0;
    virtual Geometry GetGeometry()=0;
    virtual Handle Focus()=0;
    virtual Handle Hover()=0;
    virtual Handle AppModal()=0;
    virtual Handle SurfaceModal()=0;
    virtual Handle Hit(int x, int y)=0; // window/VGUI coordinates
    virtual void RequestFocus(Handle)=0;
    virtual int HeldButtons()=0;
    virtual bool PhysicalButtonsHeld()=0;
    virtual bool Move(int x, int y)=0; // original screenshot coordinates
    virtual bool Cursor(int& x, int& y)=0; // window/VGUI coordinates
    virtual bool Mouse(int mask)=0;
    virtual const char* InputError()=0;
};
Json Parse(const std::string&);
std::string Dump(const Value&);
void Put(Value&, Json&, const char*, const std::string&);
void Put(Value&, Json&, const char*, const char*);
void Put(Value&, Json&, const char*, int);
void Put(Value&, Json&, const char*, bool);
std::string String(const Value&, const char*, const std::string& fallback="");
int Integer(const Value&, const char*, int fallback, int low, int high);
std::string Hex(const std::string&);
std::string Unhex(const std::string&);
std::string FrameReply(const Value&, const std::string& request);
Rect ScreenRect(const Geometry&, const Rect&);

class Automation
{
public:
    explicit Automation(Backend& backend);
    Json Execute(const std::string& command, const Value& args, double now, size_t budget);
    void Frame(double now);
    void Shutdown();
private:
    static constexpr int MaxNodes=MaxPanelCount, MaxSnapshots=4, MaxResults=64, PageNodes=200;
    static constexpr size_t SnapshotBytes=8*1024*1024;
    static constexpr double OperationTimeout=4.0;
    Backend& backend;
    Handle root=0;
    std::string session;
    uint64_t sequence=0;
    double lastSweep=0;
    std::map<std::string,Handle> refs;
    std::map<Handle,std::string> handles;
    // Store compact JSON rather than retaining a separate allocator arena for
    // every node. SnapshotBytes then bounds the retained node payloads.
    struct Snapshot { std::vector<std::string> nodes; Geometry geometry; };
    std::map<std::string,Snapshot> snapshots;
    std::deque<std::string> snapshotOrder, resultOrder;
    std::map<std::string,Json> results;
    struct Operation {
        std::string id, kind, ref, error;
        Json args;
        int stage=0, clicks=0, mask=0;
        bool held=false, pending=false;
        double deadline=0;
    } operation;
    std::string Token();
    void Reset();
    void Refresh(double now);
    std::string Ref(Handle);
    Handle Resolve(const std::string&);
    bool Descendant(Handle,Handle);
    bool Effective(Handle,bool enabled);
    Panel Read(Handle);
    Geometry GeometryNow();
    void Actionable(Handle);
    Json Node(Handle,int,const Geometry&);
    Json Tree(const Value&,size_t);
    Json Start(const std::string&,const Value&,double);
    bool Release();
    void Finish(const std::string& error="");
};
}
