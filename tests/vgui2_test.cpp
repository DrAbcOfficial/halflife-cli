#include "vgui2/automation.h"
#include <iostream>
#include <stdexcept>
#include <map>

using namespace VGUI2;
void Check(bool condition) { if (!condition) throw std::runtime_error("check failed"); }
struct Fake : Backend
{
    Handle root = 1, focus = 0, hover = 2, hit = 2, modal = 0;
    int held = 0, releases = 0; bool releaseFails = false;
    std::map<Handle, Panel> panels;
    Fake() {
        Panel p; p.name = "root"; p.className = "Panel";
        p.bounds = p.clip = p.local = {0, 0, 800, 600};
        panels[1] = p; p.name = "button"; p.parent = 1; panels[2] = p;
    }
    Handle Root() override { return root; }
    bool Read(Handle h, Panel& p) override { if (!panels.count(h)) return false; p = panels.at(h); return true; }
    std::vector<Handle> Children(Handle h) override {
        std::vector<Handle> out; for (auto& [id, p] : panels) if (p.parent == h) out.push_back(id); return out;
    }
    Geometry GetGeometry() override { return {800,600,800,600}; }
    Handle Focus() override { return focus; }
    Handle Hover() override { return hover; }
    Handle AppModal() override { return modal; }
    Handle SurfaceModal() override { return 0; }
    Handle Hit(int, int) override { return hit; }
    void RequestFocus(Handle h) override { focus = h; }
    int HeldButtons() override { return held; }
    bool PhysicalButtonsHeld() override { return false; }
    bool Move(int, int) override { return true; }
    bool Cursor(int& x, int& y) override { x=400; y=300; return true; }
    bool Mouse(int mask) override { if (!mask) { ++releases; if (releaseFails) return false; } held=mask; return true; }
    const char* InputError() override { return "injection failed"; }
};
Json Args(const std::string& text) { return Parse(text); }
std::string Ref(Automation& a) {
    auto page = a.Execute("tree", Args("{}"), 0, 12000);
    return page["nodes"][1]["ref"].GetString();
}
Json Target(const std::string& ref) { Json d; d.SetObject(); Put(d,d,"ref",ref); return d; }
template<class F> void Reject(F fn) { bool rejected=false; try { fn(); } catch (const std::exception&) { rejected=true; } Check(rejected); }
int main() {
    try {
        Fake b; Automation a(b); auto ref=Ref(a);
        auto node=a.Execute("inspect",Target(ref),0,12000);
        Check(!node["semantic_support"].GetBool()); Check(!node.HasMember("text"));
        for (const char* phase : {"", "begin", "chunk", "commit", "cancel"}) {
            auto args=Target(ref); Put(args,args,"phase",phase);
            Reject([&]{ a.Execute("set_text",args,0,12000); });
        }
        b.panels[2].visible=false;
        Check(1==a.Execute("tree",Args("{}"),0,12000)["total_nodes"].GetInt());
        Reject([&]{a.Execute("click",Target(ref),0,12000);});
        b.panels[2].visible=true; b.panels[2].enabled=false;
        Reject([&]{a.Execute("focus",Target(ref),0,12000);});
        b.panels[2].enabled=true; b.modal=3;
        Reject([&]{a.Execute("click",Target(ref),0,12000);}); b.modal=0;
        auto op=a.Execute("click",Target(ref),0,12000); auto id=std::string(op["operation"].GetString());
        b.hit=1; a.Frame(0.1); auto result=Args("{}"); Put(result,result,"operation",id);
        Check(std::string("failed")==a.Execute("result",result,0.1,12000)["status"].GetString());
        b.hit=2; op=a.Execute("click",Target(ref),1,12000); id=op["operation"].GetString();
        a.Frame(1.1); a.Frame(1.2); Check(1==b.held);
        b.panels.erase(2); a.Frame(1.3); a.Frame(1.4); Check(0==b.held);
        Put(result,result,"operation",id);
        Check(std::string("success")==a.Execute("result",result,1.4,12000)["status"].GetString());
        Reject([&]{a.Execute("inspect",Target(ref),1.5,12000);});
        Fake c; Automation x(c); auto r=Ref(x);
        op=x.Execute("click",Target(r),0,12000); x.Frame(0.1); x.Frame(0.2);
        c.releaseFails=true; x.Frame(0.3); Check(1==c.held);
        Reject([&]{x.Execute("focus",Target(r),0.4,12000);});
        c.releaseFails=false; x.Frame(0.5); Check(0==c.held);
        op=x.Execute("focus",Target(r),1,12000); x.Frame(6);
        Put(result,result,"operation",op["operation"].GetString());
        Check(std::string("failed")==x.Execute("result",result,6,12000)["status"].GetString());
        for (int i=3;i<25;++i) { c.panels[i]=c.panels[2]; c.panels[i].name=std::string(30,'a'); }
        auto page=x.Execute("tree",Args("{}"),7,1600); Check(Dump(page).size()<=1600);
        Check(!page["next_cursor"].IsNull()); std::string cursor=page["next_cursor"].GetString();
        c.panels[24].name="changed after snapshot";
        int count=page["nodes"].Size();
        while (!page["next_cursor"].IsNull()) {
            auto args=Args("{}"); Put(args,args,"cursor",page["next_cursor"].GetString());
            page=x.Execute("tree",args,7,1600); Check(Dump(page).size()<=1600); count+=page["nodes"].Size();
            for(auto& node:page["nodes"].GetArray()) Check(std::string("changed after snapshot")!=node["name"].GetString());
        }
        Check(24==count);
        for(int i=0;i<4;++i) x.Execute("tree",Args("{}"),7,12000);
        auto old=Args("{}"); Put(old,old,"cursor",cursor); Reject([&]{x.Execute("tree",old,7,1600);});
        c.root=0; Reject([&]{x.Execute("tree",Args("{}"),8,12000);});
        c.root=1; Reject([&]{x.Execute("inspect",Target(r),8,12000);});
        auto rect=ScreenRect({400,300,800,600},{-10,5,20,30});
        Check(-20==rect.x && 10==rect.y && 40==rect.w && 60==rect.h);
        auto utf=Args("{}"); Put(utf,utf,"name",std::string("bad\xff",4));
        auto roundtrip=Parse(Dump(utf)); Check(std::string("bad\xef\xbf\xbd")==roundtrip["name"].GetString());
        Reject([&]{ Parse("{\"name\":\"\xff\"}"); });
        Fake f; Automation focus(f); auto fr=Ref(focus);
        op=focus.Execute("focus",Target(fr),0,12000); focus.Frame(0.1); focus.Frame(0.2);
        Put(result,result,"operation",op["operation"].GetString());
        Check(std::string("success")==focus.Execute("result",result,0.2,12000)["status"].GetString());
        auto two=Target(fr); Put(two,two,"click_count",2);
        op=focus.Execute("click",two,1,12000);
        for(int i=1;i<=8;++i) focus.Frame(1+i*0.1);
        Check(2==f.releases); Check(0==f.held);
        f.held=2; Reject([&]{focus.Execute("click",Target(fr),2,12000);}); f.held=0;
        auto offset=Target(fr); Put(offset,offset,"x",900); Put(offset,offset,"y",0);
        op=focus.Execute("click",offset,2,12000); focus.Frame(2.1);
        Put(result,result,"operation",op["operation"].GetString());
        Check(std::string("failed")==focus.Execute("result",result,2.1,12000)["status"].GetString());
        f.panels[2].name=std::string(1700,'x');
        Reject([&]{focus.Execute("inspect",Target(fr),3,1600);});
        auto large=focus.Execute("inspect",Target(fr),3,12000); Check(Dump(large).size()>1600);
        auto tcp=focus.Execute("tree",Args("{\"max_depth\":0}"),3,1600);
        Check(FrameReply(tcp,std::string(32,'0')).size()<4096);
        f.root=3; f.panels[3]=f.panels[1];
        Reject([&]{focus.Execute("inspect",Target(fr),4,12000);});
        std::cout << "VGUI2 behavior tests passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
