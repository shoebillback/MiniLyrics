// DirectWrite/Win32 regression coverage of the production host implementation.
// No player connection or hotkey. Focused input checks use an ephemeral
// loopback listener without a client; production-procedure windows stay offscreen.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <winsock2.h>
#include <windows.h>
namespace {
 // Only the hover fixture substitutes cursor input and records tracking requests.
 HWND hoverTestWindow=nullptr;
 POINT hoverTestCursor{};
 DWORD hoverTrackingFlags=0;
 BOOL readCursor(LPPOINT cursor){
  if(!hoverTestWindow)return GetCursorPos(cursor);
  *cursor=hoverTestCursor;return TRUE;
 }
 BOOL trackMouse(LPTRACKMOUSEEVENT tracking){
  if(tracking->hwndTrack!=hoverTestWindow)return TrackMouseEvent(tracking);
  hoverTrackingFlags=tracking->dwFlags;return TRUE;
 }
}
#undef NOMINMAX
#define GetCursorPos readCursor
#define TrackMouseEvent trackMouse
#include "../native/Host.cpp"
#undef TrackMouseEvent
#undef GetCursorPos
#include <iostream>
#include <stdexcept>
#include <cstring>

struct NativeTest {
 static int checks;
 static void require(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
 static void expectNear(double a,double b,const char* message){require(std::abs(a-b)<.03,message);}
 struct Fixture {
  App a;
  Fixture(){
   a.background.mode=0; // Lyric-only scenarios do not request background animation frames.
   check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(a.textFactory.put())));
   a.makeFormat(a.mainFormat,CurrentFontSize,DWRITE_FONT_WEIGHT_SEMI_BOLD);a.makeFormat(a.secondaryFormat,TranslationFontSize,DWRITE_FONT_WEIGHT_NORMAL);
   a.mainRowHeight=a.layout(L"Ag",a.mainFormat.get()).height;a.updatePlaceholders();
   a.width=600;a.height=240;a.viewportBottom=212;a.freezePlayback=true;a.testWall=true;a.testWallMs=1000;
   a.motion.setRunning(true,0);a.coverSince=-1000;
   a.hwnd=CreateWindowExW(0,L"STATIC",L"MiniLyrics isolated test",WS_POPUP,0,0,600,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
   require(a.hwnd!=nullptr,"create hidden test window");
   a.renderVisible=true; // Explicitly exercise drawing/scheduling in offscreen fixtures.
   a.transport=transportGeometry(a.width,a.height,a.textX,1);
   a.frameTimer=CreateWaitableTimerW(nullptr,FALSE,nullptr);require(a.frameTimer!=nullptr,"create frame timer");
  }
  ~Fixture(){a.modalFrames.stop();if(a.frameTimer){CancelWaitableTimer(a.frameTimer);CloseHandle(a.frameTimer);a.frameTimer=nullptr;}if(a.hwnd){DestroyWindow(a.hwnd);a.hwnd=nullptr;}}
  void lines(int mode=0){
   a.lines.clear();a.groups.clear();a.focusIndex=-999;
   for(int i=0;i<40;i++){Line l;l.time=i*2000;l.duration=2000;l.text=L"Lyric "+std::to_wstring(i);if(mode==1||(mode==2&&i%2==0))l.translation=L"Translation";a.lines.push_back(std::move(l));}
   a.index=10;a.position=21000;a.valid=true;
  }
  void layout(int focus,int browse=-1){a.browseIndex=browse;a.reflow(focus,false);a.updateIndicators();a.transitionActive=false;}
  void wheel(int delta){POINT p{int(a.textX+10),int((a.viewportTop+a.viewportBottom)*.5f)};ClientToScreen(a.hwnd,&p);a.message(WM_MOUSEWHEEL,MAKEWPARAM(0,SHORT(delta)),MAKELPARAM(p.x,p.y));}
  UINT interval(double position){
   a.position=position;a.playing=true;a.valid=true;
   CancelWaitableTimer(a.frameTimer);a.frameArmed=false;a.frameRunning=false;a.frameDeadline=0;a.schedule();return a.intervalMs;
  }
 };
 static void pausedBrowse(){
  int failures=0;auto run=[&](const char*name,auto scenario){try{scenario();std::cout<<"PASS: "<<name<<"\n";}catch(const std::exception&e){failures++;std::cout<<"FAIL: "<<name<<": "<<e.what()<<"\n";}};
  auto ready=[](Fixture&f,bool playing=false){auto&a=f.a;f.lines();a.lines[10].words={{L"Lyric 10",20000,4000}};a.playing=playing;a.snapRow=false;a.update();a.testWallMs+=1200;a.update();if(!playing){a.background.mode=2;a.background.animated=true;a.update();}};
  run("paused wheel moves and scales for 250ms, then stops",[&]{
   Fixture f;ready(f);auto&a=f.a;double position=a.current(),background=a.backgroundClock.time(a.animationWall());float before=a.group(9).y.value(a.motionNow()),zoom=a.group(9).zoom.value(a.motionNow());
   f.wheel(120);require(a.browseIndex==9,"wheel selects earlier lyric");expectNear(a.group(9).y.duration,250,"browse uses 250ms");expectNear(a.group(9).y.value(a.motionNow()),before,"first frame is continuous");expectNear(a.group(9).zoom.value(a.motionNow()),zoom,"first font size is continuous");require(a.frameRunning,"paused manual animation requests frames");
   a.testWallMs+=125;a.update();float middle=a.group(9).y.value(a.motionNow());require(std::abs(middle-before)>.03&&std::abs(middle-a.group(9).y.to)>.03,"real intermediate position");require(a.group(9).zoom.value(a.motionNow())>zoom&&a.group(9).zoom.value(a.motionNow())<1,"real intermediate font size");
   a.testWallMs+=125;a.update();expectNear(a.group(9).y.value(a.motionNow()),a.group(9).y.to,"browse finishes at target");expectNear(a.group(9).zoom.value(a.motionNow()),1,"focused size finishes");require(!a.frameRunning&&!a.frameArmed,"stop frames after transition");
   double stopped=a.motionNow();a.testWallMs+=1000;a.update();expectNear(a.motionNow(),stopped,"idle animation clock stays frozen");expectNear(a.current(),position,"playback position stays paused");expectNear(a.backgroundClock.time(a.animationWall()),background,"background stays paused");require(!a.seekPending&&!a.playing,"wheel does not seek or play");
  });
  run("continuous paused wheel retargets the current frame",[&]{
   Fixture f;ready(f);auto&a=f.a;f.wheel(120);a.testWallMs+=100;a.update();float before=a.group(8).y.value(a.motionNow());f.wheel(120);require(a.browseIndex==8,"latest browse target wins");expectNear(a.group(8).y.value(a.motionNow()),before,"retarget has no jump");
   a.testWallMs+=125;a.update();require(std::abs(a.group(8).y.value(a.motionNow())-before)>.03,"second transition progresses");a.testWallMs+=125;a.update();expectNear(a.group(8).y.value(a.motionNow()),a.group(8).y.to,"latest target finishes without queued animations");require(!a.frameRunning,"continuous wheel eventually stops");
  });
  run("paused four-second return uses a bounded transition",[&]{
   Fixture f;ready(f);auto&a=f.a;double input=a.animationWall(),position=a.current(),background=a.backgroundClock.time(input);f.wheel(120);a.testWallMs+=250;a.update();require(!a.frameRunning,"no frames during browse idle");float before=a.group(10).y.value(a.motionNow());
   a.testWallMs=input+4001;a.update();require(a.browseIndex<0&&a.focusIndex==10,"return selects actual playback row");require(a.frameRunning,"return requests animation frames");expectNear(a.group(10).y.duration,500,"return uses existing 500ms duration");expectNear(a.group(10).y.value(a.motionNow()),before,"return begins at displayed position");
   a.testWallMs+=250;a.update();float middle=a.group(10).y.value(a.motionNow());require(std::abs(middle-before)>.03&&std::abs(middle-a.group(10).y.to)>.03,"return has an intermediate frame");a.testWallMs+=250;a.update();require(!a.frameRunning&&!a.frameArmed,"return stops frames");expectNear(a.current(),position,"return does not seek");expectNear(a.backgroundClock.time(a.animationWall()),background,"return does not animate background");
  });
  run("paused distant return also animates without changing playback",[&]{
   Fixture f;ready(f);auto&a=f.a;double input=a.animationWall(),position=a.current();f.wheel(600);require(a.browseIndex==5,"distant browse target");a.testWallMs=input+4001;a.update();require(a.browseIndex<0&&a.frameRunning,"distant return animates");a.testWallMs+=250;a.update();require(a.frameRunning,"distant return has intermediate frames");a.testWallMs+=251;a.update();require(!a.frameRunning&&a.focusIndex==10,"distant return stops on playback row");expectNear(a.current(),position,"distant return does not seek");
  });
  run("pause and resume preserve an in-progress manual transition",[&]{
   for(bool initiallyPlaying:{true,false}){Fixture f;ready(f,initiallyPlaying);auto&a=f.a;f.wheel(120);a.testWallMs+=100;a.update();float before=a.group(9).y.value(a.motionNow());a.playing=!initiallyPlaying;a.update();expectNear(a.group(9).y.value(a.motionNow()),before,"play state change does not jump");a.testWallMs+=150;a.update();expectNear(a.group(9).y.value(a.motionNow()),a.group(9).y.to,"manual transition completes across play-state change");require(a.frameRunning==a.playing,"paused becomes idle; resumed keeps ordinary playback frames");}
  });
  run("hiding, changing track or invalidating the clock cancels manual frames",[&]{
   for(int mode=0;mode<3;mode++){Fixture f;ready(f);auto&a=f.a;f.wheel(120);require(a.frameRunning,"start manual animation");if(mode==0)a.setRenderVisible(false);else if(mode==1){Json track=Json::obj();track["type"]="track";track["id"]="new-track";a.receive(track);}else{a.valid=false;a.update();}require(!a.frameRunning&&!a.frameArmed,"canceled state does not keep refreshing");}
  });
  run("playing wheel retains its existing timing and long-step limit",[&]{
   Fixture f;ready(f,true);auto&a=f.a;f.wheel(120);expectNear(a.group(9).y.duration,250,"playing browse is still 250ms");a.testWallMs+=250;a.update();require(a.frameRunning,"ordinary playback keeps its frame loop");f.wheel(1200);require(a.browseIndex==0,"large playing browse target");expectNear(a.group(0).y.value(a.motionNow()),a.group(0).y.to,"large manual steps retain direct positioning");
  });
  require(failures==0,"paused browse focused scenarios failed");
 }
 static void viewportRows(){Fixture f;auto&a=f.a;f.lines();a.height=1000;a.viewportBottom=970;a.position=41000;a.update();require(a.groups.count(28)&&a.groupVisible(a.group(28),a.motionNow()),"tall viewport displays more than six following rows");require(a.groups.count(12)&&a.groupVisible(a.group(12),a.motionNow()),"tall viewport displays more than six preceding rows");require(a.groups.size()<=32,"viewport cache stays bounded");std::cout<<"PASS: viewport-driven rows\n";}
 static void browseBoundary(){
  Fixture f;auto&a=f.a;f.lines();a.playing=true;a.snapRow=false;a.position=21980;a.update();
  f.wheel(-360);int preview=a.browseIndex;require(preview>10,"wheel previews future lyrics");
  a.testWallMs+=100;a.position=22020;a.update();require(a.index==11&&a.browseIndex==preview,"ordinary next line preserves browse");
  // Real progress may correct local extrapolation backwards across a line boundary.
  Json clock=Json::obj();clock["type"]="clock";clock["position"]=21980;clock["playing"]=true;clock["valid"]=true;
  a.receive(clock);require(a.browseIndex==preview&&a.focusIndex==preview,"small boundary correction must not cancel manual preview");
  clock["position"]=22050;a.receive(clock);require(a.browseIndex==preview,"corrected next line preserves preview");
  a.testWallMs+=3901;a.update();require(a.browseIndex<0,"four second return still works");
  f.wheel(-360);require(a.browseIndex>=0,"browse again");a.seekTo(40000);a.update();require(a.browseIndex<0,"explicit seek still exits browse");
  a.testWallMs+=1000;a.update();f.wheel(-360);
  clock["position"]=60000;a.receive(clock);require(a.browseIndex<0,"large external seek still exits browse");
  a.testWallMs+=1000;a.update();f.wheel(-360);a.playing=false;a.update();
  require(a.browseIndex>=0,"pause preserves manual browse");a.testWallMs+=4001;a.update();require(a.browseIndex<0,"paused timeout still returns");
  f.wheel(-360);Json track=Json::obj();track["type"]="track";track["id"]="next-song";a.receive(track);require(a.browseIndex<0,"new track exits browse");
  std::cout<<"PASS: browse boundary, timeout and explicit seek\n";
 }
 static void lyricStates(){
  struct Sockets {Sockets(){WSADATA data{};require(WSAStartup(MAKEWORD(2,2),&data)==0,"initialize lyric-state bridge");}~Sockets(){WSACleanup();}} sockets;
  int failures=0;auto run=[&](const char*name,auto scenario){try{scenario();std::cout<<"PASS: "<<name<<"\n";}catch(const std::exception&e){failures++;std::cout<<"FAIL: "<<name<<": "<<e.what()<<"\n";}};
  run("startup rows are visible and clickable",[]{Fixture f;auto&a=f.a;f.lines(1);for(auto&l:a.lines)l.time+=5000;a.valid=false;a.playing=false;a.position=0;a.index=-1;a.updatePlaceholders();a.update();auto&g=a.group(0);require(a.groupVisible(g,a.motionNow()),"preview must use normal row visibility");a.bridge=std::make_unique<Bridge>(0,std::string(32,'t'),[](Json){});a.message(WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(int(a.textX+8),int(g.y.value(a.motionNow())+g.content->mainHeight/2)));require(a.seekPending&&a.position==0,"preview click only requests a seek");});
  run("stale progress retains a visible current row",[]{Fixture f;auto&a=f.a;f.lines();Json c=Json::obj();c["type"]="clock";c["position"]=21000;c["playing"]=true;c["valid"]=true;a.receive(c);c["valid"]=false;c["playing"]=false;a.receive(c);require(a.index==10&&a.focusIndex==10&&a.groupVisible(a.group(10),a.motionNow()),"lost clock must not return to first lyric");require(!a.frameRunning,"stale clock stops frames");});
  run("untimed content stays browsable without time seeking",[]{Fixture f;auto&a=f.a;Json m=Json::obj();m["type"]="lyrics";m["songId"]="";m["mode"]="untimed";m["lines"].kind=Json::Array;for(int i=0;i<8;i++){Json l=Json::obj();l["time"]=0;l["text"]="text "+std::to_string(i);m["lines"].array.push_back(l);}a.receive(m);Json c=Json::obj();c["type"]="clock";c["position"]=21000;c["playing"]=true;c["valid"]=true;a.receive(c);require(a.focusIndex==0,"untimed rows must not follow timestamps");f.wheel(-360);int focus=a.focusIndex;a.testWallMs+=5000;a.update();require(a.focusIndex==focus,"untimed reading position does not auto-return");a.bridge=std::make_unique<Bridge>(0,std::string(32,'t'),[](Json){});auto&g=a.group(focus);a.message(WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(int(a.textX+8),int(g.y.value(a.motionNow())+g.content->mainHeight/2)));require(!a.seekPending,"untimed lyric click cannot seek");});
  run("empty result is distinct from pending lyrics",[]{Fixture f;auto&a=f.a;require(a.state("test")["lyricStatus"].s()=="waiting","waiting before a result");Json m=Json::obj();m["type"]="lyrics";m["songId"]="";m["mode"]="empty";a.receive(m);require(a.state("test")["lyricStatus"].s()=="empty","empty result state");});
  run("mode changes reset reading focus and preserve native input order",[]{Fixture f;auto&a=f.a;f.lines();a.playing=true;a.update();Json m=Json::obj();m["type"]="lyrics";m["songId"]="";m["mode"]="untimed";m["lines"].kind=Json::Array;for(int i=0;i<3;i++){Json l=Json::obj();l["time"]=9000-i*1000;l["text"]="row "+std::to_string(i);m["lines"].array.push_back(l);}a.receive(m);require(a.focusIndex==0&&a.lines[0].text==L"row 0","timed to untimed starts at the input's first row");require(!a.frameRunning,"untimed lyrics do not schedule playback frames");f.wheel(-120);int focus=a.focusIndex;a.seekTo(40000);a.update();require(a.focusIndex==focus&&a.flight.rows.empty(),"transport seek preserves untimed reading");m["mode"]="timed";a.receive(m);require(a.lines[0].time==7000&&a.index==2,"timed mode resumes sorted clock selection");});
  require(failures==0,"lyric state scenarios failed");
 }
 static void typography(){
  for(int mode:{0,1,2}){
   Fixture f;auto&a=f.a;f.lines(mode);
   for(int focus:{9,10,11,10}){
    f.layout(focus,focus);expectNear(a.diagnostics("test")["groupGap"].n(),7,"gap must not depend on translation");
    for(int at=focus-3;at<=focus+3;at++){
     const auto&g=a.group(at);float scale=g.zoom.value(a.motionNow());
     expectNear(CurrentFontSize*scale,at==focus?21:16,"focus/context original font");
     float expected=g.content->mainHeight*scale;
     if(g.translated){FLOAT size=0;check(g.content->subtitle.value->GetFontSize(0,&size,nullptr));expectNear(size,16,"translation layout remains 16");expected+=(6.3f+g.content->subtitle.height)*scale;expectNear(g.subtitleOffset(scale)-g.content->mainHeight*scale,6.3*scale,"within-group gap follows group scale");}
     expectNear(g.renderedHeight(scale),expected,"rendered group height includes scaled translation");
     if(at<focus+3){const auto&next=a.group(at+1);expectNear(next.y.value(a.motionNow())-g.y.value(a.motionNow())-g.renderedHeight(scale),7,"adjacent groups must have exact 7 DIP gap");}
    }
    auto built=a.lineBuildCalls,id=a.group(focus).identity;
    for(float height:{192.f,130.f}){a.height=height;a.viewportBottom=height-28;a.reflow(focus,false);require(a.lineBuildCalls==built,"shrinking resize must reuse text layouts");require(a.group(focus).identity==id,"resize must retain group identity");}
    a.height=240.f;a.viewportBottom=212.f;a.reflow(focus,false);require(a.group(focus).identity==id,"restoring size must retain group identity");
   }
   a.height=240;f.layout(10);a.position=21000;a.playing=true;
   a.reflow(11,true);a.testWallMs+=350;float mid=a.group(11).zoom.value(a.motionNow());require(mid>16.f/21.f&&mid<1,"promotion scales original smoothly");
   if(a.group(11).translated)expectNear(a.group(11).renderedHeight(mid)-a.group(11).content->mainHeight*mid,(6.3+a.group(11).content->subtitle.height)*mid,"translation scales during promotion");
   double paused=a.motionNow();a.motion.setRunning(false,a.testWallMs);a.testWallMs+=1000;expectNear(a.motionNow(),paused,"pause freezes the size/position clock");
   require(a.groups.size()<=32,"bounded layout cache");
  }
  // A small translated group can end inside the viewport even when its main text is above it.
  Fixture f;f.lines(2);f.layout(11,11);auto&g=f.a.group(10);float scale=g.zoom.value(f.a.motionNow());
  g.y.snap(f.a.viewportTop-g.renderedHeight(scale)+1);require(f.a.groupVisible(g,f.a.motionNow()),"scaled subtitle contributes to visible bounds");
  g.y.snap(f.a.viewportTop-g.renderedHeight(scale)-1);require(!f.a.groupVisible(g,f.a.motionNow()),"whole group outside viewport is excluded");
 }
 static void motionGeometry(){
  for(int mode:{0,1,2})for(int curve:{0,1,2,3})for(bool browse:{false,true})for(int direction:{-1,1}){
   Fixture f;auto&a=f.a;a.height=360;a.viewportBottom=332;f.lines(mode);f.layout(10);a.lineCurve=curve;a.playing=true;a.browseIndex=browse?10+direction:-1;a.reflow(10+direction,true);
   for(int t=0;t<=800;t+=25){a.testWallMs=1000+t;double now=a.motionNow();
    for(int at=8;at<13;at++){const auto&g=a.group(at);const auto&next=a.group(at+1);float bottom=g.y.value(now)+g.renderedHeight(g.zoom.value(now));require(bottom<=next.y.value(now)+.03,"adjacent original/translation groups must not overlap in motion");}
   }
  }
 }
 static void scheduling(){
  Fixture f;auto&a=f.a;f.lines();f.layout(4,4);
  for(bool stagger:{false,true})for(int fps:{30,60}){
   a.stagger=stagger;a.fps=fps;
   for(double p:{21800.,21850.,21900.,21990.})expectNear(f.interval(p),22000-p,"manual browse must not repeatedly wake for past auto-lookahead");
  }
  a.stagger=true;a.fps=30;f.layout(10);expectNear(f.interval(21750),50,"auto-follow still wakes at the 200ms lookahead boundary");
  f.layout(11);expectNear(f.interval(21850),150,"auto-follow waits for actual next lyric once lookahead completes");
  a.lines[10].words.push_back({L"word",20000,2000});f.layout(4,4);
  require(!a.groupVisible(a.group(10),a.motionNow()),"playback word is offscreen in this fixture");expectNear(f.interval(21000),200,"offscreen word does not drive high-frequency frames");
  f.layout(10,10);expectNear(f.interval(21000),34,"visible playback word still animates during browse");a.fps=60;expectNear(f.interval(21000),17,"visible playback word retains 60 FPS");
  a.fps=30;a.lines[10].words.clear();a.lines[10].text=std::wstring(150,L'W');f.layout(4,4);
  require(!a.groupVisible(a.group(10),a.motionNow()),"long playback lyric is offscreen");expectNear(f.interval(21000),200,"offscreen long lyric does not drive frames");
  f.layout(10,10);expectNear(f.interval(21000),200,"visible long original remains fixed while browsing");
  f.layout(10);expectNear(f.interval(21000),34,"visible long original scrolls during auto-follow");
  a.lines[10].text=L"Short";a.lines[10].translation=std::wstring(150,L'W');f.layout(10);
  expectNear(f.interval(21000),34,"visible long translation still scrolls");f.layout(10,10);expectNear(f.interval(21000),200,"translation scrolling also stops while browsing");
  a.lines[10].translation.clear();a.lines[11].text=std::wstring(150,L'W');f.layout(10);expectNear(f.interval(21000),200,"long future preview stays fixed without high-frequency refresh");
  f.layout(4,4);a.transitionActive=true;expectNear(f.interval(21000),34,"browse motion still receives animation frames");a.transitionActive=false;
  a.coverSince=nowMs();expectNear(f.interval(21000),34,"cover animation remains independent of lyric visibility");a.coverSince=-1000;
  a.browseUntil=a.testWallMs+4000;a.testWallMs+=4001;a.update();require(a.browseIndex<0&&a.focusIndex==10,"four-second return remains active");require(a.index==10,"browsing and return do not seek playback");
  a.playing=false;a.testWallMs+=500;a.update();require(!a.frameRunning,"pause stops frame scheduling");
 }
 struct Surface {
  static constexpr int width=600,height=160;
  HDC dc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ old=nullptr;BYTE*pixels=nullptr;
  Com<ID2D1Factory> factory;Com<ID2D1DCRenderTarget> target;Graphics g;
  Surface(){
   check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.put()));
   auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96);
   check(factory->CreateDCRenderTarget(&props,target.put()));dc=CreateCompatibleDC(nullptr);require(dc!=nullptr,"create memory DC");
   BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
   bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);require(bitmap!=nullptr,"create memory bitmap");old=SelectObject(dc,bitmap);
   RECT rect{0,0,width,height};check(target->BindDC(dc,&rect));g.target=target.get();check(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),g.brush.put()));
  }
  ~Surface(){g.clear();target.reset();if(old)SelectObject(dc,old);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}
  void save(const std::filesystem::path&file){
   BITMAPFILEHEADER head{};head.bfType=0x4d42;head.bfOffBits=sizeof(head)+sizeof(BITMAPINFOHEADER);head.bfSize=head.bfOffBits+width*height*4;
   BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=width;info.biHeight=-height;info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
   std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<char*>(&head),sizeof(head));out.write(reinterpret_cast<char*>(&info),sizeof(info));out.write(reinterpret_cast<char*>(pixels),width*height*4);require(bool(out),"write pixel artifact");
  }
 };
 static void startupPreview(){
  Fixture f;auto&a=f.a;f.lines(1);for(auto&line:a.lines)line.time+=5000;a.lines[0].text=L"Ready before playback";a.position=0;a.index=-1;a.valid=false;a.playing=false;a.updatePlaceholders();a.update();Surface surface;
  surface.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  const auto frame=[&](bool expected){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
   if(expected){const auto&p=a.group(a.preludePreview);a.drawGroup(surface.g,p,a.preludePreview,0);}
   else a.drawLyrics(surface.g,0);
   check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
  auto actual=frame(false),expected=frame(true);int top=int(a.lyricAnchor()),bottom=int(a.lyricAnchor()+a.group(0).renderedHeight(1));
  require(std::equal(actual.begin()+top*Surface::width*4,actual.begin()+bottom*Surface::width*4,expected.begin()+top*Surface::width*4),"paused startup displays first lyric and translation before a clock sample");
  auto hasInk=[&](int y0,int y1){for(int y=y0;y<y1;++y)for(int x=0;x<Surface::width;++x){size_t i=(y*Surface::width+x)*4;if(actual[i]||actual[i+1]||actual[i+2])return true;}return false;};
  const auto&p=a.group(a.preludePreview);
  require(hasInk(top,int(a.lyricAnchor()+p.content->mainHeight))&&hasInk(int(a.lyricAnchor()+p.subtitleOffset(1)),bottom),"startup preview draws both main and subtitle bands");
  require(!a.valid&&!a.frameRunning&&a.position==0,"preview must not fabricate progress or schedule animation");
  Json clock=Json::obj();clock["type"]="clock";clock["position"]=15000;clock["playing"]=true;clock["valid"]=true;a.receive(clock);require(a.valid&&a.index==5,"first actual clock resumes normal lyric selection");
  std::cout<<"PASS: startup lyric preview without fabricated playback\n";
 }
 static void pixels(const std::filesystem::path&directory){
  Fixture f;auto&a=f.a;f.lines(1);f.layout(10,10);a.textX=20;auto&row=a.group(10);row.content->subtitle=a.layout(L"Translation scales with the lyric",a.secondaryFormat.get());Surface surface;
  std::vector<BYTE> baseline;int baselineWidth=0,baselineHeight=0;
  for(float scale:{1.f,16.f/21.f}){
   row.zoom.snap(scale);row.y.snap(100-row.subtitleOffset(scale));
   surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawGroup(surface.g,row,10,21000);check(surface.target->EndDraw());GdiFlush();
   std::vector<BYTE> crop(surface.pixels+100*Surface::width*4,surface.pixels+140*Surface::width*4);
   bool ink=false;for(size_t i=0;i<crop.size();i+=4)ink=ink||crop[i]||crop[i+1]||crop[i+2];require(ink,"pixel comparison must contain rendered translation");
   int minX=Surface::width,maxX=-1,minY=40,maxY=-1;
   for(int y=0;y<40;y++)for(int x=0;x<Surface::width;x++){size_t p=(y*Surface::width+x)*4;if(crop[p]||crop[p+1]||crop[p+2]){minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);}}
   if(baseline.empty()){baseline=crop;baselineWidth=maxX-minX+1;baselineHeight=maxY-minY+1;}
   else {require(crop!=baseline,"context translation must visibly shrink");require(maxX-minX+1<baselineWidth*.85,"translation pixel width follows group scale");require(maxY-minY+1<baselineHeight,"translation pixel height follows group scale");}
   if(!directory.empty())surface.save(directory/(scale==1?"original-21-translation-16.bmp":"original-16-translation-12.bmp"));
  }
 }
 static void interludes(const std::filesystem::path&directory){
  // Match RNP Interlude's floor(duration/3), clamp, opacity and scale formulas.
  for(int i=0;i<3;i++)for(int elapsed:{-100,0,1000,2000,3000,4000,5000,6000,7000}){
   auto dot=interludeDot(10000+elapsed,10000,6000,i,true);double p=unit((elapsed-i*2000)/2000.);
   expectNear(dot.alpha,.2+.7*p,"RNP per-dot progress controls opacity");expectNear(dot.scale,.9+.1*p,"RNP per-dot progress controls scale");
   auto inactive=interludeDot(10000+elapsed,10000,6000,i,false);expectNear(inactive.alpha,.2,"inactive dots reset to RNP base opacity");expectNear(inactive.scale,.9,"inactive dots reset to RNP base scale");
  }
  expectNear(interludeDot(3000,0,6002,1,true).alpha,.55,"RNP divides duration into integer millisecond thirds");
  require(std::isfinite(interludeDot(1,0,0,2,true).alpha),"zero-duration interlude cannot divide by zero");
  InterludeAnimation visual;visual.update(true,true,0);expectNear(visual.opacity.value(499),0,"RNP waits 500ms before fading in");require(visual.opacity.value(750)>0&&visual.opacity.value(750)<1,"RNP fades in over the following 500ms");expectNear(visual.opacity.value(1000),1,"RNP interlude is fully visible after one second");
  expectNear(visual.scale(0),1,"RNP breath begins at normal scale");expectNear(visual.scale(1000),1.1,"RNP breath reaches 1.1 at one second");expectNear(visual.scale(2000),1,"RNP breath repeats every two seconds");
  visual.update(false,true,2000,6000,0,6000);expectNear(visual.dot(6000,0,6000,0,2000).alpha,.9,"RNP exit starts from the previous dot opacity");auto exiting=visual.dot(6000,0,6000,0,2100);require(exiting.alpha>.2&&exiting.alpha<.9&&exiting.scale>.9&&exiting.scale<1,"RNP dots reset smoothly over 200ms");expectNear(visual.dot(6000,0,6000,0,2200).alpha,.2,"RNP dot reset completes after 200ms");
  Fixture f;auto&a=f.a;a.width=600;a.height=160;a.viewportBottom=132;a.textX=20;a.playing=true;a.valid=true;a.position=1000;a.fps=30;
  Line first;first.time=6000;first.duration=5000;first.text=L"First lyric";a.lines={first};a.index=a.rowAt(1000);a.updatePlaceholders();f.layout(0);
  require(a.lyricView()==App::LyricView::Prelude,"confirmed time before first lyric selects prelude");
  require(a.groupVisible(a.group(0),a.motionNow()),"prelude reuses its real preview group for drawing and hit testing");
  f.interval(1000);expectNear(a.framePeriod,1000./30,"prelude follows lyric animation frame rate");a.fps=60;f.interval(1000);expectNear(a.framePeriod,1000./60,"prelude supports the 60 FPS setting");a.fps=30;
  a.testWallMs+=1000;
  Surface surface;
  auto render=[&](){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawLyrics(surface.g,a.current()+a.offset);check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
  auto indicatorCrop=[&](const std::vector<BYTE>&frame){int top=int(std::floor(a.lyricAnchor())),bottom=int(std::ceil(a.lyricAnchor()+a.mainRowHeight));return std::vector<BYTE>(frame.begin()+top*Surface::width*4,frame.begin()+bottom*Surface::width*4);};
  auto prelude=indicatorCrop(render());bool ink=false;for(size_t i=0;i<prelude.size();i+=4)ink=ink||prelude[i]||prelude[i+1]||prelude[i+2];require(ink,"prelude pixel sample contains indicator");
  if(!directory.empty())surface.save(directory/"unified-prelude.bmp");
  Line gap;gap.time=0;gap.duration=6000;gap.isInterlude=true;a.lines.insert(a.lines.begin(),gap);a.groups.clear();a.focusIndex=-999;a.index=a.rowAt(1000);a.updatePlaceholders();f.layout(0);a.testWallMs+=1000;
  require(a.lines.size()==2&&a.rowAt(5999)==0&&a.rowAt(6000)==1,"empty timestamp is retained for exact lyric switching");
  require(a.lyricView()==App::LyricView::Rows&&a.showInterlude(a.group(0)),"leading empty timed row uses shared indicator");
  require(indicatorCrop(render())==prelude,"prelude and timed empty row render identical indicator pixels");
  if(!directory.empty())surface.save(directory/"unified-timed-gap.bmp");
  f.interval(1000);expectNear(a.framePeriod,1000./30,"timed gap has the same indicator frame rate");
  auto early=indicatorCrop(render());a.position=5000;a.snapRow=true;a.update();auto late=indicatorCrop(render());require(late!=early,"seek changes actual dot pixels without restarting the fade");expectNear(a.group(0).dots.opacity.value(a.indicatorNow()),1,"seek within current interlude preserves container visibility");a.position=1000;a.snapRow=true;a.update();require(indicatorCrop(render())==early,"backward seek immediately restores dot progress");
  a.motion.setRunning(false,a.testWallMs);a.playing=false;a.updateIndicators();auto paused=render();a.testWallMs+=500;a.schedule();require(!a.frameRunning,"pause stops indicator timer");require(render()==paused,"paused indicator remains pixel-identical");
  a.motion.setRunning(true,a.testWallMs);a.playing=true;a.updateIndicators();a.testWallMs+=500;require(indicatorCrop(render())!=indicatorCrop(paused),"resumed indicator continues breathing");
  f.layout(0,0);require(a.showInterlude(a.group(0)),"browsing preserves the indicator for the current timed gap");
  a.valid=false;require(a.lyricView()==App::LyricView::Rows,"manual browse remains available while clock is unconfirmed");
  f.layout(0);require(a.lyricView()==App::LyricView::Rows,"stale progress retains rows after an earlier valid clock");require(a.groupVisible(a.group(1),a.motionNow())&&!a.showInterlude(a.group(0)),"unconfirmed progress shows text without a timed indicator");
  a.valid=true;a.index=0;f.layout(0);a.testWallMs+=1000;a.index=1;f.layout(1);require(a.indicatorVisible(a.group(0)),"ending interlude fades out for 500ms like RNP");a.testWallMs+=500;require(!a.groupVisible(a.group(0),a.motionNow()),"past empty row disappears when its fade completes");f.interval(6500);expectNear(a.framePeriod,200,"past empty row does not drive high frequency redraw after fade");
  a.index=0;f.layout(0);a.group(0).y.snap(a.viewportBottom+1);f.interval(1000);expectNear(a.framePeriod,200,"offscreen active gap does not drive high frequency redraw");
  a.lines[0].words.push_back({L"Word-only lyric",0,5000});require(!a.emptyGroup(a.group(0))&&a.isInterlude(a.group(0)),"explicit RNP interlude marker survives word data");
  a.lines[0].words.clear();a.lines[0].translation=L"Translation-only lyric";require(a.group(0).translated&&a.isInterlude(a.group(0)),"RNP marker survives translated content");
  a.lines[0].duration=0;expectNear(a.interludeDuration(a.lines[0],0),6000,"missing interlude duration uses the next timestamp");a.lines[1].duration=0;a.duration=12000;expectNear(a.interludeDuration(a.lines[1],1),6000,"last row duration can use track end");Line orphan;orphan.time=2000;expectNear(a.interludeDuration(orphan,100),10000,"retained fading row remains safe after lyric list shrinks");
  a.lines.erase(a.lines.begin());a.groups.clear();a.index=a.rowAt(-1000);a.position=0;a.offset=-1000;a.updatePlaceholders();require(a.lyricView()==App::LyricView::Prelude,"negative lyric offset uses the same prelude state");
  a.lines.clear();a.valid=false;a.updatePlaceholders();require(a.lyricView()==App::LyricView::Waiting,"missing lyrics remain a separate waiting state");
  Fixture wire;Json message=Json::obj(),raw=Json::obj();message["type"]="lyrics";message["songId"]="";message["lines"].kind=Json::Array;raw["time"]=0;raw["duration"]=6000;raw["text"]="Marked";raw["isInterlude"]=true;message["lines"].array.push_back(raw);wire.a.receive(message);
  require(wire.a.lines[0].isInterlude&&wire.a.group(0).content->model.isInterlude,"native bridge preserves explicit RNP marker");message["lines"].array[0]["isInterlude"]=false;wire.a.receive(message);require(!wire.a.lines[0].isInterlude&&!wire.a.group(0).content->model.isInterlude,"marker-only change updates native cached row");
 }
 static void browseInterludes(const std::filesystem::path&directory){
  auto prepare=[](Fixture&f){auto&a=f.a;a.lines.clear();a.groups.clear();a.focusIndex=-999;
   for(int i=0;i<8;i++){Line line;line.time=i*1000;line.duration=1000;line.isInterlude=i!=1&&i!=4&&i!=6;if(!line.isInterlude)line.text=L"Lyric "+std::to_wstring(i);if(i==1)line.translation=L"Translation";if(i==3)line.isInterlude=false;a.lines.push_back(line);}
   a.position=1500;a.index=a.rowAt(a.position);a.valid=true;a.playing=false;a.updatePlaceholders();f.layout(1);
  };
  Fixture f;prepare(f);auto&a=f.a;f.wheel(-120);require(a.browseIndex==2,"one wheel notch reaches the middle interlude placeholder");require(a.index==1&&a.position==1500,"browsing does not seek or change the playback row");
  for(int i=1;i<6;i++){const auto&g=a.group(i);const auto&next=a.group(i+1);expectNear(next.y.value(a.motionNow())-g.y.value(a.motionNow())-g.renderedHeight(g.zoom.value(a.motionNow())),7,"middle placeholders retain row height and exact 7 DIP gaps");}
  require(a.group(2).middleInterlude&&a.groupVisible(a.group(2),a.motionNow()),"focused middle interlude retains a blank hit target");require(a.group(3).middleInterlude&&a.groupVisible(a.group(3),a.motionNow()),"adjacent middle interlude also retains its row");require(!a.groupVisible(a.group(0),a.motionNow()),"inactive leading interlude remains hidden during browse");
  f.wheel(-120);require(a.browseIndex==3,"consecutive middle interludes retain separate placeholder rows");f.wheel(-120);require(a.browseIndex==4,"next notch reaches the following lyric");f.wheel(-240);require(a.browseIndex==6,"multi-notch wheel includes the middle placeholder");f.wheel(-120);require(a.browseIndex==6,"trailing interlude cannot become browse focus");
  f.wheel(120);require(a.browseIndex==5,"reverse wheel also reaches the middle placeholder");f.wheel(1200);require(a.browseIndex==1,"leading interludes remain outside browse bounds");
  f.wheel(-60);require(a.browseIndex==1,"partial wheel delta does not consume a row");f.wheel(-60);require(a.browseIndex==2,"accumulated partial deltas reach a middle placeholder");
  a.lines[2].isInterlude=false;a.lines[2].text=L"New lyric";a.layoutDirty=true;a.update();require(a.browseIndex==2&&!a.group(2).middleInterlude,"lyric update replaces placeholder content without moving browse focus");
  a.lines[2].isInterlude=true;a.lines[2].text.clear();a.layoutDirty=true;a.update();require(a.browseIndex==2&&a.group(2).middleInterlude,"lyric update can restore the middle placeholder in place");
  a.testWallMs+=4001;a.update();require(a.browseIndex<0&&a.index==1&&a.focusIndex==1,"four-second return restores original playback row");
  for(int delta:{-120,120}){Fixture gap;prepare(gap);auto&g=gap.a;g.position=2500;g.index=g.rowAt(g.position);gap.layout(2);gap.wheel(delta);require(g.browseIndex==(delta<0?3:1),"browsing from current interlude moves one row including placeholders");g.testWallMs+=4001;g.update();require(g.browseIndex<0&&g.index==2&&g.focusIndex==2,"returning to playback retains the timed interlude");require(g.lines[2].time==2000&&g.rowAt(2500)==2,"browsing never removes an interlude timestamp");require(g.indicatorVisible(g.group(2)),"returning to playback keeps the current interlude visible");}
  for(int delta:{-120,120}){Fixture intro;auto&g=intro.a;Line first;first.time=5000;first.text=L"First";Line second;second.time=8000;second.text=L"Second";g.lines={first,second};g.index=-1;g.valid=true;intro.layout(0);intro.wheel(delta);require(g.browseIndex==0,"first wheel gesture before lyrics reaches first real lyric");}
  Fixture empty;auto&e=empty.a;Line blank;blank.isInterlude=true;blank.duration=1000;e.lines={blank,blank};e.lines[1].time=1000;e.valid=true;e.index=0;empty.layout(0);empty.wheel(-120);require(e.browseIndex<0,"all-interlude data does not enter an invisible browse mode");
  Fixture single;auto&s=single.a;Line only;only.time=1000;only.text=L"Only lyric";s.lines={blank,only,blank};s.lines[2].time=2000;s.valid=true;s.index=0;single.layout(0);single.wheel(-120);require(s.browseIndex==1,"single real lyric can be reached through leading blanks");single.wheel(120);single.wheel(-120);require(s.browseIndex==1,"single real lyric remains the boundary in both directions");
  Fixture visual;prepare(visual);auto&v=visual.a;visual.wheel(-120);v.height=160;v.viewportBottom=132;v.textX=20;visual.layout(2,2);v.playing=true;v.motion.setRunning(true,v.testWallMs);v.updateIndicators();Surface surface;
  auto render=[&](){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));v.drawGroup(surface.g,v.group(2),2,v.current()+v.offset);check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
  auto blankFrame=render();bool ink=false;for(size_t i=0;i<blankFrame.size();i+=4)ink=ink||blankFrame[i]||blankFrame[i+1]||blankFrame[i+2];require(!ink,"inactive middle interlude renders blank");if(!directory.empty())surface.save(directory/"blank-middle-interlude.bmp");
  v.testWallMs+=1000;v.position=2500;v.index=2;v.playing=false;v.roles();v.updateIndicators();require(render()!=blankFrame,"paused current interlude draws dots while browsing");
  v.valid=false;require(v.groupVisible(v.group(2),v.motionNow())&&render()==blankFrame,"unconfirmed progress preserves the blank row during browse");
  v.valid=true;v.stopBrowsing();v.update();require(v.showInterlude(v.group(2)),"returning to playback keeps the current interlude selected");
  Fixture longGap;auto&l=longGap.a;for(int i=0;i<1002;i++){Line line;line.time=i*1000;line.duration=1000;line.isInterlude=i>0&&i<1001;if(!line.isInterlude)line.text=L"Visible lyric";l.lines.push_back(line);}l.position=500;l.index=0;l.valid=true;l.playing=true;longGap.layout(0);longGap.wheel(-120);require(l.browseIndex==1,"long middle gap is browsed through placeholder rows");const auto&first=l.group(0);const auto&last=l.group(1);require(l.groups.size()<=32&&l.lineBuildCalls<32,"long interlude run keeps text cache and layout construction bounded");
  require(l.transitionActive&&last.y.active(l.motionNow()),"moving onto a placeholder retains normal browse animation");for(int t=0;t<=250;t+=25){l.testWallMs=1000+t;expectNear(last.y.value(l.motionNow())-first.y.value(l.motionNow())-first.renderedHeight(first.zoom.value(l.motionNow())),7,"placeholder rows retain exact spacing throughout browse animation");}
 }
 static void fontSizes(const std::filesystem::path&directory){
  Fixture f;auto&a=f.a;f.lines(2);a.playing=false;f.layout(10);
  a.settingsPath=(directory/L"font-settings.json").wstring();
  auto verify=[&](int size){a.setLyricFontSize(size);FLOAT mainSize=0,subSize=0;
   check(a.group(10).content->main.value->GetFontSize(0,&mainSize));check(a.group(10).content->subtitle.value->GetFontSize(0,&subSize));
   expectNear(mainSize,size,"actual lyric layout font size");expectNear(subSize,16.f*size/21,"translation layout scales proportionally");
   require(a.flight.rows.empty()&&a.group(10).placed,"font change rebuilds positioned text without stale flight");
  };
  verify(14);float smallWidth=a.group(10).content->main.width;verify(36);require(a.group(10).content->main.width>smallWidth,"larger font changes measured text width");verify(28);
  auto oldSettings=env(L"MINILYRICS_SETTINGS");SetEnvironmentVariableW(L"MINILYRICS_SETTINGS",a.settingsPath.c_str());
  try{App restored;expectNear(restored.currentFontSize,28,"font size persists in native settings");expectNear(restored.translationFontSize,16.f*28/21,"restored translation scale");}catch(...){SetEnvironmentVariableW(L"MINILYRICS_SETTINGS",oldSettings.empty()?nullptr:oldSettings.c_str());throw;}
  SetEnvironmentVariableW(L"MINILYRICS_SETTINGS",oldSettings.empty()?nullptr:oldSettings.c_str());
  a.setLyricFontSize(99);expectNear(a.currentFontSize,36,"native upper size bound");a.setLyricFontSize(1);expectNear(a.currentFontSize,14,"native lower size bound");verify(21);
  std::cout<<"PASS: font size, actual DirectWrite layouts, bounds and persistence\n";
 }
 static void seekFlights(const std::filesystem::path&directory){
  {
   Fixture external;auto&x=external.a;external.lines(2);x.songId="external-test";x.playing=true;external.layout(10);
   Json clock=Json::obj();clock["type"]="clock";clock["songId"]="stale-track";clock["position"]=41000;clock["playing"]=true;clock["valid"]=true;x.receive(clock);
   require(x.flight.rows.empty()&&x.position==21000,"stale track clock ignored");clock["songId"]=x.songId;x.receive(clock);
   require(!x.flight.rows.empty()&&x.index==20,"external seek uses the existing flight entry");
   double started=x.flight.started;x.receive(clock);
   require(x.flight.started==started,"external seek timeline echo does not restart flight");
   float from=x.flight.rows[20].point.y;x.testWallMs+=300;x.update();require(x.flight.rows[20].point.y<from,"external seek has intermediate geometry");
   float current=x.flight.rows[20].point.y;clock["position"]=11000;x.receive(clock);
   expectNear(x.flight.rows[20].point.y,current,"external rapid retarget starts at displayed frame");
   x.testWallMs+=1000;x.update();require(x.flight.rows.empty()&&x.index==5,"external backward seek completes");
   x.playing=false;x.update();clock["playing"]=false;clock["position"]=41000;x.receive(clock);require(x.frameRunning,"paused external seek schedules flight");
   x.testWallMs+=1000;x.update();require(x.flight.rows.empty()&&!x.playing&&!x.frameRunning,"paused external seek finishes without resuming");
   clock["position"]=41100;x.receive(clock);
   require(x.flight.rows.empty()&&x.index==20,"small clock correction does not start a seek flight");
   clock["position"]=61000;x.receive(clock);
   require(!x.flight.rows.empty()&&x.index==30&&!x.playing,"large actual clock jump animates while remaining paused");
   x.testWallMs+=1000;x.update();require(x.flight.rows.empty()&&!x.frameRunning,"paused timeline flight completes and stops scheduling");
  }
  Fixture f;auto&a=f.a;f.lines(2);a.height=160;a.viewportBottom=132;a.textX=20;a.playing=true;f.layout(10);Surface surface;surface.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  auto render=[&](){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));surface.target->PushAxisAlignedClip(D2D1::RectF(a.textX,a.viewportTop,a.width-12,a.viewportBottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);a.drawLyrics(surface.g,a.current()+a.offset);surface.target->PopAxisAlignedClip();check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
  auto before=render();auto identity=a.group(10).identity;a.seekTo(41000);a.update();require(a.index==20&&!a.flight.rows.empty(),"seek updates progress immediately and starts a flight");require(a.group(10).identity==identity,"flight retains source layouts at its first frame");require(render()==before,"flight first frame matches the previous lyric pixels");
  float start=a.flight.rows[20].point.y;a.testWallMs=1300;a.update();bool intermediate=false;for(const auto&item:a.groups)if(item.first>10&&item.first<20&&a.groupVisible(item.second,a.motionNow()))intermediate=true;require(intermediate,"intermediate lyrics cross the viewport during a long seek");require(a.flight.rows[20].point.y<start&&a.flight.rows[20].point.y>a.lyricAnchor(),"target advances continuously towards its anchor");auto middle=render();require(middle!=before,"flight changes actual rendered pixels");require(a.groups.size()<=32,"flight preserves bounded text cache");if(!directory.empty())surface.save(directory/"seek-flight-middle.bmp");
  a.testWallMs=1900;a.update();require(a.flight.rows.empty(),"flight completes after 500ms plus stagger");expectNear(a.group(20).y.value(a.motionNow()),a.lyricAnchor(),"target lands exactly at lyric anchor");require(a.position==41000&&render()!=middle,"visual flight does not delay or change playback position");a.seekTo(41850);a.update();require(a.flight.focus==21&&a.flight.playbackIndex==20,"lookahead focus can precede actual playback row");a.position=42010;a.update();require(a.flight.playbackIndex==21&&a.flight.rows[21].to.mainAlpha==1&&!a.flight.stagger,"new playback row updates highlight without repeating the stagger");std::cout<<"PASS: forward seek and intermediate lyric pixels\n";
  a.seekTo(61000);a.update();a.testWallMs=2200;a.update();auto retargetFrame=render();a.seekTo(11000);a.update();require(render()==retargetFrame,"rapid backward seek continues from the currently displayed frame");double started=a.flight.started;Json clock=Json::obj();clock["type"]="clock";clock["position"]=11000;clock["playing"]=true;clock["valid"]=true;clock["seekState"]="pending";a.receive(clock);require(!a.flight.rows.empty()&&a.flight.started==started,"pending seek echo cannot reset flight");clock["seekState"]="confirmed";a.receive(clock);require(a.flight.started==started,"seek confirmation cannot restart flight");a.testWallMs=3100;a.update();require(a.flight.rows.empty()&&a.index==5,"rapid backward seek lands on latest target");std::cout<<"PASS: rapid retarget and pending/confirmed clock echoes\n";
  Fixture paused;auto&p=paused.a;paused.lines(2);paused.layout(10);p.playing=false;p.seekTo(41000);p.update();double frozen=p.motionNow();require(p.frameRunning,"paused seek temporarily schedules animation frames");float pausedStart=p.flight.rows[20].point.y;p.testWallMs=1300;p.update();require(p.motionNow()==frozen&&p.flight.rows[20].point.y<pausedStart,"paused playback clock stays frozen while flight advances");p.testWallMs=1900;p.update();require(p.flight.rows.empty()&&!p.frameRunning&&!p.playing&&p.position==41000,"paused seek finishes and returns to sleep without starting playback");
  for(auto&line:p.lines)line.time+=5000;p.groups.clear();p.focusIndex=-999;p.position=46000;p.index=p.rowAt(p.position);paused.layout(p.index);p.updatePlaceholders();p.seekTo(0);p.update();p.testWallMs=2800;p.update();require(p.flight.rows.empty()&&p.lyricView()==App::LyricView::Prelude,"seek before first lyric lands in prelude state");expectNear(p.preludeDots.opacity.value(p.indicatorNow()),1,"paused prelude landing keeps frozen dots visible");std::cout<<"PASS: paused seek, idle scheduling and prelude landing\n";
 }
 static void backgrounds(const std::filesystem::path&directory){
  BackgroundClock clock;clock.update(true,1,0);expectNear(clock.time(100),100,"background clock advances");clock.update(true,2,100);expectNear(clock.position(150),200,"speed changes preserve phase continuity");clock.update(false,2,150);expectNear(clock.position(1000),200,"pause freezes background phase");expectNear(clock.time(1000),150,"pause freezes texture sampling clock");clock.update(true,.5,1000);expectNear(clock.position(1200),300,"background resumes at selected speed");
  BackgroundSettings s;s.mode=1;s.fps=15;FluidBackground cached;
  for(int i=0;i<60;i++)cached.prepare(s,i*1000./60,i*1000./60);
  require(cached.gpuReady(),cached.gpuError.c_str());require(cached.revision==15,"60 FPS lyric drawing must build only 15 background textures per second");auto last=cached.pixels;auto revision=cached.revision;
  for(int i=0;i<10;i++)cached.prepare(s,983.333333,983.333333);require(cached.revision==revision&&cached.pixels==last,"static time reuses background texture");
  cached.prepare(s,2000,20000);require(cached.pixels!=last,"fluid background changes over time");auto soft=cached.pixels;s.mode=3;cached.prepare(s,2000,20000);require(cached.pixels!=soft,"dark and soft styles differ");
  uint64_t softSum=0,darkSum=0;for(size_t p=0;p<soft.size();p+=4){softSum+=soft[p]+soft[p+1]+soft[p+2];darkSum+=cached.pixels[p]+cached.pixels[p+1]+cached.pixels[p+2];}require(darkSum<softSum,"dark fluid preserves stronger text contrast");
  std::vector<uint8_t>red(16,0);for(int p=0;p<16;p+=4){red[p+2]=255;red[p+3]=255;}auto palette=coverBackgroundPalette(red,2,2);require(palette[0].r>palette[0].b,"cover quadrants determine background palette");cached.setPalette(palette);cached.prepare(s,2000,20000);require(cached.pixels!=soft,"cover changes invalidate cached texture");
  std::vector<uint8_t>blue(16,0),green(16,0);for(int p=0;p<16;p+=4){blue[p]=255;blue[p+3]=255;green[p+1]=255;green[p+3]=255;}
  s.mode=1;cached.setCover(red,2,2,3000,true);cached.prepare(s,3000,20000);auto before=cached.pixels;
  cached.setCover(blue,2,2,3000);cached.prepare(s,3000,20000);require(cached.pixels==before,"new artwork starts from previous background, without default flash");
  cached.prepare(s,3250,20000);require(cached.pixels!=before,"artwork transition has intermediate pixels");auto middle=cached.pixels;
  cached.setCover(green,2,2,3250);cached.prepare(s,3250,20000);int largest=0;for(size_t i=0;i<middle.size();i++)largest=std::max(largest,std::abs(int(middle[i])-int(cached.pixels[i])));require(largest<=2,"rapid switch continues from current blend");
  cached.prepare(s,4000,20000);require(cached.pixels[1]>cached.pixels[0]&&cached.pixels[1]>cached.pixels[2],"transition reaches current artwork");
  cached.setCover({},0,0,4000);cached.prepare(s,4000,20000);require(cached.pixels[1]>cached.pixels[0],"missing artwork also starts from retained visual");
  Fixture f;auto&a=f.a;f.lines();f.layout(4,4);
  a.cover.width=a.cover.height=2;a.cover.pixels=red;a.fluid.setCover(red,2,2,0,true);a.fluid.prepare(s,0,0);auto retained=a.fluid.pixels;a.loadCover(L"https://example.invalid/pending");a.fluid.prepare(s,0,0);require(a.fluid.pixels==retained&&a.cover.pixels==red,"pending download retains artwork and background");
  auto stale=std::make_unique<CoverData>();stale->epoch=a.coverEpoch-1;stale->width=stale->height=2;stale->pixels=blue;a.onCover(std::move(stale));require(a.cover.pixels==red,"late cover result cannot replace current artwork");
  for(int mode:{1,2,3})for(int fps:{10,15,30,45,60}){Json options=Json::obj();options["mode"]=mode;options["fps"]=fps;options["animated"]=true;a.readBackground(options);f.interval(21000);expectNear(a.framePeriod,1000./fps,"background uses selected independent frame rate");}
  a.background.animated=false;expectNear(f.interval(21000),200,"static fluid does not force background frames");a.background.mode=0;a.background.animated=true;expectNear(f.interval(21000),200,"gradient preserves idle scheduling");
  f.layout(10);a.background.mode=1;a.playing=false;a.update();require(!a.frameRunning,"paused background stops scheduling");a.playing=true;a.valid=false;a.update();require(!a.frameRunning,"stalled playback freezes background");
  a.valid=true;a.lines.clear();a.groups.clear();a.index=-1;a.focusIndex=-1;a.background.fps=15;f.interval(0);expectNear(a.framePeriod,1000./15,"fluid still animates when a playing track has no lyrics");
  Json invalid=Json::obj();invalid["mode"]=-100;invalid["fps"]=1;invalid["speed"]=1000;a.readBackground(invalid);require(a.background.mode==0&&a.background.fps==60&&a.background.speed==3,"native background options are bounded");
  if(!directory.empty()){
   a.background={3,false,2,30};a.settingsPath=(directory/"background-settings.json").wstring();a.save();require(std::filesystem::exists(a.settingsPath),"background settings saved");
   auto previous=env(L"MINILYRICS_SETTINGS");SetEnvironmentVariableW(L"MINILYRICS_SETTINGS",a.settingsPath.c_str());App restored;SetEnvironmentVariableW(L"MINILYRICS_SETTINGS",previous.empty()?nullptr:previous.c_str());require(restored.background==a.background,"background options restore from settings file");
  }
  a.fluid.setPalette(defaultBackgroundPalette());Surface surface;surface.g.init(surface.target.get(),a.bgA,a.bgB,CoverData(),600,160);a.width=600;a.height=160;a.testWallMs=1000;a.backgroundClock=BackgroundClock();a.backgroundClock.update(true,1,1000);
  auto frame=[&](){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawBackground(surface.g,D2D1::RoundedRect(D2D1::RectF(.5f,.5f,599.5f,159.5f),17,17));check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
  std::vector<BYTE>previous;
  for(int mode:{0,1,2,3}){Json options=Json::obj();options["mode"]=mode;a.readBackground(options);auto image=frame();if(!previous.empty())require(image!=previous,"each mode changes actual background pixels");previous=image;if(!directory.empty())surface.save(directory/("background-mode-"+std::to_string(mode)+".bmp"));}
  a.background.mode=1;a.fluid.invalidate();auto initial=frame();a.testWallMs=3000;auto moving=frame();require(initial!=moving,"native dynamic rendering changes pixels");a.backgroundClock.update(false,1,3000);auto frozen=frame();revision=a.fluid.revision;a.testWallMs=8000;require(frame()==frozen&&a.fluid.revision==revision,"paused render is pixel-identical without texture rebuild");
 }
 static void optimizations(const std::filesystem::path&directory){
  Json metrics=Json::obj();
  Fixture f;auto&a=f.a;f.lines(1);f.layout(10);a.songId="optimization";
  auto small=a.state("host-state"),full=a.diagnostics("inspection");
  require(small.object.count("groups")==0&&small.object.count("coverLeftPx")==0,"normal replies omit layout diagnostics");
  require(small["songId"].s()==a.songId&&small["lyricCount"].n()==40&&small["positionMs"].n()==a.position,"compact replies retain playback and acknowledgement data");
  require(full["groups"].array.size()==a.groups.size()&&full["groups"].array.size()>0,"explicit inspection retains full layout diagnostics");
  require(a.state("background-state")["background"]["mode"].n()==0,"background acknowledgement retains configuration");
  metrics["normalReplyBytes"]=int(small.dump().size());metrics["diagnosticReplyBytes"]=int(full.dump().size());
  require(small.dump().size()*5<full.dump().size(),"normal reply is substantially smaller than inspection");
  size_t bytes=0;double start=nowMs();for(int i=0;i<100;i++)bytes+=a.state("host-state").dump().size();metrics["normalReplyMeanMs"]=(nowMs()-start)/100;
  start=nowMs();for(int i=0;i<100;i++)bytes+=a.diagnostics("inspection").dump().size();metrics["diagnosticReplyMeanMs"]=(nowMs()-start)/100;require(bytes>0,"serialization benchmark consumes its results");
  // Run the production window procedure off screen, without a player or socket.
  DestroyWindow(a.hwnd);a.hwnd=nullptr;a.renderVisible=false;
  check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,a.factory.put()));
  a.makeFormat(a.metaFormat,11.5f,DWRITE_FONT_WEIGHT_NORMAL);
  WNDCLASSW cls{};cls.lpfnWndProc=App::procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MiniLyrics.OptimizationTest";RegisterClassW(&cls);
  a.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Isolated optimization checks",WS_POPUP,-30000,-30000,600,240,nullptr,nullptr,cls.hInstance,&a);
  require(a.hwnd!=nullptr,"create offscreen production-procedure window");a.geometry();a.renderVisible=true;
  a.background.mode=1;a.playing=true;a.valid=true;a.cover.width=a.cover.height=2;
  a.cover.pixels={30,60,220,255,150,20,80,255,80,180,30,255,180,100,20,255};
  ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);
  auto calls=a.renderCalls;ValidateRect(a.hwnd,nullptr);
  a.update();a.update();a.update();
  require(a.renderCalls==calls&&GetUpdateRect(a.hwnd,nullptr,FALSE),"a burst invalidates once without synchronous paints");
  UpdateWindow(a.hwnd);require(a.renderCalls==calls+1&&a.error.empty(),"one WM_PAINT draws the final batched state");
  require(a.fluid.gpuReady(),a.fluid.gpuError.c_str());
  metrics["batchedUpdates"]=3;metrics["batchPaints"]=int(a.renderCalls-calls);
  auto brush=a.graphics.brush.get();auto cover=a.graphics.cover.get();auto bitmap=a.graphics.background.get();auto bitmapBrush=a.graphics.backgroundBrush.get();
  auto image=std::make_unique<CoverData>();image->epoch=a.coverEpoch;image->width=image->height=2;image->pixels=std::vector<BYTE>(16,255);a.onCover(std::move(image));
  require(a.graphics.brush.get()==brush&&a.graphics.cover.get()==cover&&a.graphics.background.get()==bitmap&&a.graphics.backgroundBrush.get()==bitmapBrush,"cover update reuses brush, same-size cover and background resources");
  SetWindowPos(a.hwnd,nullptr,-30000,-30000,620,240,SWP_NOZORDER|SWP_NOACTIVATE);a.renderVisible=true;
  require(a.graphics.brush.get()==brush&&a.graphics.cover.get()==cover&&a.graphics.background.get()==bitmap&&a.graphics.backgroundBrush.get()==bitmapBrush,"successful HWND resize retains device resources");
  expectNear(a.graphics.gradient->GetEndPoint().x,620,"resize updates gradient geometry");
  UpdateWindow(a.hwnd);require(a.error.empty(),"retained resources still draw after cover update and resize");
  ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);require(a.renderVisible,"show message enables rendering");UpdateWindow(a.hwnd);
  ShowWindow(a.hwnd,SW_HIDE);require(!a.renderVisible&&!a.frameRunning&&!a.frameArmed,"hide message stops rendering and timer");
  auto hiddenCalls=a.renderCalls,hiddenBuilds=a.lineBuildCalls,hiddenTextures=a.fluid.revision;double phase=a.backgroundClock.position(a.animationWall());
  Json clock=Json::obj();clock["type"]="clock";clock["songId"]=a.songId;clock["playing"]=true;clock["valid"]=true;clock["position"]=43000;
  a.testWallMs+=1000;a.receive(clock);a.paint();
  require(a.position==43000&&a.valid&&a.playing,"hidden window still accepts playback updates");
  require(a.renderCalls==hiddenCalls&&a.lineBuildCalls==hiddenBuilds&&a.fluid.revision==hiddenTextures,"hidden updates do not render, build lyrics or generate textures");
  metrics["hiddenPaints"]=int(a.renderCalls-hiddenCalls);
  expectNear(a.backgroundClock.position(a.animationWall()),phase,"hidden background animation stays frozen");
  ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);require(a.renderVisible&&a.index==21&&a.focusIndex==21,"show restores the current playback row without an old flight");UpdateWindow(a.hwnd);
  ShowWindow(a.hwnd,SW_SHOWMINNOACTIVE);require(!a.renderVisible&&!a.frameRunning,"minimize also stops render scheduling");expectNear(a.width,620,"minimize does not rebuild geometry to an icon-sized window");
  ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);require(a.renderVisible&&!IsIconic(a.hwnd)&&a.index==21,"restore from minimized resumes at the current row");
  FluidBackground fallback;fallback.failed=true;fallback.gpuError="isolated device failure";BackgroundSettings settings;
  fallback.prepare(settings,0,0);auto first=fallback.pixels;fallback.prepare(settings,1000,1000);
  require(!fallback.gpuReady()&&fallback.gpuError=="isolated device failure"&&fallback.pixels!=first,"known GPU failure uses the persistent CPU fallback");
  auto revision=fallback.revision;require(!fallback.prepare(settings,1000,1000)&&fallback.revision==revision,"fallback also reuses a cached frame");
  std::cout<<"PASS: compact status, coalesced paint, resource reuse, hide/show/minimize and persistent fallback\n";
  if(!directory.empty()){std::ofstream out(directory/"optimization-results.json");out<<metrics.dump();}
 }
 static void resizeRendering(const std::filesystem::path&directory){
  // Three critical scenarios: independent rates, batched programmatic resize,
  // and actual capture-based live resize with a frozen/resumed background.
  Fixture f;auto&a=f.a;f.lines();f.layout(10);a.background={};
  require(a.background.fps==60,"new background default is 60 FPS");
  FluidBackground cached;
  Json metrics=Json::obj();metrics["rates"].kind=Json::Array;
  for(int rate:{10,15,30,45,60}){
   Json options=Json::obj();options["fps"]=rate;a.readBackground(options);f.interval(21000);expectNear(a.framePeriod,1000./rate,"requested background rate reaches the scheduler");
   auto before=cached.revision;
   for(int i=0;i<60;i++)cached.prepare(a.background,i*1000./60,i*1000./60);
   require(cached.gpuReady(),cached.gpuError.c_str());require(cached.revision-before==unsigned(rate),"independent texture rate matches each configured limit");
   metrics["rates"].array.push_back(rate);
  }
  DestroyWindow(a.hwnd);a.hwnd=nullptr;a.renderVisible=false;
  check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,a.factory.put()));a.makeFormat(a.metaFormat,11.5f,DWRITE_FONT_WEIGHT_NORMAL);
  WNDCLASSW cls{};cls.lpfnWndProc=App::procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MiniLyrics.ResizeTest";RegisterClassW(&cls);
  a.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Isolated resize checks",WS_POPUP,-30000,-30000,460,130,nullptr,nullptr,cls.hInstance,&a);
  require(a.hwnd!=nullptr,"create production-procedure resize window");ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);a.renderVisible=true;a.coverSince=-1000;a.background.fps=30;
  a.update();UpdateWindow(a.hwnd);require(a.error.empty()&&a.fluid.gpuReady(),"initial production frame is ready");
  auto brush=a.graphics.brush.get();auto bitmap=a.graphics.background.get();auto focusId=a.group(a.focusIndex).identity;
  auto old=a.target->GetPixelSize();auto calls=a.renderCalls;
  SetWindowPos(a.hwnd,nullptr,0,0,540,192,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
  auto pending=a.target->GetPixelSize();require(pending.width==old.width&&pending.height==old.height&&a.renderCalls==calls,"programmatic resize retains the completed buffer until paint");
  UpdateWindow(a.hwnd);auto rendered=a.target->GetPixelSize();require(rendered.width==540&&rendered.height==192&&a.renderCalls==calls+1,"target resize and full draw complete in one paint");
  ValidateRect(a.hwnd,nullptr);auto changes=a.layoutChanges;SendMessageW(a.hwnd,WM_SIZE,SIZE_RESTORED,MAKELPARAM(540,192));
  require(!GetUpdateRect(a.hwnd,nullptr,FALSE)&&a.layoutChanges==changes&&!a.geometry(),"duplicate size messages do not rebuild or repaint");
  a.message(WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(535,187));require(a.resizing&&GetCapture()==a.hwnd&&!a.backgroundClock.running,"resize capture freezes the background clock");
  UpdateWindow(a.hwnd);auto revision=a.fluid.revision;auto frozen=a.fluid.pixels;double phase=a.backgroundClock.position(a.animationWall());calls=a.renderCalls;
  for(SIZE size:std::vector<SIZE>{{600,210},{900,240},{360,130},{540,192},{700,210}}){
   a.testWallMs+=100;SetWindowPos(a.hwnd,nullptr,0,0,size.cx,size.cy,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOCOPYBITS);
   auto actual=a.target->GetPixelSize();require(actual.width==unsigned(size.cx)&&actual.height==unsigned(size.cy)&&a.error.empty(),"live resize paints a full frame before returning");
  }
  require(a.renderCalls-calls==5&&a.fluid.revision==revision&&a.fluid.pixels==frozen,"live size changes reuse the background without repeated GPU generation");
  expectNear(a.backgroundClock.position(a.animationWall()),phase,"resizing never advances the frozen background");
  require(a.graphics.brush.get()==brush&&a.graphics.background.get()==bitmap&&a.group(a.focusIndex).identity==focusId,"resizing retains drawing resources and focused lyric identity");
  SetWindowPos(a.hwnd,nullptr,0,0,540,192,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOCOPYBITS);
  auto sameHeightBuilds=a.lineBuildCalls;
  SetWindowPos(a.hwnd,nullptr,0,0,700,192,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOCOPYBITS);
  require(a.lineBuildCalls==sameHeightBuilds,"same-height width resize reuses all cached lyric layouts");
  SetWindowPos(a.hwnd,nullptr,0,0,700,210,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOCOPYBITS);
  FluidBackground retained;retained.setSize(a.width,a.height);retained.prepare(a.background,a.backgroundClock.time(a.animationWall()),phase);
  auto cover=std::make_unique<CoverData>();cover->epoch=a.coverEpoch;cover->width=cover->height=2;cover->pixels={20,50,220,255,20,50,220,255,20,50,220,255,20,50,220,255};a.onCover(std::move(cover));UpdateWindow(a.hwnd);
  require(a.fluid.revision==revision&&a.fluid.pixels==frozen,"artwork arriving during resize waits without replacing the displayed background");
  a.message(WM_LBUTTONUP,0,0);require(!a.resizing&&GetCapture()!=a.hwnd&&a.backgroundClock.running,"release capture resumes normal background scheduling");
  UpdateWindow(a.hwnd);require(a.fluid.revision==revision+1&&a.fluid.pixels!=frozen,"final dimensions generate one updated background after release");
  require(a.fluid.pixels==retained.pixels,"queued artwork starts its crossfade from retained artwork after resize");
  double resumed=a.backgroundClock.position(a.animationWall());a.testWallMs+=100;a.update();UpdateWindow(a.hwnd);require(a.backgroundClock.position(a.animationWall())>resumed&&a.fluid.revision>revision+1,"background motion continues after resizing");
  // Capture loss uses the same completion path, including a paused player.
  a.playing=false;a.update();UpdateWindow(a.hwnd);a.message(WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(695,205));
  SetWindowPos(a.hwnd,nullptr,0,0,540,192,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOCOPYBITS);revision=a.fluid.revision;ReleaseCapture();UpdateWindow(a.hwnd);
  require(!a.resizing&&!a.backgroundClock.running&&a.fluid.revision==revision+1,"capture loss refreshes final dimensions while keeping a paused background frozen");
  metrics["liveSizeChanges"]=5;metrics["backgroundBuildsDuringResize"]=0;metrics["finalBackgroundBuilds"]=1;metrics["resourceReuse"]=true;metrics["captureLoss"]=true;
  if(!directory.empty()){std::ofstream out(directory/"resize-refresh-results.json");out<<metrics.dump();}
  std::cout<<"PASS: background rates, complete resize frames, frozen texture reuse and capture completion\n";
 }
 static void wordScrolling(const std::filesystem::path&directory){
  Fixture f;auto&a=f.a;a.width=360;a.height=130;a.textX=20;a.viewportBottom=102;a.coverSince=-1000;a.fps=30;
  Line line;line.time=0;line.duration=30000;line.translation=std::wstring(100,L'T');
  for(int i=0;i<48;i++){line.text+=L"词";line.words.push_back({L"词",i<24?i*60.:8000+(i-24)*200.,i<24?60.:200.});}
  a.lines={line};a.index=0;a.position=1000;a.playing=true;a.valid=true;f.layout(0);a.group(0).y.snap(40);
  auto&g=a.group(0);float available=a.width-a.textX-12,total=a.rowWidth(g),overflow=total-available;
  require(overflow>available,"fixture must exceed the viewport by more than its full width");
  double maximumDeviation=0;
  for(float zoom:{1.f,16.f/21.f})for(double time:{1350.,10500.}){
   g.zoom.snap(zoom);
   auto found=std::find_if(g.content->glyphs.begin(),g.content->glyphs.end(),[&](const Glyph&w){return time>=w.time&&time<w.time+w.duration;});require(found!=g.content->glyphs.end(),"fixture has a currently sung word");
   float cursor=found->x+found->layout.width*float((time-found->time)/found->duration);
   float x=cursor*zoom-(total*zoom-available)*a.horizontalPhase(g,time,0);
   require(x>=0&&x<=available,"currently sung position must remain inside the viewport despite uneven pacing");
   require(std::abs(x-available*.5f)<.05f,"middle-of-line word position follows the center of the viewport");maximumDeviation=std::max(maximumDeviation,double(std::abs(x-available*.5f)));
  }
  g.zoom.snap(1);
  float gap=a.horizontalPhase(g,2000,0);require(std::abs(gap-a.horizontalPhase(g,7000,0))<.0001f,"word gaps hold horizontal position");
  float before=overflow*a.horizontalPhase(g,1379.9,0),after=overflow*a.horizontalPhase(g,1380.1,0);require(std::abs(after-before)<1,"contiguous word boundaries keep horizontal motion continuous");
  require(a.horizontalPhase(g,-1,0)==0&&a.horizontalPhase(g,20000,0)==1,"word-following clamps to the start and end of the line");
  a.translation=false;g.translated=false;expectNear(f.interval(4000),200,"word gap without translated marquee returns to idle scheduling");expectNear(f.interval(1350),34,"active words retain 30 FPS lyric animation frames");a.fps=60;expectNear(f.interval(1350),17,"active words retain 60 FPS lyric animation frames");a.fps=30;
  a.translation=true;g.translated=true;
  Surface surface;auto frame=[&](double time){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));surface.target->PushAxisAlignedClip(D2D1::RectF(a.textX,0,a.width-12,130),D2D1_ANTIALIAS_MODE_ALIASED);a.drawGroup(surface.g,g,0,time);surface.target->PopAxisAlignedClip();check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
  auto following=frame(1350);if(!directory.empty())surface.save(directory/"word-following.bmp");
  int subtitleTop=int(std::ceil(40+g.subtitleOffset(1.f))),subtitleBottom=std::min(130,int(std::ceil(40+g.renderedHeight(1.f))));
  for(auto&w:g.content->glyphs){w.time+=20000;}
  auto delayed=frame(1350);if(!directory.empty())surface.save(directory/"word-timing-delayed.bmp");
  require(following!=delayed,"changing word timing changes actual native original pixels");
  size_t begin=subtitleTop*Surface::width*4,end=subtitleBottom*Surface::width*4;
  require(std::equal(following.begin()+begin,following.begin()+end,delayed.begin()+begin),"translation pixels retain the existing line-time marquee despite word timing changes");
  for(auto&w:g.content->glyphs){w.time-=20000;}
  a.browseIndex=0;require(a.horizontalPhase(g,1350,0)==0,"manual browsing keeps the original aligned to its start");a.browseIndex=-1;
  a.playing=false;a.position=1350;auto paused=frame(a.current());a.testWallMs+=1000;require(frame(a.current())==paused,"paused playback holds word following without a separate scrolling clock");
  float atEnd=a.horizontalPhase(g,20000,0),atStart=a.horizontalPhase(g,0,0);require(atEnd==1&&atStart==0,"backward seeking immediately reanchors word following");
  {
   Fixture note;auto&host=note.a;host.width=360;host.height=130;host.viewportBottom=102;host.textX=30;
   Line sustained;sustained.text=L"Long note";sustained.duration=2000;sustained.words={{sustained.text,0,2000,true}};
   host.lines={sustained};host.valid=true;host.index=0;host.position=1200;note.layout(0);
   auto&row=host.group(0);row.y.snap(40);
   auto glowFrame=[&](){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));host.drawGroup(surface.g,row,0,host.current());check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
   const auto lit=glowFrame();host.testWallMs+=1000;require(glowFrame()==lit,"paused long note freezes the actual glow pixels");
   row.content->glyphs[0].longNote=false;require(glowFrame()!=lit,"long note draws a visible native glow");
   host.position=2500;const auto afterGlow=glowFrame();row.content->glyphs[0].longNote=true;require(glowFrame()==afterGlow,"finished glow leaves the ordinary lyric pixels unchanged");
   Line second;second.text=L"B";second.time=2050;Line third;third.text=L"C";third.time=2100;
   host.lines={sustained,second,third};host.index=2;host.position=2200;host.snapRow=true;note.layout(2);
   auto&past=host.group(0);past.y.snap(40);past.zoom.snap(1);
   const auto renderGlow=[&](){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));host.drawLyricGlowsAt(surface.g,host.current(),host.motionNow());check(surface.target->EndDraw());GdiFlush();};
   const auto hasGlowPixels=[&](int left,int right){
    for(int y=32;y<int(host.viewportBottom);++y)for(int x=left;x<right;++x){const auto pixel=surface.pixels+(y*Surface::width+x)*4;if(pixel[0]||pixel[1]||pixel[2])return true;}
    return false;
   };
   host.artBounds=D2D1::RectF(0,40,25,75);renderGlow();
   require(hasGlowPixels(27,30)&&!hasGlowPixels(0,27),"the fading first note survives two line changes and has room beside the cover");
   host.textX=host.width-12-host.rowWidth(past);renderGlow();
   require(hasGlowPixels(int(host.width)-12,int(host.width)-7)&&!hasGlowPixels(int(host.width)-7,int(host.width)),"right-edge glow uses the gutter and preserves the window border");
   expectNear(note.interval(2200),17,"the older visible glow still requests animation frames");
   expectNear(note.interval(2500),200,"a completed glow releases the animation schedule");
   renderGlow();require(!hasGlowPixels(0,int(host.width)),"older glow clears at the end of its 500ms fade");
  }
  Line plain;plain.time=0;plain.duration=4000;plain.text=std::wstring(100,L'W');a.lines={plain};a.groups.clear();a.focusIndex=-999;a.playing=true;f.layout(0);auto&untimed=a.group(0);
  require(a.horizontalPhase(untimed,899,0)==0&&std::abs(a.horizontalPhase(untimed,2100,0)-.5f)<.0001f&&a.horizontalPhase(untimed,3300,0)==1,"untimed long lyrics retain the existing delay and line-duration marquee");
  if(!directory.empty()){std::ofstream report(directory/"word-scroll-results.json");report<<"{\"pass\":true,\"scenarios\":3,\"maximumCursorCenterDeviationDip\":"<<maximumDeviation<<",\"nativePixels\":true,\"translationUnchanged\":true,\"wordGapsHold\":true,\"browsePauseSeek\":true,\"untimedFallback\":true}";}
  std::cout<<"PASS: timed word visibility, native pixels, translation independence, browse/pause/seek and untimed fallback\n";
 }
 static void cleanup(const std::filesystem::path&directory){
  Fixture f;auto&a=f.a;f.lines(1);f.layout(10);a.playing=false;
  auto id=a.group(10).identity,builds=a.lineBuildCalls;
  Json clock=Json::obj();clock["type"]="clock";clock["position"]=22500;clock["playing"]=false;clock["valid"]=true;a.receive(clock);
  Json metrics=Json::obj();metrics["correctionAddedLayouts"]=double(a.lineBuildCalls-builds);bool cached=a.group(10).identity==id&&a.lineBuildCalls-builds>=1&&a.lineBuildCalls-builds<=2;
  builds=a.lineBuildCalls;a.setRenderVisible(false);a.setRenderVisible(true);metrics["showAddedLayouts"]=double(a.lineBuildCalls-builds);bool shown=a.lineBuildCalls==builds;
  Json lyrics=Json::obj();lyrics["type"]="lyrics";lyrics["songId"]="";lyrics["lines"].kind=Json::Array;
  for(size_t i=0;i<a.lines.size();i++){Json line=Json::obj();line["time"]=a.lines[i].time;line["duration"]=a.lines[i].duration;line["text"]=narrow(a.lines[i].text);line["translation"]=i==12?"Updated translation":narrow(a.lines[i].translation);lyrics["lines"].array.push_back(line);}
  builds=a.lineBuildCalls;id=a.group(10).identity;a.receive(lyrics);bool changed=a.group(12).content->model.translation==L"Updated translation"&&a.group(10).identity==id&&a.lineBuildCalls==builds+1;
  metrics["changedLineAddedLayouts"]=double(a.lineBuildCalls-builds);
  {
   Fixture cache;auto&host=cache.a;host.makeFormat(host.metaFormat,11.5f,DWRITE_FONT_WEIGHT_NORMAL);
   host.receive(lyrics);
   require(host.lyricLayouts.front()&&!host.lyricLayouts.back(),"first layout displays lyrics before warming distant lines");
   host.message(WM_TIMER,host.LayoutWarmupTimer,0);
   require(host.nextLayoutToWarm>0&&host.nextLayoutToWarm<host.lines.size(),"one warmup tick processes only a batch");
   while(host.nextLayoutToWarm<host.lines.size())host.message(WM_TIMER,host.LayoutWarmupTimer,0);
   require(std::all_of(host.lyricLayouts.begin(),host.lyricLayouts.end(),[](const auto&item){return bool(item);}),"warmup caches all song lines");
   std::weak_ptr<LyricLayout> distant=host.lyricLayouts.back(),updated=host.lyricLayouts[25];
   const auto warmBuilds=host.lineBuildCalls;
   host.group(39);host.reflow(0,false);
   require(!host.groups.count(39)&&host.group(39).content==distant.lock()&&host.lineBuildCalls==warmBuilds,"offscreen eviction preserves cached layout for reuse");
   auto changedLyrics=lyrics;changedLyrics["lines"].array[25]["translation"]="Changed offscreen translation";
   host.receive(changedLyrics);
   require(updated.expired()&&host.lyricLayouts.back()==distant.lock(),"lyric revision releases only the changed cached line");
   Json track=Json::obj();track["type"]="track";track["id"]="new-song";host.receive(track);
   host.message(WM_TIMER,host.LayoutWarmupTimer,0);host.receive(lyrics);
   require(distant.expired()&&host.lyricLayouts.empty()&&host.nextLayoutToWarm==0,"new track drops old layouts and ignores late lyrics and timer work");
  }
  SetWindowPos(a.hwnd,nullptr,-30000,-30000,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);
  ValidateRect(a.hwnd,nullptr);Json style=Json::obj();style["type"]="style";style["curve"]=a.lineCurve;style["stagger"]=a.stagger;a.receive(style);bool unchanged=!GetUpdateRect(a.hwnd,nullptr,FALSE);
  if(!directory.empty()){
   a.settingsPath=(directory/"no-op-settings.json").wstring();{std::ofstream settings(directory/"no-op-settings.json");settings<<"unchanged";}
   Json background=a.backgroundState();background["type"]="background";a.receive(background);std::ifstream saved(directory/"no-op-settings.json");std::string content((std::istreambuf_iterator<char>(saved)),{});unchanged=unchanged&&content=="unchanged"&&!GetUpdateRect(a.hwnd,nullptr,FALSE);
   background["mode"]=3;a.receive(background);require(a.background.mode==3&&GetUpdateRect(a.hwnd,nullptr,FALSE),"changed background still applies and invalidates");a.background.mode=0;
  }
  metrics["unchangedSettingsSkipped"]=unchanged;
  bool visiblePixels=true;
  for(bool words:{false,true}){
   Fixture p;auto&v=p.a;p.lines();v.height=160;v.viewportBottom=132;v.textX=20;
   for(auto&line:v.lines){line.time+=5000;if(words)line.words={{line.text,line.time,line.duration}};}
   v.position=26000;v.index=v.rowAt(v.position);v.playing=false;v.updatePlaceholders();p.layout(v.index);v.seekTo(0);v.update();
   v.testWallMs+=900;v.applyFlight(v.animationWall());v.updateIndicators();Surface actual,expected;
   actual.target->BeginDraw();actual.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));v.drawLyrics(actual.g,v.current());check(actual.target->EndDraw());
   expected.target->BeginDraw();expected.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));v.drawInterludeAt(expected.g,v.preludeDots,v.textX,v.lyricAnchor(),v.current(),0,v.lines.front().time,v.indicatorNow());for(const auto& item:v.groups)if(v.groupVisible(item.second,v.motionNow()))v.drawGroup(expected.g,item.second,item.first,v.current());check(expected.target->EndDraw());GdiFlush();
   bool same=std::equal(actual.pixels,actual.pixels+Surface::width*Surface::height*4,expected.pixels);visiblePixels=visiblePixels&&same;metrics[words?"wordPreludeOnlyPreview":"plainPreludeOnlyPreview"]=same;
   if(!directory.empty())actual.save(directory/(words?"word-prelude-landing.bmp":"plain-prelude-landing.bmp"));
  }
  metrics["pass"]=cached&&shown&&changed&&visiblePixels&&unchanged;metrics["scenarios"]=3;
  if(!directory.empty()){std::ofstream report(directory/"cleanup-results.json");report<<metrics.dump();}
  require(cached,"clock correction retains cached glyphs and builds only newly visible rows");require(shown,"showing unchanged lyrics reuses cached layouts");require(changed,"one changed translation rebuilds only its own group");require(unchanged,"unchanged settings do not repaint or overwrite settings");require(visiblePixels,"plain and word lyrics retain visible rows when landing in prelude");
  std::cout<<"PASS: cached correction/show, changed-line refresh and shared plain/word visibility\n";
 }
 static void preludeRows(const std::filesystem::path&directory){
  struct Sockets {Sockets(){WSADATA data{};require(WSAStartup(MAKEWORD(2,2),&data)==0,"initialize isolated seek bridge");}~Sockets(){WSACleanup();}} sockets;
  Fixture f;auto&a=f.a;a.makeFormat(a.metaFormat,11.5f,DWRITE_FONT_WEIGHT_NORMAL);a.height=300;a.viewportBottom=272;a.textX=20;
  auto track=[&](const char*id){Json m=Json::obj();m["type"]="track";m["id"]=id;m["title"]=id;m["duration"]=30000;a.receive(m);};
  auto lyrics=[&](const char*id,double first,bool gap=false,bool words=false){Json m=Json::obj();m["type"]="lyrics";m["songId"]=id;m["lines"].kind=Json::Array;if(gap){Json l=Json::obj();l["time"]=0;l["duration"]=first;l["text"]="";l["isInterlude"]=true;m["lines"].array.push_back(l);}for(int i=0;i<3;i++){Json l=Json::obj();l["time"]=first+i*5000;l["duration"]=5000;l["text"]=std::string(id)+" lyric "+std::to_string(i);l["translation"]="Translation";if(words){Json word=Json::obj();word["text"]=l["text"];word["time"]=l["time"];word["duration"]=5000;l["words"].kind=Json::Array;l["words"].array.push_back(word);}m["lines"].array.push_back(l);}a.receive(m);};
  auto clock=[&](double time,bool playing=true){Json m=Json::obj();m["type"]="clock";m["position"]=time;m["valid"]=true;m["playing"]=playing;a.receive(m);};
  Surface surface,reference;
  auto render=[&](){surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawLyrics(surface.g,a.current()+a.offset);check(surface.target->EndDraw());GdiFlush();return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);};
  auto click=[&](int id,double target){auto&g=a.group(id);double beforePosition=a.position;int x=int(std::round((a.textX+8)*a.scale())),y=int(std::round((g.y.value(a.motionNow())+g.content->mainHeight*g.zoom.value(a.motionNow())*.5f)*a.scale()));a.hover=false;a.message(WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));require(a.seekPending&&a.position==beforePosition,"visible lyric click requests without fabricating progress");Json confirmed=Json::obj();confirmed["type"]="clock";confirmed["songId"]=a.songId;confirmed["position"]=target;confirmed["valid"]=true;confirmed["playing"]=a.playing;confirmed["seekState"]="confirmed";confirmed["seekRequest"]=double(a.seekRequest);a.receive(confirmed);require(!a.seekPending&&!a.flight.rows.empty(),"confirmed prelude seek retains the normal flight");};
  track("early-lyrics");lyrics("early-lyrics",6000);clock(1000);a.testWallMs+=1000;a.update();
  require(a.groupVisible(a.group(1),a.motionNow())&&a.groupVisible(a.group(2),a.motionNow()),"implicit prelude must display later lyrics within viewport");
  auto before=render();if(!directory.empty())surface.save(directory/"prelude-preview.bmp");
  auto&g=a.group(0);bool visible=a.groupVisible(g,a.motionNow());Json metrics=Json::obj();metrics["visibleNormalPreview"]=visible;metrics["pass"]=false;if(!directory.empty()){std::ofstream report(directory/"prelude-results.json");report<<metrics.dump();}
  require(a.lyricView()==App::LyricView::Prelude&&visible,"prelude preview must be a visible normal lyric and click target");
  expectNear(g.zoom.value(a.motionNow()),ContextFontSize/CurrentFontSize,"prelude preview retains the context size");expectNear(g.y.value(a.motionNow()),a.lyricAnchor()+a.mainRowHeight+GroupGap,"preview stays below the prelude dots");
  DWRITE_FONT_WEIGHT weight{};check(g.content->main.value->GetFontWeight(0,&weight,nullptr));require(weight==DWRITE_FONT_WEIGHT_SEMI_BOLD,"preview uses the original lyric font weight");
  reference.target->BeginDraw();reference.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawGroup(reference.g,g,0,a.current());check(reference.target->EndDraw());GdiFlush();
  int top=int(std::floor(g.y.value(a.motionNow()))),bottom=std::min(Surface::height,int(std::ceil(g.y.value(a.motionNow())+g.renderedHeight(g.zoom.value(a.motionNow())))));
  require(std::equal(before.begin()+top*Surface::width*4,before.begin()+bottom*Surface::width*4,reference.pixels+top*Surface::width*4),"prelude preview pixels match the normal semibold lyric and translation");
  a.bridge=std::make_unique<Bridge>(0,std::string(32,'p'),[](Json){});click(0,6000);require(a.flight.rows[1].from.opacity==1&&a.flight.rows[1].to.opacity==1,"seek flight preserves later rows");auto firstFrame=render();require(std::equal(before.begin()+top*Surface::width*4,before.begin()+bottom*Surface::width*4,firstFrame.begin()+top*Surface::width*4),"prelude seek starts from the displayed original and translation pixels");a.testWallMs+=900;a.update();require(a.flight.rows.empty()&&a.index==0,"prelude click lands on the first lyric");
  a.cancelSeek();a.seekTo(0);a.update();a.testWallMs+=900;a.update();a.position=5800;a.update();a.testWallMs+=200;a.position=6000;a.update();
  require(a.index==0&&a.transitionActive,"natural first-line onset animates from the preview");a.testWallMs+=500;a.update();expectNear(a.group(0).zoom.value(a.motionNow()),1,"first line grows to the normal focus size");expectNear(a.group(0).y.value(a.motionNow()),a.lyricAnchor(),"first line returns to the normal focus anchor");
  // Clock-first arrival after a different song must replace the prior preview.
  track("late-lyrics");clock(1000);require(a.lyricView()==App::LyricView::Waiting,"clock-first track waits for its lyrics");lyrics("late-lyrics",10000,false,true);a.testWallMs+=1000;a.update();
  require(a.groupVisible(a.group(0),a.motionNow())&&a.group(0).content->model.text==L"late-lyrics lyric 0","late lyrics replace the old preview and become clickable");click(0,10000);a.testWallMs+=900;a.update();
  a.cancelSeek();a.seekTo(0);a.playing=false;a.update();a.testWallMs+=900;a.update();require(a.lyricView()==App::LyricView::Prelude&&a.flight.rows.empty(),"paused seek back lands in prelude");require(a.groupVisible(a.group(1),a.motionNow())&&a.group(1).opacity==1,"paused return retains later word lyrics");
  require(a.groupVisible(a.group(0),a.motionNow()),"returned prelude retains the same visible lyric target");click(0,10000);a.testWallMs+=900;a.update();require(a.flight.rows.empty()&&!a.frameRunning,"paused prelude click finishes its animation and sleeps");
  // RNP's explicit opening gap keeps the same ordinary-row input behavior.
  track("explicit-gap");lyrics("explicit-gap",6000,true);clock(1000);a.testWallMs+=1000;a.update();
  require(a.lyricView()==App::LyricView::Rows&&a.groupVisible(a.group(1),a.motionNow()),"explicit opening gap keeps the normal preview");require(a.groupVisible(a.group(2),a.motionNow())&&a.groupVisible(a.group(3),a.motionNow()),"explicit prelude also displays later lyrics");click(1,6000);
  for(bool explicitGap:{false,true}){
   const char*id=explicitGap?"paused-explicit-gap":"paused-prelude";
   track(id);lyrics(id,6000,explicitGap);clock(1000);a.testWallMs+=1000;clock(1000,false);
   auto dots=[&]() -> const InterludeAnimation& {return explicitGap?a.groups.at(0).dots:a.preludeDots;};
   const double motion=a.motionNow();
   expectNear(dots().opacity.value(a.indicatorNow()),1,"pausing in the opening gap keeps the dots visible");
   const auto before=dots().dot(1000,0,6000,0,a.indicatorNow());
   a.hover=true;
   const LPARAM point=MAKELPARAM(int(std::round(a.transport.left+a.transport.length()*.2)),int(std::round(a.transport.y)));
   a.message(WM_LBUTTONDOWN,MK_LBUTTON,point);a.message(WM_LBUTTONUP,0,point);
   require(a.seekPending&&a.current()==1000,"progress-bar click waits for the paused seek confirmation");
   Json confirmed=Json::obj();confirmed["type"]="clock";confirmed["songId"]=id;confirmed["position"]=6000;confirmed["valid"]=true;confirmed["playing"]=false;confirmed["seekState"]="confirmed";confirmed["seekRequest"]=double(a.seekRequest);a.receive(confirmed);
   require(!a.seekPending&&!a.flight.rows.empty()&&a.frameRunning,"paused progress-bar seek animates the transition");
   expectNear(dots().dot(6000,0,6000,0,a.indicatorNow()).alpha,before.alpha,"outgoing dots start from the previous position");
   a.testWallMs+=250;a.update();
   const float opacity=dots().opacity.value(a.indicatorNow());
   require(opacity>0&&opacity<1,"opening dots fade while the paused seek animates");
   expectNear(a.motionNow(),motion,"paused seek does not advance the lyric playback animation clock");
   a.testWallMs+=750;a.update();
   require(a.index==(explicitGap?1:0)&&a.current()==6000&&!a.playing&&!a.frameRunning,"pause and target are preserved after settling");
   require(!dots().visible(a.indicatorNow()),"opening dots are gone after settling");
  }
  metrics["pass"]=true;metrics["scenarios"]=3;metrics["normalFontPixels"]=true;metrics["clickRequests"]=4;metrics["lateArrival"]=true;metrics["pausedReturn"]=true;
  if(!directory.empty()){std::ofstream report(directory/"prelude-results.json");report<<metrics.dump();}
  std::cout<<"PASS: prelude font/pixels and clicks, late lyrics and paused return, explicit opening gap\n";
 }
 static void movingFrames(const std::filesystem::path&directory){
  Fixture f;auto&a=f.a;DestroyWindow(a.hwnd);a.hwnd=nullptr;a.renderVisible=false;
  CancelWaitableTimer(a.frameTimer);CloseHandle(a.frameTimer);
  a.frameTimer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_MODIFY_STATE|SYNCHRONIZE);
  require(a.frameTimer!=nullptr,"create high-resolution move test timer");
  check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,a.factory.put()));a.makeFormat(a.metaFormat,11.5f,DWRITE_FONT_WEIGHT_NORMAL);
  WNDCLASSW cls{};cls.lpfnWndProc=App::procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MiniLyrics.MoveTest";RegisterClassW(&cls);
  a.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Isolated move checks",WS_POPUP,-30000,-30000,600,240,nullptr,nullptr,cls.hInstance,&a);
  require(a.hwnd!=nullptr,"create production-procedure move window");ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);
  Line line;line.time=0;line.duration=60000;line.text=L"Moving lyric";line.words={{line.text,0,60000}};a.lines={line};a.layoutDirty=true;
  a.testWall=false;a.freezePlayback=false;a.position=1000;a.anchor=nowMs();a.playing=a.valid=true;a.fps=60;a.background.mode=2;a.background.fps=60;a.coverSince=-1000;
  a.cover.width=a.cover.height=2;a.cover.pixels={30,60,220,255,150,20,80,255,80,180,30,255,180,100,20,255};a.fluid.setCover(a.cover.pixels,2,2,0,true);
  a.update();UpdateWindow(a.hwnd);require(a.error.empty()&&a.frameArmed,"initial animation is ready");
  // Like the native modal move loop, dispatch window messages without servicing
  // the handle waited by App::run. No real mouse input or player is involved.
  auto pump=[&](double milliseconds,bool move=false){double end=nowMs()+milliseconds;MSG msg{};int step=0;while(nowMs()<end){if(move)SetWindowPos(a.hwnd,nullptr,-30000+(step++%20),-30000,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message!=WM_QUIT){TranslateMessage(&msg);DispatchMessageW(&msg);}}MsgWaitForMultipleObjectsEx(0,nullptr,5,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}};
  SendMessageW(a.hwnd,WM_ENTERSIZEMOVE,0,0);UpdateWindow(a.hwnd);
  require(a.systemMoving&&a.modalFrames.active(),"system move forwards the existing timer");
  auto start=a.renderCalls,builds=a.lineBuildCalls,changes=a.layoutChanges,revision=a.fluid.revision;auto target=a.target.get();auto bitmap=a.graphics.background.get();
  double begin=nowMs();pump(500,true);double elapsed=nowMs()-begin;auto frames=a.renderCalls-start;auto backgroundFrames=a.fluid.revision-revision;
  // Keep one actual queued notification to verify it cannot survive this move.
  MSG stale{};double deadline=nowMs()+200;bool queued=false;while(nowMs()<deadline&&!(queued=PeekMessageW(&stale,a.hwnd,App::FrameReady,App::FrameReady,PM_REMOVE)!=FALSE))MsgWaitForMultipleObjectsEx(0,nullptr,5,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
  SendMessageW(a.hwnd,WM_EXITSIZEMOVE,0,0);
  Json metrics=Json::obj();metrics["messageOnlyPumpMs"]=elapsed;metrics["movingFrames"]=double(frames);metrics["requestedFps"]=60;metrics["pass"]=frames>=8;
  require(frames>=8,"native modal message pump must continue animation frames");
  require(backgroundFrames>=8&&a.fluid.gpuReady(),"GPU background continues alongside word animation");metrics["backgroundFrames"]=double(backgroundFrames);
  require(a.lineBuildCalls==builds&&a.layoutChanges==changes&&a.target.get()==target&&a.graphics.background.get()==bitmap,"moving keeps cached layout, target and background resources");
  require(!a.systemMoving&&!a.modalFrames.active()&&a.frameArmed,"release unregisters modal forwarding and restores the normal timer");
  require(queued&&!a.modalFrames.accepts(stale.wParam),"queued notification is invalid after release");
  auto armed=a.frameDeadline;SendMessageW(a.hwnd,App::FrameReady,stale.wParam,0);require(a.frameDeadline==armed&&a.frameArmed,"stale notification cannot advance the restored schedule");
  require(WaitForSingleObject(a.frameTimer,200)==WAIT_OBJECT_0,"normal frame handle resumes after moving");a.frameArmed=false;a.update();UpdateWindow(a.hwnd);
  a.playing=false;a.update();UpdateWindow(a.hwnd);SendMessageW(a.hwnd,WM_ENTERSIZEMOVE,0,0);UpdateWindow(a.hwnd);
  start=a.renderCalls;revision=a.fluid.revision;pump(150);
  require(!a.frameRunning&&!a.frameArmed&&a.renderCalls==start&&a.fluid.revision==revision,"paused moving does not poll or animate");
  SendMessageW(a.hwnd,WM_EXITSIZEMOVE,0,0);require(!a.modalFrames.active()&&!a.frameRunning,"paused release leaves no frame waiter or timer running");
  SendMessageW(a.hwnd,WM_ENTERSIZEMOVE,0,0);DestroyWindow(a.hwnd);a.hwnd=nullptr;
  require(!a.modalFrames.active()&&!a.frameRunning,"destroy during a move releases the waiter before closing the timer");
  metrics["scenarios"]=3;metrics["resumeAndStaleMessage"]=true;metrics["pausedFrames"]=0;metrics["resourceReuse"]=true;metrics["destroyCleanup"]=true;
  if(!directory.empty()){std::ofstream report(directory/"moving-results.json");report<<metrics.dump();}
  std::cout<<"PASS: moving frames="<<frames<<", background="<<backgroundFrames<<" in "<<elapsed<<" ms; resume, pause and cleanup passed\n";
 }
 static void windowRecovery(const std::filesystem::path&directory){
  {
   Fixture f;auto&a=f.a;
   WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MiniLyrics.HoverRecoveryTest";
   cls.lpfnWndProc=[](HWND hwnd,UINT msg,WPARAM wp,LPARAM lp)->LRESULT{
    if(msg==WM_NCLBUTTONDOWN){SetWindowLongPtrW(hwnd,GWLP_USERDATA,wp);return 0;}
    return DefWindowProcW(hwnd,msg,wp,lp);
   };
   RegisterClassW(&cls);DestroyWindow(a.hwnd);
   a.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"MiniLyrics hover recovery",WS_POPUP,-30000,-30000,600,240,nullptr,nullptr,cls.hInstance,nullptr);
   require(a.hwnd!=nullptr,"create hover and caption input window");
   struct ResetHoverProbe {~ResetHoverProbe(){hoverTestWindow=nullptr;}} resetHoverProbe;
   hoverTestWindow=a.hwnd;hoverTestCursor={30,30};ClientToScreen(a.hwnd,&hoverTestCursor);
   const auto path=directory.empty()?std::filesystem::temp_directory_path()/L"MiniLyrics-hover-recovery.json":directory/"hover-recovery.json";
   a.settingsPath=path.wstring();
   a.hover=true;a.dragProgress=true;hoverTrackingFlags=0;a.message(WM_CAPTURECHANGED,0,0);
   require(!a.dragProgress&&a.hover&&hoverTrackingFlags==TME_LEAVE,"ending a transport drag rearms leave tracking inside the client");
   a.resizing=true;hoverTrackingFlags=0;a.message(WM_CAPTURECHANGED,0,0);
   require(!a.resizing&&a.hover&&hoverTrackingFlags==TME_LEAVE,"ending a resize rearms leave tracking inside the client");
   a.systemMoving=true;hoverTrackingFlags=0;a.message(WM_EXITSIZEMOVE,0,0);
   require(!a.systemMoving&&a.hover&&hoverTrackingFlags==TME_LEAVE,"ending a caption move rearms leave tracking");
   a.message(WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(30,int(a.transport.y)));
   require(GetWindowLongPtrW(a.hwnd,GWLP_USERDATA)==HTCAPTION,"cover bottom clicks reach the caption drag path");
   {std::ofstream saved(path);saved<<"unchanged";}
   hoverTrackingFlags=0;a.finishMenu(0);
   require(a.hover&&hoverTrackingFlags==TME_LEAVE,"dismissing the menu rearms leave tracking");
   a.finishMenu(3);
   require(a.through&&!a.hover&&hoverTrackingFlags==(TME_CANCEL|TME_LEAVE),"pass-through hides the toolbar and cancels leave tracking");
   std::ifstream saved(path);std::string content((std::istreambuf_iterator<char>(saved)),{});
   require(content=="unchanged","dismissing the menu and toggling pass-through skip settings writes");
   a.setThrough(false);a.message(WM_MOUSELEAVE,0,0);
   require(!a.hover,"mouse leave hides the toolbar after recovery");
  }
  {
   Fixture f;auto&a=f.a;
   check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,a.factory.put()));
   SetWindowPos(a.hwnd,nullptr,-30000,-30000,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER|SWP_SHOWWINDOW);
   a.update();a.paint();require(a.target&&!a.frameRunning,"paused window draws without an active frame timer");
   for(unsigned attempt=0;attempt<=App::MaxTargetLossRetries;attempt++){
    ValidateRect(a.hwnd,nullptr);a.finishDraw(static_cast<HRESULT>(D2DERR_RECREATE_TARGET));
    require(!a.target&&!a.graphics.target,"device loss releases Direct2D resources");
    require(bool(GetUpdateRect(a.hwnd,nullptr,FALSE))==(attempt<App::MaxTargetLossRetries),"paused device loss queues a bounded repaint");
   }
   a.renderVisible=false;ValidateRect(a.hwnd,nullptr);a.finishDraw(static_cast<HRESULT>(D2DERR_RECREATE_TARGET));
   require(!GetUpdateRect(a.hwnd,nullptr,FALSE),"hidden device loss does not queue another repaint");
   a.renderVisible=true;InvalidateRect(a.hwnd,nullptr,FALSE);a.paint();
   require(a.target&&a.targetLossRetries==0&&a.error.empty(),"a successful redraw restores resources and resets retry budget");
   a.finishDraw(static_cast<HRESULT>(D2DERR_RECREATE_TARGET));
   require(GetUpdateRect(a.hwnd,nullptr,FALSE)&&!a.frameRunning,"a later device loss schedules recovery while paused");
  }
  std::cout<<"PASS: hover rearming, cover caption dragging, menu saves and paused device recovery\n";
 }
 static void regressions(const std::filesystem::path&directory,bool includeGpu){
  windowRecovery(directory);
  struct Sockets {Sockets(){WSADATA data{};require(WSAStartup(MAKEWORD(2,2),&data)==0,"initialize regression sockets");}~Sockets(){WSACleanup();}} sockets;
  // 1. Bridge fragmented WebSocket text frames, interleaved ping, and concurrent send/stop.
  {
   std::mutex rxMutex;std::vector<Json> received;std::atomic<bool> disconnected{false};
   std::string secret="0123456789abcdef0123456789abcdef";
   Bridge bridge(0,secret,[&](Json m){std::lock_guard<std::mutex> lock(rxMutex);received.push_back(std::move(m));},[&]{disconnected=true;});
   int port=bridge.boundPort();require(port>0,"ephemeral bridge bound");
   SOCKET sock=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(sock!=INVALID_SOCKET,"create client socket");
   sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(static_cast<unsigned short>(port));addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
   require(connect(sock,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0,"connect loopback bridge");
   std::string req="GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
   require(send(sock,req.data(),int(req.size()),0)==int(req.size()),"send WS upgrade");
   std::string resp;char buf[1024];
   while(resp.find("\r\n\r\n")==std::string::npos){int r=recv(sock,buf,sizeof(buf),0);require(r>0,"read WS upgrade response");resp.append(buf,size_t(r));}
   require(resp.find("101 Switching Protocols")!=std::string::npos,"upgrade accepted");
   auto sendWs=[&](bool fin,int opcode,const std::string&payload){
    std::string f;f.push_back(char((fin?0x80:0x00)|(opcode&0x0f)));
    size_t n=payload.size();
    if(n<126)f.push_back(char(0x80|n));else{f.push_back(char(0x80|126));f.push_back(char(n>>8));f.push_back(char(n&0xff));}
    unsigned char mask[4]={0x12,0x34,0x56,0x78};f.append(reinterpret_cast<char*>(mask),4);
    for(size_t i=0;i<n;i++)f.push_back(char(static_cast<unsigned char>(payload[i])^mask[i%4]));
    require(send(sock,f.data(),int(f.size()),0)==int(f.size()),"send masked WS frame");
   };
   sendWs(true,1,"{\"type\":\"hello\",\"token\":\""+secret+"\"}");
   sendWs(true,1,"{\"type\":\"hello\",\"token\":\""+secret+"\"}");
   sendWs(false,1,"{\"type\":\"lyrics\",");
   sendWs(true,9,"hi");
   sendWs(true,0,"\"songId\":\"frag\"}");
   std::string pong;
   for(int i=0;i<100;i++){
    {std::lock_guard<std::mutex> lock(rxMutex);if(received.size()>=2)break;}
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
   }
   {std::lock_guard<std::mutex> lock(rxMutex);require(received.size()==2&&received[0]["type"].s()=="hello"&&received[1]["type"].s()=="lyrics"&&received[1]["songId"].s()=="frag","one session hello followed by fragmented text across interleaved ping");}
   unsigned char pongHdr[2]{};require(recv(sock,reinterpret_cast<char*>(pongHdr),2,0)==2,"receive pong header");
   require((pongHdr[0]&0x0f)==10&&(pongHdr[1]&0x7f)==2,"interleaved ping answered with pong");
   char pongBody[2]{};require(recv(sock,pongBody,2,0)==2&&pongBody[0]=='h'&&pongBody[1]=='i',"pong payload intact");
   std::thread spammer([&]{Json ping=Json::obj();ping["type"]="ping";for(int i=0;i<200;i++)bridge.send(ping);});
   std::this_thread::sleep_for(std::chrono::milliseconds(5));
   bridge.stop();spammer.join();closesocket(sock);
  }
  // 2. Minimized save() preserves normal window position/size.
  {
   std::filesystem::path dir=directory.empty()?std::filesystem::temp_directory_path()/L"MiniLyrics-min-test":directory;
   std::filesystem::create_directories(dir);
   Fixture f;auto&a=f.a;DestroyWindow(a.hwnd);a.hwnd=nullptr;a.renderVisible=false;
   check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,a.factory.put()));
   a.makeFormat(a.metaFormat,11.5f,DWRITE_FONT_WEIGHT_NORMAL);
   WNDCLASSW cls{};cls.lpfnWndProc=App::procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MiniLyrics.MinimizeTest";RegisterClassW(&cls);
   a.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"Minimize bounds",WS_POPUP,120,140,480,150,nullptr,nullptr,cls.hInstance,&a);
   require(a.hwnd!=nullptr,"create minimize test window");
   a.settingsPath=(dir/"minimized-settings.json").wstring();
   ShowWindow(a.hwnd,SW_SHOWNOACTIVATE);a.geometry();
   RECT normal{};GetWindowRect(a.hwnd,&normal);
   ShowWindow(a.hwnd,SW_SHOWMINNOACTIVE);require(IsIconic(a.hwnd),"window is minimized");
   a.save();DestroyWindow(a.hwnd);a.hwnd=nullptr;
   std::ifstream in(dir/"minimized-settings.json",std::ios::binary);std::string text((std::istreambuf_iterator<char>(in)),{});
   Json saved=JsonParser(text).parse();
   require(saved["X"].n()==normal.left&&saved["Y"].n()==normal.top,"minimized save preserves X/Y instead of -32000");
   require(saved["Width"].n()>=360&&saved["Height"].n()>=130,"minimized save preserves normal dimensions");
  }
  // 3. ping clears connectionStale and error="disconnected" without unfreezing playback.
  {
   Fixture f;auto&a=f.a;a.playing=true;a.valid=true;a.freezeConnection();
   require(a.connectionStale&&a.error=="disconnected"&&!a.playing,"freeze sets disconnected state and pauses playback");
   Json ping=Json::obj();ping["type"]="ping";a.receive(ping);
   require(!a.connectionStale&&a.error.empty()&&!a.playing,"ping clears stale disconnected state without resuming playback");
  }
  {
   Fixture f;auto&a=f.a;f.lines();a.songId="same-song";a.seekSerial=7;a.seekRequest=7;a.seekPending=true;
   Json clock=Json::obj();clock["type"]="clock";clock["songId"]=a.songId;clock["position"]=27000;clock["valid"]=true;clock["seekRequest"]=0;
   a.receive(clock);require(a.position==21000&&a.seekPending,"old session seek blocks clock before a new handshake");
   // A new peer can authenticate before the UI handles the old disconnect.
   Json hello=Json::obj();hello["type"]="hello";a.receive(hello);
   require(!a.seekPending&&a.seekRequest==0&&a.seekSerial==7,"hello clears the old request and preserves the seek serial");
   a.receive(clock);require(a.position==27000&&a.valid,"first clock for the same song is accepted after hello");
   a.message(WM_TIMER,3,0);require(a.position==27000&&a.error.empty(),"a queued timeout cannot restore the old position");
  }
  // 3b. Host seek timeout clears seekRequest so later ordinary clocks are accepted.
  {
   Fixture f;auto&a=f.a;f.lines();a.songId="timeout-song";
   a.bridge=std::make_unique<Bridge>(0,std::string(32,'s'),[](Json){});
   a.requestSeek(30000);
   require(a.seekPending&&a.seekRequest>0,"requestSeek sets seekPending and seekRequest");
   a.message(WM_TIMER,3,0);
   require(!a.seekPending&&a.seekRequest==0&&a.error=="seek-timeout","seek timeout clears both seekPending and seekRequest");
   Json clock=Json::obj();clock["type"]="clock";clock["songId"]=a.songId;clock["position"]=27000;clock["valid"]=true;clock["playing"]=true;clock["seekRequest"]=0;
   a.receive(clock);
   require(a.position==27000&&a.valid&&a.playing,"ordinary clock after seek timeout is accepted");
  }
  // 3c. Cover worker delivers permanent 404 errors immediately without retrying.
  {
   SOCKET srv=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(srv!=INVALID_SOCKET,"create cover test server");
   sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
   require(bind(srv,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0&&listen(srv,4)==0,"listen cover test server");
   int addrLen=sizeof(addr);require(getsockname(srv,reinterpret_cast<sockaddr*>(&addr),&addrLen)==0,"read cover test port");
   std::atomic<int> hits{0};std::atomic<bool> stopSrv{false};
   std::thread serverThread([&]{
    while(!stopSrv){
     fd_set fds;FD_ZERO(&fds);FD_SET(srv,&fds);timeval tv{0,50000};
     if(select(0,&fds,nullptr,nullptr,&tv)<=0)continue;
     SOCKET c=accept(srv,nullptr,nullptr);if(c==INVALID_SOCKET)break;
     ++hits;char buf[1024];recv(c,buf,sizeof(buf),0);
     const char*resp="HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
     send(c,resp,int(std::strlen(resp)),0);closesocket(c);
    }
   });
   Fixture f;auto&a=f.a;DestroyWindow(a.hwnd);a.hwnd=nullptr;
   WNDCLASSW cls{};cls.lpfnWndProc=App::procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MiniLyrics.CoverWorkerRegTest";RegisterClassW(&cls);
   a.hwnd=CreateWindowExW(0,cls.lpszClassName,L"Cover worker reg",WS_POPUP,-30000,-30000,100,100,nullptr,nullptr,cls.hInstance,&a);
   require(a.hwnd!=nullptr,"create cover worker test window");
   a.startCoverWorker();
   std::wstring url=L"http://127.0.0.1:"+std::to_wstring(ntohs(addr.sin_port))+L"/404.png";
   a.loadCover(url);
   double deadline=nowMs()+2500;bool delivered=false;MSG msg{};
   while(nowMs()<deadline&&!delivered){
    while(PeekMessageW(&msg,a.hwnd,0,0,PM_REMOVE)){
     if(msg.message==App::CoverReady)delivered=true;
     TranslateMessage(&msg);DispatchMessageW(&msg);
    }
    if(!delivered)std::this_thread::sleep_for(std::chrono::milliseconds(10));
   }
   a.stopCoverWorker();stopSrv=true;closesocket(srv);serverThread.join();
   require(delivered&&hits.load()==1&&a.cover.status==CoverStatus::PermanentError&&a.coverUrl==url,"permanent cover failure does not retry and preserves coverUrl");
  }
  // 4. untimed first wheel-down notch advances and animates cleanly.
  {
   Fixture f;auto&a=f.a;Json m=Json::obj();m["type"]="lyrics";m["songId"]="";m["mode"]="untimed";m["lines"].kind=Json::Array;
   for(int i=0;i<5;i++){Json l=Json::obj();l["time"]=0;l["text"]="untimed "+std::to_string(i);m["lines"].array.push_back(l);}
   a.receive(m);a.playing=false;a.valid=false;a.update();require(a.focusIndex==0,"untimed starts at row 0");
   f.wheel(-120);require(a.browseIndex==1&&a.focusIndex==1&&a.frameRunning,"first untimed wheel-down moves to row 1 and animates");
   expectNear(a.group(1).y.duration,250,"untimed wheel uses 250ms transition");
   a.testWallMs+=250;a.update();require(!a.frameRunning&&!a.frameArmed,"untimed wheel stops frames once settled");
   expectNear(a.group(1).y.value(a.motionNow()),a.group(1).y.to,"untimed wheel finishes at target");
  }
  // 5. WM_GETMINMAXINFO enforces both minimum width and height.
  {
   Fixture f;auto&a=f.a;a.dpi=144;MINMAXINFO mmi{};
   a.message(WM_GETMINMAXINFO,0,reinterpret_cast<LPARAM>(&mmi));
   require(mmi.ptMinTrackSize.x==540&&mmi.ptMinTrackSize.y==195,"WM_GETMINMAXINFO enforces scaled 360x130 minimum");
  }
  // 6. Shader cache writes DXBC files and second Renderer loads cleanly (when hardware GPU is available).
  if(includeGpu){
   std::filesystem::path base=directory.empty()?std::filesystem::temp_directory_path()/L"MiniLyrics-cache-test":directory/"cache-appdata";
   std::filesystem::remove_all(base);std::filesystem::create_directories(base);
   struct EnvGuard {
    std::wstring prev=env(L"LOCALAPPDATA");
    ~EnvGuard(){SetEnvironmentVariableW(L"LOCALAPPDATA",prev.empty()?nullptr:prev.c_str());}
   } envGuard;
   SetEnvironmentVariableW(L"LOCALAPPDATA",base.wstring().c_str());
   FluidBackground first,second;BackgroundSettings s;s.mode=1;
   first.prepare(s,0,0);require(first.gpuReady(),first.gpuError.c_str());
   auto cacheDir=base/L"MiniLyrics"/L"shader-cache";
   int dxbcCount=0;for(const auto&entry:std::filesystem::directory_iterator(cacheDir))if(entry.path().extension()==L".dxbc")++dxbcCount;
   require(dxbcCount==8,"all 8 Lyricify shaders cached as DXBC");
   second.prepare(s,0,0);require(second.gpuReady()&&second.pixels==first.pixels,"cached DXBC shaders load and render identically");
   FluidBackground permanent;permanent.failed=true;permanent.gpuError="pinned";
   permanent.prepare(s,0,0);require(!permanent.gpuReady()&&permanent.gpuError=="pinned","permanent failure stays on CPU fallback");
  }
  // 7. StaticPreview -> first valid clock snaps alpha immediately without dipping to 0.4.
  {
   Fixture f;auto&a=f.a;a.makeFormat(a.metaFormat,11.5f,DWRITE_FONT_WEIGHT_NORMAL);a.translation=true;
   Json track=Json::obj();track["type"]="track";track["id"]="preview-snap";track["title"]="Snap";a.receive(track);
   Json lyr=Json::obj();lyr["type"]="lyrics";lyr["songId"]="preview-snap";lyr["revision"]=1;lyr["lines"].kind=Json::Array;
   Json row=Json::obj();row["time"]=0;row["duration"]=4000;row["text"]="First line";row["translation"]="First translation";lyr["lines"].array.push_back(row);
   a.receive(lyr);
   require(!a.valid&&a.lyricView()==App::LyricView::StaticPreview,"starts in StaticPreview before clock");
   Json c=Json::obj();c["type"]="clock";c["songId"]="preview-snap";c["position"]=1000;c["playing"]=true;c["valid"]=true;
   a.receive(c);
   expectNear(a.group(0).mainAlpha.value(a.motionNow()),1.f,"first valid clock snaps row 0 mainAlpha to 1.0");
   expectNear(a.group(0).subAlpha.value(a.motionNow()),.8f,"first valid clock snaps row 0 subAlpha to 0.8");
  }
  std::cout<<"PASS: fragmented WS/concurrency, minimized save, ping recovery, untimed wheel, min-track size, preview snap"<<(includeGpu?" and shader cache":"")<<"\n";
 }
 static void modernEffects(){
  // 1. Offscreen 1.0-scale RowTexture eliminates zoom=1.0 hinting jump and reuses textures.
  {
   Fixture f;auto&a=f.a;a.textX=32;a.index=0;a.valid=true;a.hasClock=true;
   Line line;line.time=0;line.duration=5000;line.text=L"Mixed 中文测试 Smooth";line.translation=L"Translation line";
   a.lines={line};auto&row=a.group(0);row.y.snap(38);row.mainAlpha.snap(.86f);row.subAlpha.snap(.58f);
   Surface surface;surface.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
   auto renderZoom=[&](float z){
    row.zoom.snap(z);
    surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    a.drawGroup(surface.g,row,0,0);check(surface.target->EndDraw());GdiFlush();
    return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);
   };
   auto approaching=renderZoom(.9999f);
   auto builds=surface.g.rowTextureBuilds;
   require(builds>=1,"offscreen row texture built on first draw");
   auto settled=renderZoom(1.f);
   require(surface.g.rowTextureBuilds==builds,"zoom transition reuses cached row texture without rebuild");
   unsigned maxDelta=0;
   for(size_t p=0;p<approaching.size();p+=4){
    unsigned d=std::abs(int(approaching[p+1])-int(settled[p+1]));
    maxDelta=std::max(maxDelta,d);
   }
   require(maxDelta<=8,"zoom=0.9999 -> 1.0 has no glyph hinting jump");
   surface.g.clear();
   require(surface.g.rowTextures.empty(),"graphics.clear() releases cached row textures");
   check(surface.target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),surface.g.brush.put()));
   surface.g.target=surface.target.get();
   require(renderZoom(1.f)==settled,"rebuilt row texture matches after clear");
  }
  // 2. Gradient word sweep (wordMode=1) produces smooth left-to-right progress and matches endpoints.
  {
   Fixture f;auto&a=f.a;a.textX=20;a.index=0;a.valid=true;a.hasClock=true;a.playing=true;
   Line line;line.time=1000;line.duration=2000;line.text=L"Sweeping";
   line.words={{L"Sweeping",1000,1000,false}};
   a.lines={line};auto&row=a.group(0);row.y.snap(40);row.zoom.snap(1.f);row.active=true;
   Surface surface;surface.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
   auto renderAt=[&](int mode,double time){
    a.wordMode=mode;
    surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    a.drawGroup(surface.g,row,0,time);check(surface.target->EndDraw());GdiFlush();
    return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);
   };
   auto fadeUnsung=renderAt(0,1000),fadeSung=renderAt(0,2000);
   auto sweepStart=renderAt(1,1000),sweepMid=renderAt(1,1500),sweepEnd=renderAt(1,2000);
   require(sweepStart==fadeUnsung,"sweep start matches unsung 0.4 alpha pixels");
   require(sweepEnd==fadeSung,"sweep end matches full 1.0 alpha pixels");
   require(sweepMid!=sweepStart&&sweepMid!=sweepEnd,"sweep midpoint renders intermediate gradient");
   const float wordW=row.content->glyphs[0].layout.width;
   int x0=int(a.textX),xMidLeft=int(a.textX+wordW*.35f),xMidRight=int(a.textX+wordW*.65f),xEnd=int(a.textX+wordW+4);
   uint64_t leftSum=0,rightSum=0;
   for(int y=35;y<80;++y){
    for(int x=x0;x<xMidLeft;++x)leftSum+=sweepMid[(y*Surface::width+x)*4+1];
    for(int x=xMidRight;x<xEnd;++x)rightSum+=sweepMid[(y*Surface::width+x)*4+1];
   }
   require(leftSum>rightSum*1.4,"sweep midpoint is brighter on the left than on the right");
  }
  // 3. Spring curve (lineCurve=4) inherits velocity on retarget and settles cleanly.
  {
   Fixture f;auto&a=f.a;f.lines();a.playing=true;a.valid=true;f.layout(10);
   Json style=Json::obj();style["type"]="style";style["curve"]=4;style["stagger"]=true;style["wordMode"]=1;
   a.receive(style);
   require(a.lineCurve==4&&a.wordMode==1,"style message applies spring curve and sweep wordMode");
   f.wheel(-120);
   require(a.browseIndex==11&&a.group(11).y.spring&&a.group(11).zoom.spring,"spring curve enables LineTrack spring mode");
   a.testWallMs+=80;a.update();
   const float yBefore=a.group(12).y.value(a.motionNow());
   const double vBefore=a.group(12).y.velocity(a.motionNow());
   require(std::abs(vBefore)>1.,"spring builds non-zero velocity mid-flight");
   f.wheel(-120);
   expectNear(a.group(12).y.value(a.motionNow()),yBefore,"spring retarget keeps continuous position");
   require(std::abs(a.group(12).y.velocity(a.motionNow())-vBefore)<1e-2,"spring retarget inherits instantaneous velocity");
   a.testWallMs+=3000;a.update();
   require(!a.transitionActive&&!a.manualTransition,"spring motion settles completely within 3s");
  }
  // 4. Long-note tail handoff across <4s gap and early cutoff release without jump.
  {
   Fixture f;auto&a=f.a;a.valid=true;a.playing=true;
   Line l0,l1;
   l0.time=0;l0.duration=2000;l0.text=L"Hold";l0.words={{L"Hold",0,2000,true}};
   l1.time=4000;l1.duration=1000;l1.text=L"Next";l1.words={{L"Next",4000,1000,false}};
   a.lines={l0,l1};a.index=0;a.position=3000;f.layout(0);
   double now=a.motionNow();
   require(a.wordGlowIntensity(a.group(0),0,0,3000,now)>.12f,"tail handoff holds ember across 2s vocal gap");
   require(a.wordGlowIntensity(a.group(0),0,0,4125,now)>.04f&&a.wordGlowIntensity(a.group(0),0,0,4125,now)<.12f,"tail handoff fades out over 250ms after next line starts");
   require(a.wordGlowIntensity(a.group(0),0,0,4250,now)==0.f,"tail handoff clears after 250ms release");
   // Early cutoff before 35% of word duration fades out from captured intensity.
   a.position=400;a.index=0;a.group(0).active=true;a.group(0).resetGlows.clear();
   a.index=1;a.roles();
   const float captured=a.wordGlowIntensity(a.group(0),0,0,400,now);
   expectNear(captured,longNoteGlow(400,2000),"early cutoff captures 35% ease-out attack glow intensity");
   require(captured>.7f&&captured<.95f,"early cutoff uses smooth fast-attack intensity without jumping");
   require(a.wordGlowIntensity(a.group(0),0,0,400,now+200)==0.f,"early cutoff completes within 200ms");
  }
  // 5. Interlude exit anticipation draws cleanly in final 750ms and 375ms.
  {
   Fixture f;auto&a=f.a;a.textX=20;a.valid=true;a.playing=true;
   InterludeAnimation ia;ia.update(true,true,0,5550,0,6000);
   Surface surface;
   auto drawDots=[&](double time){
    surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    a.drawInterludeAt(surface.g,ia,20,40,time,0,6000,1000.);
    check(surface.target->EndDraw());GdiFlush();
    uint64_t sum=0;for(size_t i=0;i<Surface::width*Surface::height*4;i+=4)sum+=surface.pixels[i+1];return sum;
   };
   require(drawDots(5550)>0&&drawDots(5800)>0&&drawDots(6000)==0,"interlude exit swell and alpha fade render cleanly");
  }
  // 6. P1 #1: Ultra-wide single word (>4096 px) tiles cleanly and draws scrolled tail in fade & sweep modes.
  {
   Fixture f;auto&a=f.a;a.width=480;a.height=160;a.viewportBottom=132;a.textX=20;a.valid=true;a.hasClock=true;a.playing=true;a.index=0;
   std::wstring hugeWord;
   for(int i=0;i<600;i++)hugeWord+=L"W";
   Line line;line.time=0;line.duration=10000;line.text=hugeWord;line.words={{hugeWord,0,10000,false}};
   a.lines={line};auto&row=a.group(0);row.y.snap(40);row.zoom.snap(1.f);row.active=true;
   require(row.content->glyphs[0].layout.width>4500.f,"ultra-wide word fixture exceeds 4096 DIP");
   Surface surface;surface.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
   for(int mode:{0,1}){
    a.wordMode=mode;
    surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    a.drawGroup(surface.g,row,0,9800);check(surface.target->EndDraw());GdiFlush();
    const auto&wordsAtlas=surface.g.rowTextures[row.content->serial].words;
    require(wordsAtlas.ready&&wordsAtlas.itemSpans.size()==1&&wordsAtlas.itemSpans[0].count>=2,"ultra-wide word splits into multiple slices");
    uint64_t ink=0;
    for(int y=40;y<75;++y)for(int x=int(a.textX);x<int(a.width-20);++x)ink+=surface.pixels[(y*Surface::width+x)*4+1];
    require(ink>0,"ultra-wide word scrolled to tail retains visible ink");
   }
  }
  // 7. P1 #2: Mid-sweep line deactivation fades out from captured sweep mask without a brightness flash.
  {
   Fixture f;auto&a=f.a;a.textX=20;a.valid=true;a.hasClock=true;a.playing=true;a.wordMode=1;
   Line l0,l1;
   l0.time=0;l0.duration=2000;l0.text=L"SweepingWord";l0.words={{L"SweepingWord",0,2000,false}};
   l1.time=1000;l1.duration=1000;l1.text=L"Overlap";l1.words={{L"Overlap",1000,1000,false}};
   a.lines={l0,l1};a.index=0;a.position=1000;a.freezePlayback=true;f.layout(0);
   auto&row=a.group(0);row.y.snap(40);row.zoom.snap(1.f);row.mainAlpha.snap(1.f);
   Surface surface;surface.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
   auto renderRow=[&](double time){
    surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    a.drawGroup(surface.g,row,0,time);check(surface.target->EndDraw());GdiFlush();
    return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);
   };
   const auto beforeCut=renderRow(1000);
   a.index=1;a.roles();
   row.mainAlpha.snap(1.f);
   const auto rightAfterCut=renderRow(1000);
   unsigned cutDiff=0;
   for(size_t i=0;i<beforeCut.size();i+=4)cutDiff=std::max<unsigned>(cutDiff,std::abs(int(beforeCut[i+1])-int(rightAfterCut[i+1])));
   require(cutDiff<=1,"mid-sweep deactivation first frame matches active sweep pixels without brightness jump");
   a.testWallMs+=100;
   const auto midFade=renderRow(1000);
   require(midFade!=rightAfterCut,"deactivated sweep mask fades during the 200ms release");
   a.testWallMs+=100;
   const auto settled=renderRow(1000);
   a.wordMode=0;
   const auto uniformInactive=renderRow(1000);
   require(settled==uniformInactive,"sweep release settles cleanly to 0.4 inactive brightness at 200ms");
  }
  // 8. P1 #3: Paused seek clears stale long-note resetGlows so glow does not stay frozen after seek completes.
  {
   Fixture f;auto&a=f.a;a.width=360;a.height=240;a.viewportBottom=212;a.textX=20;a.valid=true;a.playing=true;
   Line l0,l1,l2;
   l0.time=0;l0.duration=2000;l0.text=L"Hold";l0.words={{L"Hold",0,2000,true}};
   l1.time=500;l1.duration=1000;l1.text=L"Cut";l1.words={{L"Cut",500,1000,false}};
   l2.time=1000;l2.duration=1000;l2.text=L"Later";l2.words={{L"Later",1000,1000,false}};
   a.lines={l0,l1,l2};a.position=500;a.index=1;f.layout(0);
   a.group(0).active=true;a.group(0).resetGlows.clear();
   a.roles();
   require(!a.group(0).resetGlows.empty(),"early cutoff captures release glow");
   a.playing=false;a.update();
   const float frozenGlow=a.wordGlowIntensity(a.group(0),0,0,500,a.motionNow());
   require(frozenGlow>0.1f,"ordinary pause preserves frozen release glow");
   a.seekTo(1100);a.update();
   a.testWallMs+=1000;a.update();
   require(a.flight.rows.empty()&&!a.frameRunning,"paused seek finishes and stops animation timer");
   const auto&old=a.groups.at(0);
   require(a.groupVisible(old,a.motionNow())&&old.resetGlows.empty()&&a.wordGlowIntensity(old,0,0,a.current(),a.motionNow())==0.f,
           "paused seek does not relight a visible inactive word whose nominal interval still covers the destination");
  }
  // 9. P1 #5: Explicit interlude stops 60 FPS scheduling once exitAlpha reaches 0 while preserving next-line wakeup.
  {
   Fixture f;auto&a=f.a;a.width=600;a.height=160;a.viewportBottom=132;a.textX=20;a.valid=true;a.playing=true;a.fps=60;
   Line gap,next;
   gap.time=0;gap.duration=2000;gap.isInterlude=true;
   next.time=22000;next.duration=1000;next.text=L"Resume";
   a.lines={gap,next};a.index=0;a.position=1000;f.layout(0);
   a.testWallMs+=1000;a.updateIndicators();
   expectNear(f.interval(1000),17,"active visible explicit interlude uses 60 FPS");
   a.position=3000;a.updateIndicators();
   require(!a.indicatorVisible(a.group(0),0,3000),"explicit interlude becomes invisible once exitAlpha reaches 0");
   expectNear(f.interval(3000),200,"elapsed explicit interlude drops back to low-frequency idle waiting");
   expectNear(f.interval(21750),50,"idle interlude still wakes accurately for the next lyric lookahead");
  }
  // 10. P1 #6 & P2 #10: Toggling wordMode or switching curve mid-motion preserves active spring without snapping or velocity jump.
  {
   Fixture f;auto&a=f.a;f.lines();a.playing=false;a.valid=true;f.layout(10);
   Json springStyle=Json::obj();springStyle["type"]="style";springStyle["curve"]=4;springStyle["stagger"]=true;springStyle["wordMode"]=0;
   a.receive(springStyle);
   f.wheel(-120);
   a.testWallMs+=60;a.update();
   const float yBefore=a.group(11).y.value(a.motionNow());
   const float zBefore=a.group(11).zoom.value(a.motionNow());
   const double vBefore=a.group(11).y.velocity(a.motionNow());
   require(a.manualTransition&&a.group(11).y.active(a.motionNow()),"paused browse starts active spring");
   Json wmOnly=Json::obj();wmOnly["type"]="style";wmOnly["curve"]=4;wmOnly["stagger"]=true;wmOnly["wordMode"]=1;
   a.receive(wmOnly);
   expectNear(a.group(11).y.value(a.motionNow()),yBefore,"wordMode change during paused browse does not snap Y");
   expectNear(a.group(11).zoom.value(a.motionNow()),zBefore,"wordMode change during paused browse does not snap zoom");
   require(a.wordMode==1&&a.manualTransition&&a.group(11).y.active(a.motionNow()),"wordMode applies without aborting paused browse spring");
   Json toBezier=Json::obj();toBezier["type"]="style";toBezier["curve"]=0;toBezier["stagger"]=true;toBezier["wordMode"]=1;
   a.receive(toBezier);
   a.testWallMs+=1;
   const double vAfter=a.group(11).y.velocity(a.motionNow());
   require(a.lineCurve==0&&a.group(11).y.spring&&std::abs(vAfter-vBefore)<50.,"switching from spring to bezier lets active spring finish without velocity jump");
   f.wheel(-120);
   require(!a.group(12).y.spring&&a.group(12).y.curve==0,"new bezier curve applies on the next transition");
  }
  // 11. P2 #7: Ultra-wide plain line tile seam at non-integer zoom (0.763x) matches unsplit continuous bitmap reference.
  {
   Fixture f;auto&a=f.a;a.width=600;a.height=160;a.viewportBottom=132;a.textX=20;a.valid=true;a.hasClock=true;a.playing=true;a.index=0;
   std::wstring wideLine;
   for(int i=0;i<500;i++)wideLine+=L"W";
   Line line;line.time=0;line.duration=10000;line.text=wideLine;
   a.lines={line};auto&row=a.group(0);row.y.snap(40);row.zoom.snap(.763f);row.mainAlpha.snap(1.f);
   const float available=a.width-a.textX-12.f,total=a.rowWidth(row),overflow=total*.763f-available;
   // Scroll so 4092 px boundary (~4090 DIP at 96 DPI) falls cleanly inside the viewport.
   const double seamTime=900.+(4090.*.763f-available*.5f)/std::max(1.f,overflow)*8400.;
   Surface surface,refSurface;
   surface.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
   refSurface.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
   surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
   a.drawGroup(surface.g,row,0,seamTime);
   check(surface.target->EndDraw());GdiFlush();
   const auto&mainAtlas=surface.g.rowTextures[row.content->serial].main;
   require(mainAtlas.ready&&mainAtlas.slices.size()>=2,"ultra-wide plain line splits into multiple tiles");
   const auto&tile0=mainAtlas.slices[0];
   const float seamDip=tile0.clipRight;
   const float refLeft=std::floor(seamDip-512.f);
   const UINT refW=1024u,refH=std::max(1u,UINT(std::round(tile0.height)));
   Com<ID2D1BitmapRenderTarget> refRt;
   const auto refDip=D2D1::SizeF(float(refW),float(refH));
   const auto refPx=D2D1::SizeU(refW,refH);
   const auto refFmt=D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED);
   check(refSurface.target->CreateCompatibleRenderTarget(&refDip,&refPx,&refFmt,D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,refRt.put()));
   refRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
   Com<ID2D1SolidColorBrush> refWhite;check(refRt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),refWhite.put()));
   refRt->BeginDraw();refRt->Clear(D2D1::ColorF(0.f,0.f,0.f,0.f));
   refRt->DrawTextLayout(D2D1::Point2F(-refLeft,-tile0.top),row.content->main.value.get(),refWhite.get(),D2D1_DRAW_TEXT_OPTIONS_NONE);
   check(refRt->EndDraw());
   Com<ID2D1Bitmap> refBmp;check(refRt->GetBitmap(refBmp.put()));
   const float scrollX=overflow*a.timedHorizontalPhase(row,seamTime,0),rowLeft=a.textX-scrollX;
   const float seamScreenX=rowLeft+seamDip*.763f;
   require(seamScreenX>=60.f&&seamScreenX<=540.f,"tile seam lies inside the compared viewport region");
   const D2D1_RECT_F refDst=D2D1::RectF(rowLeft+refLeft*.763f,40.f+tile0.top*.763f,rowLeft+(refLeft+float(refW))*.763f,40.f+(tile0.top+float(refH))*.763f);
   refSurface.target->BeginDraw();refSurface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
   refSurface.target->DrawBitmap(refBmp.get(),refDst,1.f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
   check(refSurface.target->EndDraw());GdiFlush();
   int maxDiff=0;uint64_t seamInk=0;
   for(int y=40;y<75;++y)for(int x=40;x<560;++x){
    const int p=surface.pixels[(y*Surface::width+x)*4+1],q=refSurface.pixels[(y*Surface::width+x)*4+1];
    maxDiff=std::max(maxDiff,std::abs(p-q));
    if(std::abs(x-int(std::round(seamScreenX)))<=8)seamInk+=p;
   }
   require(seamInk>0,"tile seam region contains rendered glyph ink");
   require(maxDiff<=2,"tiled ultra-wide line seam matches continuous bitmap reference at non-integer zoom");
  }
  // 12. P2 #8: Byte budget rejects oversized 192 DPI atlas cleanly, falls back without retrying every frame, and LRU evicts by bytes.
  {
   Fixture f;auto&a=f.a;a.width=280;a.height=80;a.viewportBottom=80;a.textX=10;a.valid=true;a.hasClock=true;a.index=0;
   Line huge;huge.time=0;huge.duration=10000;
   double wt=0;
   for(int i=0;i<500;i++){huge.text+=L"Word1234";huge.words.push_back({L"Word1234",wt,10});wt+=10;}
   a.lines={huge};auto&row=a.group(0);row.y.snap(20);row.zoom.snap(1.f);row.active=true;
   HDC hiDc=CreateCompatibleDC(nullptr);
   BITMAPINFO hiInfo{};hiInfo.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);hiInfo.bmiHeader.biWidth=Surface::width;hiInfo.bmiHeader.biHeight=-Surface::height;hiInfo.bmiHeader.biPlanes=1;hiInfo.bmiHeader.biBitCount=32;hiInfo.bmiHeader.biCompression=BI_RGB;
   BYTE* hiPixels=nullptr;HBITMAP hiBmp=CreateDIBSection(hiDc,&hiInfo,DIB_RGB_COLORS,reinterpret_cast<void**>(&hiPixels),nullptr,0);
   require(hiDc!=nullptr&&hiBmp!=nullptr,"create 192 DPI memory DIB");
   HGDIOBJ hiOld=SelectObject(hiDc,hiBmp);
   Com<ID2D1Factory> hiFactory;check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,hiFactory.put()));
   Com<ID2D1DCRenderTarget> hiDpi;
   auto hiProps=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),192.f,192.f);
   check(hiFactory->CreateDCRenderTarget(&hiProps,hiDpi.put()));
   RECT hiRect{0,0,Surface::width,Surface::height};check(hiDpi->BindDC(hiDc,&hiRect));
   hiDpi->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
   Graphics hiG;hiG.init(hiDpi.get(),a.bgA,a.bgB,CoverData(),280.f,80.f);
   auto drawHi=[&]{hiDpi->BeginDraw();hiDpi->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawGroup(hiG,row,0,50);check(hiDpi->EndDraw());GdiFlush();};
   drawHi();
   const auto buildsAfterFail=hiG.rowTextureBuilds;
   require(buildsAfterFail==0&&hiG.rowTextures[row.content->serial].words.failed&&hiG.totalRowTextureBytes==0,"oversized 192 DPI atlas hits byte budget and marks failed");
   drawHi();
   require(hiG.rowTextureBuilds==buildsAfterFail,"failed atlas does not retry allocation on every frame");
   uint64_t hiInk=0;for(size_t i=0;i<size_t(Surface::width)*Surface::height*4;i+=4)hiInk+=hiPixels[i+1];
   require(hiInk>0,"oversized line falls back to direct text rendering with visible ink");
   hiG.clear();hiDpi.reset();if(hiOld)SelectObject(hiDc,hiOld);if(hiBmp)DeleteObject(hiBmp);if(hiDc)DeleteDC(hiDc);
   Fixture lru;auto&la=lru.a;la.valid=true;la.hasClock=true;
   for(int i=0;i<25;i++){
    Line rowLine;rowLine.time=i*1000.;rowLine.duration=1000;rowLine.text=L"LRU cache row text sample "+std::to_wstring(i)+L" for byte eviction";
    la.lines.push_back(rowLine);
   }
   Surface lruSurface;lruSurface.g.maxRowTextureBytes=200*1024;
   for(int i=0;i<25;i++){
    la.index=i;auto&lr=la.group(i);lr.y.snap(40);lr.zoom.snap(1.f);
    lruSurface.target->BeginDraw();lruSurface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    la.drawGroup(lruSurface.g,lr,i,i*1000.);check(lruSurface.target->EndDraw());
    require(lruSurface.g.totalRowTextureBytes<=lruSurface.g.maxRowTextureBytes,"row texture LRU stays within total byte budget");
   }
   require(lruSurface.g.totalRowTextureBytes>0&&lruSurface.g.rowTextures.size()<25,"row texture LRU evicts entries to enforce byte cap");
  }
  // 13. P2 #9: Standalone Chinese punctuation does not break long-note tail handoff.
  {
   Fixture f;auto&a=f.a;a.valid=true;a.playing=true;
   Line l0,l1;
   l0.time=0;l0.duration=2050;l0.text=L"Hold\x3002";
   l0.words={{L"Hold",0,2000,true,false},{L"\x3002",2000,50,false,false}};
   l1.time=3600;l1.duration=1000;l1.text=L"Next";l1.words={{L"Next",3600,1000,false,false}};
   a.lines={l0,l1};a.index=0;a.position=2800;f.layout(0);
   expectNear(a.longNoteTailGap(a.lines[0],0,0),1600.,"GetStringTypeW fallback ignores standalone Chinese punctuation");
   require(a.wordGlowIntensity(a.group(0),0,0,2800,a.motionNow())>.12f,"long-note handoff continues across gap despite trailing Chinese punctuation");
   // Verify JSON lineTail wire marker preserves explicit tail continuity too.
   Json msg=Json::obj();msg["type"]="lyrics";msg["songId"]="";msg["lines"].kind=Json::Array;
   Json jl0=Json::obj(),jw0=Json::obj(),jw1=Json::obj(),jl1=Json::obj();
   jw0["text"]="Hold";jw0["time"]=0;jw0["duration"]=2000;jw0["longNote"]=true;jw0["lineTail"]=true;
   jw1["text"]="\xe3\x80\x82";jw1["time"]=2000;jw1["duration"]=50;
   jl0["time"]=0;jl0["duration"]=2050;jl0["words"].kind=Json::Array;jl0["words"].array={jw0,jw1};
   jl1["time"]=3600;jl1["duration"]=1000;jl1["text"]="Next";
   msg["lines"].array={jl0,jl1};
   a.receive(msg);
   require(a.lines[0].words[0].lineTail&&!a.lines[0].words[1].lineTail,"JSON lyrics bridge preserves lineTail marker");
   Line punctuation;punctuation.time=2200;punctuation.text=L"\x2026";
   a.lines.insert(a.lines.begin()+1,punctuation);
   expectNear(a.longNoteTailGap(a.lines[0],0,0),1600.,"lineTail marker yields 1600ms handoff gap");
  }
  // 14. P2 #11: Invalid native style values preserve existing state instead of clamping to new effects.
  {
   Fixture f;auto&a=f.a;a.lineCurve=2;a.stagger=false;a.wordMode=0;
   Json bad=Json::obj();bad["type"]="style";bad["curve"]=99;bad["stagger"]="false";bad["wordMode"]=99;
   a.receive(bad);
   require(a.lineCurve==2&&!a.stagger&&a.wordMode==0,"out-of-range and wrong-type native style fields preserve existing settings");
   bad["curve"]=1.5;bad["wordMode"]=true;
   a.receive(bad);
   require(a.lineCurve==2&&a.wordMode==0,"fractional and boolean native style fields are ignored");
   Json validStyle=Json::obj();validStyle["type"]="style";validStyle["curve"]=4;validStyle["stagger"]=true;validStyle["wordMode"]=1;
   a.receive(validStyle);
   bad["curve"]=-1;bad["wordMode"]=-1;bad["stagger"]=Json();
   a.receive(bad);
   require(a.lineCurve==4&&a.stagger&&a.wordMode==1,"negative invalid native style fields preserve valid active effects");
  }
  // 15. Dual-layer pure-white Gaussian halo bitmap cache: reuse, pure-white color, smooth falloff, and byte accounting.
  {
   Fixture f;auto&a=f.a;a.width=360;a.height=160;a.viewportBottom=132;a.textX=40;a.valid=true;a.playing=true;a.index=0;
   Line l;l.time=0;l.duration=2000;l.text=L"Halo";l.words={{L"Halo",0,2000,true,true,false}};
   a.lines={l};auto&row=a.group(0);row.y.snap(50);row.zoom.snap(1.f);row.active=true;
   Surface surface;
   auto drawHalo=[&](double time){
    surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    a.drawGroupAt(surface.g,row,0,time,a.motionNow(),true);
    check(surface.target->EndDraw());GdiFlush();
   };
   drawHalo(1000);
   require(surface.g.glowTextureBuilds==1&&surface.g.totalRowTextureBytes>0,"first halo draw builds and tracks cached Gaussian bitmap");
   const auto&gt=surface.g.rowTextures[row.content->serial].glows[0];
   require(gt.ready&&gt.bytes>0,"glow texture is marked ready with exact byte budget");
   const size_t trackedBytes=surface.g.totalRowTextureBytes;
   int whitePixels=0;bool pureWhite=true;
   for(int y=0;y<Surface::height;++y)for(int x=0;x<Surface::width;++x){
    const size_t idx=(y*Surface::width+x)*4;
    const int b=surface.pixels[idx],g=surface.pixels[idx+1],r=surface.pixels[idx+2];
    if(b||g||r){++whitePixels;if(std::abs(r-g)>1||std::abs(g-b)>1)pureWhite=false;}
   }
   require(whitePixels>50&&pureWhite,"dual-layer Gaussian halo renders pure white without dark background tint");
   // Verify smooth monotonic outward falloff from the left stroke of 'H' at vertical midline.
   const int midY=int(50.f+row.content->mainHeight*.55f),leftEdge=int(a.textX);
   const int innerVal=surface.pixels[(midY*Surface::width+leftEdge)*4+1];
   const int outerMid=surface.pixels[(midY*Surface::width+(leftEdge-3))*4+1];
   const int outerFar=surface.pixels[(midY*Surface::width+(leftEdge-6))*4+1];
   require(innerVal>outerMid&&outerMid>outerFar&&outerFar>0,"Gaussian halo falls off smoothly without discrete rings");
   drawHalo(1200);
   require(surface.g.glowTextureBuilds==1&&surface.g.totalRowTextureBytes==trackedBytes,"subsequent halo draws reuse cached Gaussian bitmap");
   surface.g.clearRowTextures();
   require(surface.g.totalRowTextureBytes==0&&surface.g.rowTextures.empty(),"clearRowTextures releases cached Gaussian halo bytes");
  }
  // 16. CJK short-tail (500ms) tiered glow + 35% fast attack + <4s tail handoff.
  {
   Fixture f;auto&a=f.a;a.valid=true;a.playing=true;
   Line l0,l1;
   l0.time=0;l0.duration=500;l0.text=L"风";l0.words={{L"风",0,500,true,true,false}};
   l1.time=2500;l1.duration=1000;l1.text=L"Next";l1.words={{L"Next",2500,1000,false,false,false}};
   a.lines={l0,l1};a.index=0;a.position=175;f.layout(0);
   const float shortPeak=a.wordGlowIntensity(a.group(0),0,0,175,a.motionNow());
   expectNear(shortPeak,longNoteStrength(500),"500ms CJK short tail reaches tiered peak by 35%");
   require(shortPeak>.35f&&shortPeak<.5f,"500ms short tail peaks gentler than >=1s full glow");
   expectNear(a.wordGlowIntensity(a.group(0),0,0,2500,a.motionNow()),shortPeak*LongNoteTailEmber,"500ms CJK tail hands off to scaled ember at next vocal");
   require(a.wordGlowIntensity(a.group(0),0,0,2750,a.motionNow())==0.f,"500ms tail handoff releases after 250ms");
  }
  // 17. Widened sweep feather on glowing words (wordMode=1) while keeping zero-flash deactivation.
  {
   Fixture f;auto&a=f.a;a.width=360;a.height=160;a.viewportBottom=132;a.textX=20;a.valid=true;a.hasClock=true;a.playing=true;a.wordMode=1;
   Line lPlain,lGlow;
   lPlain.time=0;lPlain.duration=900;lPlain.text=L"Sweeping";lPlain.words={{L"Sweeping",0,900,false,false,false}};
   lGlow.time=0;lGlow.duration=900;lGlow.text=L"Sweeping";lGlow.words={{L"Sweeping",0,900,true,true,false}};
   Surface sPlain,sGlow;
   a.lines={lPlain};a.index=0;f.layout(0);a.group(0).y.snap(40);a.group(0).zoom.snap(1.f);a.group(0).active=true;
   sPlain.target->BeginDraw();sPlain.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawGroupAt(sPlain.g,a.group(0),0,450,a.motionNow(),false);check(sPlain.target->EndDraw());GdiFlush();
   a.lines={lGlow};a.groups.clear();a.lyricLayouts.clear();a.focusIndex=-999;a.index=0;f.layout(0);a.group(0).y.snap(40);a.group(0).zoom.snap(1.f);a.group(0).active=true;
   sGlow.target->BeginDraw();sGlow.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawGroupAt(sGlow.g,a.group(0),0,450,a.motionNow(),false);check(sGlow.target->EndDraw());GdiFlush();
   const bool sweepDiff=!std::equal(sPlain.pixels,sPlain.pixels+Surface::width*Surface::height*4,sGlow.pixels);
   require(sweepDiff,"glowing word widens the sweep gradient feather transition");
   a.index=1;a.lines.push_back({L"Next",L"",900,1000,{},false});
   sGlow.target->BeginDraw();sGlow.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawGroupAt(sGlow.g,a.group(0),0,450,a.motionNow(),false);check(sGlow.target->EndDraw());GdiFlush();
   const auto beforeCut=std::vector<BYTE>(sGlow.pixels,sGlow.pixels+Surface::width*Surface::height*4);
   a.position=450;a.roles();a.group(0).mainAlpha.snap(1.f);
   sGlow.target->BeginDraw();sGlow.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));a.drawGroupAt(sGlow.g,a.group(0),0,450,a.motionNow(),false);check(sGlow.target->EndDraw());GdiFlush();
   unsigned cutDiff=0;
   for(size_t i=0;i<beforeCut.size();i+=4)cutDiff=std::max<unsigned>(cutDiff,std::abs(int(beforeCut[i+1])-int(sGlow.pixels[i+1])));
   require(cutDiff<=1,"widened-feather deactivation keeps smooth first frame without jump");
  }
  // 18. Whole-word emphasis swell on mid-line >=1s words, rendered enlargement, smooth release, and bell glow.
  {
   Fixture f;auto&a=f.a;a.width=360;a.height=160;a.viewportBottom=132;a.textX=30;a.valid=true;a.hasClock=true;a.playing=true;
   Line lPlain,l0,l1;
   lPlain.time=0;lPlain.duration=2500;lPlain.text=L"Hello";lPlain.words={{L"Hello",0,2000,false,false,false}};
   l0.time=0;l0.duration=2500;l0.text=L"Hello";l0.words={{L"Hello",0,2000,false,false,true}};
   l1.time=2500;l1.duration=1000;l1.text=L"Next";l1.words={{L"Next",2500,1000,false,false,false}};
   Surface surface;
   auto measureForeground=[&](const LyricGroup&g){
    surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    a.drawGroupAt(surface.g,g,0,1000,a.motionNow(),false);check(surface.target->EndDraw());GdiFlush();
    int minY=Surface::height,maxY=-1,minX=Surface::width,maxX=-1;
    for(int y=0;y<Surface::height;++y)for(int x=0;x<Surface::width;++x){
     const size_t idx=(y*Surface::width+x)*4;
     if(surface.pixels[idx+1]>8){minY=std::min(minY,y);maxY=std::max(maxY,y);minX=std::min(minX,x);maxX=std::max(maxX,x);}
    }
    return std::make_pair(maxX-minX,maxY-minY);
   };
   a.lines={lPlain,l1};a.index=0;a.position=1000;a.freezePlayback=true;f.layout(0);
   a.group(0).y.snap(40);a.group(0).zoom.snap(1.f);a.group(0).active=true;
   const auto plainBounds=measureForeground(a.group(0));
   a.groups.clear();a.lyricLayouts.clear();a.focusIndex=-999;
   a.lines={l0,l1};a.index=0;a.position=1000;a.freezePlayback=true;f.layout(0);
   auto&row=a.group(0);row.y.snap(40);row.zoom.snap(1.f);row.active=true;
   expectNear(a.wordEmphasisScale(row,0,0,a.motionNow()),1.f,"emphasis swell starts at 1.0x");
   const float midScale=a.wordEmphasisScale(row,0,1000,a.motionNow());
   require(midScale>1.03f&&midScale<=1.08f,"emphasized word swells cleanly at midpoint");
   expectNear(a.wordEmphasisScale(row,0,2000,a.motionNow()),1.f,"emphasis swell returns to 1.0x at word end");
   require(a.wordGlowIntensity(row,0,0,1000,a.motionNow())>.35f,"mid-line emphasized word emits bell glow without requiring longNote");
   require(a.wordGlowIntensity(row,0,0,2000,a.motionNow())==0.f,"mid-line emphasis glow returns to 0 at word end");
   const auto emphBounds=measureForeground(row);
   require(emphBounds.first>=plainBounds.first&&emphBounds.second>=plainBounds.second&&(emphBounds.first>plainBounds.first||emphBounds.second>plainBounds.second),"emphasized midpoint visibly expands foreground bounds");
   surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
   a.drawGroupAt(surface.g,row,0,1000,a.motionNow(),true);check(surface.target->EndDraw());GdiFlush();
   size_t haloPixels=0;
   for(int y=0;y<Surface::height;++y)for(int x=0;x<Surface::width;++x){
    const size_t idx=(y*Surface::width+x)*4;
    if(surface.pixels[idx+1]>0){
     require(surface.pixels[idx]==surface.pixels[idx+1]&&surface.pixels[idx+1]==surface.pixels[idx+2],"mid-line emphasis halo is pure white");
     ++haloPixels;
    }
   }
   require(haloPixels>0,"mid-line emphasized word renders pure-white halo without longNote");
   a.position=1000;a.index=1;a.roles();
   const float releaseStart=a.wordEmphasisScale(row,0,1000,a.motionNow());
   expectNear(releaseStart,midScale,"early line switch captures current emphasis scale");
   const float releaseMid=a.wordEmphasisScale(row,0,1000,a.motionNow()+100);
   require(releaseMid<releaseStart&&releaseMid>1.f,"deactivated line smoothly relaxes emphasis scale toward 1.0x");
   expectNear(a.wordEmphasisScale(row,0,1000,a.motionNow()+200),1.f,"emphasis scale returns to 1.0x after 200ms release");
   require(a.wordGlowIntensity(row,0,0,1000,a.motionNow()+100)>0.f&&a.wordGlowIntensity(row,0,0,1000,a.motionNow()+200)==0.f,
           "emphasis-only glow settles together with scale, even after a late cutoff");
  }
  std::cout<<"PASS: row texture reuse/continuity, word gradient sweep, spring velocity, tail handoff, Gaussian glow, swell and all review fixes\n";
 }
 static void effects091(){
  {
   Fixture f;auto&a=f.a;Line first,next;first.text=L"Hold";first.duration=1000;
   first.words={{L"Hold",0,1000,true,true,false}};
   next.time=1050;next.text=L"Next";next.words={{L"Next",1050,500,false}};
   a.lines={first,next};a.valid=a.playing=true;a.index=0;a.position=1049;f.layout(0);
   a.position=1050;a.update();const double now=a.motionNow();
   require(a.index==1&&a.wordGlowIntensity(a.group(1),1,0,1050,now)==0.f,
           "next line takes over on time without inheriting the previous glow");
   expectNear(a.wordGlowIntensity(a.group(0),0,0,1050,now),.9f,"previous line continues fading after the 50ms gap");
   require(a.wordGlowIntensity(a.group(0),0,0,1500,now)==0.f,"short-gap glow is gone by the normal fade deadline");
  }
  {
   Fixture f;auto&a=f.a;Line line;line.text=L"First Second";
   line.words={{L"First ",0,2000,false,false,true},{L"Second",2000,2000,false,false,true}};
   a.lines={line};const auto&row=a.group(0);Surface surface;surface.target->SetDpi(192,192);
   auto&g=surface.g;const auto*first=g.acquireGlowTexture(*row.content,0,CurrentFontSize);
   require(first&&first->ready&&first->bytes>0,"first word fits in the test cache");
   const auto used=first->bytes;g.maxRowTextureBytes=used+1;
   const auto*second=g.acquireGlowTexture(*row.content,1,CurrentFontSize);
   require(second&&second->failed&&!second->ready&&g.totalRowTextureBytes==used,
           "adding a second glow to the protected row cannot bypass the byte cap");
  }
  {
   Fixture f;auto&a=f.a;Line line,next;line.text=std::wstring(90,L'W')+L"Glow";line.duration=2000;
   line.words={{std::wstring(90,L'W'),0,200,false},{L"Glow",200,1200,true,true,false}};
   next.time=5000;next.text=L"Next";a.lines={line,next};a.playing=a.valid=true;a.index=0;a.position=3000;
   a.fps=60;a.textX=20;a.width=280;f.layout(0,0);
   auto&row=a.group(0);row.y.snap(55);row.zoom.snap(1.f);
   require(a.groupVisible(row,a.motionNow()),"long lyric row remains visible while its tail is horizontally offscreen");
   expectNear(f.interval(3000),200,"offscreen glow does not hold 60 FPS while browsing");
   Surface surface;surface.target->BeginDraw();a.drawGroupAt(surface.g,row,0,3000,a.motionNow(),true);check(surface.target->EndDraw());
   require(surface.g.glowTextureBuilds==0,"offscreen tail is culled before allocating its glow texture");
  }
  {
   Fixture f;auto&a=f.a;Line line;line.text=L"Wide";line.words={{L"Wide",0,2000,false,false,true}};
   a.lines={line};a.valid=a.hasClock=a.playing=true;a.index=0;a.textX=30;a.width=500;
   auto&row=a.group(0);row.y.snap(40);row.zoom.snap(1.f);
   Surface surface;surface.g.maxAtlasBytes=1;
   surface.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
   auto drawWord=[&](int mode){
    a.wordMode=mode;surface.target->BeginDraw();surface.target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    a.drawGroupAt(surface.g,row,0,1000,a.motionNow());check(surface.target->EndDraw());GdiFlush();
    return std::vector<BYTE>(surface.pixels,surface.pixels+Surface::width*Surface::height*4);
   };
   const auto fade=drawWord(0),sweep=drawWord(1);
   require(surface.g.rowTextures.at(row.content->serial).words.failed&&fade!=sweep,"direct fallback retains the sweep gradient");
   auto rightInk=[](const auto&pixels){
    int right=-1;
    for(int y=0;y<Surface::height;++y)for(int x=0;x<Surface::width;++x)if(pixels[(y*Surface::width+x)*4+1]>24)right=std::max(right,x);
    return right;
   };
   a.lines[0].words[0].emphasize=false;a.group(0);
   require(rightInk(fade)>rightInk(drawWord(0)),"direct fallback retains foreground swell as well as the glow placement");
  }
  {
   Fixture f;auto&a=f.a;a.currentFontSize=36.f;a.makeFormat(a.mainFormat,36.f,DWRITE_FONT_WEIGHT_SEMI_BOLD);
   Line line;line.text=L"WWWWWWWWM";line.words={{L"WWWWWWW",0,4000,false,false,true},{L"WM",4000,1000,false}};
   a.lines={line};a.index=0;auto&row=a.group(0);
   const auto&left=row.content->glyphs[0];const auto&right=row.content->glyphs[1];
   const float center=left.layout.width*.5f,scale=a.wordEmphasisScale(row,0,2000,a.motionNow());
   require(scale<1.075f&&left.x+center+(left.inkRight-center)*scale<=right.x+right.inkLeft+.01f,
           "swell leaves room for the next word's ink without moving its advance");
  }
  std::cout<<"PASS: bounded glow cache, screen culling, direct sweep and swell, and adjacent word spacing\n";
 }
 static void focusedEffects(){modernEffects();effects091();std::cout<<"PASS: "<<checks<<" focused native effect checks\n";}
 static void run(const std::filesystem::path&directory,bool includeGpu=true){
  lyricStates();browseBoundary();startupPreview();typography();motionGeometry();scheduling();pixels(directory);interludes(directory);browseInterludes(directory);seekFlights(directory);
  modernEffects();
  effects091();
  if(includeGpu)backgrounds(directory);
  pausedBrowse();viewportRows();fontSizes(directory);cleanup(directory);preludeRows(directory);
  if(includeGpu)movingFrames(directory);
  wordScrolling(directory);
  if(includeGpu){resizeRendering(directory);optimizations(directory);}
  regressions(directory,includeGpu);
  std::cout<<"PASS: "<<checks<<" native checks ("<<(includeGpu?"all":"ci")<<" suite)\n";
 }
};
int NativeTest::checks=0;
int main(int argc,char**argv){try{auto directory=argc>1?std::filesystem::path(argv[1]):std::filesystem::path();if(argc>2&&std::strcmp(argv[2],"effects")==0)NativeTest::focusedEffects();else if(argc>2&&std::strcmp(argv[2],"paused-browse")==0)NativeTest::pausedBrowse();else if(argc>2&&std::strcmp(argv[2],"viewport")==0)NativeTest::viewportRows();else if(argc>2&&std::strcmp(argv[2],"lyric-state")==0)NativeTest::lyricStates();else if(argc>2&&std::strcmp(argv[2],"startup")==0)NativeTest::startupPreview();else if(argc>2&&std::strcmp(argv[2],"browse")==0)NativeTest::browseBoundary();else if(argc>2&&std::strcmp(argv[2],"font")==0)NativeTest::fontSizes(directory);else if(argc>2&&std::strcmp(argv[2],"flight")==0)NativeTest::seekFlights(directory);else if(argc>2&&std::strcmp(argv[2],"cleanup")==0)NativeTest::cleanup(directory);else if(argc>2&&std::strcmp(argv[2],"prelude")==0){NativeTest::startupPreview();NativeTest::preludeRows(directory);NativeTest::interludes(directory);}else if(argc>2&&std::strcmp(argv[2],"moving")==0)NativeTest::movingFrames(directory);else if(argc>2&&std::strcmp(argv[2],"words")==0)NativeTest::wordScrolling(directory);else if(argc>2&&std::strcmp(argv[2],"resize")==0)NativeTest::resizeRendering(directory);else if(argc>2&&std::strcmp(argv[2],"background")==0){NativeTest::backgrounds(directory);std::cout<<"PASS: "<<NativeTest::checks<<" focused background checks\n";}else if(argc>2&&std::strcmp(argv[2],"optimization")==0){NativeTest::optimizations(directory);NativeTest::seekFlights(directory);std::cout<<"PASS: "<<NativeTest::checks<<" focused optimization checks\n";}else if(argc>2&&std::strcmp(argv[2],"regressions")==0)NativeTest::regressions(directory,true);else if(argc>2&&std::strcmp(argv[2],"ci")==0)NativeTest::run(directory,false);else NativeTest::run(directory,true);return 0;}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
