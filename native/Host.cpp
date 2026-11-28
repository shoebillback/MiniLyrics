// MiniLyrics native Win32 / Direct2D / DirectWrite host.
// Copyright (C) 2026 shoebillback. GPL-3.0-or-later.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include "bridge.hpp"
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <winhttp.h>
#include <shlobj.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <chrono>
#include <condition_variable>
#include "cover_geometry.hpp"
#include "transport_geometry.hpp"
#include "lyric_motion.hpp"
#include "line_motion.hpp"
#include "window_mode.hpp"
#include "background.hpp"
#include "interlude.hpp"
#include "modal_frames.hpp"
#include "owner_process.hpp"

template<class T> class Com {
 T* p=nullptr;
 public:Com()=default;~Com(){reset();}Com(const Com&)=delete;Com&operator=(const Com&)=delete;
 Com(Com&&o)noexcept:p(o.p){o.p=nullptr;}Com&operator=(Com&&o)noexcept{if(this!=&o){reset();p=o.p;o.p=nullptr;}return *this;}
 T* get()const{return p;}T*operator->()const{return p;}explicit operator bool()const{return p!=nullptr;}
 T**put(){reset();return &p;}void reset(){if(p){p->Release();p=nullptr;}}
};
static constexpr double MinWindowWidth=360,MaxWindowWidth=1200,MinWindowHeight=130,MaxWindowHeight=600;
static constexpr float CurrentFontSize=21,ContextFontSize=16,TranslationFontSize=16,GroupGap=7,TranslationGap=6.3f;
static double nowMs(){static auto origin=std::chrono::steady_clock::now();return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-origin).count();}
static double clamp(double x,double lo,double hi){return std::max(lo,std::min(hi,std::isfinite(x)?x:lo));}
static std::wstring widen(const std::string&s){if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);if(n<=0)return {};std::wstring out(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),&out[0],n);return out;}
static std::string narrow(const std::wstring&s){if(s.empty())return {};int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);std::string out(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),&out[0],n,nullptr,nullptr);return out;}
static std::wstring env(const wchar_t*name){DWORD n=GetEnvironmentVariableW(name,nullptr,0);if(!n)return {};std::wstring out(n,0);GetEnvironmentVariableW(name,&out[0],n);out.resize(n-1);return out;}
static void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Windows graphics initialization failed ("+std::to_string(uint32_t(hr))+")");}
struct Word {std::wstring text;double time=0,duration=1;bool operator==(const Word&o)const{return text==o.text&&time==o.time&&duration==o.duration;}};
struct Line {std::wstring text,translation;double time=0,duration=0;std::vector<Word> words;bool isInterlude=false;bool operator==(const Line&o)const{return text==o.text&&translation==o.translation&&time==o.time&&duration==o.duration&&words==o.words&&isInterlude==o.isInterlude;}};
struct Layout {Com<IDWriteTextLayout> value;float width=0,height=0;};
struct Glyph {Layout layout;double time=0,duration=1;float x=0;};
struct LyricLayout {
 Line model;Layout main,subtitle;std::vector<Glyph> glyphs;float mainHeight=0;
 float height(bool translated)const{return mainHeight+(translated?TranslationGap+subtitle.height:0);}
};
struct LyricGroup {
 std::shared_ptr<LyricLayout> content;
 std::vector<FloatWord> resetWords;InterludeAnimation dots;
 LineTrack y,zoom,mainAlpha,subAlpha;float opacity=1;double resetAt=-1000;uint64_t identity=0;bool translated=false,placed=false,active=false,middleInterlude=false;
 float subtitleOffset(float scale)const{return (content->mainHeight+TranslationGap)*scale;}
 float renderedHeight(float scale)const{return content->height(translated)*scale;}
};
// Whole-row visibility is independent of plain-text emphasis and word progress.
struct FlightPoint {float y=0,zoom=1,mainAlpha=.4f,subAlpha=.4f,opacity=1;};
struct FlightRow {FlightPoint from,to,point;float height=0;int bucket=0;};
struct LyricFlight {
 std::vector<FlightRow> rows;double started=0;int focus=0,playbackIndex=-1,curve=0;bool stagger=true,prelude=false,firstFrame=true;
 bool running(double now)const{return !rows.empty()&&now<started+500+(stagger?400:0);}
 struct Phase {double p[9]{},alpha=0;};
 Phase phase(double now)const{Phase out;double elapsed=firstFrame?0:now-started;for(int i=0;i<9;i++)out.p[i]=lineEase(unit((elapsed-(stagger?i*50:0))/500),curve);out.alpha=lyricEase(unit(elapsed/500));return out;}
 FlightPoint sample(const FlightRow&r,const Phase&p)const{return {mixf(r.from.y,r.to.y,p.p[r.bucket]),mixf(r.from.zoom,r.to.zoom,p.p[r.bucket]),mixf(r.from.mainAlpha,r.to.mainAlpha,p.alpha),mixf(r.from.subAlpha,r.to.subAlpha,p.alpha),mixf(r.from.opacity,r.to.opacity,p.alpha)};}
};
struct CoverData {uint64_t epoch=0;UINT width=0,height=0;std::vector<BYTE> pixels;};
struct CoverQueue {
 std::mutex mutex;std::condition_variable changed;bool stop=false,pending=false;std::wstring url;uint64_t epoch=0;HINTERNET activeRequest=nullptr;
 void cancelActiveRequest(){HINTERNET req=nullptr;{std::lock_guard<std::mutex> lock(mutex);req=activeRequest;activeRequest=nullptr;}if(req)WinHttpCloseHandle(req);}
};

struct CoverDownloader {
 HINTERNET session=nullptr;Com<IWICImagingFactory> factory;bool comInit=false;
 CoverDownloader(){
  HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);comInit=SUCCEEDED(hr);
  session=WinHttpOpen(L"MiniLyrics/0.8",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
  if(session)WinHttpSetTimeouts(session,3000,3000,8000,8000);
 }
 ~CoverDownloader(){factory.reset();if(session)WinHttpCloseHandle(session);if(comInit)CoUninitialize();}
 std::unique_ptr<CoverData> fetch(const std::wstring&url,uint64_t epoch,CoverQueue*queue=nullptr){
  auto out=std::make_unique<CoverData>();out->epoch=epoch;if(!session)return out;
  URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=DWORD(-1);
  if(!WinHttpCrackUrl(url.c_str(),0,0,&parts)||(parts.nScheme!=INTERNET_SCHEME_HTTP&&parts.nScheme!=INTERNET_SCHEME_HTTPS))return out;
  std::wstring host(parts.lpszHostName,parts.dwHostNameLength),path(parts.lpszUrlPath,parts.dwUrlPathLength);
  if(parts.dwExtraInfoLength)path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);if(path.empty())path=L"/";
  HINTERNET connection=WinHttpConnect(session,host.c_str(),parts.nPort,0),request=nullptr;std::vector<BYTE> bytes;
  if(connection)request=WinHttpOpenRequest(connection,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);
  if(request&&queue){std::lock_guard<std::mutex> lock(queue->mutex);if(queue->stop||queue->epoch!=epoch){WinHttpCloseHandle(request);request=nullptr;}else queue->activeRequest=request;}
  if(request&&WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(request,nullptr)){
   DWORD status=0,size=sizeof(status);WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr);
   if(status==200){BYTE buf[8192];DWORD got=0;while(WinHttpReadData(request,buf,sizeof(buf),&got)&&got){if(bytes.size()+got>8*1024*1024){bytes.clear();break;}bytes.insert(bytes.end(),buf,buf+got);}}
  }
  if(request){if(queue){queue->cancelActiveRequest();}else WinHttpCloseHandle(request);}
  if(connection)WinHttpCloseHandle(connection);if(bytes.empty())return out;
  try{
   if(!factory)check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.put())));
   Com<IWICStream> stream;check(factory->CreateStream(stream.put()));check(stream->InitializeFromMemory(bytes.data(),DWORD(bytes.size())));
   Com<IWICBitmapDecoder> decoder;check(factory->CreateDecoderFromStream(stream.get(),nullptr,WICDecodeMetadataCacheOnLoad,decoder.put()));
   Com<IWICBitmapFrameDecode> frame;check(decoder->GetFrame(0,frame.put()));UINT w=0,h=0;check(frame->GetSize(&w,&h));if(!w||!h||w>8192||h>8192||uint64_t(w)*h>32*1024*1024)throw std::runtime_error("cover dimensions");
   // Keep enough detail for the 160-DIP cover at up to 300% display scaling.
   // The animated background independently downsamples this artwork to 192px.
   double scale=std::min(1.,512./std::max(w,h));out->width=std::max(1u,UINT(w*scale));out->height=std::max(1u,UINT(h*scale));
   Com<IWICBitmapScaler> scaler;check(factory->CreateBitmapScaler(scaler.put()));check(scaler->Initialize(frame.get(),out->width,out->height,WICBitmapInterpolationModeFant));
   Com<IWICFormatConverter> converter;check(factory->CreateFormatConverter(converter.put()));check(converter->Initialize(scaler.get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
   out->pixels.resize(size_t(out->width)*out->height*4);check(converter->CopyPixels(nullptr,out->width*4,UINT(out->pixels.size()),out->pixels.data()));
  }catch(...){out->width=out->height=0;out->pixels.clear();}
  return out;
 }
};

inline std::unique_ptr<CoverData> downloadCover(const std::wstring&url,uint64_t epoch){CoverDownloader downloader;return downloader.fetch(url,epoch);}

struct Graphics {
 ID2D1RenderTarget*target=nullptr;Com<ID2D1SolidColorBrush> brush;Com<ID2D1LinearGradientBrush> gradient;Com<ID2D1Bitmap> cover,background;Com<ID2D1BitmapBrush> coverBrush,backgroundBrush;uint64_t backgroundRevision=0;bool coverOpaque=true;
 void clear(){backgroundBrush.reset();background.reset();backgroundRevision=0;coverBrush.reset();cover.reset();coverOpaque=true;gradient.reset();brush.reset();target=nullptr;}
 void resize(float w,float h){if(gradient)gradient->SetEndPoint(D2D1::Point2F(w,h*.5f));}
 void updateGradient(D2D1_COLOR_F a,D2D1_COLOR_F b,float w,float h){
  D2D1_GRADIENT_STOP stops[]={{0,a},{1,b}};Com<ID2D1GradientStopCollection> collection;Com<ID2D1LinearGradientBrush> next;
  check(target->CreateGradientStopCollection(stops,2,collection.put()));check(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(w,h*.5f)),collection.get(),next.put()));gradient=std::move(next);
 }
 void updateCover(const CoverData&image){
  if(image.pixels.empty()){coverBrush.reset();cover.reset();coverOpaque=true;return;}
  bool opaque=true;for(size_t i=3;i<image.pixels.size();i+=4)if(image.pixels[i]<255){opaque=false;break;}
  coverOpaque=opaque;
  if(cover&&cover->GetPixelSize().width==image.width&&cover->GetPixelSize().height==image.height){
   check(cover->CopyFromMemory(nullptr,image.pixels.data(),image.width*4));
   if(!coverBrush)check(target->CreateBitmapBrush(cover.get(),D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),coverBrush.put()));
   return;
  }
  Com<ID2D1Bitmap> next;check(target->CreateBitmap(D2D1::SizeU(image.width,image.height),image.pixels.data(),image.width*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),next.put()));
  Com<ID2D1BitmapBrush> nextBrush;check(target->CreateBitmapBrush(next.get(),D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),nextBrush.put()));
  cover=std::move(next);coverBrush=std::move(nextBrush);
 }
 void init(ID2D1RenderTarget*t,D2D1_COLOR_F a,D2D1_COLOR_F b,const CoverData&image,float w,float h){
  clear();target=t;check(t->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),brush.put()));
  updateGradient(a,b,w,h);updateCover(image);
 }
};

// Per-pixel-alpha layered presentation. A window region is a 1-bit mask, so
// clipping a rounded card with SetWindowRgn staircases the corner and border.
// Direct2D renders into a DIBSection instead and UpdateLayeredWindow composites
// the premultiplied result, keeping every edge antialiased.
class LayeredSurface {
 Com<ID2D1DCRenderTarget> render;
 HDC memory=nullptr,screen=nullptr;
 HBITMAP bitmap=nullptr;
 HGDIOBJ original=nullptr;
 UINT pixelWidth=0,pixelHeight=0;
 void release(){
  render.reset();
  if(memory){
   if(bitmap){if(original)SelectObject(memory,original);DeleteObject(bitmap);bitmap=nullptr;}
   DeleteDC(memory);memory=nullptr;
  }
  if(screen){ReleaseDC(nullptr,screen);screen=nullptr;}
  original=nullptr;pixelWidth=pixelHeight=0;
 }
 public:
 LayeredSurface()=default;
 LayeredSurface(const LayeredSurface&)=delete;LayeredSurface&operator=(const LayeredSurface&)=delete;
 ~LayeredSurface(){release();}
 ID2D1DCRenderTarget*get()const{return render.get();}
 ID2D1DCRenderTarget*operator->()const{return render.get();}
 explicit operator bool()const{return bool(render);}
 D2D1_SIZE_U GetPixelSize()const{return D2D1::SizeU(pixelWidth,pixelHeight);}
 void GetDpi(FLOAT*x,FLOAT*y)const{render->GetDpi(x,y);}
 void SetDpi(float value)const{if(render)render->SetDpi(value,value);}
 void SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE mode)const{if(render)render->SetTextAntialiasMode(mode);}
 void BeginDraw()const{render->BeginDraw();}
 HRESULT EndDraw()const{return render->EndDraw();}
 void reset(){release();}
 bool create(ID2D1Factory*factory,float dpi,bool&hardware){
  for(int pass=0;pass<2;pass++){
   D2D1_RENDER_TARGET_PROPERTIES props=D2D1::RenderTargetProperties(pass?D2D1_RENDER_TARGET_TYPE_SOFTWARE:D2D1_RENDER_TARGET_TYPE_HARDWARE,
    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),dpi,dpi);
   Com<ID2D1DCRenderTarget> next;
   if(SUCCEEDED(factory->CreateDCRenderTarget(&props,next.put()))){render=std::move(next);hardware=pass==0;return true;}
  }
  return false;
 }
 bool bind(UINT w,UINT h){
  if(!render)return false;
  if(!memory){memory=CreateCompatibleDC(nullptr);if(!memory)return false;}
  if(bitmap&&pixelWidth==w&&pixelHeight==h)return true;
  BITMAPINFO info{};
  info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=LONG(w);info.bmiHeader.biHeight=-LONG(h);
  info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
  void*bits=nullptr;HBITMAP next=CreateDIBSection(memory,&info,DIB_RGB_COLORS,&bits,nullptr,0);if(!next)return false;
  HGDIOBJ previous=SelectObject(memory,next);
  if(!original)original=previous;
  if(bitmap&&previous==bitmap)DeleteObject(bitmap);
  bitmap=next;pixelWidth=w;pixelHeight=h;
  RECT area{0,0,LONG(w),LONG(h)};
  return SUCCEEDED(render->BindDC(memory,&area));
 }
 bool present(HWND hwnd,BYTE alpha=255){
  if(!render||!memory||!bitmap)return false;
  RECT window{};if(!GetWindowRect(hwnd,&window))return false;
  if(!screen){screen=GetDC(nullptr);if(!screen)return false;}
  POINT destination{window.left,window.top},source{0,0};
  SIZE size{LONG(pixelWidth),LONG(pixelHeight)};
  BLENDFUNCTION blend{AC_SRC_OVER,0,alpha,AC_SRC_ALPHA};
  return UpdateLayeredWindow(hwnd,screen,&destination,&size,memory,&source,0,&blend,ULW_ALPHA)!=FALSE;
 }
};

