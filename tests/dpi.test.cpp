// Mixed-DPI startup, round trips and saved-position restoration through App::run.
// Intercept ShowWindow so all test windows remain hidden; no player connection.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include "../native/bridge.hpp"
struct NativeTest {static BOOL show(HWND,int);static int run(const char*);};
#undef NOMINMAX
#define ShowWindow NativeTest::show
#include "../native/Host.cpp"
#undef ShowWindow
#include <iostream>

struct TestMonitor {RECT rect;UINT dpi;};
static std::vector<TestMonitor>monitors;
static RECT expectedPosition{};
static int scenario=0;static size_t origin=0,other=1;
static std::vector<std::string>results;
static void require(bool value,const char*message){if(!value)throw std::runtime_error(message);}
static BOOL CALLBACK collect(HMONITOR h,HDC,LPRECT,LPARAM){
 MONITORINFO info{};info.cbSize=sizeof(info);GetMonitorInfoW(h,&info);
 HWND window=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"DPI fixture",WS_POPUP,info.rcMonitor.left+40,info.rcMonitor.top+40,100,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 monitors.push_back({info.rcMonitor,GetDpiForWindow(window)});DestroyWindow(window);return TRUE;
}
BOOL NativeTest::show(HWND window,int){
 auto&a=*reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
 for(int i=0;i<40;i++){Line line;line.time=i*2000;line.duration=2000;line.text=L"Current lyric "+std::to_wstring(i);line.translation=L"Translation";a.lines.push_back(std::move(line));}
 a.position=21000;a.valid=true;a.playing=false;a.freezePlayback=true;a.layoutDirty=true;
 a.cover.width=a.cover.height=2;a.cover.pixels=std::vector<BYTE>(16,255);
 auto verify=[&](const char*stage){
  a.renderVisible=true;a.update();a.paint();UINT actual=GetDpiForWindow(window);RECT client{};GetClientRect(window,&client);
  require(actual==UINT(a.dpi),"startup/layout DPI must match the actual monitor");
  require(std::abs(a.width-460)<1&&std::abs(a.height-130)<1,"mixed-DPI movement must preserve logical window dimensions");
  require(std::abs(client.right-460*a.scale())<=1&&std::abs(client.bottom-130*a.scale())<=1,"physical window dimensions must use the current monitor DPI");
  FLOAT x=0,y=0;a.target->GetDpi(&x,&y);auto pixels=a.target->GetPixelSize();
  require(x==actual&&y==actual&&pixels.width==UINT(client.right)&&pixels.height==UINT(client.bottom),"render-target DPI and pixel dimensions update together");
  auto&line=a.group(a.index);FLOAT font=0;check(line.main.value->GetFontSize(0,&font));
  require(font==21&&std::abs(line.zoom.value(a.motionNow())-1)<.001,"focused lyric font keeps its DIP size");
  float top=line.y.value(a.motionNow()),bottom=top+line.renderedHeight(1);
  require(top>=a.viewportTop-.1f&&bottom<=a.viewportBottom+.1f,"focused original/translation fit inside the lyric viewport");
  require(a.artBounds.top>=0&&a.artBounds.bottom<=a.height&&a.transport.right<=a.width,"cover and transport remain within the client area");
  require(a.error.empty(),"DPI change must not leave a drawing error");
  std::string result="{\"stage\":\""+std::string(stage)+"\",\"dpi\":"+std::to_string(actual)+",\"widthPx\":"+std::to_string(client.right)+",\"heightPx\":"+std::to_string(client.bottom)+"}";results.push_back(result);std::cout<<result<<"\n";
 };
 verify("created");
 if(scenario<2){
  for(size_t at:{other,origin}){const auto&r=monitors[at].rect;SetWindowPos(window,nullptr,r.left+40,r.top+40,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);verify("moved");}
 }else{RECT actual{};GetWindowRect(window,&actual);require(EqualRect(&actual,&expectedPosition),"reopening restores the same physical position and size");}
 GetWindowRect(window,&expectedPosition);a.save();App restored;
 require(restored.positionInPixels&&restored.savedX==expectedPosition.left&&restored.savedY==expectedPosition.top,"screen coordinates persist as physical pixels, including negative positions");
 require(std::abs(restored.savedWidth-460)<1&&std::abs(restored.savedHeight-130)<1,"saved window dimensions remain in DIP");
 a.lines.clear();a.groups.clear();a.focusIndex=-999;a.layoutDirty=true;a.valid=false;a.renderVisible=false;
 return ::ShowWindow(window,SW_HIDE);
}
int NativeTest::run(const char*dir){
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);HRESULT init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
 EnumDisplayMonitors(nullptr,nullptr,collect,0);auto different=std::find_if(monitors.begin(),monitors.end(),[](const TestMonitor&m){return m.dpi!=monitors.front().dpi;});
 if(monitors.empty()||different==monitors.end()){std::cout<<"SKIP: two monitors with different DPI are required\n";if(SUCCEEDED(init))CoUninitialize();return 0;}
 other=size_t(different-monitors.begin());size_t first=0,second=other;
 for(scenario=0;scenario<3;scenario++){
  App a;a.background.mode=0;a.savedWidth=460;a.savedHeight=130;
  if(scenario<2){origin=scenario==0?first:second;other=scenario==0?second:first;const auto&r=monitors[origin].rect;a.positionInPixels=false;a.savedX=(r.left+40)/(GetDpiForSystem()/96.);a.savedY=(r.top+40)/(GetDpiForSystem()/96.);}
  else require(a.positionInPixels,"restoration uses the new physical-position format");
  require(a.run(GetModuleHandleW(nullptr),0,"",(std::filesystem::path(dir)/("dpi-"+std::to_string(scenario)+".png")).wstring(),true)==0,"production host exits cleanly");
 }
 std::ofstream report(std::filesystem::path(dir)/"dpi-results.json");report<<"{\"pass\":true,\"scenarios\":3,\"samples\":[";for(size_t i=0;i<results.size();i++){if(i)report<<',';report<<results[i];}report<<"]}";
 std::cout<<"PASS: mixed-DPI startup, bidirectional round trips, layout and position restoration\n";
 if(SUCCEEDED(init))CoUninitialize();return 0;
}
int main(int argc,char**argv){try{require(argc>1,"results directory required");return NativeTest::run(argv[1]);}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
