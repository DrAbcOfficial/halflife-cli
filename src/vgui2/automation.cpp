#include "automation.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/memorystream.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>

namespace VGUI2
{
namespace
{
[[noreturn]] void Fail(const std::string& error) { throw std::runtime_error(error); }
Json Object() { Json d; d.SetObject(); return d; }
void Member(Value& object, Json& owner, const char* key, Value value)
{
    if (object.HasMember(key)) object[key]=std::move(value);
    else object.AddMember(Value(key,owner.GetAllocator()),value,owner.GetAllocator());
}
void CopyMember(Value& object, Json& owner, const char* key, const Value& value)
{
    Value copy; copy.CopyFrom(value,owner.GetAllocator()); Member(object,owner,key,std::move(copy));
}
Value Array(Json& owner, std::initializer_list<int> values)
{
    Value array(rapidjson::kArrayType);
    for (int v:values) array.PushBack(v,owner.GetAllocator());
    return array;
}
void Rectangle(Json& node,const char* name,Rect r) { Member(node,node,name,Array(node,{r.x,r.y,r.w,r.h})); }
Json Status(const char* status,const std::string& id)
{
    auto d=Object(); Put(d,d,"status",status); Put(d,d,"operation",id); return d;
}
bool Boolean(const Value& args,const char* key,bool fallback)
{
    if (!args.HasMember(key)) return fallback;
    if (!args[key].IsBool()) Fail(std::string(key)+" must be a boolean");
    return args[key].GetBool();
}
}
void Put(Value& o,Json& d,const char* k,const std::string& v)
{
    // Game control names are not guaranteed to contain valid UTF-8. Preserve
    // valid sequences and replace bad ones rather than emitting invalid JSON.
    rapidjson::MemoryStream in(v.data(),v.size()); rapidjson::StringBuffer out;
    while(in.Tell()<v.size()) {
        unsigned codepoint=0;
        if(!rapidjson::UTF8<>::Decode(in,&codepoint)) codepoint=0xfffd;
        rapidjson::UTF8<>::Encode(out,codepoint);
    }
    Member(o,d,k,Value(out.GetString(),static_cast<rapidjson::SizeType>(out.GetSize()),d.GetAllocator()));
}
void Put(Value& o,Json& d,const char* k,const char* v) { Put(o,d,k,std::string(v)); }
void Put(Value& o,Json& d,const char* k,int v) { Member(o,d,k,Value(v)); }
void Put(Value& o,Json& d,const char* k,bool v) { Member(o,d,k,Value(v)); }
Json Parse(const std::string& text)
{
    Json d;
    d.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(),text.size());
    if(d.HasParseError() || !d.IsObject()) Fail("VGUI2 arguments must be a valid UTF-8 JSON object");
    return d;
}
std::string Dump(const Value& value)
{
    rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    if(!value.Accept(writer)) Fail("VGUI2 JSON serialization failed");
    return {buffer.GetString(),buffer.GetSize()};
}
std::string String(const Value& args,const char* key,const std::string& fallback)
{
    if(!args.HasMember(key) || args[key].IsNull()) return fallback;
    if(!args[key].IsString()) Fail(std::string(key)+" must be a string");
    return {args[key].GetString(),args[key].GetStringLength()};
}
int Integer(const Value& args,const char* key,int fallback,int low,int high)
{
    if(!args.HasMember(key) || args[key].IsNull()) return fallback;
    if(!args[key].IsInt64()) Fail(std::string(key)+" must be an integer");
    auto v=args[key].GetInt64();
    if(v<low || v>high) Fail(std::string(key)+" is out of range");
    return static_cast<int>(v);
}
std::string Hex(const std::string& bytes)
{
    constexpr char digits[]="0123456789abcdef"; std::string out; out.reserve(bytes.size()*2);
    for(unsigned char c:bytes) { out+=digits[c>>4]; out+=digits[c&15]; } return out;
}
std::string Unhex(const std::string& value)
{
    constexpr size_t MaxRequestBytes=65536;
    if(value.size()%2 || value.size()>MaxRequestBytes*2) Fail("invalid hex length");
    auto digit=[](char c) { if(c>='0' && c<='9') return c-'0'; if(c>='a' && c<='f') return c-'a'+10; Fail("invalid hex digit"); };
    std::string out; out.reserve(value.size()/2);
    for(size_t i=0;i<value.size();i+=2) out+=static_cast<char>((digit(value[i])<<4)|digit(value[i+1]));
    return out;
}
std::string FrameReply(const Value& value,const std::string& request)
{
    auto payload=Dump(value); size_t count=(payload.size()+ChunkBytes-1)/ChunkBytes;
    auto prefix="VGUI2 "+request+" ";
    std::string out=prefix+"begin "+std::to_string(count)+"\n";
    for(size_t i=0;i<count;++i) out+=prefix+"data "+std::to_string(i)+" "+Hex(payload.substr(i*ChunkBytes,ChunkBytes))+"\n";
    return out+prefix+"end "+std::to_string(count)+"\n";
}
Rect ScreenRect(const Geometry& g,const Rect& r)
{
    if(g.windowWidth<=0 || g.windowHeight<=0 || g.imageWidth<=0 || g.imageHeight<=0) Fail("VGUI2 window geometry is unavailable");
    auto coordinate=[](double v) {
        if(v<std::numeric_limits<int>::min() || v>std::numeric_limits<int>::max()) Fail("VGUI2 coordinate overflow");
        return static_cast<int>(v);
    };
    double x=std::floor(double(r.x)*g.imageWidth/g.windowWidth), y=std::floor(double(r.y)*g.imageHeight/g.windowHeight);
    return {coordinate(x),coordinate(y),coordinate(std::ceil((double(r.x)+r.w)*g.imageWidth/g.windowWidth)-x),
        coordinate(std::ceil((double(r.y)+r.h)*g.imageHeight/g.windowHeight)-y)};
}
Automation::Automation(Backend& b):backend(b)
{
    std::random_device random; session=std::to_string(random())+"-"+std::to_string(random());
}
std::string Automation::Token() { return session+"-"+std::to_string(++sequence); }
bool Automation::Release()
{
    if(operation.held && backend.Mouse(backend.HeldButtons() & ~operation.mask)) operation.held=false;
    return !operation.held;
}
void Automation::Reset()
{
    // A failed release must keep its ownership and block subsequent actions.
    // Frame will retry it even if the target/root no longer exists.
    if(!Release()) { operation.error="VGUI2 reset interrupted the operation"; operation.pending=true; }
    else operation=Operation{};
    refs.clear(); handles.clear(); snapshots.clear(); snapshotOrder.clear(); results.clear(); resultOrder.clear(); root=0; lastSweep=0;
}
void Automation::Shutdown() { Reset(); }
void Automation::Refresh(double now)
{
    Handle current=backend.Root();
    if(current!=root) { Reset(); root=current; }
    if(!root) Fail("VGUI2 is not initialized");
    if(now-lastSweep>=1) {
        lastSweep=now;
        for(auto it=refs.begin();it!=refs.end();) {
            if(!Descendant(it->second,root)) { handles.erase(it->second); it=refs.erase(it); } else ++it;
        }
    }
}
Panel Automation::Read(Handle h) { Panel p; if(!h || !backend.Read(h,p)) Fail("VGUI2 control was destroyed or detached"); return p; }
bool Automation::Descendant(Handle h,Handle ancestor)
{
    for(int i=0;h && i<MaxNodes;++i) {
        Panel p; if(!backend.Read(h,p)) return false;
        if(h==ancestor) return true; h=p.parent;
    } return false;
}
bool Automation::Effective(Handle h,bool enabled)
{
    for(int i=0;h && i<MaxNodes;++i) { auto p=Read(h); if(!(enabled?p.enabled:p.visible)) return false; h=p.parent; }
    return !h;
}
std::string Automation::Ref(Handle h)
{
    if(!h) return "";
    auto it=handles.find(h); if(it!=handles.end()) return it->second;
    auto token=Token(); refs[token]=h; handles[h]=token; return token;
}
Handle Automation::Resolve(const std::string& ref)
{
    auto it=refs.find(ref);
    if(it==refs.end() || !Descendant(it->second,root)) Fail("unknown or expired VGUI2 reference");
    return it->second;
}
Geometry Automation::GeometryNow() { auto g=backend.GetGeometry(); ScreenRect(g,{}); return g; }
void Automation::Actionable(Handle h)
{
    if(!Effective(h,false) || !Effective(h,true)) Fail("VGUI2 control is hidden or disabled");
    for(auto modal:{backend.AppModal(),backend.SurfaceModal()})
        if(modal && !Descendant(h,modal)) Fail("VGUI2 control is blocked by a modal panel");
}
Json Automation::Node(Handle h,int depth,const Geometry& g)
{
    auto p=Read(h); auto d=Object(); auto bounds=ScreenRect(g,p.bounds), clip=ScreenRect(g,p.clip);
    Put(d,d,"ref",Ref(h)); Put(d,d,"parent",Ref(p.parent)); Put(d,d,"name",p.name); Put(d,d,"class",p.className); Put(d,d,"depth",depth);
    Rectangle(d,"local_bounds",p.local); Rectangle(d,"vgui2_bounds",p.bounds); Rectangle(d,"vgui2_clip",p.clip);
    Rectangle(d,"bounds",bounds); Rectangle(d,"clip",clip);
    Put(d,d,"visible",Effective(h,false)); Put(d,d,"self_visible",p.visible); Put(d,d,"enabled",Effective(h,true));
    Put(d,d,"mouse_input",p.mouse); Put(d,d,"keyboard_input",p.keyboard);
    Put(d,d,"focused",backend.Focus()==h); Put(d,d,"hovered",backend.Hover()==h); Put(d,d,"popup",p.popup);
    Put(d,d,"semantic_support",false);
    Put(d,d,"clipped",p.clip.x>p.bounds.x || p.clip.y>p.bounds.y || int64_t(p.clip.x)+p.clip.w<int64_t(p.bounds.x)+p.bounds.w || int64_t(p.clip.y)+p.clip.h<int64_t(p.bounds.y)+p.bounds.h);
    Put(d,d,"offscreen",int64_t(bounds.x)+bounds.w<=0 || int64_t(bounds.y)+bounds.h<=0 || bounds.x>=g.imageWidth || bounds.y>=g.imageHeight);
    Value actions(rapidjson::kArrayType);
    if(p.mouse) actions.PushBack(Value("click"),d.GetAllocator());
    if(p.keyboard) actions.PushBack(Value("focus"),d.GetAllocator());
    Member(d,d,"actions",std::move(actions)); return d;
}
Json Automation::Tree(const Value& args,size_t budget)
{
    auto cursor=String(args,"cursor"); std::string snapshot; size_t offset=0;
    if(!cursor.empty()) {
        auto colon=cursor.find(':');
        if(colon==std::string::npos) Fail("invalid VGUI2 cursor");
        auto tail=cursor.substr(colon+1);
        if(tail.empty() || tail.find_first_not_of("0123456789")!=std::string::npos) Fail("invalid VGUI2 cursor offset");
        snapshot=cursor.substr(0,colon); auto number=std::stoull(tail);
        if(number>MaxNodes) Fail("VGUI2 cursor is past the snapshot"); offset=static_cast<size_t>(number);
    } else {
        Handle start=root; auto requested=String(args,"root"); if(!requested.empty()) start=Resolve(requested);
        bool hidden=Boolean(args,"include_hidden",false); int maxDepth=Integer(args,"max_depth",MaxNodes,0,MaxNodes);
        Snapshot capture; capture.geometry=GeometryNow(); size_t bytes=0;
        std::vector<std::pair<Handle,int>> stack{{start,0}}; std::set<Handle> seen;
        while(!stack.empty()) {
            auto [h,depth]=stack.back(); stack.pop_back();
            if(!seen.insert(h).second) Fail("cycle in VGUI2 tree");
            if(seen.size()>MaxNodes) Fail("VGUI2 tree exceeds node safety limit");
            if(!hidden && h!=start && !Effective(h,false)) continue;
            auto node=Dump(Node(h,depth,capture.geometry)); bytes+=node.size();
            if(bytes>SnapshotBytes) Fail("VGUI2 snapshot exceeds memory budget; select a subtree");
            capture.nodes.push_back(std::move(node));
            if(depth>=maxDepth) continue;
            auto children=backend.Children(h);
            if(children.size()+stack.size()>MaxNodes) Fail("VGUI2 tree exceeds node safety limit");
            for(auto it=children.rbegin();it!=children.rend();++it) stack.emplace_back(*it,depth+1);
        }
        snapshot=Token();
        while(snapshotOrder.size()>=MaxSnapshots) { snapshots.erase(snapshotOrder.front()); snapshotOrder.pop_front(); }
        snapshotOrder.push_back(snapshot); snapshots.emplace(snapshot,std::move(capture));
    }
    auto it=snapshots.find(snapshot); if(it==snapshots.end()) Fail("VGUI2 snapshot expired; request a new tree");
    auto& capture=it->second; if(offset>capture.nodes.size()) Fail("VGUI2 cursor is past the snapshot");
    auto d=Object(); Put(d,d,"snapshot",snapshot); Put(d,d,"total_nodes",static_cast<int>(capture.nodes.size()));
    Member(d,d,"nodes",Value(rapidjson::kArrayType)); Member(d,d,"next_cursor",Value());
    Value geometry(rapidjson::kObjectType); auto g=capture.geometry;
    Member(geometry,d,"original_size",Array(d,{g.imageWidth,g.imageHeight})); Member(geometry,d,"window_size",Array(d,{g.windowWidth,g.windowHeight}));
    Member(d,d,"geometry",std::move(geometry));
    while(offset<capture.nodes.size() && d["nodes"].Size()<PageNodes) {
        auto stored=Parse(capture.nodes[offset]);
        Value node; node.CopyFrom(stored,d.GetAllocator()); d["nodes"].PushBack(node,d.GetAllocator());
        if(offset+1<capture.nodes.size()) Put(d,d,"next_cursor",snapshot+":"+std::to_string(offset+1));
        else d["next_cursor"].SetNull();
        if(Dump(d).size()>budget) { d["nodes"].PopBack(); break; }
        ++offset;
    }
    if(offset<capture.nodes.size()) {
        if(d["nodes"].Empty()) Fail("VGUI2 node exceeds page budget; shorten control names or use UDP");
        Put(d,d,"next_cursor",snapshot+":"+std::to_string(offset));
    } else d["next_cursor"].SetNull();
    return d;
}
Json Automation::Start(const std::string& kind,const Value& args,double now)
{
    if(operation.pending || operation.held) Fail("another VGUI2 operation is pending");
    auto ref=String(args,"ref"); auto h=Resolve(ref); Actionable(h); auto p=Read(h);
    if(kind=="click") {
        if(!p.mouse) Fail("VGUI2 mouse input is disabled");
        if(backend.HeldButtons() || backend.PhysicalButtonsHeld()) Fail("mouse buttons are already held");
        auto button=String(args,"button","left"); if(button!="left" && button!="right") Fail("button must be left or right");
        Integer(args,"click_count",1,1,2);
        if(args.HasMember("x")!=args.HasMember("y")) Fail("x and y must be provided together");
        if(args.HasMember("x")) { Integer(args,"x",0,0,INT32_MAX); Integer(args,"y",0,0,INT32_MAX); }
    } else if(!p.keyboard) Fail("VGUI2 keyboard input is disabled");
    operation=Operation{}; operation.id=Token(); operation.ref=ref; operation.kind=kind;
    operation.args.CopyFrom(args,operation.args.GetAllocator()); operation.pending=true; operation.deadline=now+OperationTimeout;
    operation.mask=String(args,"button","left")=="left"?1:2;
    return Status("pending",operation.id);
}
void Automation::Finish(const std::string& error)
{
    if(!error.empty()) operation.error=error;
    if(!Release()) { if(operation.error.empty()) operation.error="failed to release VGUI2 mouse button"; return; }
    auto d=Status(operation.error.empty()?"success":"failed",operation.id);
    if(!operation.error.empty()) Put(d,d,"error",operation.error);
    else if(operation.kind=="focus") Put(d,d,"focused_ref",Ref(backend.Focus()));
    else Put(d,d,"ref",operation.ref);
    results[operation.id]=std::move(d); resultOrder.push_back(operation.id);
    while(resultOrder.size()>MaxResults) { results.erase(resultOrder.front()); resultOrder.pop_front(); }
    operation.pending=false;
}
Json Automation::Execute(const std::string& command,const Value& args,double now,size_t budget)
{
    if(!args.IsObject()) Fail("VGUI2 arguments must be an object");
    if(command=="set_text") Fail("VGUI2 set_text is unsupported (RequestInfo automation is unavailable)");
    Refresh(now);
    Json d;
    if(command=="tree") d=Tree(args,budget);
    else if(command=="inspect") { Integer(args,"text_offset",0,0,INT32_MAX); d=Node(Resolve(String(args,"ref")),0,GeometryNow()); }
    else if(command=="click" || command=="focus") d=Start(command,args,now);
    else if(command=="result") {
        auto id=String(args,"operation");
        if(!id.empty() && operation.pending && id==operation.id) d=Status("pending",id);
        else { auto found=results.find(id); if(found==results.end()) Fail("VGUI2 operation expired"); d.CopyFrom(found->second,d.GetAllocator()); }
    } else Fail("unknown VGUI2 operation");
    if(Dump(d).size()>budget) Fail("VGUI2 response exceeds page budget");
    return d;
}
void Automation::Frame(double now)
{
    // Refresh even when idle so a teardown invalidates frozen snapshots/refs.
    // Release stages deliberately never dereference a possibly destroyed target.
    if(!operation.pending) { try { Refresh(now); } catch(const std::exception&) {} return; }
    try {
        if(!operation.error.empty()) { Finish(operation.error); return; }
        if(now>operation.deadline) { Finish("VGUI2 operation timed out"); return; }
        if(operation.kind=="click" && operation.stage==2) {
            if(!Release()) { Finish("failed to release VGUI2 mouse button"); return; }
            operation.stage=3; return;
        }
        if(operation.kind=="click" && operation.stage==3) {
            if(++operation.clicks<Integer(operation.args,"click_count",1,1,2)) operation.stage=0;
            else Finish(); return;
        }
        Refresh(now); if(!operation.pending) return;
        auto h=Resolve(operation.ref); Actionable(h); auto p=Read(h);
        if(operation.kind=="focus") {
            if(!p.keyboard) Fail("VGUI2 keyboard input is disabled");
            if(operation.stage++==0) { backend.RequestFocus(h); return; }
            if(!Descendant(backend.Focus(),h)) Fail("VGUI2 focus request was not accepted");
            Finish(); return;
        }
        if(!p.mouse) Fail("VGUI2 mouse input is disabled");
        if(operation.stage==0) {
            auto g=GeometryNow(); auto clip=ScreenRect(g,p.clip), bounds=ScreenRect(g,p.bounds);
            int64_t x0=(std::max)(int64_t(0),int64_t(clip.x)), y0=(std::max)(int64_t(0),int64_t(clip.y));
            int64_t x1=(std::min)(int64_t(g.imageWidth),int64_t(clip.x)+clip.w), y1=(std::min)(int64_t(g.imageHeight),int64_t(clip.y)+clip.h);
            if(x1<=x0 || y1<=y0) Fail("VGUI2 control has no on-screen click area");
            int64_t x=x0+(x1-x0-1)/2, y=y0+(y1-y0-1)/2;
            if(operation.args.HasMember("x")) {
                x=int64_t(bounds.x)+Integer(operation.args,"x",0,0,g.imageWidth); y=int64_t(bounds.y)+Integer(operation.args,"y",0,0,g.imageHeight);
                if(x<x0 || y<y0 || x>=x1 || y>=y1) Fail("VGUI2 click offset is outside clipped bounds");
            }
            if(!Descendant(backend.Hit(static_cast<int>(x*g.windowWidth/g.imageWidth),static_cast<int>(y*g.windowHeight/g.imageHeight)),h)) Fail("VGUI2 click is obstructed");
            if(!backend.Move(static_cast<int>(x),static_cast<int>(y))) Fail(backend.InputError());
            operation.stage=1; return;
        }
        int x=0,y=0;
        if(!backend.Cursor(x,y) || !Descendant(backend.Hit(x,y),h) || !Descendant(backend.Hover(),h)) Fail("VGUI2 target moved or did not receive mouse focus");
        if(backend.HeldButtons() || backend.PhysicalButtonsHeld()) Fail("mouse buttons became held");
        // Remember ownership even if a backend reports failure after partial dispatch.
        bool accepted=backend.Mouse(operation.mask);
        operation.held=(backend.HeldButtons() & operation.mask)!=0;
        if(!accepted) Fail(backend.InputError());
        operation.held=true; operation.stage=2;
    } catch(const std::exception& e) { Finish(e.what()); }
}
}