class App {
 friend struct NativeTest;
 float currentFontSize=::CurrentFontSize,contextFontSize=::ContextFontSize,translationFontSize=::TranslationFontSize;
 static constexpr UINT Incoming=WM_APP+1,CoverReady=WM_APP+2,FrameReady=WM_APP+3,BridgeDisconnected=WM_APP+4;
 HWND hwnd=nullptr;HANDLE frameTimer=nullptr;bool frameArmed=false;double frameDeadline=0,framePeriod=0;std::unique_ptr<Bridge> bridge;
 ModalFrames modalFrames;OwnerProcess ownerProcess;bool systemMoving=false,connectionStale=false;
 std::shared_ptr<CoverQueue> coverQueue;std::thread coverWorker;
 Com<ID2D1Factory> factory;Com<IDWriteFactory> textFactory;Com<IWICImagingFactory> imaging;
 std::unique_ptr<LayeredSurface> target;Graphics graphics;Com<ID2D1RoundedRectangleGeometry> coverClip;
 Com<IDWriteTextFormat> mainFormat,secondaryFormat,metaFormat;
 Layout metaLayout,placeholder;int preludePreview=-1;std::map<int,LyricGroup>groups;InterludeAnimation preludeDots;LyricFlight flight;float secondaryRowHeight=0;
 std::vector<std::shared_ptr<LyricLayout>> lyricLayouts;
 size_t nextLayoutToWarm=0;
 static constexpr UINT_PTR LayoutWarmupTimer=5;
 static constexpr UINT_PTR PlaylistRefreshTimer=6,VinylConfirmTimer=7,PlaylistWatchTimer=1,PlaylistAnimTimer=8,SecondaryAnimTimer=9,LongPressTimer=10;
 static constexpr double OpenAnimMs=170;
 static constexpr float PlaylistRowHeight=44,PlaylistHeaderHeight=46;
 struct PlaylistItem {std::wstring title,artist;std::string resourceId;double duration=0;int index=0;bool current=false;};
 std::vector<PlaylistItem> playlist;int playlistCurrent=-1;
 Com<IDWriteTextFormat> playlistHeaderFormat,playlistTitleFormat,playlistMetaFormat;
 std::unique_ptr<LayeredSurface> playlistTarget;Graphics playlistGraphics;HWND playlistHwnd=nullptr;
 float playlistWidth=360,playlistHeight=180;int playlistScroll=0,playlistHover=-1;
 bool playlistOpen=false,playlistLoading=false,playlistUnsupported=false,playlistDrag=false,playlistMouseDown=false,playlistDragMoved=false,playlistTracking=false,playlistLocateHover=false,playlistLocatePress=false;
 LineTrack playlistScrollTrack;
 double playlistOpenAt=0,panelOpenAt=0;
 bool playlistOpening=false,panelOpening=false;
 POINT playlistDragStart{};int playlistDragScroll=0;double playlistRevision=0;
 LyricClock indicatorMotion;double lastIndicatorTime=0;LineTrack browseDotsFade{1.f,1.f};
 LyricClock motion;bool transitionActive=false,snapRow=false,testWall=false,freezePlayback=false,layoutDirty=true,stagger=true;double testWallMs=0;int focusIndex=-999,lineCurve=0,browseIndex=-1,wheelRemainder=0;double browseUntil=0;bool returnFromBrowse=false,manualTransition=false,manualLayoutPending=false;float viewportTop=32,viewportBottom=98,layoutAnchor=44,mainRowHeight=28;uint64_t groupSerial=0,layoutChanges=0;
 Com<ID2D1PathGeometry> playGeometry,likeGeometry;
 std::vector<Line> lines;CoverData cover;BackgroundSettings background;BackgroundClock backgroundClock;FluidBackground fluid;
 D2D1_COLOR_F bgA=D2D1::ColorF(33/255.f,42/255.f,66/255.f),bgB=D2D1::ColorF(76/255.f,67/255.f,99/255.f);
 D2D1_RECT_F artBounds{};float artRadius=10;TransportGeometry transport{};
 // Three fixed slots: 0 = left column, 1 = right top, 2 = right bottom.
 // slotModule[slot] holds the module assigned to that slot.
 static constexpr int ModuleTitle=0,ModuleCover=1,ModuleLyrics=2;
 static constexpr float SlotMargin=12,SlotBottom=32,SlotGap=14,SlotGapY=8;
 static constexpr float DragThreshold=6.f;static constexpr UINT LongPressMs=380;
 int slotModule[3]={ModuleCover,ModuleTitle,ModuleLyrics};
 float leftRatio=.28f,topHeight=20.f,titleFontSize=12.f;
 bool miniMode=false,slotLayout=false,layoutPanel=false;
 int splitterDrag=0,splitterHover=0;
 D2D1_RECT_F slotRects[3]{};
 // Long-press module drag: a short press is replayed on release, a long press
 // lifts the module and swaps it with the slot under the cursor on drop.
 struct SlotSwap {int a=-1,b=-1;D2D1_RECT_F fromA{},toA{},fromB{},toB{};LineTrack p;};
 // The drop highlight glides between slots (PowerPoint-morph style) instead of
 // jumping when the pointer enters a different slot.
 struct DragHighlight {D2D1_RECT_F from{},to{};LineTrack p;bool visible=false;};
 bool modulePress=false,moduleDragging=false;int pressSlot=-1,pressAction=0,pressIcon=-1;double pressSeek=-1;float pressX=0,pressY=0,dragX=0,dragY=0;int dragTargetSlot=-1;SlotSwap slotSwap;DragHighlight dragHighlight;int dragHighlightSlot=-1;
 float lyricsRight=348,lyricsCenterY=65;
 Layout metaVertical,metaWide;
 // Per-module alignment inside its slot (0 left, 1 center, 2 right), the
 // title's flow direction (0 auto, 1 horizontal, 2 vertical) and the active
 // settings-panel tab (0 layout, 1 alignment).
 int alignCover=1,alignTitle=0,alignLyrics=0,titleOrientation=0,panelTab=0;
 bool seekPending=false;uint64_t seekSerial=0,seekRequest=0;double seekStarted=0,seekFallback=0;
 std::wstring settingsPath,capturePath,title=L"MiniLyrics",artist,coverUrl;std::string songId,error;
 double position=0,anchor=0,duration=0,offset=0,received=0,revision=0,rowSince=-1000,coverSince=-1000;
 double savedX=80,savedY=80,savedWidth=360,savedHeight=130;float dpi=96,width=360,height=130,artSize=96,textX=124;
 bool positionInPixels=false;
 SIZE geometryPixels{};float geometryDpi=0;
 RECT normalWindowRect{};float normalWindowScale=1.f;bool hasNormalRect=false;
 bool playing=false,valid=false,hasClock=false,untimed=false,lyricsReceived=false,through=false,top=true,translation=true,hover=false,dragProgress=false,resizing=false,closing=false,hardware=false,frameRunning=false;
 bool demo=false,renderVisible=false;int fps=60,index=-999;UINT intervalMs=200;uint64_t renderCalls=0,lineBuildCalls=0,coverEpoch=0;double dragRatio=0;
 bool vinylRequested=false,vinylHidden=false,liked=false;
 POINT resizeStart{};SIZE resizeSize{};
 double current()const{return position+(playing&&valid&&!freezePlayback?std::max(0.,nowMs()-anchor):0.);}
 double animationWall()const{return testWall?testWallMs:nowMs();}
 double motionNow()const{return motion.now(animationWall());}
 double indicatorNow()const{return indicatorMotion.now(animationWall());}
 void stopBrowsing(bool animateReturn=false){browseIndex=-1;wheelRemainder=0;browseUntil=0;returnFromBrowse=true;manualTransition=manualLayoutPending=animateReturn;KillTimer(hwnd,4);layoutDirty=true;}
 void finishTransition(){for(auto&item:groups){auto&g=item.second;g.y.snap(g.y.to);g.zoom.snap(g.zoom.to);g.mainAlpha.snap(g.mainAlpha.to);g.subAlpha.snap(g.subAlpha.to);}transitionActive=false;manualTransition=false;manualLayoutPending=false;}
 // Slot index under a DIP point, or -1 when the point falls between slots.
 int moduleAt(float x,float y)const{for(int i=0;i<3;i++){const auto&r=slotRects[i];if(x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom)return i;}return -1;}
 static const wchar_t* moduleLabel(int module){return module==ModuleCover?L"封面":module==ModuleLyrics?L"歌词":L"标题";}
 bool swapAnimating()const{return slotSwap.a>=0&&slotSwap.p.active(animationWall());}
 // Drop any pending press/drag without replaying the click and without
 // touching the mouse capture (used by WM_CAPTURECHANGED and resize paths).
 void clearModulePress(){modulePress=moduleDragging=false;pressSlot=-1;pressAction=0;pressIcon=-1;pressSeek=-1;dragTargetSlot=-1;dragHighlight.visible=false;dragHighlight.p.snap(0);dragHighlightSlot=-1;KillTimer(hwnd,LongPressTimer);}
 void morphDragHighlight(int slot){
  if(!dragHighlight.visible||slot<0||slot==dragHighlightSlot)return;
  double now=animationWall();
  dragHighlight.from=lerpRect(dragHighlight.from,dragHighlight.to,dragHighlight.p.value(now));
  dragHighlight.to=slotRects[slot];dragHighlight.p.snap(0);dragHighlight.p.target(1,now,0,200,0);
  dragHighlightSlot=slot;
 }
 static D2D1_RECT_F lerpRect(const D2D1_RECT_F&a,const D2D1_RECT_F&b,float p){return D2D1::RectF(mixf(a.left,b.left,p),mixf(a.top,b.top,p),mixf(a.right,b.right,p),mixf(a.bottom,b.bottom,p));}
 static D2D1_RECT_F squareIn(const D2D1_RECT_F&r){float side=std::min(r.right-r.left,r.bottom-r.top),cx=(r.left+r.right)*.5f,cy=(r.top+r.bottom)*.5f;return D2D1::RectF(cx-side*.5f,cy-side*.5f,cx+side*.5f,cy+side*.5f);}
 static D2D1_RECT_F animatedCoverRect(const D2D1_RECT_F&a,const D2D1_RECT_F&b,float p){return lerpRect(squareIn(a),squareIn(b),p);}
 float scale()const{return dpi/96.f;}
 int slotOfModule(int module)const{for(int i=0;i<3;i++)if(slotModule[i]==module)return i;return -1;}
 static float alignFactor(int align){return align==1?.5f:align==2?1.f:0.f;}
 float alignmentOffset(float available,float content,int align)const{return std::max(0.f,available-content)*alignFactor(align);}
 // The automatic direction keeps the historical behaviour: a title in the
 // narrow left column flows vertically, anywhere else horizontally.
 bool titleVerticalActive()const{int slot=slotLayout?slotOfModule(ModuleTitle):-1;return slot>=0&&(titleOrientation==2||(titleOrientation==0&&slot==0));}
 // Width a horizontal title may use. The compact variant reserves room for the
 // hover icons, the wide one is used while the icons are hidden.
 float titleAvailable(bool wide)const{
  if(!slotLayout)return std::max(20.f,width-textX-(wide?24.f:157.f));
  int slot=slotOfModule(ModuleTitle);if(slot<0)return std::max(20.f,width-textX);
  const auto&r=slotRects[slot];float base=std::max(20.f,r.right-r.left);
  return slot==0?base:std::max(20.f,base-(wide?24.f:182.f));
 }
 float lyricRight()const{return slotLayout?lyricsRight:width-12;}
 float lyricWidth()const{return std::max(1.f,lyricRight()-textX);}
 // Horizontal shift that places fitting content at the configured lyric edge.
 // Overflowing rows keep the sung-position scroll and get no shift.
 float lyricAlignShift(float content,float zoom=1.f)const{
  float available=lyricWidth(),scaled=content*zoom;
  return scaled<available?(available-scaled)/zoom*alignFactor(alignLyrics):0.f;
 }
 float verticalIconStep(const D2D1_RECT_F&r)const{return std::max(12.f,std::min(24.f,(r.bottom-r.top)*.55f/7));}
 int iconCount()const{return slotLayout?7:6;}
 int iconActionOf(int index)const{return slotLayout?index:index+1;}
 float lyricAnchor()const{return (slotLayout?lyricsCenterY:height/2)+4-(mainRowHeight*(1+contextFontSize/currentFontSize)+11)/2;}
 enum class LyricView {Waiting,StaticPreview,Prelude,Rows};
 bool staticPreview()const{return untimed||(!valid&&!hasClock);}
 LyricView lyricView()const{if(lines.empty())return LyricView::Waiting;if(browseIndex>=0)return LyricView::Rows;if(staticPreview())return LyricView::StaticPreview;return index<0?LyricView::Prelude:LyricView::Rows;}
 float rowAnchor()const{return lyricAnchor()+(lyricView()==LyricView::Prelude?mainRowHeight+GroupGap:0);}
 bool emptyGroup(const LyricGroup&g)const{return g.content->model.text.empty()&&g.content->model.words.empty()&&!g.translated;}
 bool isInterlude(const Line&line)const{return line.isInterlude||(line.text.empty()&&line.words.empty()&&line.translation.empty());}
 bool isInterlude(const LyricGroup&g)const{return isInterlude(g.content->model);}
 std::pair<int,int> browseBounds()const{int first=0,last=int(lines.size())-1;while(first<=last&&isInterlude(lines[first]))++first;while(last>=first&&isInterlude(lines[last]))--last;return first<=last?std::make_pair(first,last):std::make_pair(-1,-1);}
 int browseNeighbor(int at,int direction)const{auto bounds=browseBounds();if(bounds.first<0)return -1;int next=at+direction;next=direction>0?std::max(next,bounds.first):std::min(next,bounds.second);return next>=bounds.first&&next<=bounds.second?next:-1;}
 int browseFocus(int at)const{auto bounds=browseBounds();return bounds.first<0?-1:std::max(bounds.first,std::min(bounds.second,at));}
 int browseStep(int at,int steps)const{
  auto bounds=browseBounds();if(bounds.first<0)return -1;int direction=steps>0?-1:1,count=std::abs(steps);if(!count)return browseFocus(at);
  if(at<bounds.first||at>bounds.second){at=std::max(bounds.first,std::min(bounds.second,at));--count;}
  return std::max(bounds.first,std::min(bounds.second,at+direction*count));
 }
 bool showInterlude(const LyricGroup&g)const{return valid&&!untimed&&g.active&&isInterlude(g);}
 bool indicatorVisible(const LyricGroup&g)const{return valid&&!untimed&&isInterlude(g)&&g.dots.visible(indicatorNow());}
 bool groupVisible(const LyricGroup&g,double now)const{if(g.opacity<=.001f)return false;if(emptyGroup(g)&&!indicatorVisible(g)&&!g.middleInterlude)return false;float y=g.y.value(now);return y+g.renderedHeight(g.zoom.value(now))>=viewportTop&&y<=viewportBottom;}
 void updateIndicators(){
  const bool playingTimedLyrics=playing&&valid&&!untimed;
  const bool seeking=!flight.rows.empty();
  const double wall=animationWall(),time=current()+offset;
  // A paused seek still animates the indicator's exit without advancing playback.
  indicatorMotion.setRunning(playingTimedLyrics||seeking,wall);
  const double now=indicatorMotion.now(wall);
  auto updateIndicator=[&](InterludeAnimation&dots,bool selected,double start,double duration){
   const double dotTime=seeking&&!selected?lastIndicatorTime:time;
   dots.update(selected,playingTimedLyrics,now,dotTime,start,duration);
   if(!playingTimedLyrics){
    if(selected)dots.opacity.snap(1);
    else if(!seeking)dots.opacity.snap(0);
   }
  };
  updateIndicator(preludeDots,valid&&lyricView()==LyricView::Prelude,0,lines.empty()?0:lines.front().time);
  for(auto&item:groups){auto&g=item.second;const auto&line=g.content->model;updateIndicator(g.dots,showInterlude(g),line.time,interludeDuration(line,item.first));}
  lastIndicatorTime=time;
 }
 double interludeDuration(const Line&line,int id)const{if(line.duration>0)return line.duration;if(id>=0&&id+1<int(lines.size()))return std::max(0.,lines[id+1].time-line.time);return std::max(0.,duration-line.time);}
 int rowAt(double time)const {int lo=0,hi=int(lines.size())-1,result=-1;while(lo<=hi){int mid=(lo+hi)/2;if(lines[mid].time<=time){result=mid;lo=mid+1;}else hi=mid-1;}return result;}
 Layout layout(const std::wstring&text,IDWriteTextFormat*format,float maxWidth=50000){Layout l;if(text.empty())return l;check(textFactory->CreateTextLayout(text.c_str(),UINT32(text.size()),format,maxWidth,64,l.value.put()));DWRITE_TEXT_METRICS metrics{};check(l.value->GetMetrics(&metrics));l.width=metrics.widthIncludingTrailingWhitespace;l.height=metrics.height;return l;}
 void makeFormat(Com<IDWriteTextFormat>&format,float size,DWRITE_FONT_WEIGHT weight){check(textFactory->CreateTextFormat(L"Segoe UI",nullptr,weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"zh-cn",format.put()));format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);}
 void setLyricFontSize(double value){
  float size=float(std::round(clamp(value,14,36)));if(size==currentFontSize)return;
  Com<IDWriteTextFormat> nextMain,nextSecondary;
  makeFormat(nextMain,size,DWRITE_FONT_WEIGHT_SEMI_BOLD);makeFormat(nextSecondary,::TranslationFontSize*size/::CurrentFontSize,DWRITE_FONT_WEIGHT_NORMAL);
  mainFormat=std::move(nextMain);secondaryFormat=std::move(nextSecondary);
  currentFontSize=size;contextFontSize=::ContextFontSize*size/::CurrentFontSize;translationFontSize=::TranslationFontSize*size/::CurrentFontSize;
  mainRowHeight=layout(L"Ag",mainFormat.get()).height;secondaryRowHeight=0;
  flight=LyricFlight();groups.clear();focusIndex=-999;transitionActive=false;layoutDirty=true;snapRow=true;
  lyricLayouts.clear();nextLayoutToWarm=0;
  placeholder=Layout();updatePlaceholders();save();update();scheduleLayoutWarmup();command("font",size);
 }
 void setTitleFontSize(double value){
  float size=float(std::round(clamp(value,9,18)));if(size==titleFontSize)return;
  makeFormat(metaFormat,size,DWRITE_FONT_WEIGHT_NORMAL);
  titleFontSize=size;rebuildMeta();save();update();
 }
 void trimLayout(Layout&l){
  if(!l.value)return;DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};Com<IDWriteInlineObject>sign;
  if(SUCCEEDED(textFactory->CreateEllipsisTrimmingSign(metaFormat.get(),sign.put())))l.value->SetTrimming(&trim,sign.get());
 }
 void rebuildMeta(){
  std::wstring text=title+(artist.empty()?L"":L"  ·  "+artist);
  metaLayout=Layout();metaWide=Layout();metaVertical=Layout();
  if(titleVerticalActive()){
   metaLayout=layout(text,metaFormat.get(),32.f);trimLayout(metaLayout);
   if(!text.empty()){
    // Vertical flow starts at the right edge of the layout box, so the box must
    // be exactly one line wide; a wide box would push glyphs outside the slot.
    float lineHeight=std::max(16.f,metaLayout.height);
    check(textFactory->CreateTextLayout(text.c_str(),UINT32(text.size()),metaFormat.get(),lineHeight,50000,metaVertical.value.put()));
    metaVertical.value->SetReadingDirection(DWRITE_READING_DIRECTION_TOP_TO_BOTTOM);
    metaVertical.value->SetFlowDirection(DWRITE_FLOW_DIRECTION_RIGHT_TO_LEFT);
    DWRITE_TEXT_METRICS metrics{};check(metaVertical.value->GetMetrics(&metrics));metaVertical.width=metrics.width;metaVertical.height=metrics.height;
   }
   return;
  }
  // Two widths: the compact one reserves room for the hover icons, while the
  // idle one lets a long title use the free space once the icons hide.
  metaLayout=layout(text,metaFormat.get(),titleAvailable(false));trimLayout(metaLayout);
  metaWide=layout(text,metaFormat.get(),titleAvailable(true));trimLayout(metaWide);
 }
 Layout fitted(const std::wstring&text,IDWriteTextFormat*format,float maxWidth){Layout l=layout(text,format,std::max(12.f,maxWidth));if(l.value){DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};Com<IDWriteInlineObject>sign;if(SUCCEEDED(textFactory->CreateEllipsisTrimmingSign(format,sign.put())))l.value->SetTrimming(&trim,sign.get());}return l;}
 int playlistVisibleRows()const{return std::max(1,int((playlistHeight-PlaylistHeaderHeight-6)/PlaylistRowHeight));}
 int playlistMaxScroll()const{return std::max(0,int(playlist.size())-playlistVisibleRows());}
 float playlistVisualScroll()const{return playlistScrollTrack.value(animationWall());}
 float openProgress(double at)const{return float(lyricEase(unit((nowMs()-at)/OpenAnimMs)));}
 float playlistOpenProgress()const{return playlistOpening?openProgress(playlistOpenAt):1.f;}
 float panelOpenProgress()const{return panelOpening?openProgress(panelOpenAt):1.f;}
 bool playlistLocateVisible()const{
  if(playlistCurrent<0||playlist.empty())return false;
  float top=PlaylistHeaderHeight+(playlistCurrent-playlistVisualScroll())*PlaylistRowHeight;
  return !(top>=PlaylistHeaderHeight-.5f&&top+PlaylistRowHeight<=playlistHeight-5);
 }
 bool playlistLocateHit(float x,float y)const{
  if(!playlistLocateVisible())return false;
  float dx=x-(playlistWidth-28),dy=y-(playlistHeight-28);
  return dx*dx+dy*dy<=18*18;
 }
 void snapPlaylistScroll(){playlistScrollTrack.snap(float(playlistScroll));}
 void animatePlaylistScroll(int target){
  target=int(clamp(target,0,playlistMaxScroll()));playlistScroll=target;
  double now=animationWall();playlistScrollTrack.target(float(target),now,0,260,3);
  if(playlistOpen&&playlistScrollTrack.active(now))SetTimer(hwnd,PlaylistAnimTimer,8,nullptr);
  paintPlaylist();
 }
 int playlistRowAt(float x,float y)const{
  if(y<PlaylistHeaderHeight||y>playlistHeight-6||x<8||x>playlistWidth-8)return -1;
  int row=int(std::floor((y-PlaylistHeaderHeight)/PlaylistRowHeight+playlistVisualScroll()));
  return row>=0&&row<int(playlist.size())?row:-1;
 }
 static std::wstring playlistTime(double ms){
  int total=int(std::max(0.,ms)/1000+.5),minutes=total/60,seconds=total%60;
  return std::to_wstring(minutes)+L":"+(seconds<10?L"0":L"")+std::to_wstring(seconds);
 }
 void layoutPlaylistWindow(bool centerCurrent){
  int count=int(playlist.size()),rows=std::max(1,std::min(count,8));
  playlistWidth=std::max(200.f,width);
  playlistHeight=count?PlaylistHeaderHeight+rows*PlaylistRowHeight+10.f:156.f;
  playlistHeight=std::max(120.f,playlistHeight);
  if(playlistHwnd){
   int w=std::max(1,int(std::lround(playlistWidth*scale()))),h=std::max(1,int(std::lround(playlistHeight*scale())));
   RECT rect{};GetWindowRect(playlistHwnd,&rect);
   if(rect.right-rect.left!=w||rect.bottom-rect.top!=h)SetWindowPos(playlistHwnd,nullptr,0,0,w,h,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
  }
  playlistScroll=int(clamp(playlistScroll,0,playlistMaxScroll()));
  if(centerCurrent&&playlistCurrent>=0)playlistScroll=int(clamp(playlistCurrent-playlistVisibleRows()/2,0,playlistMaxScroll()));
 }
 void createPlaylistWindow(){
  if(playlistHwnd)return;
  DWORD ex=WS_EX_TOOLWINDOW|WS_EX_LAYERED|WS_EX_NOACTIVATE|(top?WS_EX_TOPMOST:0);
  int w=std::max(1,int(std::lround(playlistWidth*scale()))),h=std::max(1,int(std::lround(playlistHeight*scale())));
  playlistHwnd=CreateWindowExW(ex,L"MiniLyrics.Playlist",L"",WS_POPUP,0,0,w,h,hwnd,nullptr,GetModuleHandleW(nullptr),this);
  if(playlistHwnd)SetTimer(playlistHwnd,PlaylistWatchTimer,120,nullptr);
 }
 void positionPlaylist(){
  if(!playlistHwnd)return;
  RECT mainRect{};if(!GetWindowRect(hwnd,&mainRect))return;
  MONITORINFO monitor{sizeof(monitor)};if(!GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTOPRIMARY),&monitor))return;
  int w=int(std::lround(playlistWidth*scale())),h=int(std::lround(playlistHeight*scale())),gap=int(std::lround(7*scale()));
  int x=mainRect.left,y=mainRect.top-h-gap;
  if(y<monitor.rcWork.top){y=mainRect.bottom+gap;if(y+h>monitor.rcWork.bottom)y=monitor.rcWork.bottom-h-gap;}
  y=std::max<int>(int(monitor.rcWork.top),y);
  x=std::max<int>(int(monitor.rcWork.left),std::min<int>(x,int(monitor.rcWork.right)-w));
  SetWindowPos(playlistHwnd,top?HWND_TOPMOST:HWND_NOTOPMOST,x,y,w,h,SWP_NOACTIVATE);
 }
 void closePlaylist(){
  if(!playlistOpen&&!playlistHwnd)return;
  playlistOpen=false;playlistDrag=false;playlistDragMoved=false;playlistHover=-1;playlistLocateHover=false;playlistLocatePress=false;playlistLoading=false;playlistMouseDown=false;playlistTracking=false;playlistOpening=false;
  KillTimer(hwnd,PlaylistRefreshTimer);KillTimer(hwnd,PlaylistAnimTimer);
  if(playlistHwnd){HWND window=playlistHwnd;playlistHwnd=nullptr;KillTimer(window,PlaylistWatchTimer);DestroyWindow(window);}
  playlistTarget.reset();playlistGraphics.clear();
  if(!closing)update();
 }
 void togglePlaylist(){
  if(playlistOpen){closePlaylist();return;}
  playlistOpen=true;playlistHover=-1;playlistLocateHover=false;playlistLoading=playlist.empty();playlistOpenAt=nowMs();playlistOpening=true;
  layoutPlaylistWindow(true);snapPlaylistScroll();createPlaylistWindow();positionPlaylist();
  if(playlistHwnd)ShowWindow(playlistHwnd,SW_SHOWNOACTIVATE);
  paintPlaylist();
  SetTimer(hwnd,PlaylistRefreshTimer,2000,nullptr);SetTimer(hwnd,PlaylistAnimTimer,8,nullptr);
  command("playlist");update();
 }
 void selectPlaylistRow(int row){
  if(row<0||row>=int(playlist.size()))return;
  int index=playlist[row].index;std::string resourceId=playlist[row].resourceId;
  if(bridge){Json j=Json::obj();j["type"]="command";j["action"]="select";j["index"]=index;j["resourceId"]=resourceId;bridge->send(j);}
  closePlaylist();
 }
 void watchPlaylistOutside(){
  if(!playlistOpen||!playlistHwnd)return;
  bool down=(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0;
  if(down&&!playlistMouseDown){
   POINT point{};GetCursorPos(&point);RECT popup{},owner{};
   if(GetWindowRect(playlistHwnd,&popup)&&GetWindowRect(hwnd,&owner)&&!PtInRect(&popup,point)&&!PtInRect(&owner,point)){closePlaylist();return;}
  }
  playlistMouseDown=down;
 }
 void ensurePlaylistTarget(){
  RECT rect{};GetClientRect(playlistHwnd,&rect);
  D2D1_SIZE_U size=D2D1::SizeU(UINT(std::max<LONG>(1,rect.right)),UINT(std::max<LONG>(1,rect.bottom)));
  if(playlistTarget){
   D2D1_SIZE_U current=playlistTarget->GetPixelSize();
   if(current.width!=size.width||current.height!=size.height){if(!playlistTarget->bind(size.width,size.height)){playlistGraphics.clear();playlistTarget.reset();}}
  }
  if(!playlistTarget){
   auto next=std::make_unique<LayeredSurface>();
   if(!next->create(factory.get(),dpi,hardware)||!next->bind(size.width,size.height))throw std::runtime_error("playlist surface");
   next->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
   playlistTarget=std::move(next);
  }
  if(!playlistGraphics.target)playlistGraphics.init(playlistTarget->get(),bgA,bgB,CoverData(),playlistWidth,playlistHeight);
  else playlistGraphics.updateGradient(bgA,bgB,playlistWidth,playlistHeight);
 }
 void paintPlaylist(){
  if(!playlistHwnd||!IsWindow(playlistHwnd))return;
  PAINTSTRUCT ps{};BeginPaint(playlistHwnd,&ps);
  try{
   ensurePlaylistTarget();
   playlistTarget->BeginDraw();
   drawPlaylist(playlistGraphics);
   HRESULT hr=playlistTarget->EndDraw();
   if(hr==D2DERR_RECREATE_TARGET||FAILED(hr)){playlistGraphics.clear();playlistTarget.reset();}
   else if(IsWindowVisible(playlistHwnd)&&!playlistTarget->present(playlistHwnd,BYTE(std::lround(255.f*(playlistOpening?playlistOpenProgress():1.f))))){playlistGraphics.clear();playlistTarget.reset();}
  }catch(...){playlistGraphics.clear();playlistTarget.reset();}
  EndPaint(playlistHwnd,&ps);
 }
 void drawPlaylist(Graphics&g){
  auto*t=g.target;
  t->Clear(D2D1::ColorF(0,0,0,0));
  float open=playlistOpenProgress();
  D2D1_MATRIX_3X2_F savedTransform;t->GetTransform(&savedTransform);
  if(open<1){
   float scale=.965f+.035f*open,dy=(1-open)*7.f;
   t->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale,D2D1::Point2F(playlistWidth*.5f,playlistHeight*.5f))*D2D1::Matrix3x2F::Translation(0,dy)*savedTransform);
  }
  auto card=D2D1::RoundedRect(D2D1::RectF(.5f,.5f,std::max(1.f,playlistWidth-.5f),std::max(1.f,playlistHeight-.5f)),17,17);
  t->FillRoundedRectangle(card,g.gradient.get());
  g.brush->SetColor(D2D1::ColorF(0,0,0,.17f));t->FillRoundedRectangle(card,g.brush.get());
  g.brush->SetColor(D2D1::ColorF(.62f,.65f,.80f,.65f));t->DrawRoundedRectangle(card,g.brush.get(),1);
  auto header=layout(L"播放列表",playlistHeaderFormat.get());
  text(g,header,18,12,D2D1::ColorF(.95f,.96f,.99f));
  auto count=layout(playlistLoading?L"读取中":std::to_wstring(playlist.size())+L" 首",playlistMetaFormat.get());
  text(g,count,std::max(18.f,playlistWidth-72-count.width),17,D2D1::ColorF(.72f,.75f,.84f),.9f);
  g.brush->SetColor(D2D1::ColorF(1,1,1,.62f));
  float closeX=playlistWidth-24,closeY=21;
  t->DrawLine(D2D1::Point2F(closeX-4,closeY-4),D2D1::Point2F(closeX+4,closeY+4),g.brush.get(),1.4f);
  t->DrawLine(D2D1::Point2F(closeX+4,closeY-4),D2D1::Point2F(closeX-4,closeY+4),g.brush.get(),1.4f);
  g.brush->SetColor(D2D1::ColorF(1,1,1,.10f));
  t->DrawLine(D2D1::Point2F(14,PlaylistHeaderHeight-1),D2D1::Point2F(playlistWidth-14,PlaylistHeaderHeight-1),g.brush.get(),1);
  float listTop=PlaylistHeaderHeight;
  t->PushAxisAlignedClip(D2D1::RectF(6,listTop-1,std::max(7.f,playlistWidth-6),std::max(listTop,playlistHeight-5)),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  float visual=playlistVisualScroll();
  if(playlist.empty()){
   auto empty=layout(playlistLoading?L"正在读取播放列表…":playlistUnsupported?L"当前版本不支持读取播放列表":L"播放列表为空",playlistMetaFormat.get());
   text(g,empty,(playlistWidth-empty.width)/2,listTop+42,D2D1::ColorF(.75f,.78f,.86f),.9f);
  }else{
   int count=int(playlist.size()),visible=playlistVisibleRows();
   float durationX=playlistWidth-64;
   int first=std::max(0,int(std::floor(visual))-1),last=std::min(count,first+visible+3);
   for(int i=first;i<last;i++){
    float top=listTop+(i-visual)*PlaylistRowHeight;
    if(top>playlistHeight-4)break;
    if(top+PlaylistRowHeight<listTop)continue;
    const auto&item=playlist[i];bool current=i==playlistCurrent,hovered=i==playlistHover;
    if(current||hovered){
     auto row=D2D1::RoundedRect(D2D1::RectF(8,top+2,playlistWidth-8,top+PlaylistRowHeight-2),8,8);
     g.brush->SetColor(D2D1::ColorF(1,1,1,current?.10f:.055f));t->FillRoundedRectangle(row,g.brush.get());
    }
    if(current){g.brush->SetColor(D2D1::ColorF(1,1,1,.92f));t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(11,top+11,14,top+PlaylistRowHeight-11),1.5f,1.5f),g.brush.get());}
    auto number=layout(std::to_wstring(item.index+1),playlistMetaFormat.get());
    text(g,number,23,top+14,D2D1::ColorF(.66f,.69f,.78f),current?.95f:.72f);
    float textLeft=50,textWidth=std::max(20.f,durationX-textLeft-6);
    auto title=fitted(item.title,playlistTitleFormat.get(),textWidth);
    text(g,title,textLeft,top+5,current?D2D1::ColorF(1,1,1):D2D1::ColorF(.92f,.93f,.97f),current?1:.9f);
    auto artist=fitted(item.artist,playlistMetaFormat.get(),textWidth);
    text(g,artist,textLeft,top+23,D2D1::ColorF(.80f,.83f,.90f),current?.8f:.55f);
    auto time=layout(playlistTime(item.duration),playlistMetaFormat.get());
    text(g,time,durationX+std::max(0.f,42-time.width),top+14,D2D1::ColorF(.66f,.69f,.78f),.7f);
   }
  }
  t->PopAxisAlignedClip();
  int maxScroll=playlistMaxScroll();
  if(maxScroll>0){
   float trackTop=listTop+4,trackHeight=std::max(20.f,playlistHeight-5-trackTop);
   float thumb=std::max(24.f,trackHeight*float(playlistVisibleRows())/float(playlist.size()));
   float offset=float((trackHeight-thumb)*clamp(double(visual),0,maxScroll)/float(maxScroll));
   g.brush->SetColor(D2D1::ColorF(1,1,1,.18f));t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(playlistWidth-6,trackTop,playlistWidth-3.5f,trackTop+trackHeight),1.25f,1.25f),g.brush.get());
   g.brush->SetColor(D2D1::ColorF(1,1,1,.42f));t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(playlistWidth-6,trackTop+offset,playlistWidth-3.5f,trackTop+offset+thumb),1.25f,1.25f),g.brush.get());
  }
  if(playlistLocateVisible()){
   // NetEase-style locate reticle on a frosted, semi-transparent floating button.
   float cx=playlistWidth-28,cy=playlistHeight-28;bool hi=playlistLocateHover;
   g.brush->SetColor(D2D1::ColorF(0,0,0,hi?.28f:.22f));t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy+1.5f),15.5f,15.5f),g.brush.get());
   g.brush->SetColor(D2D1::ColorF(1,1,1,hi?.42f:.32f));t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),14.5f,14.5f),g.brush.get());
   g.brush->SetColor(D2D1::ColorF(1,1,1,hi?.52f:.40f));t->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),14.5f,14.5f),g.brush.get(),1);
   g.brush->SetColor(D2D1::ColorF(1,1,1,hi?.98f:.90f));
   t->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),5.6f,5.6f),g.brush.get(),1.3f);
   t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),1.7f,1.7f),g.brush.get());
   for(int k=0;k<4;k++){float a=float(k)*3.14159265f*.5f,c=std::cos(a),s=std::sin(a);
    t->DrawLine(D2D1::Point2F(cx+c*8.4f,cy+s*8.4f),D2D1::Point2F(cx+c*10.9f,cy+s*10.9f),g.brush.get(),1.3f);}
  }
  t->SetTransform(savedTransform);
 }
 LRESULT playlistMessage(HWND window,UINT msg,WPARAM wp,LPARAM lp){
  switch(msg){
   case WM_PAINT:paintPlaylist();return 0;
   case WM_ERASEBKGND:return 1;
   case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
   case WM_SETCURSOR:{
    if(LOWORD(lp)==HTCLIENT){
     POINT point{};GetCursorPos(&point);ScreenToClient(window,&point);
     float x=point.x/scale(),y=point.y/scale();
     bool hand=(x>playlistWidth-38&&y<42)||playlistLocateHit(x,y)||playlistRowAt(x,y)>=0;
     SetCursor(LoadCursorW(nullptr,hand?IDC_HAND:IDC_ARROW));return TRUE;
    }
    break;
   }
   case WM_MOUSEMOVE:{
    float x=GET_X_LPARAM(lp)/scale(),y=GET_Y_LPARAM(lp)/scale();
    if(playlistDrag){
     if(std::abs(int(GET_Y_LPARAM(lp))-int(playlistDragStart.y))>int(4*scale()))playlistDragMoved=true;
     int delta=int((playlistDragStart.y-int(GET_Y_LPARAM(lp)))/scale()/PlaylistRowHeight);
     int next=int(clamp(playlistDragScroll+delta,0,playlistMaxScroll()));
     if(next!=playlistScroll){playlistScroll=next;snapPlaylistScroll();playlistHover=-1;paintPlaylist();}
     return 0;
    }
    bool locateHover=playlistLocateHit(x,y);
    int hover=locateHover?-1:playlistRowAt(x,y);
    if(hover!=playlistHover||locateHover!=playlistLocateHover){playlistHover=hover;playlistLocateHover=locateHover;paintPlaylist();}
    if(!playlistTracking){playlistTracking=true;TRACKMOUSEEVENT event{sizeof(event),TME_LEAVE,window,0};TrackMouseEvent(&event);}
    return 0;
   }
   case WM_MOUSELEAVE:playlistTracking=false;if(playlistHover!=-1||playlistLocateHover){playlistHover=-1;playlistLocateHover=false;paintPlaylist();}return 0;
   case WM_LBUTTONDOWN:{
    float x=GET_X_LPARAM(lp)/scale(),y=GET_Y_LPARAM(lp)/scale();
    if(x>playlistWidth-38&&y<42){closePlaylist();return 0;}
    if(playlistLocateHit(x,y)){playlistLocatePress=true;SetCapture(window);if(playlistCurrent>=0)animatePlaylistScroll(playlistCurrent-playlistVisibleRows()/2);return 0;}
    SetCapture(window);playlistDrag=true;playlistDragMoved=false;playlistDragStart={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};playlistDragScroll=playlistScroll;
    return 0;
   }
   case WM_LBUTTONUP:{
    bool dragged=playlistDragMoved,locate=playlistLocatePress;playlistDrag=false;playlistDragMoved=false;playlistLocatePress=false;ReleaseCapture();
    if(!dragged&&!locate)selectPlaylistRow(playlistRowAt(GET_X_LPARAM(lp)/scale(),GET_Y_LPARAM(lp)/scale()));
    return 0;
   }
   case WM_MOUSEWHEEL:{
    int steps=GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA;
    if(steps){animatePlaylistScroll(playlistScroll-steps*2);playlistHover=-1;}
    return 0;
   }
   case WM_TIMER:if(wp==PlaylistWatchTimer)watchPlaylistOutside();return 0;
   case WM_DESTROY:{
    KillTimer(window,PlaylistWatchTimer);playlistHwnd=nullptr;playlistOpen=false;playlistDrag=false;playlistDragMoved=false;playlistLocateHover=false;playlistLocatePress=false;
    playlistTarget.reset();playlistGraphics.clear();KillTimer(hwnd,PlaylistRefreshTimer);KillTimer(hwnd,PlaylistAnimTimer);
    if(!closing)update();
    return 0;
   }
  }
  return DefWindowProcW(window,msg,wp,lp);
 }
 static LRESULT CALLBACK playlistProcedure(HWND window,UINT msg,WPARAM wp,LPARAM lp){
  App*app=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
  if(msg==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
  if(app){try{return app->playlistMessage(window,msg,wp,lp);}catch(...){app->error="playlist-message-failed";}}
  return DefWindowProcW(window,msg,wp,lp);
 }
 const std::shared_ptr<LyricLayout>& cachedLayout(size_t at){
  if(lyricLayouts.size()!=lines.size())lyricLayouts.resize(lines.size());
  auto&cached=lyricLayouts[at];const auto&line=lines[at];
  if(cached&&cached->model==line)return cached;
  auto built=std::make_shared<LyricLayout>();built->model=line;
  built->main=layout(line.text,mainFormat.get());built->mainHeight=built->main.height;
  built->glyphs.reserve(line.words.size());
  float x=0;
  for(const auto&word:line.words){
   Glyph glyph;
   glyph.layout=layout(word.text,mainFormat.get());
   glyph.time=word.time;
   glyph.duration=word.duration;
   glyph.x=x;
   x+=glyph.layout.width;
   built->mainHeight=std::max(built->mainHeight,glyph.layout.height);
   built->glyphs.push_back(std::move(glyph));
  }
  if(built->mainHeight<=0)built->mainHeight=mainRowHeight;
  built->subtitle=layout(line.translation,secondaryFormat.get());
  cached=std::move(built);lineBuildCalls++;return cached;
 }
 void scheduleLayoutWarmup(){
  if(!hwnd)return;
  if(renderVisible&&nextLayoutToWarm<lines.size())SetTimer(hwnd,LayoutWarmupTimer,15,nullptr);
  else KillTimer(hwnd,LayoutWarmupTimer);
 }
 void warmLyricLayouts(){
  if(!renderVisible||closing||nextLayoutToWarm>=lines.size()){KillTimer(hwnd,LayoutWarmupTimer);return;}
  if(seekPending||!flight.rows.empty()||resizing||systemMoving)return;
  // WM_TIMER lets painting and input take priority. Yield after a small batch.
  const double deadline=nowMs()+2;
  for(int count=0;count<8&&nextLayoutToWarm<lines.size();++count){
   cachedLayout(nextLayoutToWarm++);
   if(nowMs()>=deadline)break;
  }
  if(nextLayoutToWarm>=lines.size())KillTimer(hwnd,LayoutWarmupTimer);
 }
 LyricGroup& group(int at){
  const auto&content=cachedLayout(at);const auto&line=content->model;
  auto&g=groups.try_emplace(at).first->second;
  bool translated=translation&&!line.translation.empty();
  if(g.content==content&&g.translated==translated)return g;
  bool same=g.content&&g.content->model.time==line.time&&g.content->model.text==line.text;
  auto y=g.y,z=g.zoom,ma=g.mainAlpha,sa=g.subAlpha;auto dots=g.dots;float opacity=g.opacity;bool placed=g.placed;uint64_t id=same?g.identity:++groupSerial;
  g=LyricGroup();g.content=content;g.identity=id;g.translated=translated;g.y=y;g.zoom=z;g.placed=placed;g.mainAlpha=ma;g.subAlpha=sa;g.opacity=opacity;
  if(same)g.dots=dots;
  g.mainAlpha.snap(at==index?1.f:.4f);g.subAlpha.snap(at==index?.8f:.4f);g.active=at==index;return g;
 }
 float groupScale(int at,int focus){return at==focus&&lyricView()!=LyricView::Prelude?1.f:contextFontSize/currentFontSize;}
 int layoutDistance(int at,int focus,int limit)const{
  if(at<0||focus<0||at>=int(lines.size())||focus>=int(lines.size()))return limit;int distance=0;
  auto bounds=browseIndex>=0?browseBounds():std::make_pair(-1,-1);for(int n=std::min(at,focus);n<std::max(at,focus);n++)if((browseIndex<0||(n>=bounds.first&&n<=bounds.second))&&++distance>=limit)break;return distance;
 }
 float positionFor(int at,int focus,float anchorY){
  if(at<0||focus<0||at>=int(lines.size())||focus>=int(lines.size()))return anchorY+(at>focus?1:-1)*(height+200);
  float y=anchorY;int from=std::min(at,focus),to=std::max(at,focus);
  auto bounds=browseIndex>=0?browseBounds():std::make_pair(-1,-1);auto included=[&](int n){return browseIndex<0||(n>=bounds.first&&n<=bounds.second);};
  for(int n=from;n<to;n++)if(included(n))y+=(at>focus?1:-1)*(group(n).renderedHeight(groupScale(n,focus))+GroupGap);
  return y;
 }
 void roles(){
  double now=motionNow(),time=current()+offset;
  for(auto&item:groups){auto&g=item.second;bool active=item.first==index;if(active==g.active)continue;
   if(g.active){g.resetWords.clear();for(const auto&w:g.content->glyphs)g.resetWords.push_back(floatWord(time-w.time,w.duration));g.resetAt=now;}
   g.active=active;g.mainAlpha.target(active?1.f:.4f,now);g.subAlpha.target(active?.8f:.4f,now);
  }
 }
 void beginFlight(int desired,int playbackIndex){
  if(lines.empty()||focusIndex<0)return;if(playbackIndex<0&&preludePreview>=0)desired=preludePreview;desired=std::max(0,std::min(int(lines.size())-1,desired));double now=animationWall(),motionTime=motionNow();bool sourcePrelude=flight.rows.empty()&&lyricView()==LyricView::Prelude,sourceBrowse=browseIndex>=0;int sourceFocus=std::max(0,std::min(int(lines.size())-1,focusIndex));
  // Only the destination rows need exact heights before we know what is visible.
  group(desired);
  if(playbackIndex>=0&&playbackIndex!=desired)group(playbackIndex);
  if(secondaryRowHeight<=0)secondaryRowHeight=layout(L"Ag",secondaryFormat.get()).height;
  std::vector<float> heights(lines.size()),prefix(lines.size()+1),sourcePrefix(lines.size()+1);auto bounds=sourceBrowse?browseBounds():std::make_pair(-1,-1);float context=contextFontSize/currentFontSize;
  for(size_t i=0;i<lines.size();i++){
   const auto&cached=lyricLayouts[i];const bool translated=translation&&!lines[i].translation.empty();
   heights[i]=cached?cached->height(translated):mainRowHeight+(translated?TranslationGap+secondaryRowHeight:0);
   const float step=heights[i]*context+GroupGap;prefix[i+1]=prefix[i]+step;sourcePrefix[i+1]=sourcePrefix[i]+(!sourceBrowse||(int(i)>=bounds.first&&int(i)<=bounds.second)?step:0);
  }
  auto previous=std::move(flight);auto previousPhase=previous.phase(now);flight=LyricFlight();flight.rows.resize(lines.size());flight.focus=desired;flight.playbackIndex=playbackIndex;flight.curve=lineCurve;flight.stagger=stagger&&desired!=sourceFocus;flight.prelude=playbackIndex<0;int direction=desired>=sourceFocus?1:-1;
  float destinationAnchor=lyricAnchor()+(flight.prelude?mainRowHeight+GroupGap:0),sourceAnchor=layoutAnchor;
  for(size_t i=0;i<lines.size();i++){
   auto&r=flight.rows[i];r.height=heights[i];r.bucket=std::max(-4,std::min(4,int(i)-desired))*direction+4;
   r.from={sourceAnchor+sourcePrefix[i]-sourcePrefix[sourceFocus]+(!sourcePrelude&&int(i)>sourceFocus?heights[sourceFocus]*(1-context):0),!sourcePrelude&&int(i)==sourceFocus?1.f:context,int(i)==index?1.f:.4f,int(i)==index?.8f:.4f,1.f};
   if(previous.rows.size()==lines.size())r.from=previous.sample(previous.rows[i],previousPhase);
   else{auto found=groups.find(int(i));if(found!=groups.end()&&found->second.placed&&((!sourcePrelude&&std::abs(int(i)-sourceFocus)<=6)||groupVisible(found->second,motionTime))){const auto&g=found->second;r.from={g.y.value(motionTime),g.zoom.value(motionTime),g.mainAlpha.value(motionTime),g.subAlpha.value(motionTime),g.opacity};}}
   r.to={destinationAnchor+prefix[i]-prefix[desired]+(!flight.prelude&&int(i)>desired?heights[desired]*(1-context):0),!flight.prelude&&int(i)==desired?1.f:context,int(i)==playbackIndex?1.f:.4f,int(i)==playbackIndex?.8f:.4f,1.f};
  }
 }
 void applyFlight(double now){
  auto phase=flight.phase(now);std::vector<int>wanted;wanted.reserve(32);auto add=[&](int id){if(id>=0&&id<int(lines.size())&&wanted.size()<32&&std::find(wanted.begin(),wanted.end(),id)==wanted.end())wanted.push_back(id);};
  for(size_t i=0;i<flight.rows.size();i++){auto&r=flight.rows[i];r.point=flight.sample(r,phase);if(r.point.y+r.height*r.point.zoom>=viewportTop&&r.point.y<=viewportBottom)add(int(i));}
  for(size_t i=0;i<flight.rows.size();i++){const auto&r=flight.rows[i];if(r.point.y+r.height*r.point.zoom>=viewportTop-mainRowHeight*2&&r.point.y<=viewportBottom+mainRowHeight*2)add(int(i));}
  add(flight.focus);add(flight.playbackIndex);
  for(auto it=groups.begin();it!=groups.end();)if(std::find(wanted.begin(),wanted.end(),it->first)==wanted.end())it=groups.erase(it);else ++it;
  auto interludeBounds=browseBounds();
  for(int id:wanted){auto&g=group(id);const auto&p=flight.rows[id].point;g.y.snap(p.y);g.zoom.snap(p.zoom);g.mainAlpha.snap(p.mainAlpha);g.subAlpha.snap(p.subAlpha);g.opacity=p.opacity;g.placed=true;g.active=id==index;g.middleInterlude=interludeBounds.first>=0&&id>interludeBounds.first&&id<interludeBounds.second&&isInterlude(g);}
  focusIndex=flight.focus;layoutAnchor=lyricAnchor()+(flight.prelude?mainRowHeight+GroupGap:0);layoutDirty=false;
  // Keep layout work from consuming the first frames of the transition.
  if(flight.firstFrame){flight.started=animationWall();flight.firstFrame=false;}
 }
 void seekTo(double target){
  target=std::max(0.,duration>0?std::min(duration,target):target);bool canFly=renderVisible&&!untimed&&valid&&!lines.empty()&&focusIndex>=0&&!groups.empty();
  if(canFly){int actual=rowAt(target+offset),desired=std::max(0,rowAt(target+offset+(playing&&stagger?200:0)));beginFlight(desired,actual);}else flight=LyricFlight();
  if(browseIndex>=0&&!untimed)stopBrowsing();returnFromBrowse=false;error.clear();snapRow=!canFly;position=target;anchor=nowMs();valid=true;layoutDirty=true;
 }
 void reflow(int desired,bool animate){
  if(lines.empty()){groups.clear();focusIndex=-1;transitionActive=false;layoutDirty=false;return;}
  desired=std::max(0,std::min(int(lines.size())-1,desired));int previous=focusIndex;
  std::vector<int>wanted;auto add=[&](int n){if(n>=0&&n<int(lines.size())&&std::find(wanted.begin(),wanted.end(),n)==wanted.end())wanted.push_back(n);};
  add(desired);group(desired);
  float newAnchor=rowAnchor();
  std::map<int,float>targets;targets[desired]=newAnchor;
  auto bounds=browseIndex>=0?browseBounds():std::make_pair(-1,-1);
  auto included=[&](int n){return browseIndex<0||(n>=bounds.first&&n<=bounds.second);};
  int before=desired,after=desired,prevUp=desired,prevDown=desired;float yUp=newAnchor,yDown=newAnchor;bool up=true,down=true;
  // Fill the viewport plus a row of overscan, retaining a bounded working set.
  while((up||down)&&wanted.size()<28){for(int direction:{-1,1}){bool&more=direction<0?up:down;int&at=direction<0?before:after;if(!more)continue;
   at=browseIndex>=0?browseNeighbor(at,direction):at+direction;if(at<0||at>=int(lines.size())){more=false;continue;}
   add(at);auto&g=group(at);
   float y=0;
   if(direction<0){
    if(at==prevUp-1)yUp-=included(at)?(g.renderedHeight(groupScale(at,desired))+GroupGap):0.f;
    else yUp=positionFor(at,desired,newAnchor);
    prevUp=at;y=yUp;
   }else{
    if(at==prevDown+1){const auto&pg=group(prevDown);yDown+=included(prevDown)?(pg.renderedHeight(groupScale(prevDown,desired))+GroupGap):0.f;}
    else yDown=positionFor(at,desired,newAnchor);
    prevDown=at;y=yDown;
   }
   targets[at]=y;
   more=direction<0?y+g.renderedHeight(groupScale(at,desired))>=viewportTop-mainRowHeight:y<=viewportBottom+mainRowHeight;
  }}
  for(int d=-2;d<=2;d++)add(index+d);
  for(int n:wanted)group(n);
  double now=motionNow();int direction=desired>=previous?1:-1;
  for(int n:wanted){auto&g=group(n);g.opacity=1;
   if(!g.placed){int initial=previous>=0&&animate?previous:desired;g.y.snap(positionFor(n,initial,previous>=0&&animate?layoutAnchor:newAnchor));g.zoom.snap(groupScale(n,initial));g.placed=true;}
   auto itTarget=targets.find(n);
   float target=itTarget!=targets.end()?itTarget->second:positionFor(n,desired,newAnchor);
   if(animate){double delay=browseIndex>=0||returnFromBrowse||previous==desired?0:rnpDelay(n-desired,direction,stagger),duration=browseIndex>=0?250:500;g.y.target(target,now,delay,duration,lineCurve);g.zoom.target(groupScale(n,desired),now,delay,duration,lineCurve);}else{g.y.snap(target);g.zoom.snap(groupScale(n,desired));}
  }
  layoutAnchor=newAnchor;focusIndex=desired;
  for(auto it=groups.begin();it!=groups.end();){
   bool keep=std::find(wanted.begin(),wanted.end(),it->first)!=wanted.end();float y=it->second.y.value(now);
   if(!keep&&(!animate||!it->second.y.active(now)||y+it->second.renderedHeight(it->second.zoom.value(now))<viewportTop-8||y>viewportBottom+8))it=groups.erase(it);else ++it;
  }
  while(groups.size()>32){auto farthest=groups.end();for(auto it=groups.begin();it!=groups.end();++it)if(it->first!=index&&(farthest==groups.end()||std::abs(it->first-desired)>std::abs(farthest->first-desired)))farthest=it;if(farthest==groups.end())break;groups.erase(farthest);}
  auto interludeBounds=browseIndex>=0?bounds:browseBounds();
  for(auto&item:groups)item.second.middleInterlude=interludeBounds.first>=0&&item.first>interludeBounds.first&&item.first<interludeBounds.second&&isInterlude(item.second);
  if(previous!=desired){rowSince=now;layoutChanges++;}
  roles();layoutDirty=false;
 }
 void updatePlaceholders(){
  if(lines.empty())preludeDots=InterludeAnimation();
  placeholder=layout(lyricsReceived&&lines.empty()?L"暂无歌词":L"等待歌词",mainFormat.get());
  preludePreview=-1;for(size_t i=0;i<lines.size();i++){const auto&line=lines[i];if(!line.isInterlude&&(!line.text.empty()||!line.translation.empty()||std::any_of(line.words.begin(),line.words.end(),[](const Word&w){return !w.text.empty();}))){preludePreview=int(i);break;}}
 }
 void recordNormalBounds(){if(!hwnd||IsIconic(hwnd))return;RECT r{};if(GetWindowRect(hwnd,&r)&&r.right>r.left&&r.bottom>r.top){normalWindowRect=r;normalWindowScale=scale();hasNormalRect=true;}}
 bool geometry(){
  RECT rect{};GetClientRect(hwnd,&rect);
  if(geometryPixels.cx==rect.right&&geometryPixels.cy==rect.bottom&&geometryDpi==dpi)return false;
  geometryPixels={rect.right,rect.bottom};geometryDpi=dpi;flight=LyricFlight();recordNormalBounds();
  width=std::max(1.f,float(rect.right)/scale());height=std::max(1.f,float(rect.bottom)/scale());
  miniMode=width<=400.f||height<=170.f;
  if(miniMode){
   clearModulePress();slotSwap.a=slotSwap.b=-1;layoutPanel=false;panelOpening=false;KillTimer(hwnd,SecondaryAnimTimer);splitterDrag=0;splitterHover=0;slotLayout=false;
   auto art=coverGeometry(rect.right,rect.bottom,scale());artSize=art.side;artRadius=art.radius;
   artBounds=D2D1::RectF(art.left,art.top,art.left+art.side,art.top+art.side);textX=artBounds.right+16;transport=transportGeometry(width,height,textX,scale());
   viewportTop=32;viewportBottom=std::min(height-28,transport.y-14);
   lyricsRight=width-12;lyricsCenterY=height/2;
   check(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(artBounds,artRadius,artRadius),coverClip.put()));
  }else{
   slotLayout=true;applySlotLayout();
  }
  graphics.resize(width,height);rebuildMeta();layoutDirty=true;return true;
 }
 void applySlotLayout(){
  const float availW=std::max(1.f,width-SlotMargin*2),availH=std::max(1.f,height-SlotMargin-SlotBottom);
  const float minLeft=slotModule[0]==ModuleCover?110.f:96.f,maxLeft=std::max(minLeft,availW-SlotGap-110.f);
  const float leftW=float(clamp(leftRatio*availW,minLeft,maxLeft));
  // The top strip keeps the historical 20-DIP title bar by default; when a
  // picture or lyrics live there it must grow to a usable height.
  const float minTop=slotModule[1]==ModuleTitle?18.f:72.f;
  const float maxTop=std::max(minTop,availH-SlotGapY-60.f);
  const float topH=float(clamp(topHeight,minTop,maxTop));
  const float rightX=SlotMargin+leftW+SlotGap;
  slotRects[0]=D2D1::RectF(SlotMargin,SlotMargin,SlotMargin+leftW,height-SlotBottom);
  slotRects[1]=D2D1::RectF(rightX,SlotMargin,width-SlotMargin,SlotMargin+topH);
  slotRects[2]=D2D1::RectF(rightX,SlotMargin+topH+SlotGapY,width-SlotMargin,height-SlotBottom);
  const int lyricSlot=slotOfModule(ModuleLyrics);
  textX=slotRects[lyricSlot].left;lyricsRight=slotRects[lyricSlot].right;
  lyricsCenterY=(slotRects[lyricSlot].top+slotRects[lyricSlot].bottom)*.5f;
  viewportTop=slotRects[lyricSlot].top;viewportBottom=slotRects[lyricSlot].bottom;
  transport=transportGeometry(width,height,textX,scale());
  const auto&coverSlot=slotRects[slotOfModule(ModuleCover)];
  RECT client{};GetClientRect(hwnd,&client);
  const int W=std::max<LONG>(1,client.right),H=std::max<LONG>(1,client.bottom);const float s=scale();
  float sideDip=std::max(1.f,std::min(coverSlot.right-coverSlot.left,coverSlot.bottom-coverSlot.top));
  int side=int(std::lround(sideDip*s));
  if((side&1)!=(H&1))--side;
  side=std::max(1,std::min(side,std::min(W,H)));
  int top=(H-side)/2;
  const int slotTop=int(std::lround(coverSlot.top*s)),slotBottom=int(std::lround(coverSlot.bottom*s));
  top=std::max(slotTop,std::min(top,slotBottom-side));
  const int slotLeft=int(std::lround(coverSlot.left*s)),slotRight=int(std::lround(coverSlot.right*s));
  int left=slotLeft;
  if(alignCover==1)left=slotLeft+(slotRight-slotLeft-side)/2;
  else if(alignCover==2)left=slotRight-side;
  left=std::max(slotLeft,std::min(left,slotRight-side));
  artBounds=D2D1::RectF(left/s,top/s,(left+side)/s,(top+side)/s);artSize=float(side)/s;
  artRadius=float(std::max(1,std::min(side/2,int(std::lround(10*s)))))/s;
  check(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(artBounds,artRadius,artRadius),coverClip.put()));
 }
 void ensureTarget(){
  RECT rect{};GetClientRect(hwnd,&rect);auto size=D2D1::SizeU(std::max(1L,rect.right),std::max(1L,rect.bottom));
  // UpdateLayeredWindow requires this style; isolated-test windows may lack it.
  LONG_PTR style=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
  if(!(style&WS_EX_LAYERED))SetWindowLongPtrW(hwnd,GWL_EXSTYLE,style|WS_EX_LAYERED);
  // Resize discards the buffer. Pair the rebind with a full draw in this paint.
  if(target){auto old=target->GetPixelSize();if((old.width!=size.width||old.height!=size.height)&&!target->bind(size.width,size.height)){graphics.clear();target.reset();}}
  if(!target){auto next=std::make_unique<LayeredSurface>();
   if(!next->create(factory.get(),dpi,hardware)||!next->bind(size.width,size.height))throw std::runtime_error("layered render target creation failed");
   target=std::move(next);target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  }if(!graphics.target)graphics.init(target->get(),bgA,bgB,cover,width,height);
 }
 void text(Graphics&g,const Layout&l,float x,float y,D2D1_COLOR_F color,float alpha=1){if(!l.value)return;g.brush->SetColor(color);g.brush->SetOpacity(alpha);g.target->DrawTextLayout(D2D1::Point2F(x,y),l.value.get(),g.brush.get(),D2D1_DRAW_TEXT_OPTIONS_NONE);g.brush->SetOpacity(1);}
 float rowWidth(const LyricGroup&g)const{return g.content->glyphs.empty()?g.content->main.width:g.content->glyphs.back().x+g.content->glyphs.back().layout.width;}
 bool canScroll(int id)const{return !staticPreview()&&browseIndex<0&&id<=index;}
 float timedHorizontalPhase(const LyricGroup&g,double time,int id)const{
  if(!canScroll(id))return 0;
  const Line*line=&g.content->model;
  return float(clamp((time-line->time-900)/std::max(1200.,(line->duration>0?line->duration:4000)-1600),0,1));
 }
 float horizontalPhase(const LyricGroup&g,double time,int id,double now)const{
  if(g.content->glyphs.empty())return timedHorizontalPhase(g,time,id);
  if(!canScroll(id))return 0;
  float zoom=std::max(.01f,g.zoom.value(now)),available=lyricWidth(),overflow=rowWidth(g)*zoom-available;
  if(overflow<=0)return 0;
  const auto&last=g.content->glyphs.back();if(time>=last.time+last.duration)return 1;
  // Track the sung position in measured text space. Gaps hold this cursor;
  // seeking is stateless, and contiguous word boundaries stay continuous.
  float cursor=0;
  for(const auto&w:g.content->glyphs)if(time>=w.time)cursor=std::max(cursor,w.x+w.layout.width*float(unit((time-w.time)/w.duration)));
  return float(clamp((cursor*zoom-available*.5f)/overflow,0,1));
 }
 float horizontalPhase(const LyricGroup&g,double time,int id)const{return horizontalPhase(g,time,id,motionNow());}
 void drawInterludeAt(Graphics&g,const InterludeAnimation&animation,float x,float y,double time,double start,double duration,double now,float opacity=1){
  float alpha=opacity*animation.opacity.value(now);if(alpha<=0)return;
  float breath=animation.scale(now),diameter=currentFontSize*.7f,gap=currentFontSize*.5f;
  g.brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
  for(int i=0;i<3;i++){auto dot=animation.dot(time,start,duration,i,now);float radius=diameter*.5f*dot.scale*breath;g.brush->SetOpacity(alpha*dot.alpha);g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x+(diameter*.5f+i*(diameter+gap))*breath,y+mainRowHeight*.5f),radius,radius),g.brush.get());}
  g.brush->SetOpacity(1);
 }
 void drawGroupAt(Graphics&g,const LyricGroup&r,int id,double time,double now){
  if(r.opacity<=0)return;float zoom=std::max(.01f,r.zoom.value(now)),y=r.y.value(now);
  float available=lyricWidth(),phase=horizontalPhase(r,time,id,now);
  // Alignment only applies while the row fits inside the viewport; overflowing
  // rows keep the sung-position scroll, so the two offsets never stack.
  float shift=lyricAlignShift(rowWidth(r),zoom);
  D2D1_MATRIX_3X2_F transform;bool scaled=zoom!=1.f;float originX=textX,originY=y;
  if(scaled){g.target->GetTransform(&transform);g.target->SetTransform(D2D1::Matrix3x2F::Scale(zoom,zoom)*D2D1::Matrix3x2F::Translation(textX,y)*transform);originX=originY=0;}
  float x=originX-std::max(0.f,rowWidth(r)*zoom-available)*phase/zoom+shift;
  bool resetting=!r.active&&!r.resetWords.empty()&&now-r.resetAt<200;
  if(r.content->glyphs.empty()||staticPreview())text(g,r.content->main,x,originY,D2D1::ColorF(D2D1::ColorF::White),(staticPreview()?(id==focusIndex?1.f:.4f):r.mainAlpha.value(now))*r.opacity);
  else{
   float clipLeft=originX+shift-2.f,clipRight=originX+shift+available/zoom+2.f;
   for(size_t i=0;i<r.content->glyphs.size();i++){
    const auto&w=r.content->glyphs[i];float wx=x+w.x;
    if(wx+w.layout.width<clipLeft||wx>clipRight)continue;
    FloatWord f{.4f,0};
    if(r.active)f=floatWord(time-w.time,w.duration);
    else if(resetting&&i<r.resetWords.size()){double p=unit((now-r.resetAt)/200);f={mixf(r.resetWords[i].alpha,.4f,p),mixf(r.resetWords[i].lift,0,lyricEase(p))};}
    text(g,w.layout,wx,originY-f.lift,D2D1::ColorF(D2D1::ColorF::White),f.alpha*r.opacity);
   }
  }
  if(indicatorVisible(r)){
   // Leading/trailing interludes sit outside the browsable range and are pinned to the
   // anchor while scrolling; fade their dots instead of hiding them abruptly.
   float fade=1.f;
   if(browseIndex>=0){auto bounds=browseBounds();if(id<bounds.first||id>bounds.second)fade=float(clamp(browseDotsFade.value(animationWall()),0.,1.));}
   if(fade>.001f)drawInterludeAt(g,r.dots,originX+lyricAlignShift(currentFontSize*3.1f,zoom),originY,time,r.content->model.time,interludeDuration(r.content->model,id),indicatorNow(),fade*r.mainAlpha.value(now)*r.opacity);
  }
  if(r.translated){
   float subWidth=r.content->subtitle.width*zoom;
   float subX=originX-std::max(0.f,subWidth-available)*timedHorizontalPhase(r,time,id)/zoom+lyricAlignShift(r.content->subtitle.width,zoom);
   text(g,r.content->subtitle,subX,originY+r.subtitleOffset(1.f),D2D1::ColorF(D2D1::ColorF::White),(staticPreview()?(id==focusIndex?.8f:.4f):r.subAlpha.value(now))*r.opacity);
  }
  if(scaled)g.target->SetTransform(transform);
 }
 void drawGroup(Graphics&g,const LyricGroup&r,int id,double time){drawGroupAt(g,r,id,time,motionNow());}
 bool backgroundPlaying()const{return renderVisible&&background.mode>0&&background.animated&&playing&&valid;}
 bool backgroundAnimating()const{return !resizing&&backgroundPlaying();}
 Json backgroundState()const{Json j=Json::obj();j["mode"]=background.mode;j["animated"]=background.animated;j["speed"]=background.speed;j["fps"]=background.fps;return j;}
 bool readBackground(const Json&m){
  auto next=background;
  if(m["mode"].kind==Json::Number)next.mode=int(clamp(m["mode"].n(),0,3));
  if(m["animated"].kind==Json::Bool)next.animated=m["animated"].b();
  if(m["speed"].kind==Json::Number)next.speed=clamp(m["speed"].n(),.25,3);
  if(m["fps"].kind==Json::Number)next.fps=backgroundFrameRate(int(m["fps"].n()));
  if(next==background)return false;background=next;fluid.invalidate();return true;
 }
 void drawBackground(Graphics&g,const D2D1_ROUNDED_RECT&card){
  if(background.mode==0){g.target->FillRoundedRectangle(card,g.gradient.get());return;}
  // Reuse the last texture while sizing; pending cover/settings changes and
  // the final dimensions are applied after mouse capture ends.
  bool liveWindow=(target&&g.target==target->get())&&backgroundAnimating()&&!resizing;
  if(!resizing||fluid.pixels.empty()){double wall=animationWall();fluid.setSize(width,height);fluid.prepare(background,backgroundClock.time(wall),backgroundClock.position(wall),liveWindow);}
  if(!g.background){check(g.target->CreateBitmap(D2D1::SizeU(FluidBackground::width,FluidBackground::height),fluid.pixels.data(),FluidBackground::width*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),g.background.put()));check(g.target->CreateBitmapBrush(g.background.get(),D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),g.backgroundBrush.put()));g.backgroundRevision=fluid.revision;}
  else if(g.backgroundRevision!=fluid.revision){check(g.background->CopyFromMemory(nullptr,fluid.pixels.data(),FluidBackground::width*4));g.backgroundRevision=fluid.revision;}
  g.backgroundBrush->SetTransform(D2D1::Matrix3x2F::Scale(width/FluidBackground::width,height/FluidBackground::height));g.target->FillRoundedRectangle(card,g.backgroundBrush.get());
 }
 void drawLyricsAt(Graphics&g,double time,double now){
  float y=lyricAnchor();double dotsNow=indicatorNow();float dotsFade=float(clamp(browseDotsFade.value(animationWall()),0.,1.));
  if(!flight.rows.empty()){if(dotsFade>.001f&&flight.prelude&&preludeDots.visible(dotsNow))drawInterludeAt(g,preludeDots,textX+lyricAlignShift(currentFontSize*3.1f),y,time,0,lines.front().time,dotsNow,dotsFade);}
  else switch(lyricView()){
   case LyricView::Waiting:text(g,placeholder,textX+lyricAlignShift(placeholder.width),y,D2D1::ColorF(D2D1::ColorF::White));return;
   case LyricView::StaticPreview:break;
   case LyricView::Prelude:if(dotsFade>.001f)drawInterludeAt(g,preludeDots,textX+lyricAlignShift(currentFontSize*3.1f),y,time,0,lines.front().time,dotsNow,dotsFade);break;
   case LyricView::Rows:break;
  }
  for(const auto&item:groups)if(groupVisible(item.second,now))drawGroupAt(g,item.second,item.first,time,now);
 }
 void drawLyrics(Graphics&g,double time){drawLyricsAt(g,time,motionNow());}
 void drawCoverArt(Graphics&g,const D2D1_RECT_F&rect,float radius,float alpha,ID2D1Geometry*clip){
  auto*t=g.target;
  if(!g.cover){g.brush->SetColor(D2D1::ColorF(.33f,.39f,.51f));t->FillRoundedRectangle(D2D1::RoundedRect(rect,radius,radius),g.brush.get());return;}
  float side=float(std::min(cover.width,cover.height)),x=(cover.width-side)/2,y=(cover.height-side)/2;
  if(g.coverBrush&&alpha>=.999f&&g.coverOpaque){
   float w=std::max(1.f,rect.right-rect.left),scaleArt=w/std::max(1.f,side);
   g.coverBrush->SetTransform(D2D1::Matrix3x2F::Translation(-x,-y)*D2D1::Matrix3x2F::Scale(scaleArt,scaleArt)*D2D1::Matrix3x2F::Translation(rect.left,rect.top));
   t->FillRoundedRectangle(D2D1::RoundedRect(rect,radius,radius),g.coverBrush.get());
  }else{
   // One rounded mask for both layers avoids accumulated antialiasing halos.
   t->PushLayer(D2D1::LayerParameters(rect,clip),nullptr);
   g.brush->SetColor(D2D1::ColorF(.33f,.39f,.51f));t->FillRectangle(rect,g.brush.get());
   t->DrawBitmap(g.cover.get(),rect,alpha,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,D2D1::RectF(x,y,x+side,y+side));
   t->PopLayer();
  }
 }
 void draw(Graphics&g){
  auto*t=g.target;double time=current()+offset,now=motionNow();

  t->Clear(D2D1::ColorF(0,0));auto card=D2D1::RoundedRect(D2D1::RectF(.5f,.5f,width-.5f,height-.5f),17,17);drawBackground(g,card);g.brush->SetColor(D2D1::ColorF(.62f,.65f,.80f,.65f));t->DrawRoundedRectangle(card,g.brush.get(),1);
  D2D1_MATRIX_3X2_F moduleBase;t->GetTransform(&moduleBase);
  float coverAlpha=(g.cover&&playing)?float(clamp((nowMs()-coverSince)/250,0,1)):1.f;
  if(slotSwap.a>=0&&slotSwap.p.active(animationWall())&&(slotSwap.a==ModuleCover||slotSwap.b==ModuleCover)){
   bool isA=slotSwap.a==ModuleCover;const auto&from=isA?slotSwap.fromA:slotSwap.fromB;const auto&to=isA?slotSwap.toA:slotSwap.toB;
   float p=slotSwap.p.value(animationWall());
   t->PushAxisAlignedClip(lerpRect(from,to,p),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
   drawCoverArt(g,animatedCoverRect(from,to,p),artRadius,coverAlpha,nullptr);
   t->PopAxisAlignedClip();
  }else drawCoverArt(g,artBounds,artRadius,coverAlpha,coverClip.get());
  bool titleMoved=beginModuleDraw(g,ModuleTitle,moduleBase);
  drawTitle(g);
  endModuleDraw(g,titleMoved,moduleBase);
  bool lyricMoved=beginModuleDraw(g,ModuleLyrics,moduleBase);
  t->PushAxisAlignedClip(D2D1::RectF(textX,viewportTop,lyricRight(),viewportBottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  drawLyricsAt(g,time,now);
  t->PopAxisAlignedClip();
  endModuleDraw(g,lyricMoved,moduleBase);
  if(hover&&!through){
   float y=transport.y;g.brush->SetColor(D2D1::ColorF(.55f,.58f,.69f));t->DrawLine(D2D1::Point2F(transport.left,y),D2D1::Point2F(transport.right,y),g.brush.get(),3);
   float ratio=float(dragProgress?dragRatio:duration>0?clamp(current()/duration,0,1):0);g.brush->SetColor(D2D1::ColorF(.91f,.93f,.99f));t->DrawLine(D2D1::Point2F(transport.left,y),D2D1::Point2F(transport.left+transport.length()*ratio,y),g.brush.get(),3);t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(transport.left+transport.length()*ratio,y),3,3),g.brush.get());
   // Transport buttons use vectors, not font glyphs with different baselines.
   float cy=y,previous=transport.previous,next=transport.next,play=transport.play;
   g.brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
   t->DrawLine(D2D1::Point2F(previous+3,cy-5),D2D1::Point2F(previous-3,cy),g.brush.get(),2);
   t->DrawLine(D2D1::Point2F(previous-3,cy),D2D1::Point2F(previous+3,cy+5),g.brush.get(),2);
   t->DrawLine(D2D1::Point2F(next-3,cy-5),D2D1::Point2F(next+3,cy),g.brush.get(),2);
   t->DrawLine(D2D1::Point2F(next+3,cy),D2D1::Point2F(next-3,cy+5),g.brush.get(),2);
   if(playing){float halfGap=std::round(2*scale())/scale(),barWidth=std::round(3*scale())/scale(),halfHeight=std::round(7*scale())/scale();t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(play-halfGap-barWidth,cy-halfHeight,play-halfGap,cy+halfHeight),.6f,.6f),g.brush.get());t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(play+halfGap,cy-halfHeight,play+halfGap+barWidth,cy+halfHeight),.6f,.6f),g.brush.get());}
   else {D2D1_MATRIX_3X2_F transform;t->GetTransform(&transform);t->SetTransform(D2D1::Matrix3x2F::Translation(play,cy)*transform);t->FillGeometry(playGeometry.get(),g.brush.get());t->SetTransform(transform);}
  }
  if(!through){g.brush->SetColor(D2D1::ColorF(1,1,1,hover?.5f:.2f));for(int line=1;line<=3;line++){float d=3.f+line*4.5f;t->DrawLine(D2D1::Point2F(width-d,height-3),D2D1::Point2F(width-3,height-d),g.brush.get(),1);}}
  drawSplitters(g);
  drawDragOverlay(g);
  drawLayoutPanel(g);
 }
 // While a module swap animates, draw it translated from its previous slot and
 // clipped to the moving rectangle, so the two modules glide into place.
 bool beginModuleDraw(Graphics&g,int module,const D2D1_MATRIX_3X2_F&base){
  auto*t=g.target;if(slotSwap.a<0||!slotSwap.p.active(animationWall()))return false;
  bool isA=module==slotSwap.a,isB=module==slotSwap.b;if(!isA&&!isB)return false;
  const auto&from=isA?slotSwap.fromA:slotSwap.fromB;const auto&to=isA?slotSwap.toA:slotSwap.toB;
  float p=slotSwap.p.value(animationWall()),rest=1-p;
  float dx=((from.left+from.right)-(to.left+to.right))*.5f*rest,dy=((from.top+from.bottom)-(to.top+to.bottom))*.5f*rest;
  t->SetTransform(D2D1::Matrix3x2F::Translation(dx,dy)*base);
  t->PushAxisAlignedClip(to,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  return true;
 }
 void endModuleDraw(Graphics&g,bool active,const D2D1_MATRIX_3X2_F&base){if(!active)return;g.target->PopAxisAlignedClip();g.target->SetTransform(base);}
 void drawDragOverlay(Graphics&g){
  if(!moduleDragging||pressSlot<0)return;auto*t=g.target;
  // The lifted source stays dimmed while its highlight glides to the target.
  const auto&src=slotRects[pressSlot];g.brush->SetColor(D2D1::ColorF(0,0,0,.28f));t->FillRoundedRectangle(D2D1::RoundedRect(src,12,12),g.brush.get());
  if(dragHighlight.visible){
   float p=dragHighlight.p.value(animationWall());auto dst=lerpRect(dragHighlight.from,dragHighlight.to,p);
   g.brush->SetColor(D2D1::ColorF(1,1,1,.06f));t->FillRoundedRectangle(D2D1::RoundedRect(dst,12,12),g.brush.get());
   g.brush->SetColor(D2D1::ColorF(1,1,1,.5f));t->DrawRoundedRectangle(D2D1::RoundedRect(dst,12,12),g.brush.get(),1.5f);
  }
  Layout chip=layout(moduleLabel(slotModule[pressSlot]),playlistMetaFormat.get());
  float w=chip.width+18,h=chip.height+10,x=float(clamp(dragX+12.f,8.f,width-w-8.f)),y=float(clamp(dragY-10.f,8.f,height-h-8.f));
  g.brush->SetColor(D2D1::ColorF(0,0,0,.35f));t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x,y+1.5f,x+w,y+h+1.5f),9,9),g.brush.get());
  g.brush->SetColor(D2D1::ColorF(.14f,.15f,.20f,.94f));t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x,y,x+w,y+h),9,9),g.brush.get());
  g.brush->SetColor(D2D1::ColorF(1,1,1,.20f));t->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x,y,x+w,y+h),9,9),g.brush.get(),1);
  text(g,chip,x+9,y+5,D2D1::ColorF(1,1,1),.95f);
 }
 void drawTitle(Graphics&g){
  auto*t=g.target;D2D1_COLOR_F color=D2D1::ColorF(.87f,.89f,.94f);
  if(!slotLayout){
   bool wide=(!hover||through)&&metaWide.value;const Layout&m=wide?metaWide:metaLayout;
   text(g,m,textX+alignmentOffset(titleAvailable(wide),m.width,alignTitle),12,color);
  }else{
   int slot=slotOfModule(ModuleTitle);const auto&r=slotRects[slot];
   // The left column keeps its vertical icon stack, so a title there must stay
   // above it; other slots reserve the icon strip horizontally instead.
   float iconsTop=slot==0?r.bottom-8-verticalIconStep(r)*7:r.bottom;
   if(titleVerticalActive()){
    if(metaVertical.value){
     float x=r.left+1+alignmentOffset(r.right-r.left-2,metaVertical.width,alignTitle);
     float bottom=std::max(r.top+2.f,iconsTop-4);
     if(bottom>r.top+2.f){
      t->PushAxisAlignedClip(D2D1::RectF(r.left+1,r.top+2,r.right-1,bottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
      text(g,metaVertical,x,r.top+4,color);
      t->PopAxisAlignedClip();
     }
    }
   }else{
    bool wide=(!hover||through)&&metaWide.value;const Layout&m=wide?metaWide:metaLayout;
    float x=r.left+alignmentOffset(titleAvailable(wide),m.width,alignTitle);
    float y=slot==0?r.top+4:r.top+(r.bottom-r.top-m.height)*.5f;
    float bottom=std::max(r.top+2.f,(slot==0?iconsTop:r.bottom)-2);
    if(bottom>r.top+2.f){
     t->PushAxisAlignedClip(D2D1::RectF(r.left+1,r.top+1,r.right-1,bottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
     text(g,m,x,y,color);
     t->PopAxisAlignedClip();
    }
   }
  }
  if(hover&&!through)drawIcons(g);
 }
 int iconAt(float x,float y)const{
  if(through||!hover)return -1;
  if(!slotLayout){
   if(y>=0&&y<=32&&x>=width-150){int i=int((x-(width-150))/24);return i>=0&&i<6?i+1:-1;}
   return -1;
  }
  int slot=slotOfModule(ModuleTitle);const auto&r=slotRects[slot];
  if(slot==0){
   float step=verticalIconStep(r),top=r.bottom-8-step*7,cx=(r.left+r.right)*.5f;
   if(std::abs(x-cx)<=10&&y>=top&&y<=r.bottom){int i=int((y-top)/step);return i>=0&&i<7?i:-1;}
  }else{
   float zoneLeft=r.right-174;
   if(y>=r.top&&y<=r.bottom&&x>=zoneLeft&&x<=r.right){int i=int((x-zoneLeft)/24);return i>=0&&i<7?i:-1;}
  }
  return -1;
 }
 void drawIcons(Graphics&g){
  auto*t=g.target;int count=iconCount();
  float step=24.f,cx=0.f,cy=0.f;bool vertical=slotLayout&&slotModule[0]==ModuleTitle;
  if(!slotLayout){cx=width-138;cy=20;}
  else{
   int slot=slotOfModule(ModuleTitle);const auto&r=slotRects[slot];
   if(vertical){step=verticalIconStep(r);cx=(r.left+r.right)*.5f;cy=r.bottom-8-step*7+step*.5f;}
   else{cx=r.right-162;cy=r.top+(r.bottom-r.top)*.5f;}
  }
  for(int i=0;i<count;i++){
   float x=vertical?cx:cx+step*i,y=vertical?cy+step*i:cy;
   drawIcon(g,iconActionOf(i),x,y);
  }
 }
 void drawIcon(Graphics&g,int action,float x,float y){
  auto*t=g.target;float acc=.82f;
  if(action==0){
   g.brush->SetColor(D2D1::ColorF(1,1,1,.82f));
   t->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),4.4f,4.4f),g.brush.get(),1.1f);
   t->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),1.7f,1.7f),g.brush.get(),1);
   for(int k=0;k<8;k++){float a=float(k)*3.14159265f/4;float c=std::cos(a),s=std::sin(a);t->DrawLine(D2D1::Point2F(x+c*4.4f,y+s*4.4f),D2D1::Point2F(x+c*6.3f,y+s*6.3f),g.brush.get(),1.2f);}
   return;
  }
  if(action==1){
   D2D1_MATRIX_3X2_F transform;t->GetTransform(&transform);t->SetTransform(D2D1::Matrix3x2F::Translation(x,y)*transform);
   if(liked){g.brush->SetColor(D2D1::ColorF(1.f,.36f,.47f));t->FillGeometry(likeGeometry.get(),g.brush.get());}
   else{g.brush->SetColor(D2D1::ColorF(1,1,1,acc));t->DrawGeometry(likeGeometry.get(),g.brush.get(),1.2f);}
   t->SetTransform(transform);return;
  }
  if(action==2){
   g.brush->SetColor(D2D1::ColorF(1,1,1,vinylRequested?1.f:acc));
   t->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),5.5f,5.5f),g.brush.get(),1.1f);
   t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),1.5f,1.5f),g.brush.get());return;
  }
  if(action==3){
   g.brush->SetColor(D2D1::ColorF(1,1,1,playlistOpen?1.f:acc));
   for(int line=0;line<3;line++){float ly=y-4+line*4;t->DrawLine(D2D1::Point2F(x-3,ly),D2D1::Point2F(x+5,ly),g.brush.get(),1.4f);t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x-6,ly),1.1f,1.1f),g.brush.get());}
   return;
  }
  if(action==4){
   g.brush->SetColor(D2D1::ColorF(1,1,1,top?1.f:.4f));
   t->DrawLine(D2D1::Point2F(x-3,y-5),D2D1::Point2F(x+3,y-5),g.brush.get(),2);t->DrawLine(D2D1::Point2F(x,y-7),D2D1::Point2F(x,y+5),g.brush.get(),1.5f);return;
  }
  if(action==5){
   g.brush->SetColor(D2D1::ColorF(1,1,1));
   t->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x-4,y-2,x+4,y+5),1,1),g.brush.get(),1);t->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x,y-3),3,4),g.brush.get(),1);return;
  }
  g.brush->SetColor(D2D1::ColorF(1,1,1));
  t->DrawLine(D2D1::Point2F(x-3,y-4),D2D1::Point2F(x+3,y+2),g.brush.get(),1.5f);t->DrawLine(D2D1::Point2F(x+3,y-4),D2D1::Point2F(x-3,y+2),g.brush.get(),1.5f);
 }
 void drawSplitters(Graphics&g){
  int active=splitterDrag?splitterDrag:splitterHover;
  if(!slotLayout||layoutPanel||through||!active)return;
  auto*t=g.target;g.brush->SetColor(D2D1::ColorF(1,1,1,splitterDrag?.5f:.42f));
  if(active==1){float vx=slotRects[0].right+SlotGap*.5f;t->DrawLine(D2D1::Point2F(vx,slotRects[0].top),D2D1::Point2F(vx,slotRects[0].bottom),g.brush.get(),1);}
  else{float hy=slotRects[1].bottom+SlotGapY*.5f;t->DrawLine(D2D1::Point2F(slotRects[1].left,hy),D2D1::Point2F(slotRects[1].right,hy),g.brush.get(),1);}
 }
 int splitterAt(float x,float y)const{
  if(!slotLayout||miniMode||layoutPanel)return 0;
  if(std::abs(x-(slotRects[0].right+SlotGap*.5f))<=4&&y>=slotRects[0].top&&y<=slotRects[0].bottom)return 1;
  if(std::abs(y-(slotRects[1].bottom+SlotGapY*.5f))<=3&&x>=slotRects[1].left&&x<=slotRects[1].right)return 2;
  return 0;
 }
 struct LayoutPanelGeometry {D2D1_RECT_F card,close,tab[2],chips[3][3],fontMinus[2],fontPlus[2],align[4][3];};
 LayoutPanelGeometry panelGeometry()const{
  LayoutPanelGeometry p{};
  float w=std::min(width-20.f,380.f),h=std::min(height-12.f,154.f);
  float x=(width-w)*.5f,y=(height-h)*.5f;
  p.card=D2D1::RectF(x,y,x+w,y+h);p.close=D2D1::RectF(x+w-26,y+7,x+w-8,y+25);
  // Two tabs replace the former heading: layout (slots/fonts) and alignment.
  p.tab[0]=D2D1::RectF(x+10,y+6,x+58,y+28);p.tab[1]=D2D1::RectF(x+62,y+6,x+110,y+28);
  float contentTop=y+42.f;
  float rowH=std::min(36.f,(h-46.f)/3.f),left=x+12.f,labelW=42.f,gap=6.f;
  float colRight=x+w-12.f,leftW=(colRight-142.f-8.f)-left;
  float chipW=std::min(66.f,(leftW-labelW-2*gap)/3.f);
  for(int s=0;s<3;s++)for(int c=0;c<3;c++){float cx=left+labelW+c*(chipW+gap),cy=contentTop+s*rowH;p.chips[s][c]=D2D1::RectF(cx,cy,cx+chipW,cy+rowH-8.f);}
  for(int i=0;i<2;i++){
   float cy=contentTop+i*rowH+(rowH-8.f)*.5f,bh=std::min(22.f,std::max(16.f,rowH-10.f));
   p.fontPlus[i]=D2D1::RectF(colRight-20,cy-bh*.5f,colRight,cy+bh*.5f);
   p.fontMinus[i]=D2D1::RectF(colRight-66,cy-bh*.5f,colRight-46,cy+bh*.5f);
  }
  // Alignment tab: two columns of two label+triplet rows. Item order is
  // 0 title direction, 1 cover, 2 title, 3 lyrics.
  float alignH=std::min(44.f,(h-46.f)/2.f),aLabelW=54.f,aGap=4.f;
  float colW=(w-24.f)*.5f,aChipW=std::min(42.f,(colW-aLabelW-2*aGap)/3.f);
  for(int c=0;c<2;c++){
   float colX=x+12.f+c*colW;
   for(int r=0;r<2;r++){
    int item=c*2+r;float cy=contentTop+r*alignH;
    for(int i=0;i<3;i++){float cx=colX+aLabelW+i*(aChipW+aGap);p.align[item][i]=D2D1::RectF(cx,cy,cx+aChipW,cy+alignH-8.f);}
   }
  }
  return p;
 }
 bool panelTabAt(float x,float y,int&tab)const{
  auto p=panelGeometry();
  for(int i=0;i<2;i++){const auto&r=p.tab[i];if(x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom){tab=i;return true;}}
  return false;
 }
 bool panelChipAt(float x,float y,int&slot,int&module)const{
  auto p=panelGeometry();
  for(int s=0;s<3;s++)for(int c=0;c<3;c++){const auto&r=p.chips[s][c];if(x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom){slot=s;module=c==0?ModuleCover:c==1?ModuleLyrics:ModuleTitle;return true;}}
  return false;
 }
 bool panelFontAt(float x,float y,int&index,int&delta)const{
  auto p=panelGeometry();
  for(int i=0;i<2;i++){
   const auto&m=p.fontMinus[i],&pl=p.fontPlus[i];
   if(x>=m.left&&x<=m.right&&y>=m.top&&y<=m.bottom){index=i;delta=-1;return true;}
   if(x>=pl.left&&x<=pl.right&&y>=pl.top&&y<=pl.bottom){index=i;delta=1;return true;}
  }
  return false;
 }
 // item: 0 title direction, 1 cover, 2 title, 3 lyrics.
 bool panelAlignAt(float x,float y,int&item,int&index)const{
  auto p=panelGeometry();
  for(int r=0;r<4;r++)for(int i=0;i<3;i++){const auto&box=p.align[r][i];if(x>=box.left&&x<=box.right&&y>=box.top&&y<=box.bottom){item=r;index=i;return true;}}
  return false;
 }
 bool panelCloseAt(float x,float y)const{auto p=panelGeometry();return x>=p.close.left&&x<=p.close.right&&y>=p.close.top&&y<=p.close.bottom;}
 bool panelInside(float x,float y)const{auto p=panelGeometry();return x>=p.card.left&&x<=p.card.right&&y>=p.card.top&&y<=p.card.bottom;}
 void drawLayoutPanel(Graphics&g){
  if(!layoutPanel)return;
  auto*t=g.target;auto p=panelGeometry();auto*format=playlistMetaFormat.get();auto card=D2D1::RoundedRect(p.card,14,14);
  float open=panelOpenProgress();
  D2D1_MATRIX_3X2_F savedTransform;t->GetTransform(&savedTransform);
  if(open<1){float scale=.955f+.045f*open;t->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale,D2D1::Point2F((p.card.left+p.card.right)*.5f,(p.card.top+p.card.bottom)*.5f))*D2D1::Matrix3x2F::Translation(0,(1-open)*6.f)*savedTransform);}
  g.brush->SetColor(D2D1::ColorF(.09f,.10f,.14f,.98f));t->FillRoundedRectangle(card,g.brush.get());
  g.brush->SetColor(D2D1::ColorF(.62f,.65f,.80f,.6f));t->DrawRoundedRectangle(card,g.brush.get(),1);
  // Compact close icon: smaller hit area with a thicker stroke.
  float ins=4.5f;g.brush->SetColor(D2D1::ColorF(1,1,1,.58f));
  t->DrawLine(D2D1::Point2F(p.close.left+ins,p.close.top+ins),D2D1::Point2F(p.close.right-ins,p.close.bottom-ins),g.brush.get(),2);
  t->DrawLine(D2D1::Point2F(p.close.right-ins,p.close.top+ins),D2D1::Point2F(p.close.left+ins,p.close.bottom-ins),g.brush.get(),2);
  // Tabs replace the former plain heading: layout (slots/fonts) and alignment.
  const wchar_t* tabs[2]={L"布局",L"对齐"};
  for(int i=0;i<2;i++){
   const auto&r=p.tab[i];bool active=panelTab==i;
   if(active){g.brush->SetColor(D2D1::ColorF(1,1,1,.16f));t->FillRoundedRectangle(D2D1::RoundedRect(r,7,7),g.brush.get());}
   Layout tabLabel=layout(tabs[i],format);
   text(g,tabLabel,r.left+(r.right-r.left-tabLabel.width)*.5f,r.top+(r.bottom-r.top-tabLabel.height)*.5f,active?D2D1::ColorF(1,1,1):D2D1::ColorF(.72f,.75f,.84f),active?1.f:.85f);
  }
  g.brush->SetColor(D2D1::ColorF(1,1,1,.08f));
  t->DrawLine(D2D1::Point2F(p.card.left+10,p.card.top+34),D2D1::Point2F(p.card.right-10,p.card.top+34),g.brush.get(),1);
  auto drawChip=[&](const D2D1_RECT_F&r,const wchar_t*label,bool active,float radius){
   g.brush->SetColor(D2D1::ColorF(1,1,1,active?.20f:.06f));t->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),g.brush.get());
   if(active){g.brush->SetColor(D2D1::ColorF(1,1,1,.55f));t->DrawRoundedRectangle(D2D1::RoundedRect(r,radius,radius),g.brush.get(),1);}
   Layout chip=layout(label,format);
   text(g,chip,r.left+(r.right-r.left-chip.width)*.5f,r.top+(r.bottom-r.top-chip.height)*.5f,active?D2D1::ColorF(1,1,1):D2D1::ColorF(.80f,.83f,.90f),active?1.f:.85f);
  };
  if(panelTab==0){
   g.brush->SetColor(D2D1::ColorF(1,1,1,.08f));
   float div=p.fontMinus[0].left-54.f;
   t->DrawLine(D2D1::Point2F(div,p.card.top+40),D2D1::Point2F(div,p.card.bottom-10),g.brush.get(),1);
   for(int s=0;s<3;s++){
    Layout label=layout(s==0?L"左槽":s==1?L"右上":L"右下",format);
    text(g,label,p.card.left+12,p.chips[s][0].top+(p.chips[s][0].bottom-p.chips[s][0].top-label.height)*.5f,D2D1::ColorF(.72f,.75f,.84f),.9f);
    for(int c=0;c<3;c++){
     int module=c==0?ModuleCover:c==1?ModuleLyrics:ModuleTitle;
     drawChip(p.chips[s][c],module==ModuleCover?L"封面":module==ModuleLyrics?L"歌词":L"标题",slotModule[s]==module,8);
    }
   }
   for(int i=0;i<2;i++){
    const auto&m=p.fontMinus[i],&pl=p.fontPlus[i];
    Layout label=layout(i==0?L"标题字号":L"歌词字号",format);
    text(g,label,m.left-8-label.width,(m.top+m.bottom-label.height)*.5f,D2D1::ColorF(.72f,.75f,.84f),.9f);
    for(int b=0;b<2;b++){
     const auto&r=b==0?m:pl;g.brush->SetColor(D2D1::ColorF(1,1,1,.08f));t->FillRoundedRectangle(D2D1::RoundedRect(r,6,6),g.brush.get());
     g.brush->SetColor(D2D1::ColorF(1,1,1,.20f));t->DrawRoundedRectangle(D2D1::RoundedRect(r,6,6),g.brush.get(),1);
     float cx=(r.left+r.right)*.5f,cy=(r.top+r.bottom)*.5f;
     g.brush->SetColor(D2D1::ColorF(1,1,1,.88f));t->DrawLine(D2D1::Point2F(cx-4,cy),D2D1::Point2F(cx+4,cy),g.brush.get(),1.6f);
     if(b==1)t->DrawLine(D2D1::Point2F(cx,cy-4),D2D1::Point2F(cx,cy+4),g.brush.get(),1.6f);
    }
    std::wstring value=std::to_wstring(int(std::lround(i==0?titleFontSize:currentFontSize)));
    Layout valueLayout=layout(value,format);
    float center=(m.right+pl.left)*.5f;
    text(g,valueLayout,center-valueLayout.width*.5f,(m.top+m.bottom-valueLayout.height)*.5f,D2D1::ColorF(.90f,.92f,.97f));
   }
  }else{
   g.brush->SetColor(D2D1::ColorF(1,1,1,.08f));
   float div=(p.card.left+p.card.right)*.5f-4.f;
   t->DrawLine(D2D1::Point2F(div,p.card.top+40),D2D1::Point2F(div,p.card.bottom-10),g.brush.get(),1);
   const wchar_t* labels[4]={L"标题方向",L"封面对齐",L"标题对齐",L"歌词对齐"};
   for(int item=0;item<4;item++){
    const auto&first=p.align[item][0];
    Layout label=layout(labels[item],format);
    text(g,label,first.left-6-label.width,first.top+(first.bottom-first.top-label.height)*.5f,D2D1::ColorF(.72f,.75f,.84f),.9f);
    int value=item==0?titleOrientation:item==1?alignCover:item==2?alignTitle:alignLyrics;
    for(int i=0;i<3;i++){
     const wchar_t*name=item==0?(i==0?L"自动":i==1?L"横排":L"竖排"):(i==0?L"左":i==1?L"中":L"右");
     drawChip(p.align[item][i],name,value==i,7);
    }
   }
  }
  t->SetTransform(savedTransform);
 }
 void stopFrames(){if(frameRunning||frameArmed){KillTimer(hwnd,1);if(frameTimer)CancelWaitableTimer(frameTimer);}frameArmed=false;frameRunning=false;frameDeadline=0;}
 void setRenderVisible(bool visible){
  if(renderVisible==visible)return;renderVisible=visible;
  if(!visible){indicatorMotion.setRunning(false,animationWall());flight=LyricFlight();stopBrowsing();snapRow=true;layoutDirty=true;}
  update();scheduleLayoutWarmup();
 }
 void schedule(){
  if(!renderVisible){stopFrames();return;}
  double time=current()+offset,next=200;bool swapping=swapAnimating()||dragHighlight.p.active(animationWall());bool active=manualTransition||swapping||(playing&&valid&&(transitionActive||lyricView()==LyricView::Prelude));
  if(playing&&valid&&!untimed&&index+1<int(lines.size())){next=std::min(next,std::max(1.,lines[index+1].time-time));if(browseIndex<0&&stagger&&focusIndex<=index)next=std::min(next,std::max(1.,lines[index+1].time-time-200));}
  if(playing&&valid&&!untimed)for(const auto&item:groups){const auto&g=item.second;if(!groupVisible(g,motionNow()))continue;const auto&line=g.content->model;
   if(indicatorVisible(g))active=true;
   if(g.active)for(const auto&word:line.words){if(time>=word.time&&time<word.time+word.duration+150)active=true;else if(word.time>time)next=std::min(next,word.time-time);}
   if(canScroll(item.first)&&((g.content->glyphs.empty()&&rowWidth(g)*g.zoom.value(motionNow())>lyricWidth())||(g.translated&&g.content->subtitle.width*g.zoom.value(motionNow())>lyricWidth()))){double begin=line.time+900,end=begin+std::max(1200.,(line.duration>0?line.duration:4000)-1600);if(time<begin)next=std::min(next,begin-time);else if(time<end)active=true;}
  }
  bool flying=!flight.rows.empty(),coverAnimating=coverSince>=0&&nowMs()-coverSince<250;active=active||coverAnimating||flying;
  bool backgroundActive=backgroundAnimating();bool run=flying||manualTransition||swapping||(playing&&valid&&((!untimed&&!lines.empty())||hover||coverAnimating||backgroundActive));double wanted=active?1000./(fps==60?60:30):std::max(1.,next);if(backgroundActive)wanted=std::min(wanted,1000./background.fps);
  if(!run){stopFrames();return;}
  if(frameTimer&&(!systemMoving||modalFrames.active())){
   bool same=frameRunning&&std::abs(framePeriod-wanted)<.001;if(same&&frameArmed)return;
   double now=nowMs();if(!same||frameDeadline<=0)frameDeadline=now+wanted;else{frameDeadline+=wanted;if(now-frameDeadline>wanted*2)frameDeadline=now+wanted;}
   LARGE_INTEGER due;due.QuadPart=-LONGLONG(std::max(.2,frameDeadline-now)*10000);
   if(SetWaitableTimer(frameTimer,&due,0,nullptr,nullptr,FALSE)){frameArmed=true;frameRunning=true;framePeriod=wanted;intervalMs=UINT(std::ceil(wanted));return;}
   modalFrames.stop();CloseHandle(frameTimer);frameTimer=nullptr;frameArmed=false;frameRunning=false;
  }
  UINT period=UINT(std::ceil(wanted));if(!frameRunning||period!=intervalMs){KillTimer(hwnd,1);SetTimer(hwnd,1,period,nullptr);}intervalMs=period;framePeriod=wanted;frameRunning=true;
 }
 void update(){
  if(valid)hasClock=true;
  if(slotSwap.a>=0&&!slotSwap.p.active(animationWall()))slotSwap.a=slotSwap.b=-1;
  if(!renderVisible||(!valid&&!untimed))manualTransition=manualLayoutPending=false;
  // Browse animation can run while paused; the playback and background clocks stay separate.
  if(browseIndex>=0&&(lines.empty()||(!untimed&&animationWall()>=browseUntil))){
   bool animateReturn=renderVisible&&valid&&!untimed&&!lines.empty();
   if(animateReturn){int actual=rowAt(current()+offset),desired=std::max(0,actual<0?preludePreview:rowAt(current()+offset+(playing&&stagger?200:0)));
    if(layoutDistance(desired,focusIndex,4)>3){beginFlight(desired,actual);flight.stagger=false;animateReturn=false;}
   }
   stopBrowsing(animateReturn);
  }
  motion.setRunning(renderVisible&&((valid&&!untimed&&playing)||manualTransition),animationWall());backgroundClock.update(backgroundAnimating(),background.speed,animationWall());
  if(!renderVisible){stopFrames();return;}
  if(!valid&&!flight.rows.empty()){flight=LyricFlight();layoutDirty=true;}
  if(browseIndex>=0){int normalized=browseFocus(browseIndex);if(normalized<0)stopBrowsing();else if(normalized!=browseIndex){browseIndex=normalized;layoutDirty=true;}}
  double time=current()+offset;int next=staticPreview()?-1:rowAt(time);bool actualChanged=next!=index;if(next<index&&flight.rows.empty())snapRow=true;index=next;
  if(!flight.rows.empty()){
   if(flight.rows.size()!=lines.size())flight=LyricFlight();
   else{int desired=std::max(0,index<0?preludePreview:rowAt(time+(playing&&valid&&stagger?200:0)));if(desired!=flight.focus||flight.playbackIndex!=index)beginFlight(desired,index);if(actualChanged)roles();applyFlight(animationWall());snapRow=false;returnFromBrowse=false;transitionActive=false;
    if(flight.running(animationWall())){browseDotsFade.target(browseIndex>=0?0.f:1.f,animationWall(),0,200,0);updateIndicators();InvalidateRect(hwnd,nullptr,FALSE);schedule();return;}flight=LyricFlight();layoutDirty=true;
   }
  }
  // A layout snap (including a small clock correction across a line boundary)
  // must not cancel the user's browse session. Explicit seeks and track changes
  // already leave browse mode at their own entry points.
  int desired=focusIndex;
  if(browseIndex>=0)desired=browseIndex=std::min(browseIndex,int(lines.size())-1);
  else if(staticPreview())desired=untimed&&focusIndex>=0?focusIndex:std::max(0,preludePreview);
  else if(lyricView()==LyricView::Prelude)desired=std::max(0,preludePreview);
  else if(snapRow||focusIndex<0||groups.empty()||(!playing&&actualChanged))desired=std::max(0,index);
  else if(returnFromBrowse)desired=std::max(0,playing&&valid?rowAt(time+(stagger?200:0)):index);
  else if(playing&&valid)desired=std::max(0,rowAt(time+(stagger?200:0)));
  bool animate=((playing&&valid&&!untimed)||manualLayoutPending)&&!snapRow&&focusIndex>=0&&layoutDistance(desired,focusIndex,4)<=3;
  if(snapRow)layoutDirty=true;
  // Prelude and the first active row can share an index but use different anchors.
  bool reflowed=layoutDirty||desired!=focusIndex||groups.empty()||layoutAnchor!=rowAnchor();if(reflowed)reflow(desired,animate);
  if(actualChanged&&!reflowed)roles();
  if(snapRow||(!playing&&browseIndex<0&&actualChanged))for(auto&item:groups){auto&g=item.second;g.mainAlpha.snap(g.mainAlpha.to);g.subAlpha.snap(g.subAlpha.to);if(snapRow)g.resetWords.clear();}
  snapRow=false;returnFromBrowse=false;transitionActive=false;double now=motionNow();
  updateIndicators();
  if(lyricView()==LyricView::Rows)for(const auto&item:groups){
   const auto&g=item.second;
   bool hasContent=!emptyGroup(g)||indicatorVisible(g);
   if(!hasContent&&!g.middleInterlude)continue;
   bool visible=groupVisible(g,now);
   bool crosses=std::min(g.y.from,g.y.to)<=viewportBottom&&std::max(g.y.from,g.y.to)+g.renderedHeight(std::max(g.zoom.from,g.zoom.to))>=viewportTop;
   bool contentAnimating=hasContent&&(g.mainAlpha.active(now)||g.subAlpha.active(now)||(!g.active&&!g.resetWords.empty()&&now-g.resetAt<200));
   transitionActive=transitionActive||(g.y.active(now)&&crosses)||(visible&&(g.zoom.active(now)||contentAnimating));
  }
  bool layoutActive=false;for(const auto&item:groups){const auto&g=item.second;layoutActive=layoutActive||g.y.active(now)||g.zoom.active(now);}
  manualTransition=manualTransition&&layoutActive;manualLayoutPending=false;
  browseDotsFade.target(browseIndex>=0?0.f:1.f,animationWall(),0,200,0);
  motion.setRunning(renderVisible&&((valid&&!untimed&&playing)||manualTransition),animationWall());
  InvalidateRect(hwnd,nullptr,FALSE);schedule();
 }
 void paint(){PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);if(renderVisible)try{ensureTarget();renderCalls++;target->BeginDraw();draw(graphics);HRESULT hr=target->EndDraw();if(hr==static_cast<HRESULT>(D2DERR_RECREATE_TARGET)){graphics.clear();target.reset();}else if(FAILED(hr))error="draw-failed";else if(IsWindowVisible(hwnd)&&!target->present(hwnd))error="present-failed";}catch(...){graphics.clear();target.reset();error="render-target-failed";}EndPaint(hwnd,&ps);}
 void capture(){
  if(capturePath.empty())return;try{
   Com<IWICBitmap> bitmap;check(imaging->CreateBitmap(UINT(width*scale()),UINT(height*scale()),GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,bitmap.put()));
   Com<ID2D1RenderTarget> offscreen;check(factory->CreateWicBitmapRenderTarget(bitmap.get(),D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),dpi,dpi),offscreen.put()));
   Graphics g;g.init(offscreen.get(),bgA,bgB,cover,width,height);offscreen->BeginDraw();draw(g);check(offscreen->EndDraw());
   Com<IWICStream> stream;check(imaging->CreateStream(stream.put()));check(stream->InitializeFromFilename(capturePath.c_str(),GENERIC_WRITE));Com<IWICBitmapEncoder> encoder;check(imaging->CreateEncoder(GUID_ContainerFormatPng,nullptr,encoder.put()));check(encoder->Initialize(stream.get(),WICBitmapEncoderNoCache));
   Com<IWICBitmapFrameEncode> frame;check(encoder->CreateNewFrame(frame.put(),nullptr));check(frame->Initialize(nullptr));check(frame->SetSize(UINT(width*scale()),UINT(height*scale())));WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;check(frame->SetPixelFormat(&format));Com<IWICFormatConverter> converter;check(imaging->CreateFormatConverter(converter.put()));check(converter->Initialize(bitmap.get(),format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));check(frame->WriteSource(converter.get(),nullptr));check(frame->Commit());check(encoder->Commit());
  }catch(...){error="capture-failed";}
 }
 Json state(const std::string&type,bool accepted=true){
  Json j=Json::obj();j["type"]=type;j["accepted"]=accepted;j["songId"]=songId;j["lyricCount"]=int(lines.size());j["lyricStatus"]=!lyricsReceived?"waiting":lines.empty()?"empty":untimed?"untimed":"timed";j["hasClock"]=hasClock;j["revision"]=revision;j["positionMs"]=current();j["playing"]=playing;j["clockValid"]=valid;j["seekPending"]=seekPending;j["seekRequest"]=double(seekRequest);j["error"]=error;j["connectionStale"]=connectionStale;j["ownerMonitored"]=ownerProcess.monitored();
  j["backgroundBackend"]=fluid.backend();j["backgroundGpuReady"]=fluid.gpuReady();if(!fluid.gpuError.empty())j["backgroundGpuError"]=fluid.gpuError;
  if(type=="background-state")j["background"]=backgroundState();return j;
 }
 Json diagnostics(const std::string&type,bool accepted=true){Json j=state(type,accepted);j["currentLineIndex"]=index;j["index"]=index;j["frameRunning"]=frameRunning;j["intervalMs"]=int(intervalMs);j["renderCalls"]=double(renderCalls);j["lineBuildCalls"]=double(lineBuildCalls);j["backend"]="Direct2D/DirectWrite";j["hardware"]=hardware;j["renderVisible"]=renderVisible;j["through"]=through;j["top"]=top;j["liked"]=liked;j["windowTopmost"]=(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;j["windowNoActivate"]=(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_NOACTIVATE)!=0;j["fps"]=fps;j["fontSize"]=currentFontSize;j["systemMoving"]=systemMoving;j["frameScheduler"]=frameTimer&&(!systemMoving||modalFrames.active())?(systemMoving?"waitable-posted":"waitable-deadline"):"window-timer";j["coverLoaded"]=!cover.pixels.empty();j["dpi"]=double(dpi);j["coverLeftPx"]=std::round(artBounds.left*scale());j["coverTopPx"]=std::round(artBounds.top*scale());j["coverSidePx"]=std::round(artSize*scale());j["coverRadiusPx"]=std::round(artRadius*scale());j["windowHeightPx"]=std::round(height*scale());j["iconBackend"]="vector";j["windowWidthPx"]=std::round(width*scale());j["barLeftPx"]=transport.left*scale();j["barRightPx"]=transport.right*scale();j["controlYPx"]=transport.y*scale();j["previousPx"]=transport.previous*scale();j["playPx"]=transport.play*scale();j["nextPx"]=transport.next*scale();j["rowStartedMs"]=rowSince;j["focusIndex"]=focusIndex;j["browsing"]=browseIndex>=0;j["browseIndex"]=browseIndex;j["browseRemainingMs"]=browseIndex>=0?std::max(0.,browseUntil-animationWall()):0;j["groupCacheLimit"]=32;j["transitionActive"]=transitionActive;j["transitionProgress"]=unit((motionNow()-rowSince-200)/500);j["transitionMode"]=transitionActive?"group-scroll":"none";j["animationTimeMs"]=motionNow();j["retainedRows"]=int(groups.size());j["layoutChanges"]=double(layoutChanges);j["lookaheadMs"]=stagger?200:0;j["stagger"]=stagger;j["curve"]=lineCurve;j["viewportTop"]=viewportTop;j["viewportBottom"]=viewportBottom;j["groupGap"]=GroupGap;j["layoutAnchor"]=layoutAnchor;j["layoutMini"]=miniMode;j["layoutSlots"].kind=Json::Array;for(int i=0;i<3;i++)j["layoutSlots"].array.push_back(Json(slotModule[i]));j["layoutLeftRatio"]=double(leftRatio);j["layoutTopHeight"]=double(topHeight);j["layoutPanelOpen"]=layoutPanel;j["titleFontSize"]=titleFontSize;j["lyricLeftPx"]=textX*scale();j["lyricRightPx"]=lyricRight()*scale();j["metaVerticalWidth"]=metaVertical.width;j["metaVerticalHeight"]=metaVertical.height;j["groups"].kind=Json::Array;
  for(const auto&item:groups){const auto&g=item.second;Json v=Json::obj();v["index"]=item.first;v["identity"]=double(g.identity);v["text"]=narrow(g.content->model.text);v["translation"]=g.translated?narrow(g.content->model.translation):"";v["top"]=g.y.value(motionNow());v["target"]=g.y.to;v["startAt"]=g.y.start;v["durationMs"]=g.y.duration;v["height"]=g.renderedHeight(1.f);v["scale"]=g.zoom.value(motionNow());v["scaleTarget"]=g.zoom.to;v["scaleStartAt"]=g.zoom.start;v["scaleDurationMs"]=g.zoom.duration;v["renderedHeight"]=g.renderedHeight(g.zoom.value(motionNow()));v["fontSize"]=currentFontSize*g.zoom.value(motionNow());v["translationFontSize"]=g.translated?translationFontSize*g.zoom.value(motionNow()):0.f;v["rendered"]=groupVisible(g,motionNow());v["mainHeight"]=g.content->mainHeight;v["subOffset"]=g.subtitleOffset(g.zoom.value(motionNow()));v["mainAlpha"]=g.mainAlpha.value(motionNow());v["subAlpha"]=g.subAlpha.value(motionNow());v["active"]=g.active;j["groups"].array.push_back(std::move(v));}
  j["seekFlight"]=!flight.rows.empty();if(!flight.rows.empty()){j["transitionMode"]="seek-flight";j["transitionActive"]=true;j["transitionProgress"]=unit((animationWall()-flight.started)/(500+(flight.stagger?400:0)));}j["background"]=backgroundState();j["backgroundAnimating"]=backgroundAnimating();j["backgroundFrameBuilds"]=double(fluid.revision);j["backgroundBackend"]=fluid.backend();j["backgroundGpuReady"]=fluid.gpuReady();j["backgroundGpuError"]=fluid.gpuError;j["backgroundTimeMs"]=backgroundClock.time(animationWall());j["first"]=index>=0&&index<int(lines.size())?narrow(lines[index].text):"";j["second"]=index>=0&&index<int(lines.size())?(translation&&!lines[index].translation.empty()?narrow(lines[index].translation):index+1<int(lines.size())?narrow(lines[index+1].text):""):"";j["playlistCount"]=int(playlist.size());j["playlistOpen"]=playlistOpen;return j;}
 void report(const std::string&type,bool accepted=true,bool detailed=false){if(bridge)bridge->send(detailed?diagnostics(type,accepted):state(type,accepted));}
 void command(const std::string&action,double pos=0){if(!bridge)return;Json j=Json::obj();j["type"]="command";j["action"]=action;j["position"]=pos;if(action=="seek"){j["requestId"]=double(seekRequest);j["songId"]=songId;}bridge->send(j);}
 void requestVinylPage(){
  if(!bridge)return;
  vinylRequested=true;
  command("detail");
  SetTimer(hwnd,VinylConfirmTimer,2500,nullptr);
 }
 void hideForVinyl(){
  if(vinylHidden||closing)return;
  KillTimer(hwnd,VinylConfirmTimer);
  vinylRequested=false;vinylHidden=true;
  closePlaylist();
  ShowWindow(hwnd,SW_HIDE);
 }
 void showFromVinyl(){
  vinylRequested=false;
  if(!vinylHidden)return;
  vinylHidden=false;
  ShowWindow(hwnd,SW_SHOWNOACTIVATE);
  update();
 }
 void cancelSeek(){seekPending=false;KillTimer(hwnd,3);}
 void requestSeek(double target){
  if(!bridge)return;
  if(!seekPending)seekFallback=current();seekStarted=nowMs();seekRequest=++seekSerial;seekPending=true;
  SetTimer(hwnd,3,3500,nullptr);
  command("seek",target);
 }
 void stopCoverWorker(){
  if(coverQueue){
   {std::lock_guard<std::mutex> lock(coverQueue->mutex);coverQueue->stop=true;}
   coverQueue->cancelActiveRequest();
   coverQueue->changed.notify_one();
  }
  if(coverWorker.joinable())coverWorker.join();
 }
 void startCoverWorker(){
  stopCoverWorker();
  coverQueue=std::make_shared<CoverQueue>();auto queue=coverQueue;HWND window=hwnd;
  coverWorker=std::thread([queue,window]{
   CoverDownloader downloader;
   for(;;){std::wstring url;uint64_t epoch;
    {std::unique_lock<std::mutex> lock(queue->mutex);queue->changed.wait(lock,[&]{return queue->stop||queue->pending;});if(queue->stop)return;url=queue->url;epoch=queue->epoch;queue->pending=false;}
    std::unique_ptr<CoverData> result;bool superseded=false;
    for(int attempt=0;attempt<3;attempt++){
     result=downloader.fetch(url,epoch,queue.get());
     std::unique_lock<std::mutex> lock(queue->mutex);
     if(queue->stop)return;if(queue->epoch!=epoch){superseded=true;break;}
     if(!result->pixels.empty()||url.empty()||attempt==2)break;
     // Retry the same request, but wake immediately when a new cover or shutdown arrives.
     queue->changed.wait_for(lock,std::chrono::milliseconds(attempt==0?1000:3000),[&]{return queue->stop||queue->epoch!=epoch;});
     if(queue->stop)return;if(queue->epoch!=epoch){superseded=true;break;}
    }
    if(superseded)continue;
    if(PostMessageW(window,CoverReady,0,reinterpret_cast<LPARAM>(result.get())))result.release();
   }
  });
 }
 void loadCover(const std::wstring&url){coverUrl=url;uint64_t epoch=++coverEpoch;if(!coverQueue)return;
  coverQueue->cancelActiveRequest();
  {std::lock_guard<std::mutex> lock(coverQueue->mutex);coverQueue->url=url;coverQueue->epoch=epoch;coverQueue->pending=true;}coverQueue->changed.notify_one();
 }
 void onCover(std::unique_ptr<CoverData> data){if(data->epoch!=coverEpoch||closing)return;cover=std::move(*data);fluid.setCover(cover.pixels,cover.width,cover.height,backgroundClock.time(animationWall()),!backgroundPlaying());coverSince=cover.pixels.empty()?-1000:nowMs();if(!cover.pixels.empty()){uint64_t r=0,g=0,b=0;for(size_t i=0;i<cover.pixels.size();i+=4){b+=cover.pixels[i];g+=cover.pixels[i+1];r+=cover.pixels[i+2];}float count=float(cover.pixels.size()/4);bgA=D2D1::ColorF((r/count*.3f+15)/255,(g/count*.3f+15)/255,(b/count*.3f+22)/255);bgB=D2D1::ColorF(std::min(100/255.f,bgA.r+24/255.f),std::min(100/255.f,bgA.g+17/255.f),std::min(120/255.f,bgA.b+30/255.f));}if(graphics.target){try{graphics.updateCover(cover);graphics.updateGradient(bgA,bgB,width,height);}catch(...){graphics.clear();}}update();if(playlistOpen)paintPlaylist();}
 void freezeConnection(){
  if(connectionStale)return;
  if(vinylHidden)showFromVinyl();
  closePlaylist();
  position=current();anchor=nowMs();playing=false;connectionStale=true;
  cancelSeek();seekRequest=0;flight=LyricFlight();error="disconnected";update();
 }
 void receive(const Json&m){
  received=nowMs();auto type=m["type"].s();
  if(type=="ping"){if(connectionStale||error=="disconnected"){connectionStale=false;if(error=="disconnected")error.clear();update();}}
  else if(type=="track"){
   auto id=m["id"].s();
   if(id!=songId){
    KillTimer(hwnd,LayoutWarmupTimer);nextLayoutToWarm=0;
    flight=LyricFlight();stopBrowsing();songId=id;groups.clear();lyricLayouts.clear();
    focusIndex=-999;layoutDirty=true;snapRow=true;cancelSeek();seekRequest=0;lines.clear();lyricsReceived=false;untimed=false;hasClock=false;updatePlaceholders();index=-999;position=0;revision=0;valid=false;liked=false;error.clear();
   }
   title=widen(m["title"].s());artist=widen(m["artist"].s());duration=std::max(0.,m["duration"].n());rebuildMeta();auto cover=widen(m["cover"].s());if(cover!=coverUrl)loadCover(cover);update();
  }
  else if(type=="lyrics"){
   if(m["songId"].s()!=songId){error="song-mismatch";report("lyrics-ack",false);return;}
   std::vector<Line> next;for(const auto&raw:m["lines"].array){if(next.size()>=5000)break;if(raw.kind!=Json::Object)continue;Line line;line.time=std::max(0.,raw["time"].n());line.duration=std::max(0.,raw["duration"].n());line.text=widen(raw["text"].s());line.isInterlude=raw["isInterlude"].b();line.translation=widen(raw["translation"].s());for(const auto&w:raw["words"].array){if(line.words.size()>=500)break;if(w.kind!=Json::Object)continue;line.words.push_back({widen(w["text"].s()),w["time"].n(),std::max(1.,w["duration"].n(1))});}if(line.text.empty())for(const auto&w:line.words)line.text+=w.text;next.push_back(std::move(line));}
   bool nextUntimed=m["mode"].s()=="untimed",wasReceived=lyricsReceived;lyricsReceived=true;
   if(!nextUntimed)std::stable_sort(next.begin(),next.end(),[](const Line&a,const Line&b){return a.time<b.time;});
   bool contentChanged=next!=lines||nextUntimed!=untimed;
   if(contentChanged){if(nextUntimed!=untimed){stopBrowsing();focusIndex=-999;snapRow=true;}untimed=nextUntimed;bool same=index>=0&&index<int(lines.size())&&index<int(next.size())&&lines[index].time==next[index].time&&lines[index].text==next[index].text;
    flight=LyricFlight();lines=std::move(next);layoutDirty=true;if(!same)snapRow=true;
    lyricLayouts.resize(lines.size());
    for(size_t i=0;i<lyricLayouts.size();++i)if(lyricLayouts[i]&&!(lyricLayouts[i]->model==lines[i]))lyricLayouts[i].reset();
    nextLayoutToWarm=0;
   }
   if(contentChanged||!wasReceived)updatePlaceholders();
   revision=m["revision"].n();error.clear();update();if(contentChanged)scheduleLayoutWarmup();report("lyrics-ack");
  }else if(type=="seek"){
   if(m["songId"].s()!=songId||m["position"].kind!=Json::Number||!std::isfinite(m["position"].n())||m["position"].n()<0)return;
   auto request=uint64_t(m["requestId"].n());if(request&&request!=seekRequest)return;
   if(!request){cancelSeek();seekRequest=0;}seekTo(m["position"].n());update();report("host-state");
  }else if(type=="clock"){
   if(m["songId"].kind==Json::String&&m["songId"].s()!=songId)return;
   if(seekRequest&&uint64_t(m["seekRequest"].n())!=seekRequest)return;
   auto seekState=m["seekState"].s();if(seekPending&&seekState!="pending"&&seekState!="confirmed"&&seekState!="failed")return;
   if(seekState=="confirmed"||seekState=="failed")cancelSeek();
   double incoming=std::max(0.,m["position"].n());bool changed=std::abs(incoming-current())>1500;
   bool animate=!untimed&&valid&&m["valid"].b()&&seekState!="failed"&&seekState!="pending"&&(changed||seekState=="confirmed");
   connectionStale=false;if(error=="disconnected")error.clear();
   playing=m["playing"].b();offset=m["offset"].n();if(m["duration"].n()>0)duration=m["duration"].n();
   bool becomeValid=!valid&&m["valid"].b();
   // Retain the displayed geometry before accepting the new playback anchor.
   if(animate)seekTo(incoming);else{if((becomeValid||(valid&&changed))&&flight.rows.empty())snapRow=true;position=incoming;anchor=nowMs();}
   valid=m["valid"].b();update();report("host-state");}
  else if(type=="style"){int curve=int(clamp(m["curve"].n(),0,3));bool wave=m["stagger"].b(true);if(curve!=lineCurve||wave!=stagger){lineCurve=curve;stagger=wave;layoutDirty=true;update();}}
  else if(type=="playlist"){
   std::vector<PlaylistItem> next;int current=-1,follow=m["current"].kind==Json::Number?int(m["current"].n()):-1;
   for(const auto&raw:m["items"].array){
    if(next.size()>=10000)break;if(raw.kind!=Json::Object)continue;
    PlaylistItem item;item.index=raw["index"].kind==Json::Number?int(raw["index"].n()):int(next.size());
    item.resourceId=raw["resourceId"].s();item.title=widen(raw["title"].s());if(item.title.empty())item.title=L"未知歌曲";
    item.artist=widen(raw["artist"].s());item.duration=std::max(0.,raw["duration"].n());item.current=raw["current"].b();
    if(item.current)current=int(next.size());
    next.push_back(std::move(item));
   }
   if(current<0&&follow>=0&&follow<int(next.size()))current=follow;
   playlist=std::move(next);playlistCurrent=current;playlistLoading=false;playlistUnsupported=m["unsupported"].b();playlistRevision=m["revision"].n();
   if(playlistOpen){
    int visibleBefore=playlistVisibleRows();layoutPlaylistWindow(false);
    if(playlistScroll==0&&playlistCurrent>=visibleBefore)playlistScroll=int(clamp(playlistCurrent-playlistVisibleRows()/2,0,playlistMaxScroll()));
    playlistScroll=int(clamp(playlistScroll,0,playlistMaxScroll()));
    double now=animationWall();
    if(playlistScrollTrack.active(now))playlistScrollTrack.target(float(playlistScroll),now,0,260,3);
    else snapPlaylistScroll();
    positionPlaylist();paintPlaylist();
   }
  }
  else if(type=="nowplaying"){
   bool open=m["open"].b();
   // The lyrics page may be opened by the app itself (for example RNP's own
   // page); hide whenever it is open, not only for our own detail request.
   if(open)hideForVinyl();
   else if(vinylHidden)showFromVinyl();
  }
  else if(type=="liked"){bool next=m["liked"].b();if(next!=liked){liked=next;update();}}
  else if(type=="font"){if(m["size"].kind==Json::Number)setLyricFontSize(m["size"].n());report("host-state");}
  else if(type=="background"){if(readBackground(m)){save();update();}report("background-state");}
  else if(type=="show"){vinylRequested=false;vinylHidden=false;ShowWindow(hwnd,SW_SHOWNOACTIVATE);constrain();}
  else if(type=="unlock"){setThrough(false);update();}
  else if(type=="reset"){setThrough(false);vinylRequested=false;vinylHidden=false;SetWindowPos(hwnd,nullptr,int(80*scale()),int(80*scale()),int(360*scale()),int(130*scale()),SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(hwnd,SW_SHOWNOACTIVATE);save();update();}
  else if(type=="exit")DestroyWindow(hwnd);
  else if(type=="capture")capture();
  else if(type=="test-wheel"&&!capturePath.empty()){POINT p{int(std::round(m["x"].n()*scale())),int(std::round(m["y"].n()*scale()))};ClientToScreen(hwnd,&p);message(WM_MOUSEWHEEL,MAKEWPARAM(0,SHORT(clamp(m["delta"].n(),-12000,12000))),MAKELPARAM(p.x,p.y));report("test-wheel-ack",true,true);}
  else if(type=="test-click"&&!capturePath.empty()){int x=int(std::round(m["x"].n()*scale())),y=int(std::round(m["y"].n()*scale()));message(WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));message(WM_LBUTTONUP,0,MAKELPARAM(x,y));report("test-click-ack",true,true);}
  else if(type=="test-mouse"&&!capturePath.empty()){
   int x=int(std::round(m["x"].n()*scale())),y=int(std::round(m["y"].n()*scale()));std::string event=m["event"].s();
   UINT msg=event=="down"?WM_LBUTTONDOWN:event=="up"?WM_LBUTTONUP:WM_MOUSEMOVE;
   message(msg,event=="up"?WPARAM(0):WPARAM(MK_LBUTTON),MAKELPARAM(x,y));report("test-mouse-ack",true,true);
  }
  else if(type=="inspect"&&!capturePath.empty())report("inspection",true,true);
  else if(type=="test-settings"&&!capturePath.empty()){
   if(m["motionWall"].kind==Json::Number){testWall=true;testWallMs=m["motionWall"].n();}
   if(m["freezeClock"].kind==Json::Bool)freezePlayback=m["freezeClock"].b();
   if(m["width"].kind==Json::Number&&m["height"].kind==Json::Number)SetWindowPos(hwnd,nullptr,0,0,int(clamp(m["width"].n(),MinWindowWidth,MaxWindowWidth)*scale()),int(clamp(m["height"].n(),MinWindowHeight,MaxWindowHeight)*scale()),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
   if(m["hover"].kind==Json::Bool)hover=m["hover"].b();
   if(m["translation"].kind==Json::Bool&&translation!=m["translation"].b()){translation=m["translation"].b();layoutDirty=true;}
   if(m["through"].kind==Json::Bool)setThrough(m["through"].b());
   if(m["fps"].kind==Json::Number)fps=m["fps"].n()==60?60:30;
   if(m["top"].kind==Json::Bool)setTop(m["top"].b());
   update();report("test-settings-ack",true,true);
  }
 }
 void setTop(bool value){if(setWindowTopmost(hwnd,value,through))top=value;else error="topmost-change-failed";if(playlistHwnd){LONG_PTR style=GetWindowLongPtrW(playlistHwnd,GWL_EXSTYLE);style=value?(style|WS_EX_TOPMOST):(style&~LONG_PTR(WS_EX_TOPMOST));SetWindowLongPtrW(playlistHwnd,GWL_EXSTYLE,style);SetWindowPos(playlistHwnd,value?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);}}
 void setThrough(bool value){if(value)closePlaylist();through=value;LONG_PTR style=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);if(value)style|=WS_EX_TRANSPARENT|WS_EX_LAYERED;else style=(style|WS_EX_LAYERED)&~LONG_PTR(WS_EX_TRANSPARENT);SetWindowLongPtrW(hwnd,GWL_EXSTYLE,style);if(!applyWindowActivationMode(hwnd,top,through))error="activation-mode-failed";hover=false;splitterHover=0;}
 void constrain(){RECT rect{};GetWindowRect(hwnd,&rect);if(!MonitorFromRect(&rect,MONITOR_DEFAULTTONULL))SetWindowPos(hwnd,nullptr,int(80*scale()),int(80*scale()),0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);}
 void save(){
  if(!hwnd)return;
  recordNormalBounds();
  RECT rect{};float rectScale=scale();
  if(!IsIconic(hwnd)){if(!GetWindowRect(hwnd,&rect))return;}
  else if(hasNormalRect){rect=normalWindowRect;rectScale=normalWindowScale;}
  else{float s=positionInPixels?1.f:rectScale;rect={int(std::lround(savedX*s)),int(std::lround(savedY*s)),0,0};rect.right=rect.left+int(std::lround(savedWidth*rectScale));rect.bottom=rect.top+int(std::lround(savedHeight*rectScale));}
  Json j=Json::obj();j["X"]=int(rect.left);j["Y"]=int(rect.top);j["PositionInPixels"]=true;j["Width"]=(rect.right-rect.left)/rectScale;j["Height"]=(rect.bottom-rect.top)/rectScale;j["Top"]=top;j["Translation"]=translation;j["Fps"]=fps;j["FontSize"]=currentFontSize;j["TitleFontSize"]=titleFontSize;j["Background"]=backgroundState();j["Layout"]=layoutState();
  try{
   auto path=std::filesystem::path(settingsPath);std::filesystem::create_directories(path.parent_path());
   auto tmp=path;tmp+=L".tmp."+std::to_wstring(GetCurrentProcessId());
   std::string payload=j.dump();bool wrote=false;
   {std::ofstream file(tmp,std::ios::binary|std::ios::trunc);if(file){file.write(payload.data(),std::streamsize(payload.size()));wrote=bool(file.flush());}}
   if(!wrote||!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING)){
    std::error_code ec;std::filesystem::remove(tmp,ec);
    std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(payload.data(),std::streamsize(payload.size()));
   }
  }catch(...){}
 }
 Json layoutState()const{
  Json j=Json::obj();Json slots;slots.kind=Json::Array;
  for(int i=0;i<3;i++)slots.array.push_back(Json(slotModule[i]));
  j["Slots"]=slots;j["Left"]=double(leftRatio);j["Top"]=double(topHeight);
  Json align;align.kind=Json::Array;
  align.array.push_back(Json(alignCover));align.array.push_back(Json(alignTitle));align.array.push_back(Json(alignLyrics));
  j["Align"]=align;j["TitleOrientation"]=titleOrientation;return j;
 }
 void readLayout(const Json&j){
  if(j.kind!=Json::Object)return;
  const auto&s=j["Slots"];
  if(s.kind==Json::Array&&s.array.size()==3){
   int next[3];bool seen[3]={false,false,false},ok=true;
   for(int i=0;i<3;i++){double v=s.array[i].n(-1);int m=int(v);if(v<0||v>2||m<0||m>2||seen[m]){ok=false;break;}seen[m]=true;next[i]=m;}
   if(ok)for(int i=0;i<3;i++)slotModule[i]=next[i];
  }
  if(j["Left"].kind==Json::Number)leftRatio=float(clamp(j["Left"].n(),0.12,0.6));
  if(j["Top"].kind==Json::Number)topHeight=float(clamp(j["Top"].n(),18,400));
  const auto&a=j["Align"];
  if(a.kind==Json::Array&&a.array.size()==3){
   alignCover=int(clamp(a.array[0].n(1),0,2));alignTitle=int(clamp(a.array[1].n(0),0,2));alignLyrics=int(clamp(a.array[2].n(0),0,2));
  }
  if(j["TitleOrientation"].kind==Json::Number)titleOrientation=int(clamp(j["TitleOrientation"].n(),0,2));
 }
 void toggleLayoutPanel(){if(miniMode)return;layoutPanel=!layoutPanel;splitterHover=0;if(layoutPanel){closePlaylist();panelOpenAt=nowMs();panelOpening=true;SetTimer(hwnd,SecondaryAnimTimer,8,nullptr);}else{panelOpening=false;KillTimer(hwnd,SecondaryAnimTimer);}update();}
 void assignSlot(int slot,int module){
  if(miniMode)return;int from=slotOfModule(module);if(from<0||from==slot)return;
  int displaced=slotModule[slot];slotModule[slot]=slotModule[from];slotModule[from]=displaced;
  if(slotLayout)applySlotLayout();
  rebuildMeta();layoutDirty=true;save();update();
 }
 void setTitleOrientation(int value){
  if(titleOrientation==value)return;titleOrientation=value;
  if(slotLayout)applySlotLayout();
  rebuildMeta();layoutDirty=true;save();update();
 }
 // item: 0 title direction (handled above), 1 cover, 2 title, 3 lyrics.
 void setAlignment(int item,int value){
  int&target=item==1?alignCover:item==2?alignTitle:alignLyrics;
  if(target==value)return;target=value;
  if(item==1&&slotLayout)applySlotLayout();
  layoutDirty=true;save();update();
 }
 void activateIcon(int icon){switch(icon){case 0:toggleLayoutPanel();break;case 1:command("like");break;case 2:requestVinylPage();break;case 3:togglePlaylist();break;case 4:setTop(!top);save();update();break;case 5:setThrough(true);update();break;default:DestroyWindow(hwnd);break;}}
 // Row under the pointer inside the lyric viewport, used to replay a short press.
 bool lyricRowAt(float x,float y,double&seek)const{
  if(untimed||y<viewportTop||y>viewportBottom||x<textX||x>lyricRight())return false;
  double now=motionNow();
  for(const auto&item:groups){const auto&g=item.second;if(!groupVisible(g,now))continue;float top=g.y.value(now);if(y>=top-2&&y<=top+g.renderedHeight(g.zoom.value(now))){seek=std::max(0.,g.content->model.time-offset);return true;}}
  return false;
 }
 void startSlotSwap(int fromSlot,int toSlot){
  if(fromSlot<0||toSlot<0||fromSlot==toSlot)return;
  int moduleA=slotModule[fromSlot],moduleB=slotModule[toSlot];
  D2D1_RECT_F fromA=slotRects[fromSlot],fromB=slotRects[toSlot];
  slotModule[toSlot]=moduleA;slotModule[fromSlot]=moduleB;
  if(slotLayout)applySlotLayout();
  rebuildMeta();layoutDirty=true;save();
  slotSwap.a=moduleA;slotSwap.b=moduleB;
  slotSwap.fromA=fromA;slotSwap.toA=slotRects[toSlot];
  slotSwap.fromB=fromB;slotSwap.toB=slotRects[fromSlot];
  slotSwap.p.snap(0);slotSwap.p.target(1,animationWall(),0,260,0);
  update();
 }
 void menu(POINT point){HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING|(translation?MF_CHECKED:0),1,L"显示翻译");AppendMenuW(menu,MF_STRING|(top?MF_CHECKED:0),2,L"置顶");AppendMenuW(menu,MF_STRING|(through?MF_CHECKED:0),3,L"鼠标穿透（在插件设置中恢复）");AppendMenuW(menu,MF_STRING|(fps==30?MF_CHECKED:0),4,L"30 FPS");AppendMenuW(menu,MF_STRING|(fps==60?MF_CHECKED:0),5,L"60 FPS");AppendMenuW(menu,MF_STRING,6,L"关闭小窗");SetForegroundWindow(hwnd);int choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,hwnd,nullptr);DestroyMenu(menu);if(choice==1){translation=!translation;layoutDirty=true;}else if(choice==2)setTop(!top);else if(choice==3)setThrough(!through);else if(choice==4)fps=30;else if(choice==5)fps=60;else if(choice==6){DestroyWindow(hwnd);return;}save();update();}
 LRESULT message(UINT msg,WPARAM wp,LPARAM lp){
  switch(msg){
   case WM_CREATE:{auto getWindowDpi=reinterpret_cast<UINT(WINAPI*)(HWND)>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow")));if(getWindowDpi){UINT value=getWindowDpi(hwnd);if(value)dpi=float(value);}return 0;}
   case WM_PAINT:paint();return 0;case WM_ERASEBKGND:return 1;case WM_MOUSEACTIVATE:return windowMouseActivate(top,through);
   case Incoming:{std::unique_ptr<Json> value(reinterpret_cast<Json*>(lp));if(!closing)receive(*value);return 0;}
   case BridgeDisconnected:if(!closing&&bridge&&!bridge->connected())freezeConnection();return 0;
   case CoverReady:{std::unique_ptr<CoverData> value(reinterpret_cast<CoverData*>(lp));onCover(std::move(value));return 0;}
   case FrameReady:if(systemMoving&&modalFrames.accepts(wp)&&frameArmed&&nowMs()+1>=frameDeadline){frameArmed=false;update();if(renderVisible)UpdateWindow(hwnd);}return 0;
   case WM_GETMINMAXINFO:{auto*limits=reinterpret_cast<MINMAXINFO*>(lp);limits->ptMinTrackSize.x=LONG(std::ceil(MinWindowWidth*scale()));limits->ptMinTrackSize.y=LONG(std::ceil(MinWindowHeight*scale()));return 0;}
   case WM_WINDOWPOSCHANGED:{auto result=DefWindowProcW(hwnd,msg,wp,lp);setRenderVisible(IsWindowVisible(hwnd)&&!IsIconic(hwnd));if(playlistOpen)positionPlaylist();return result;}
   case WM_SIZE:if(wp==SIZE_MINIMIZED){closePlaylist();setRenderVisible(false);return 0;}if(wp!=SIZE_MAXHIDE&&wp!=SIZE_MAXSHOW&&factory&&textFactory&&hwnd&&geometry()){if(playlistOpen)closePlaylist();update();if(resizing&&renderVisible)UpdateWindow(hwnd);}return 0;
   case WM_DPICHANGED:{closePlaylist();dpi=float(HIWORD(wp));if(target)target->SetDpi(dpi);auto*rect=reinterpret_cast<RECT*>(lp);SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOCOPYBITS);recordNormalBounds();if(resizing){GetCursorPos(&resizeStart);RECT current{};GetWindowRect(hwnd,&current);resizeSize={current.right-current.left,current.bottom-current.top};}if(geometry())update();if(renderVisible)UpdateWindow(hwnd);return 0;}
   case WM_TIMER:if(wp==LongPressTimer){KillTimer(hwnd,LongPressTimer);if(modulePress&&!moduleDragging&&slotLayout&&!miniMode&&!layoutPanel){moduleDragging=true;pressAction=0;dragX=pressX;dragY=pressY;dragTargetSlot=pressSlot;dragHighlight.visible=true;dragHighlightSlot=pressSlot;dragHighlight.from=dragHighlight.to=slotRects[pressSlot];dragHighlight.p.snap(1);update();}return 0;}if(wp==1||wp==4){update();if(systemMoving&&renderVisible)UpdateWindow(hwnd);}else if(wp==3){cancelSeek();position=seekFallback+(playing&&valid?nowMs()-seekStarted:0);anchor=nowMs();error="seek-timeout";update();}else if(wp==2&&!demo){if(ownerProcess.exited())DestroyWindow(hwnd);else if(!bridge||!bridge->connected()||nowMs()-received>45000)freezeConnection();}else if(wp==LayoutWarmupTimer)warmLyricLayouts();else if(wp==PlaylistAnimTimer){double now=animationWall();bool scroll=playlistOpen&&playlistHwnd&&playlistScrollTrack.active(now);if(playlistOpen&&playlistHwnd&&(scroll||playlistOpening)){paintPlaylist();if(playlistOpening&&playlistOpenProgress()>=1){playlistOpening=false;paintPlaylist();}if(!scroll&&!playlistOpening)KillTimer(hwnd,PlaylistAnimTimer);}else{KillTimer(hwnd,PlaylistAnimTimer);if(playlistOpen&&playlistHwnd)paintPlaylist();}}else if(wp==SecondaryAnimTimer){if(layoutPanel&&panelOpening&&panelOpenProgress()<1)update();else{panelOpening=false;KillTimer(hwnd,SecondaryAnimTimer);update();}}else if(wp==VinylConfirmTimer){vinylRequested=false;KillTimer(hwnd,VinylConfirmTimer);}else if(wp==PlaylistRefreshTimer){if(playlistOpen)command("playlist");else KillTimer(hwnd,PlaylistRefreshTimer);}return 0;
   case WM_MOUSEWHEEL:{
    if(through||dragProgress||resizing||layoutPanel||moduleDragging||lines.empty())return 0;POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(hwnd,&p);float x=p.x/scale(),y=p.y/scale();
    if(x<textX||x>lyricRight()||y<viewportTop||y>viewportBottom)return 0;
    wheelRemainder+=GET_WHEEL_DELTA_WPARAM(wp);int steps=wheelRemainder/WHEEL_DELTA;wheelRemainder%=WHEEL_DELTA;if(!steps)return 0;
    int origin=browseIndex>=0?browseIndex:(untimed&&focusIndex>=0?focusIndex:index);
    int next=browseStep(origin,steps);if(next<0)return 0;flight=LyricFlight();browseIndex=next;manualTransition=manualLayoutPending=true;browseUntil=animationWall()+4000;if(!untimed)SetTimer(hwnd,4,4000,nullptr);layoutDirty=true;update();return 0;
   }
   case WM_SETCURSOR:{
    if(LOWORD(lp)==HTCLIENT&&!through){
     if(moduleDragging){SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));return TRUE;}
     POINT pt{};GetCursorPos(&pt);ScreenToClient(hwnd,&pt);float x=pt.x/scale(),y=pt.y/scale();LPCWSTR id=IDC_ARROW;
     int split=splitterAt(x,y);
     if(layoutPanel){
      int slot=0,module=0,fi=0,delta=0,tab=-1,item=0,index=0;
      id=(panelTabAt(x,y,tab)||panelCloseAt(x,y)||(panelTab==0?(panelChipAt(x,y,slot,module)||panelFontAt(x,y,fi,delta)):panelAlignAt(x,y,item,index)))?IDC_HAND:IDC_ARROW;
     }
     else if(split==1)id=IDC_SIZEWE;
     else if(split==2)id=IDC_SIZENS;
     else if(x>width-17&&y>height-17)id=IDC_SIZENWSE;
     else if(iconAt(x,y)>=0||(hover&&y>=transport.y-14&&transport.hit(x,y)!=ControlHit::None))id=IDC_HAND;
     else if(!untimed&&y>=viewportTop&&y<=viewportBottom&&x>=textX&&x<=lyricRight()){
      double now=motionNow();
      for(const auto&item:groups){if(!groupVisible(item.second,now))continue;float top=item.second.y.value(now);if(y>=top-2&&y<=top+item.second.renderedHeight(item.second.zoom.value(now))){id=IDC_HAND;break;}}
     }
     SetCursor(LoadCursorW(nullptr,id));return TRUE;
    }
    break;
   }
   case WM_MOUSEMOVE:{float x=GET_X_LPARAM(lp)/scale(),y=GET_Y_LPARAM(lp)/scale();
    if(modulePress&&!moduleDragging){float dx=x-pressX,dy=y-pressY;if(dx*dx+dy*dy>DragThreshold*DragThreshold){bool windowDrag=pressAction==3;clearModulePress();if(windowDrag){ReleaseCapture();SendMessageW(hwnd,WM_NCLBUTTONDOWN,HTCAPTION,0);}else update();return 0;}return 0;}
    if(moduleDragging){dragX=x;dragY=y;int target=moduleAt(x,y);dragTargetSlot=target;if(target>=0)morphDragHighlight(target);update();return 0;}
    if(resizing){POINT point;GetCursorPos(&point);int w=int(clamp(resizeSize.cx+point.x-resizeStart.x,MinWindowWidth*scale(),MaxWindowWidth*scale())),h=int(clamp(resizeSize.cy+point.y-resizeStart.y,MinWindowHeight*scale(),MaxWindowHeight*scale()));if(w!=geometryPixels.cx||h!=geometryPixels.cy)SetWindowPos(hwnd,nullptr,0,0,w,h,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);}else if(splitterDrag){if(splitterDrag==1)leftRatio=float(clamp((x-SlotMargin)/std::max(1.f,width-SlotMargin*2),.10,.60));else topHeight=float(clamp(y-SlotMargin-SlotGapY*.5f,18.f,std::max(18.f,height-SlotMargin-SlotBottom-SlotGapY-60.f)));applySlotLayout();rebuildMeta();layoutDirty=true;update();}else if(dragProgress){dragRatio=transport.ratio(x);update();}else if(!hover&&!through){hover=true;TRACKMOUSEEVENT event{sizeof(event),TME_LEAVE,hwnd,0};TrackMouseEvent(&event);update();}else if(hover&&!through&&slotLayout&&!layoutPanel){int split=splitterAt(x,y);if(split!=splitterHover){splitterHover=split;update();}}return 0;}
   case WM_MOUSELEAVE:if(!dragProgress&&!resizing&&!splitterDrag){hover=false;splitterHover=0;update();}return 0;
   case WM_LBUTTONDOWN:{if(through)return 0;float x=GET_X_LPARAM(lp)/scale(),y=GET_Y_LPARAM(lp)/scale();
    if(layoutPanel){
     int slot=0,module=0,fi=0,delta=0,tab=-1,item=0,index=0;
     if(panelCloseAt(x,y)){layoutPanel=false;update();return 0;}
     if(panelTabAt(x,y,tab)){panelTab=tab;update();return 0;}
     if(panelTab==0){
      if(panelChipAt(x,y,slot,module)){assignSlot(slot,module);return 0;}
      if(panelFontAt(x,y,fi,delta)){if(fi==0)setTitleFontSize(titleFontSize+delta);else setLyricFontSize(currentFontSize+delta);return 0;}
     }else if(panelAlignAt(x,y,item,index)){
      if(item==0)setTitleOrientation(index);else setAlignment(item,index);
      return 0;
     }
     if(!panelInside(x,y)){layoutPanel=false;update();}
     return 0;
    }
    int split=splitterAt(x,y);if(split){splitterDrag=split;splitterHover=split;SetCapture(hwnd);update();return 0;}
    int icon=iconAt(x,y);bool iconZone=icon>=0;if(playlistOpen&&!iconZone&&!(x>width-17&&y>height-17))closePlaylist();if(x>width-17&&y>height-17){resizing=true;GetCursorPos(&resizeStart);RECT r;GetWindowRect(hwnd,&r);resizeSize={r.right-r.left,r.bottom-r.top};SetCapture(hwnd);update();return 0;}
    // Slot surfaces defer their click to the release so a long press can lift
    // the module instead of firing a seek/icon/window-drag action.
    if(slotLayout&&!miniMode&&!layoutPanel&&!swapAnimating()){
     int slot=moduleAt(x,y);
     if(slot>=0){
      modulePress=true;pressSlot=slot;pressX=x;pressY=y;pressIcon=iconZone?icon:-1;pressAction=iconZone?1:3;pressSeek=-1;
      if(!iconZone){double seek;if(lyricRowAt(x,y,seek)){pressAction=2;pressSeek=seek;}}
      SetCapture(hwnd);SetTimer(hwnd,LongPressTimer,LongPressMs,nullptr);update();return 0;
     }
    }
    if(iconZone){activateIcon(icon);return 0;}
    if(hover&&y>=transport.y-14){
     switch(transport.hit(x,y)){
      case ControlHit::Seek:if(duration>0){dragProgress=true;dragRatio=transport.ratio(x);SetCapture(hwnd);update();}break;
      case ControlHit::Previous:command("previous");break;case ControlHit::Play:command("play");break;case ControlHit::Next:command("next");break;default:break;
     }return 0;
    }
    if(!untimed&&y>=viewportTop&&y<=viewportBottom&&x>=textX&&x<=lyricRight()){for(const auto&item:groups){if(!groupVisible(item.second,motionNow()))continue;float top=item.second.y.value(motionNow());if(y>=top-2&&y<=top+item.second.renderedHeight(item.second.zoom.value(motionNow()))){requestSeek(std::max(0.,item.second.content->model.time-offset));update();return 0;}}}
    ReleaseCapture();SendMessageW(hwnd,WM_NCLBUTTONDOWN,HTCAPTION,0);return 0;}
   case WM_LBUTTONUP:if(moduleDragging||modulePress){
    float x=GET_X_LPARAM(lp)/scale(),y=GET_Y_LPARAM(lp)/scale();bool dragging=moduleDragging;int from=pressSlot,action=pressAction,icon=pressIcon;double seek=pressSeek;int target=dragging?moduleAt(x,y):-1;
    clearModulePress();ReleaseCapture();
    if(dragging){if(target>=0&&target!=from)startSlotSwap(from,target);else update();}
    else{if(action==1&&icon>=0)activateIcon(icon);else if(action==2&&seek>=0)requestSeek(seek);update();}
    return 0;}if(splitterDrag){splitterDrag=0;splitterHover=0;ReleaseCapture();save();update();return 0;}if(dragProgress){requestSeek(duration*dragRatio);dragProgress=false;ReleaseCapture();update();}if(resizing)ReleaseCapture();return 0;
   case WM_LBUTTONDBLCLK:{if(modulePress){clearModulePress();ReleaseCapture();}float x=GET_X_LPARAM(lp)/scale(),y=GET_Y_LPARAM(lp)/scale();if((x>=artBounds.left&&x<=artBounds.right&&y>=artBounds.top&&y<=artBounds.bottom)||x<textX)command("play");return 0;}
   case WM_CAPTURECHANGED:{clearModulePress();bool sized=resizing,split=splitterDrag!=0;splitterDrag=0;dragProgress=false;resizing=false;POINT p;GetCursorPos(&p);ScreenToClient(hwnd,&p);hover=!through&&p.x>=0&&p.y>=0&&p.x<width*scale()&&p.y<height*scale();update();if(sized||split)save();return 0;}
   case WM_KEYDOWN:if(wp==VK_ESCAPE&&layoutPanel){layoutPanel=false;update();return 0;}break;
   case WM_CONTEXTMENU:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};if(p.x==-1)GetCursorPos(&p);menu(p);return 0;}
   case WM_ENTERSIZEMOVE:stopFrames();if(frameTimer)WaitForSingleObject(frameTimer,0);systemMoving=true;modalFrames.start(hwnd,frameTimer,FrameReady);update();return 0;
   case WM_EXITSIZEMOVE:systemMoving=false;stopFrames();modalFrames.stop();recordNormalBounds();update();save();return 0;
   case WM_CLOSE:DestroyWindow(hwnd);return 0;
   case WM_DESTROY:try{if(bridge)bridge->finish();}catch(...){}closing=true;closePlaylist();systemMoving=false;stopFrames();modalFrames.stop();save();KillTimer(hwnd,1);KillTimer(hwnd,2);KillTimer(hwnd,3);KillTimer(hwnd,4);KillTimer(hwnd,LayoutWarmupTimer);KillTimer(hwnd,PlaylistRefreshTimer);KillTimer(hwnd,PlaylistAnimTimer);KillTimer(hwnd,SecondaryAnimTimer);KillTimer(hwnd,VinylConfirmTimer);KillTimer(hwnd,LongPressTimer);stopCoverWorker();PostQuitMessage(0);return 0;
  }return DefWindowProcW(hwnd,msg,wp,lp);
 }
 static LRESULT CALLBACK procedure(HWND window,UINT msg,WPARAM wp,LPARAM lp){App*app=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(msg==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);app->hwnd=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}if(app){try{return app->message(msg,wp,lp);}catch(...){app->error="window-message-failed";}}return DefWindowProcW(window,msg,wp,lp);}
 public:
 ~App(){stopCoverWorker();}
 App(){settingsPath=env(L"MINILYRICS_SETTINGS");if(settingsPath.empty())settingsPath=env(L"LOCALAPPDATA")+L"\\MiniLyrics\\settings.json";
  try{std::ifstream f(std::filesystem::path(settingsPath),std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(f)),{});if(bytes.size()>65536)throw std::runtime_error("settings size");auto settings=JsonParser(bytes).parse();savedX=settings["X"].n(80);savedY=settings["Y"].n(80);positionInPixels=settings["PositionInPixels"].b();savedWidth=clamp(settings["Width"].n(MinWindowWidth),MinWindowWidth,MaxWindowWidth);savedHeight=clamp(settings["Height"].n(MinWindowHeight),MinWindowHeight,MaxWindowHeight);top=settings["Top"].b(true);translation=settings["Translation"].b(true);fps=settings["Fps"].n(60)==60?60:30;readBackground(settings["Background"]);readLayout(settings["Layout"]);currentFontSize=float(std::round(clamp(settings["FontSize"].n(::CurrentFontSize),14,36)));titleFontSize=float(std::round(clamp(settings["TitleFontSize"].n(12),9,18)));contextFontSize=::ContextFontSize*currentFontSize/::CurrentFontSize;translationFontSize=::TranslationFontSize*currentFontSize/::CurrentFontSize;}catch(...){}
 }
 int run(HINSTANCE instance,int port,const std::string&token,const std::wstring&captureFile,bool demoMode){
  capturePath=captureFile;demo=demoMode;if(!demo)ownerProcess.capture();check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.put()));check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(textFactory.put())));check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(imaging.put())));
  makeFormat(mainFormat,currentFontSize,DWRITE_FONT_WEIGHT_SEMI_BOLD);makeFormat(secondaryFormat,translationFontSize,DWRITE_FONT_WEIGHT_NORMAL);makeFormat(metaFormat,titleFontSize,DWRITE_FONT_WEIGHT_NORMAL);makeFormat(playlistHeaderFormat,15,DWRITE_FONT_WEIGHT_SEMI_BOLD);makeFormat(playlistTitleFormat,13.5f,DWRITE_FONT_WEIGHT_NORMAL);makeFormat(playlistMetaFormat,11.5f,DWRITE_FONT_WEIGHT_NORMAL);mainRowHeight=layout(L"Ag",mainFormat.get()).height;updatePlaceholders();
  check(factory->CreatePathGeometry(playGeometry.put()));Com<ID2D1GeometrySink> sink;check(playGeometry->Open(sink.put()));sink->BeginFigure(D2D1::Point2F(-5,-7),D2D1_FIGURE_BEGIN_FILLED);sink->AddLine(D2D1::Point2F(-5,7));sink->AddLine(D2D1::Point2F(6,0));sink->EndFigure(D2D1_FIGURE_END_CLOSED);check(sink->Close());
  // Heart glyph for the current-song like toggle, centred on the icon row.
  check(factory->CreatePathGeometry(likeGeometry.put()));Com<ID2D1GeometrySink> likeSink;check(likeGeometry->Open(likeSink.put()));likeSink->BeginFigure(D2D1::Point2F(0,5.6f),D2D1_FIGURE_BEGIN_FILLED);
  likeSink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(-1.1f,4.2f),D2D1::Point2F(-6,1.6f),D2D1::Point2F(-6,-1.6f)));
  likeSink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(-6,-4.1f),D2D1::Point2F(-4.3f,-5.6f),D2D1::Point2F(-2.4f,-5.6f)));
  likeSink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(-1.2f,-5.6f),D2D1::Point2F(-.4f,-4.8f),D2D1::Point2F(0,-3.7f)));
  likeSink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(.4f,-4.8f),D2D1::Point2F(1.2f,-5.6f),D2D1::Point2F(2.4f,-5.6f)));
  likeSink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(4.3f,-5.6f),D2D1::Point2F(6,-4.1f),D2D1::Point2F(6,-1.6f)));
  likeSink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(6,1.6f),D2D1::Point2F(1.1f,4.2f),D2D1::Point2F(0,5.6f)));
  likeSink->EndFigure(D2D1_FIGURE_END_CLOSED);check(likeSink->Close());
  frameTimer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_MODIFY_STATE|SYNCHRONIZE);if(!frameTimer)frameTimer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
  WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.style=CS_DBLCLKS;cls.lpfnWndProc=procedure;cls.hInstance=instance;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.lpszClassName=L"MiniLyrics.Direct2D";RegisterClassExW(&cls);
  WNDCLASSEXW playlistClass{};playlistClass.cbSize=sizeof(playlistClass);playlistClass.lpfnWndProc=playlistProcedure;playlistClass.hInstance=instance;playlistClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);playlistClass.lpszClassName=L"MiniLyrics.Playlist";RegisterClassExW(&playlistClass);
  auto getSystemDpi=reinterpret_cast<UINT(WINAPI*)()>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForSystem")));if(getSystemDpi)dpi=float(getSystemDpi());
  float positionScale=positionInPixels?1.f:scale();
  hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_LAYERED|(top?WS_EX_TOPMOST|WS_EX_NOACTIVATE:0),cls.lpszClassName,L"MiniLyrics",WS_POPUP,int(std::round(savedX*positionScale)),int(std::round(savedY*positionScale)),int(std::round(savedWidth*scale())),int(std::round(savedHeight*scale())),nullptr,nullptr,instance,this);if(!hwnd)throw std::runtime_error("window creation failed");
  // WM_CREATE has the actual monitor DPI. Correct the initial physical size
  // before showing, keeping saved dimensions in DIP and screen positions in px.
  SetWindowPos(hwnd,nullptr,0,0,int(std::round(savedWidth*scale())),int(std::round(savedHeight*scale())),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
  geometry();startCoverWorker();received=nowMs();SetTimer(hwnd,2,5000,nullptr);
  if(!demo)bridge=std::make_unique<Bridge>(port,token,[window=hwnd](Json message){auto data=std::make_unique<Json>(std::move(message));if(PostMessageW(window,Incoming,0,reinterpret_cast<LPARAM>(data.get())))data.release();},[window=hwnd]{PostMessageW(window,BridgeDisconnected,0,0);});
  ShowWindow(hwnd,SW_SHOWNOACTIVATE);constrain();update();
  if(demo){title=L"晚风来信";artist=L"示例音乐";songId="demo";Line l;l.text=L"让晚风轻轻捎去消息";l.translation=L"Let the evening carry your words";l.duration=6000;lines.push_back(std::move(l));position=1700;anchor=nowMs();duration=16000;valid=true;playing=capturePath.empty();rebuildMeta();updatePlaceholders();layoutDirty=true;update();if(!capturePath.empty()){capture();DestroyWindow(hwnd);}}
  MSG message{};bool quit=false;while(!quit){
   DWORD count=frameTimer?1:0;DWORD ready=MsgWaitForMultipleObjectsEx(count,frameTimer?&frameTimer:nullptr,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
   if(ready==WAIT_FAILED)break;if(frameTimer&&ready==WAIT_OBJECT_0){frameArmed=false;update();}
   while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_QUIT){quit=true;break;}TranslateMessage(&message);DispatchMessageW(&message);}
  }
  modalFrames.stop();closePlaylist();if(frameTimer){CancelWaitableTimer(frameTimer);CloseHandle(frameTimer);frameTimer=nullptr;}bridge.reset();graphics.clear();target.reset();return 0;
 }
};

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int){
 auto awareness=reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext")));if(awareness)awareness(reinterpret_cast<HANDLE>(-4));else SetProcessDPIAware();
 HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa)!=0)return 2;
 int result=1;try{int argc=0;LPWSTR*argv=CommandLineToArgvW(GetCommandLineW(),&argc);int port=0;std::string token;std::wstring capture;bool demo=false;for(int i=1;i<argc;i++){std::wstring arg=argv[i];if(arg==L"--demo")demo=true;else if(i+1<argc&&arg==L"--port")port=_wtoi(argv[++i]);else if(i+1<argc&&arg==L"--token")token=narrow(argv[++i]);else if(i+1<argc&&arg==L"--capture")capture=argv[++i];}LocalFree(argv);if(!demo&&(port<1024||port>65535||token.size()<32))result=2;else{App app;result=app.run(instance,port,token,capture,demo);}}
 catch(const std::exception&error){try{std::ofstream f(std::filesystem::path(env(L"TEMP")+L"\\MiniLyrics-error.log"),std::ios::binary);f<<error.what();}catch(...){}result=1;}
 WSACleanup();if(SUCCEEDED(com))CoUninitialize();return result;
}
