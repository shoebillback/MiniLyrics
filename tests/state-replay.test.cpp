// Replay bridge output and exercise the production cover worker using loopback HTTP.
#include "../native/Host.cpp"
#include <iostream>
static void require(bool value,const char*message){if(!value)throw std::runtime_error(message);}
struct NativeTest {
 static void run(const Json&root,const std::wstring&base,const std::filesystem::path&dir){
  App a;a.background.mode=0;a.coverSince=-1000;a.freezePlayback=true;a.testWall=true;a.testWallMs=1000;
  check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(a.textFactory.put())));
  a.makeFormat(a.mainFormat,a.currentFontSize,DWRITE_FONT_WEIGHT_SEMI_BOLD);a.makeFormat(a.secondaryFormat,a.translationFontSize,DWRITE_FONT_WEIGHT_NORMAL);
  a.mainRowHeight=a.layout(L"Ag",a.mainFormat.get()).height;a.updatePlaceholders();
  a.hwnd=CreateWindowExW(0,L"STATIC",L"Isolated state replay",WS_POPUP,0,0,600,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  require(a.hwnd!=nullptr,"create hidden replay window");
  a.renderVisible=true;a.width=600;a.height=240;a.viewportBottom=212;a.songId="local-a";a.duration=90000;
  for(int i=0;i<9;i++){Line l;l.time=i*10000;l.duration=10000;l.text=L"Line "+std::to_wstring(i);a.lines.push_back(l);}a.updatePlaceholders();
  a.position=1000;a.valid=true;a.playing=true;a.index=0;a.reflow(0,false);
  require(root["messages"].array.size()>0&&root["messages"].array[0]["type"].s()=="clock","actual timeline alone drives animations");
  double travel=0;for(const auto&m:root["messages"].array){a.testWallMs+=100;a.update();a.receive(m);for(const auto&r:a.flight.rows)travel=std::max(travel,double(std::abs(r.to.y-r.from.y)));}
  require(travel>1&&a.index==2,"timeline-only alternating seeks retain an actual moving animation");
  std::cout<<"PASS progress-first replay: target line="<<a.index<<", animation travel="<<travel<<" DIP\n";
  a.playing=false;a.update();Json pause=Json::obj();pause["type"]="clock";pause["songId"]="local-a";pause["valid"]=true;pause["playing"]=false;pause["position"]=50000;a.receive(pause);
  require(!a.flight.rows.empty()&&a.index==5,"paused timeline jump animates");
  a.testWallMs+=2000;a.update();a.valid=false;a.flight=LyricFlight();pause["position"]=10000;a.receive(pause);
  require(a.flight.rows.empty(),"first sync after invalid clock does not animate across the song");
  if(base.empty()){a.stopFrames();DestroyWindow(a.hwnd);a.hwnd=nullptr;std::cout<<"PASS paused jump and first-sync handling\n";return;}
  a.startCoverWorker();
  auto receiveUntil=[&](uint64_t epoch,int timeout){auto start=nowMs();while(nowMs()-start<timeout){MSG msg{};while(PeekMessageW(&msg,a.hwnd,App::CoverReady,App::CoverReady,PM_REMOVE)){a.onCover(std::unique_ptr<CoverData>(reinterpret_cast<CoverData*>(msg.lParam)));if(a.cover.epoch==epoch)return true;}std::this_thread::sleep_for(std::chrono::milliseconds(10));}return false;};
  a.loadCover(base+L"/retry.bmp");require(receiveUntil(a.coverEpoch,5000)&&!a.cover.pixels.empty(),"same URL recovers after initial HTTP failure");
  std::cout<<"PASS cover: initial HTTP failure recovers automatically\n";
  a.loadCover(base+L"/superseded.bmp");auto start=nowMs();while(!std::filesystem::exists(dir/"superseded-requested")&&nowMs()-start<3000)std::this_thread::sleep_for(std::chrono::milliseconds(10));
  require(std::filesystem::exists(dir/"superseded-requested"),"old cover reached HTTP server");
  a.loadCover(base+L"/new.bmp");require(receiveUntil(a.coverEpoch,2500)&&!a.cover.pixels.empty(),"new cover interrupts old retry backoff");
  std::cout<<"PASS cover: new URL cancels stale retry\n";
  a.loadCover(base+L"/always-fail.bmp");require(receiveUntil(a.coverEpoch,7500)&&a.cover.pixels.empty(),"persistent failure terminates bounded retries");
  std::cout<<"PASS cover: permanent failure stops retrying\n";
  {std::lock_guard<std::mutex> lock(a.coverQueue->mutex);a.coverQueue->stop=true;}a.coverQueue->changed.notify_one();
  a.stopFrames();DestroyWindow(a.hwnd);a.hwnd=nullptr;
 }
};
int main(int argc,char**argv){CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);try{require(argc==2||argc==4,"expected replay and optional cover fixtures");std::ifstream f(argv[1]);std::string s((std::istreambuf_iterator<char>(f)),{});NativeTest::run(JsonParser(s).parse(),argc==4?widen(argv[2]):L"",argc==4?std::filesystem::path(argv[3]):std::filesystem::path());CoUninitialize();return 0;}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
