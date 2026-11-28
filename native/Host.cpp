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
#include <unordered_map>
#include <optional>
#include <cwctype>
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
static constexpr double MinWindowHeight=130;
static constexpr float CurrentFontSize=21,ContextFontSize=16,TranslationFontSize=16,GroupGap=7,TranslationGap=6.3f;
static double nowMs(){static auto origin=std::chrono::steady_clock::now();return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-origin).count();}
static double clamp(double x,double lo,double hi){return std::max(lo,std::min(hi,std::isfinite(x)?x:lo));}
static std::wstring widen(const std::string&s){if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);if(n<=0)return {};std::wstring out(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),&out[0],n);return out;}
static std::string narrow(const std::wstring&s){if(s.empty())return {};int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);std::string out(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),&out[0],n,nullptr,nullptr);return out;}
static std::wstring env(const wchar_t*name){DWORD n=GetEnvironmentVariableW(name,nullptr,0);if(!n)return {};std::wstring out(n,0);GetEnvironmentVariableW(name,&out[0],n);out.resize(n-1);return out;}
static void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Windows graphics initialization failed ("+std::to_string(uint32_t(hr))+")");}
struct Word {std::wstring text;double time=0,duration=1;bool longNote=false,lineTail=false,emphasize=false;bool operator==(const Word&o)const{return text==o.text&&time==o.time&&duration==o.duration&&longNote==o.longNote&&lineTail==o.lineTail&&emphasize==o.emphasize;}};
struct Line {std::wstring text,translation;double time=0,duration=0;std::vector<Word> words;bool isInterlude=false;bool operator==(const Line&o)const{return text==o.text&&translation==o.translation&&time==o.time&&duration==o.duration&&words==o.words&&isInterlude==o.isInterlude;}};
struct Layout {Com<IDWriteTextLayout> value;float width=0,height=0;};
struct Glyph {
 Layout layout;double time=0,duration=1;float x=0,inkLeft=0,inkRight=0,maxEmphasisScale=1.075f;
 bool longNote=false,emphasize=false;
};
struct LyricLayout {
 uint64_t serial=0;
 Line model;Layout main,subtitle;std::vector<Glyph> glyphs;float mainHeight=0;
 float height(bool translated)const{return mainHeight+(translated?TranslationGap+subtitle.height:0);}
};
struct LyricGroup {
 struct GlowRelease {size_t wordIndex=0;float longNote=0,emphasis=0;double longNoteDuration=LongNoteFadeOut;};
 struct ResetWord {FloatWord motion;float sweep=0.f;float scale=1.f;};
 std::shared_ptr<LyricLayout> content;
 std::vector<ResetWord> resetWords;std::vector<GlowRelease> resetGlows;InterludeAnimation dots;
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
enum class CoverStatus {Ok,Empty,Cancelled,TransientError,PermanentError};
struct CoverData {uint64_t epoch=0;CoverStatus status=CoverStatus::Empty;UINT width=0,height=0;std::vector<BYTE> pixels;};
struct CoverQueue {
 std::mutex mutex;std::condition_variable changed;bool stop=false,pending=false;std::wstring url;uint64_t epoch=0;
 std::unique_ptr<CoverData> ready;
 bool cancelled(uint64_t requestEpoch){std::lock_guard<std::mutex> lock(mutex);return stop||epoch!=requestEpoch;}
};

struct CoverDownloader {
 HINTERNET session=nullptr;Com<IWICImagingFactory> factory;bool comInit=false;
 CoverDownloader(){
  HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);comInit=SUCCEEDED(hr);
  session=WinHttpOpen(L"MiniLyrics/0.8",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
  if(session)WinHttpSetTimeouts(session,2000,2000,3000,3000);
 }
 ~CoverDownloader(){factory.reset();if(session)WinHttpCloseHandle(session);if(comInit)CoUninitialize();}
 std::unique_ptr<CoverData> fetch(const std::wstring&url,uint64_t epoch,CoverQueue*queue=nullptr){
  auto out=std::make_unique<CoverData>();out->epoch=epoch;
  auto cancelled=[&]{return queue&&queue->cancelled(epoch);};
  if(url.empty()){out->status=CoverStatus::Empty;return out;}
  if(cancelled()){out->status=CoverStatus::Cancelled;return out;}
  if(!session){out->status=CoverStatus::PermanentError;return out;}
  URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=DWORD(-1);
  if(!WinHttpCrackUrl(url.c_str(),0,0,&parts)||(parts.nScheme!=INTERNET_SCHEME_HTTP&&parts.nScheme!=INTERNET_SCHEME_HTTPS)){out->status=CoverStatus::PermanentError;return out;}
  std::wstring host(parts.lpszHostName,parts.dwHostNameLength),path(parts.lpszUrlPath,parts.dwUrlPathLength);
  if(parts.dwExtraInfoLength)path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);if(path.empty())path=L"/";
  std::vector<BYTE> bytes;
  {
   using HttpHandle=std::unique_ptr<void,decltype(&WinHttpCloseHandle)>;
   HttpHandle connection(WinHttpConnect(session,host.c_str(),parts.nPort,0),WinHttpCloseHandle);
   if(!connection||cancelled()){out->status=cancelled()?CoverStatus::Cancelled:CoverStatus::TransientError;return out;}
   HttpHandle request(WinHttpOpenRequest(connection.get(),L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,
    WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0),WinHttpCloseHandle);
   if(!request||cancelled()||!WinHttpSendRequest(request.get(),WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)){out->status=cancelled()?CoverStatus::Cancelled:CoverStatus::TransientError;return out;}
   if(cancelled()||!WinHttpReceiveResponse(request.get(),nullptr)){out->status=cancelled()?CoverStatus::Cancelled:CoverStatus::TransientError;return out;}
   DWORD status=0,size=sizeof(status);
   if(cancelled()||!WinHttpQueryHeaders(request.get(),WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr)){out->status=cancelled()?CoverStatus::Cancelled:CoverStatus::TransientError;return out;}
   if(status!=200){out->status=(status==408||status==429||(status>=500&&status<600))?CoverStatus::TransientError:CoverStatus::PermanentError;return out;}
   BYTE buffer[8192];
   for(;;){
    DWORD count=0;
    if(cancelled()){out->status=CoverStatus::Cancelled;return out;}
    if(!WinHttpReadData(request.get(),buffer,sizeof(buffer),&count)){out->status=cancelled()?CoverStatus::Cancelled:CoverStatus::TransientError;return out;}
    if(bytes.size()+count>8*1024*1024){out->status=CoverStatus::PermanentError;return out;}
    if(!count)break;
    bytes.insert(bytes.end(),buffer,buffer+count);
   }
  }
  if(cancelled()){out->status=CoverStatus::Cancelled;return out;}
  if(bytes.empty()){out->status=CoverStatus::PermanentError;return out;}
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
   out->status=CoverStatus::Ok;
  }catch(...){out->width=out->height=0;out->pixels.clear();out->status=CoverStatus::PermanentError;}
  return out;
 }
};

inline std::unique_ptr<CoverData> downloadCover(const std::wstring&url,uint64_t epoch){CoverDownloader downloader;return downloader.fetch(url,epoch);}

struct TextSlice {
 size_t page=0;
 D2D1_RECT_F src{};
 float left=0,top=0,width=0,height=0;
 float clipLeft=0,clipRight=0;
 bool clipLeftEdge=false,clipRightEdge=false;
 bool valid=false;
};
struct TextAtlas {
 struct SliceSpan {size_t begin=0,count=0;};
 std::vector<Com<ID2D1Bitmap>> pages;
 std::vector<TextSlice> slices;
 std::vector<SliceSpan> itemSpans;
 size_t bytes=0;
 bool ready=false,failed=false;
};
struct GlowTexture {
 Com<ID2D1Bitmap> bitmap;
 float left=0,top=0,width=0,height=0,fontSize=0;
 size_t bytes=0;
 bool ready=false,failed=false;
};
struct RowTexture {
 float dpi=96.f;
 uint64_t lastUsed=0;
 IDWriteTextLayout* mainKey=nullptr;
 IDWriteTextLayout* subKey=nullptr;
 size_t wordCountKey=0;
 IDWriteTextLayout* firstWordKey=nullptr;
 TextAtlas main,subtitle,words;
 std::unordered_map<size_t,GlowTexture> glows;
 size_t bytes()const{
  size_t total=main.bytes+subtitle.bytes+words.bytes;
  for(const auto&kv:glows)total+=kv.second.bytes;
  return total;
 }
};

struct Graphics {
 ID2D1RenderTarget*target=nullptr;Com<ID2D1SolidColorBrush> brush;Com<ID2D1LinearGradientBrush> gradient,sweepBrush,sweepFadeBrush;Com<ID2D1Bitmap> cover,background;Com<ID2D1BitmapBrush> coverBrush,backgroundBrush;uint64_t backgroundRevision=0;bool coverOpaque=true;
 float sweepFadeHigh=-1.f;
 std::unordered_map<uint64_t,RowTexture> rowTextures;uint64_t textTick=0,rowTextureBuilds=0,glowTextureBuilds=0;
 size_t totalRowTextureBytes=0,maxAtlasBytes=4u*1024u*1024u,maxRowTextureBytes=32u*1024u*1024u;
 void clearRowTextures(){rowTextures.clear();totalRowTextureBytes=0;}
 void clear(){clearRowTextures();sweepFadeBrush.reset();sweepFadeHigh=-1.f;sweepBrush.reset();backgroundBrush.reset();background.reset();backgroundRevision=0;coverBrush.reset();cover.reset();coverOpaque=true;gradient.reset();brush.reset();target=nullptr;}
 void resize(float w,float h){if(gradient)gradient->SetEndPoint(D2D1::Point2F(w,h*.5f));}
 ID2D1LinearGradientBrush* ensureSweepBrush(float highAlpha=1.f){
  if(!target)return nullptr;
  if(highAlpha>=.999f){
   if(!sweepBrush){
    D2D1_GRADIENT_STOP stops[]={{0.f,D2D1::ColorF(1.f,1.f,1.f,1.f)},{1.f,D2D1::ColorF(1.f,1.f,1.f,.4f)}};
    Com<ID2D1GradientStopCollection> collection;Com<ID2D1LinearGradientBrush> next;
    if(SUCCEEDED(target->CreateGradientStopCollection(stops,2,collection.put()))&&
       SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(1,0)),collection.get(),next.put()))){
     sweepBrush=std::move(next);
    }
   }
   return sweepBrush.get();
  }
  if(!sweepFadeBrush||std::abs(sweepFadeHigh-highAlpha)>.001f){
   D2D1_GRADIENT_STOP stops[]={{0.f,D2D1::ColorF(1.f,1.f,1.f,highAlpha)},{1.f,D2D1::ColorF(1.f,1.f,1.f,.4f)}};
   Com<ID2D1GradientStopCollection> collection;Com<ID2D1LinearGradientBrush> next;
   if(SUCCEEDED(target->CreateGradientStopCollection(stops,2,collection.put()))&&
      SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(1,0)),collection.get(),next.put()))){
    sweepFadeBrush=std::move(next);sweepFadeHigh=highAlpha;
   }
  }
  return sweepFadeBrush.get();
 }
 TextAtlas rasterizeLayouts(const std::vector<const Layout*>&items,float dpi,uint64_t rowSerial){
  TextAtlas atlas;
  atlas.itemSpans.resize(items.size());
  if(!target||items.empty())return atlas;
  const float s=std::max(.5f,dpi/96.f);
  const UINT maxBmp=std::max(512u,target->GetMaximumBitmapSize());
  const UINT shelfMaxW=std::min(maxBmp,4096u);
  constexpr UINT padPx=2u,gapPx=2u,tileOverlapPx=2u;
  const UINT stepPx=shelfMaxW>2u*tileOverlapPx?shelfMaxW-2u*tileOverlapPx:shelfMaxW;
  struct PendingSlice {
   IDWriteTextLayout* layout=nullptr;
   float left=0,top=0,width=0,height=0;
   float clipLeft=0,clipRight=0;
   UINT pxW=0,pxH=0,atlasX=0,atlasY=0;
   size_t page=0;
   bool clipLeftEdge=false,clipRightEdge=false,valid=false;
  };
  std::vector<PendingSlice> pending;
  pending.reserve(items.size());
  for(size_t idx=0;idx<items.size();++idx){
   const Layout* item=items[idx];
   const size_t spanBegin=pending.size();
   if(!item||!item->value){atlas.itemSpans[idx]={spanBegin,0};continue;}
   DWRITE_OVERHANG_METRICS ov{};
   if(FAILED(item->value->GetOverhangMetrics(&ov))){atlas.itemSpans[idx]={spanBegin,0};continue;}
   const float boxW=item->value->GetMaxWidth(),boxH=item->value->GetMaxHeight();
   const float inkLeft=std::max(-16.f,std::min(0.f,-ov.left));
   const float inkTop=std::max(-16.f,std::min(0.f,-ov.top));
   const float inkRight=std::max(item->width,std::min(item->width+16.f,boxW+ov.right));
   const float inkBottom=std::max(item->height,std::min(item->height+16.f,boxH+ov.bottom));
   const float padDip=float(padPx)/s;
   const float originX=std::floor((inkLeft-padDip)*s)/s;
   const float originY=std::floor((inkTop-padDip)*s)/s;
   const UINT totalPxW=std::max(1u,UINT(std::ceil((inkRight+padDip-originX)*s)));
   const UINT pxH=std::min(maxBmp,std::max(1u,UINT(std::ceil((inkBottom+padDip-originY)*s))));
   if(totalPxW>shelfMaxW){
    for(UINT spanStart=0;spanStart<totalPxW;spanStart+=stepPx){
     const UINT spanEnd=std::min(totalPxW,spanStart+stepPx);
     const UINT srcStart=spanStart>0u?spanStart-tileOverlapPx:0u;
     const UINT srcEnd=spanEnd<totalPxW?std::min(totalPxW,spanEnd+tileOverlapPx):totalPxW;
     PendingSlice ps;
     ps.layout=item->value.get();
     ps.left=originX+float(srcStart)/s;
     ps.top=originY;
     ps.pxW=srcEnd-srcStart;
     ps.pxH=pxH;
     ps.width=float(ps.pxW)/s;
     ps.height=float(pxH)/s;
     ps.clipLeft=originX+float(spanStart)/s;
     ps.clipRight=originX+float(spanEnd)/s;
     ps.clipLeftEdge=spanStart>0u;
     ps.clipRightEdge=spanEnd<totalPxW;
     ps.valid=true;
     pending.push_back(ps);
    }
   }else{
    PendingSlice ps;
    ps.layout=item->value.get();
    ps.left=originX;
    ps.top=originY;
    ps.pxW=totalPxW;
    ps.pxH=pxH;
    ps.width=float(ps.pxW)/s;
    ps.height=float(ps.pxH)/s;
    ps.valid=true;
    pending.push_back(ps);
   }
   atlas.itemSpans[idx]={spanBegin,pending.size()-spanBegin};
  }
  struct PageSpec {UINT width=0,height=0;std::vector<size_t> indices;};
  std::vector<PageSpec> pages;
  UINT curX=0,curY=0,shelfH=0,pageW=0;
  std::vector<size_t> currentIndices;
  auto flushPage=[&]{
   if(currentIndices.empty())return;
   const UINT finalH=std::min(maxBmp,std::max(1u,curY+shelfH));
   pages.push_back({std::max(1u,pageW),finalH,std::move(currentIndices)});
   currentIndices.clear();curX=curY=shelfH=pageW=0;
  };
  for(size_t i=0;i<pending.size();++i){
   auto& ps=pending[i];
   if(!ps.valid)continue;
   if(curX>0&&curX+gapPx+ps.pxW>shelfMaxW){
    curY+=shelfH+gapPx;curX=0;shelfH=0;
   }
   if(curY+ps.pxH>maxBmp){
    flushPage();
   }
   if(curX>0)curX+=gapPx;
   ps.page=pages.size();
   ps.atlasX=curX;
   ps.atlasY=curY;
   curX+=ps.pxW;
   pageW=std::max(pageW,curX);
   shelfH=std::max(shelfH,ps.pxH);
   currentIndices.push_back(i);
  }
  flushPage();
  if(pages.empty()){atlas.ready=true;atlas.slices.resize(pending.size());return atlas;}
  size_t totalBytes=0;
  for(const auto&spec:pages){
   const uint64_t pb=uint64_t(spec.width)*spec.height*4ull;
   if(pb>maxAtlasBytes||totalBytes+pb>maxAtlasBytes){atlas.failed=true;return atlas;}
   totalBytes+=size_t(pb);
  }
  if(!reserveRowTextureBytes(rowSerial,totalBytes)){atlas.failed=true;return atlas;}
  auto failAtlas=[&]{
   TextAtlas out;out.itemSpans.resize(items.size());out.failed=true;return out;
  };
  for(const auto& spec:pages){
   Com<ID2D1BitmapRenderTarget> rt;
   const auto dipSize=D2D1::SizeF(float(spec.width)/s,float(spec.height)/s);
   const auto pxSize=D2D1::SizeU(spec.width,spec.height);
   const auto fmt=D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED);
   if(FAILED(target->CreateCompatibleRenderTarget(&dipSize,&pxSize,&fmt,D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,rt.put())))return failAtlas();
   rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
   Com<ID2D1SolidColorBrush> white;
   if(FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),white.put())))return failAtlas();
   rt->BeginDraw();
   rt->Clear(D2D1::ColorF(0.f,0.f,0.f,0.f));
   for(size_t idx:spec.indices){
    const auto& ps=pending[idx];
    const D2D1_RECT_F clip=D2D1::RectF(float(ps.atlasX)/s,float(ps.atlasY)/s,float(ps.atlasX+ps.pxW)/s,float(ps.atlasY+ps.pxH)/s);
    rt->PushAxisAlignedClip(clip,D2D1_ANTIALIAS_MODE_ALIASED);
    rt->DrawTextLayout(D2D1::Point2F(clip.left-ps.left,clip.top-ps.top),ps.layout,white.get(),D2D1_DRAW_TEXT_OPTIONS_NONE);
    rt->PopAxisAlignedClip();
   }
   if(FAILED(rt->EndDraw()))return failAtlas();
   Com<ID2D1Bitmap> bmp;
   if(FAILED(rt->GetBitmap(bmp.put())))return failAtlas();
   atlas.pages.push_back(std::move(bmp));
  }
  atlas.slices.resize(pending.size());
  for(size_t i=0;i<pending.size();++i){
   const auto& ps=pending[i];
   if(!ps.valid)continue;
   TextSlice ts;
   ts.page=ps.page;
   ts.src=D2D1::RectF(float(ps.atlasX)/s,float(ps.atlasY)/s,float(ps.atlasX+ps.pxW)/s,float(ps.atlasY+ps.pxH)/s);
   ts.left=ps.left;ts.top=ps.top;ts.width=ps.width;ts.height=ps.height;
   ts.clipLeft=ps.clipLeft;ts.clipRight=ps.clipRight;
   ts.clipLeftEdge=ps.clipLeftEdge;ts.clipRightEdge=ps.clipRightEdge;
   ts.valid=true;
   atlas.slices[i]=ts;
  }
  atlas.bytes=totalBytes;
  atlas.ready=true;
  ++rowTextureBuilds;
  return atlas;
 }
 static std::vector<float> gaussianBlur1D(const std::vector<float>&src,UINT w,UINT h,float sigma){
  if(w==0||h==0||src.size()!=size_t(w)*h)return {};
  const int radius=std::max(1,std::min(40,int(std::ceil(sigma*3.f))));
  std::vector<float> kernel(size_t(radius)*2+1);
  float kSum=0.f;
  const float invTwoSigSq=1.f/(2.f*std::max(.1f,sigma)*std::max(.1f,sigma));
  for(int i=-radius;i<=radius;++i){
   const float v=std::exp(-float(i*i)*invTwoSigSq);
   kernel[size_t(i+radius)]=v;
   kSum+=v;
  }
  for(float&v:kernel)v/=kSum;
  std::vector<float> tmp(src.size(),0.f),dst(src.size(),0.f);
  for(UINT y=0;y<h;++y){
   const size_t row=size_t(y)*w;
   for(UINT x=0;x<w;++x){
    float acc=0.f;
    const int xMin=std::max(0,int(x)-radius),xMax=std::min(int(w)-1,int(x)+radius);
    for(int sx=xMin;sx<=xMax;++sx)acc+=src[row+size_t(sx)]*kernel[size_t(sx-int(x)+radius)];
    tmp[row+x]=acc;
   }
  }
  for(UINT y=0;y<h;++y){
   const int yMin=std::max(0,int(y)-radius),yMax=std::min(int(h)-1,int(y)+radius);
   const size_t row=size_t(y)*w;
   for(int sy=yMin;sy<=yMax;++sy){
    const float kw=kernel[size_t(sy-int(y)+radius)];
    const size_t sRow=size_t(sy)*w;
    for(UINT x=0;x<w;++x)dst[row+x]+=tmp[sRow+x]*kw;
   }
  }
  return dst;
 }
 GlowTexture rasterizeGlow(const Layout&item,float dpi,float fontSize,uint64_t rowSerial){
  GlowTexture gt;gt.fontSize=fontSize;
  if(!target||!item.value){gt.failed=true;return gt;}
  const float s=std::max(.5f,dpi/96.f);
  const UINT maxBmp=std::max(512u,target->GetMaximumBitmapSize());
  const UINT shelfMaxW=std::min(maxBmp,4096u);
  DWRITE_OVERHANG_METRICS ov{};
  if(FAILED(item.value->GetOverhangMetrics(&ov))){gt.failed=true;return gt;}
  const float boxW=item.value->GetMaxWidth(),boxH=item.value->GetMaxHeight();
  const float inkLeft=std::max(-16.f,std::min(0.f,-ov.left));
  const float inkTop=std::max(-16.f,std::min(0.f,-ov.top));
  const float inkRight=std::max(item.width,std::min(item.width+16.f,boxW+ov.right));
  const float inkBottom=std::max(item.height,std::min(item.height+16.f,boxH+ov.bottom));
  const float sigmaInner=std::max(.8f,fontSize*.065f*s);
  const float sigmaOuter=std::max(2.f,fontSize*.17f*s);
  const UINT padPx=std::min(48u,UINT(std::ceil(sigmaOuter*3.f))+2u);
  const float padDip=float(padPx)/s;
  const float originX=std::floor((inkLeft-padDip)*s)/s;
  const float originY=std::floor((inkTop-padDip)*s)/s;
  const UINT pxW=std::max(1u,UINT(std::ceil((inkRight+padDip-originX)*s)));
  const UINT pxH=std::max(1u,UINT(std::ceil((inkBottom+padDip-originY)*s)));
  const uint64_t byteCount=uint64_t(pxW)*pxH*4ull;
  if(pxW>shelfMaxW||pxH>maxBmp||byteCount>maxAtlasBytes||!reserveRowTextureBytes(rowSerial,size_t(byteCount))){gt.failed=true;return gt;}
  Com<ID2D1Factory> fac;target->GetFactory(fac.put());
  if(!fac){gt.failed=true;return gt;}
  struct DibGuard {
   HDC dc=nullptr;HBITMAP bmp=nullptr;HGDIOBJ old=nullptr;BYTE* bits=nullptr;
   ~DibGuard(){if(old&&dc)SelectObject(dc,old);if(bmp)DeleteObject(bmp);if(dc)DeleteDC(dc);}
  } dib;
  dib.dc=CreateCompatibleDC(nullptr);
  BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=LONG(pxW);info.bmiHeader.biHeight=-LONG(pxH);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
  if(dib.dc)dib.bmp=CreateDIBSection(dib.dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&dib.bits),nullptr,0);
  if(!dib.dc||!dib.bmp||!dib.bits){gt.failed=true;return gt;}
  dib.old=SelectObject(dib.dc,dib.bmp);
  std::memset(dib.bits,0,size_t(byteCount));
  Com<ID2D1DCRenderTarget> dcRt;
  const auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),dpi,dpi);
  if(FAILED(fac->CreateDCRenderTarget(&props,dcRt.put()))){gt.failed=true;return gt;}
  RECT bindRect{0,0,LONG(pxW),LONG(pxH)};
  if(FAILED(dcRt->BindDC(dib.dc,&bindRect))){gt.failed=true;return gt;}
  dcRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  Com<ID2D1SolidColorBrush> white;
  if(FAILED(dcRt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),white.put()))){gt.failed=true;return gt;}
  dcRt->BeginDraw();dcRt->Clear(D2D1::ColorF(0.f,0.f,0.f,0.f));
  dcRt->DrawTextLayout(D2D1::Point2F(-originX,-originY),item.value.get(),white.get(),D2D1_DRAW_TEXT_OPTIONS_NONE);
  if(FAILED(dcRt->EndDraw())){gt.failed=true;return gt;}
  GdiFlush();
  const size_t nPix=size_t(pxW)*pxH;
  std::vector<float> src(nPix,0.f);
  for(size_t k=0;k<nPix;++k)src[k]=dib.bits[k*4+1]/255.f;
  const auto inner=gaussianBlur1D(src,pxW,pxH,sigmaInner);
  const auto outer=gaussianBlur1D(src,pxW,pxH,sigmaOuter);
  std::vector<BYTE> bgra(size_t(byteCount),0);
  for(size_t k=0;k<nPix;++k){
   const float aInner=std::min(1.f,inner[k]*.85f);
   const float aOuter=std::min(1.f,outer[k]*.72f);
   const float alpha=std::min(1.f,aInner+aOuter*(1.f-aInner));
   const uint8_t v=uint8_t(std::lround(alpha*255.f));
   bgra[k*4+0]=v;bgra[k*4+1]=v;bgra[k*4+2]=v;bgra[k*4+3]=v;
  }
  if(FAILED(target->CreateBitmap(D2D1::SizeU(pxW,pxH),bgra.data(),pxW*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),dpi,dpi),gt.bitmap.put()))){gt.failed=true;return gt;}
  gt.left=originX;gt.top=originY;gt.width=float(pxW)/s;gt.height=float(pxH)/s;
  gt.bytes=size_t(byteCount);gt.ready=true;++glowTextureBuilds;
  return gt;
 }
 void evictRowTextures(uint64_t keepSerial,size_t budget){
  while(rowTextures.size()>48||totalRowTextureBytes>budget){
   auto oldest=rowTextures.end();
   for(auto it=rowTextures.begin();it!=rowTextures.end();++it){
    if(it->first==keepSerial)continue;
    if(oldest==rowTextures.end()||it->second.lastUsed<oldest->second.lastUsed)oldest=it;
   }
   if(oldest==rowTextures.end())break;
   totalRowTextureBytes-=std::min(totalRowTextureBytes,oldest->second.bytes());
   rowTextures.erase(oldest);
  }
 }
 bool reserveRowTextureBytes(uint64_t serial,size_t bytes){
  const auto row=rowTextures.find(serial);
  if(bytes>maxRowTextureBytes||(row!=rowTextures.end()&&row->second.bytes()>maxRowTextureBytes-bytes))return false;
  const size_t budget=maxRowTextureBytes-bytes;
  evictRowTextures(serial,budget);
  return totalRowTextureBytes<=budget;
 }
 RowTexture* acquireRowTexture(const LyricLayout&content,bool needMain,bool needWords,bool needSubtitle){
  if(!target||content.serial==0)return nullptr;
  FLOAT dx=96.f,dy=96.f;target->GetDpi(&dx,&dy);
  const float dpi=dx>0.f?dx:96.f;
  auto& entry=rowTextures[content.serial];
  if(std::abs(entry.dpi-dpi)>.01f){
   totalRowTextureBytes-=std::min(totalRowTextureBytes,entry.bytes());
   entry=RowTexture{};entry.dpi=dpi;
  }
  IDWriteTextLayout* mainPtr=content.main.value.get();
  IDWriteTextLayout* subPtr=content.subtitle.value.get();
  IDWriteTextLayout* firstWordPtr=content.glyphs.empty()?nullptr:content.glyphs.front().layout.value.get();
  auto replaceAtlas=[&](TextAtlas&slot,TextAtlas next){
   totalRowTextureBytes-=std::min(totalRowTextureBytes,slot.bytes);
   slot=std::move(next);
   totalRowTextureBytes+=slot.bytes;
  };
  if(entry.mainKey!=mainPtr){replaceAtlas(entry.main,TextAtlas{});entry.mainKey=mainPtr;}
  if(entry.subKey!=subPtr){replaceAtlas(entry.subtitle,TextAtlas{});entry.subKey=subPtr;}
  if(entry.wordCountKey!=content.glyphs.size()||entry.firstWordKey!=firstWordPtr){
   replaceAtlas(entry.words,TextAtlas{});
   for(const auto&kv:entry.glows)totalRowTextureBytes-=std::min(totalRowTextureBytes,kv.second.bytes);
   entry.glows.clear();
   entry.wordCountKey=content.glyphs.size();entry.firstWordKey=firstWordPtr;
  }
  if(needMain&&!entry.main.ready&&!entry.main.failed&&mainPtr){
   replaceAtlas(entry.main,rasterizeLayouts({&content.main},dpi,content.serial));
  }
  if(needWords&&!entry.words.ready&&!entry.words.failed&&!content.glyphs.empty()){
   std::vector<const Layout*> items;items.reserve(content.glyphs.size());
   for(const auto& g:content.glyphs)items.push_back(&g.layout);
   replaceAtlas(entry.words,rasterizeLayouts(items,dpi,content.serial));
  }
  if(needSubtitle&&!entry.subtitle.ready&&!entry.subtitle.failed&&subPtr){
   replaceAtlas(entry.subtitle,rasterizeLayouts({&content.subtitle},dpi,content.serial));
  }
  entry.lastUsed=++textTick;
  evictRowTextures(content.serial,maxRowTextureBytes);
  return &entry;
 }
 GlowTexture* acquireGlowTexture(const LyricLayout&content,size_t wordIdx,float fontSize){
  if(!target||content.serial==0||wordIdx>=content.glyphs.size())return nullptr;
  FLOAT dx=96.f,dy=96.f;target->GetDpi(&dx,&dy);
  const float dpi=dx>0.f?dx:96.f;
  auto& entry=rowTextures[content.serial];
  if(std::abs(entry.dpi-dpi)>.01f){
   totalRowTextureBytes-=std::min(totalRowTextureBytes,entry.bytes());
   entry=RowTexture{};entry.dpi=dpi;
  }
  IDWriteTextLayout* firstWordPtr=content.glyphs.empty()?nullptr:content.glyphs.front().layout.value.get();
  if(entry.wordCountKey!=content.glyphs.size()||entry.firstWordKey!=firstWordPtr){
   totalRowTextureBytes-=std::min(totalRowTextureBytes,entry.words.bytes);
   entry.words=TextAtlas{};
   for(const auto&kv:entry.glows)totalRowTextureBytes-=std::min(totalRowTextureBytes,kv.second.bytes);
   entry.glows.clear();
   entry.wordCountKey=content.glyphs.size();entry.firstWordKey=firstWordPtr;
  }
  auto& slot=entry.glows[wordIdx];
  if(slot.ready&&std::abs(slot.fontSize-fontSize)>.01f){
   totalRowTextureBytes-=std::min(totalRowTextureBytes,slot.bytes);
   slot=GlowTexture{};
  }
  if(!slot.ready&&!slot.failed){
   totalRowTextureBytes-=std::min(totalRowTextureBytes,slot.bytes);
   slot=rasterizeGlow(content.glyphs[wordIdx].layout,dpi,fontSize,content.serial);
   totalRowTextureBytes+=slot.bytes;
  }
  entry.lastUsed=++textTick;
  evictRowTextures(content.serial,maxRowTextureBytes);
  return &slot;
 }
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

class App {
 friend struct NativeTest;
 float currentFontSize=::CurrentFontSize,contextFontSize=::ContextFontSize,translationFontSize=::TranslationFontSize;
 static constexpr UINT Incoming=WM_APP+1,CoverReady=WM_APP+2,FrameReady=WM_APP+3,BridgeDisconnected=WM_APP+4;
 HWND hwnd=nullptr;HANDLE frameTimer=nullptr;bool frameArmed=false;double frameDeadline=0,framePeriod=0;std::unique_ptr<Bridge> bridge;
 ModalFrames modalFrames;OwnerProcess ownerProcess;bool systemMoving=false,connectionStale=false;
 std::shared_ptr<CoverQueue> coverQueue;std::thread coverWorker;
 Com<ID2D1Factory> factory;Com<IDWriteFactory> textFactory;Com<IWICImagingFactory> imaging;
 Com<ID2D1HwndRenderTarget> target;Graphics graphics;Com<ID2D1RoundedRectangleGeometry> coverClip;
 static constexpr unsigned MaxTargetLossRetries=3;
 unsigned targetLossRetries=0;
 Com<IDWriteTextFormat> mainFormat,secondaryFormat,metaFormat;
 Layout metaLayout,placeholder;int preludePreview=-1;std::map<int,LyricGroup>groups;InterludeAnimation preludeDots;LyricFlight flight;float secondaryRowHeight=0;
 std::vector<std::shared_ptr<LyricLayout>> lyricLayouts;
 size_t nextLayoutToWarm=0;
 static constexpr UINT_PTR LayoutWarmupTimer=5;
 LyricClock indicatorMotion;double lastIndicatorTime=0;
 LyricClock motion;bool transitionActive=false,snapRow=false,testWall=false,freezePlayback=false,layoutDirty=true,stagger=true;double testWallMs=0;int focusIndex=-999,lineCurve=0,wordMode=0,browseIndex=-1,wheelRemainder=0;double browseUntil=0;bool returnFromBrowse=false,manualTransition=false,manualLayoutPending=false;float viewportTop=32,viewportBottom=98,layoutAnchor=44,mainRowHeight=28;uint64_t groupSerial=0,layoutSerial=0,layoutChanges=0;
 Com<ID2D1PathGeometry> playGeometry;
 std::vector<Line> lines;CoverData cover;BackgroundSettings background;BackgroundClock backgroundClock;FluidBackground fluid;
 D2D1_COLOR_F bgA=D2D1::ColorF(33/255.f,42/255.f,66/255.f),bgB=D2D1::ColorF(76/255.f,67/255.f,99/255.f);
 D2D1_RECT_F artBounds{};float artRadius=10;TransportGeometry transport{};
 bool seekPending=false;uint64_t seekSerial=0,seekRequest=0;double seekStarted=0,seekFallback=0;
 std::wstring settingsPath,capturePath,title=L"MiniLyrics",artist,coverUrl;std::string songId,error,lyricMode="waiting";
 double position=0,anchor=0,duration=0,offset=0,received=0,revision=0,rowSince=-1000,coverSince=-1000;
 double savedX=80,savedY=80,savedWidth=360,savedHeight=130;float dpi=96,width=360,height=130,artSize=96,textX=124;
 bool positionInPixels=false;
 SIZE geometryPixels{};float geometryDpi=0;
 RECT normalWindowRect{};float normalWindowScale=1.f;bool hasNormalRect=false;
 bool playing=false,valid=false,hasClock=false,untimed=false,through=false,top=true,translation=true,hover=false,dragProgress=false,resizing=false,closing=false,hardware=false,frameRunning=false;
 bool demo=false,renderVisible=false;int fps=60,index=-999;UINT intervalMs=200;uint64_t renderCalls=0,lineBuildCalls=0,coverEpoch=0;double dragRatio=0;
 POINT resizeStart{};SIZE resizeSize{};
 double current()const{return position+(playing&&valid&&!freezePlayback?std::max(0.,nowMs()-anchor):0.);}
 double animationWall()const{return testWall?testWallMs:nowMs();}
 double motionNow()const{return motion.now(animationWall());}
 double indicatorNow()const{return indicatorMotion.now(animationWall());}
 void stopBrowsing(bool animateReturn=false){browseIndex=-1;wheelRemainder=0;browseUntil=0;returnFromBrowse=true;manualTransition=manualLayoutPending=animateReturn;KillTimer(hwnd,4);layoutDirty=true;}
 void finishTransition(){for(auto&item:groups){auto&g=item.second;g.y.snap(g.y.to);g.zoom.snap(g.zoom.to);g.mainAlpha.snap(g.mainAlpha.to);g.subAlpha.snap(g.subAlpha.to);}transitionActive=false;manualTransition=false;manualLayoutPending=false;}
 float scale()const{return dpi/96.f;}
 float lyricAnchor()const{return height/2+4-(mainRowHeight*(1+contextFontSize/currentFontSize)+11)/2;}
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
 int groupIdFor(const LyricGroup&g)const{for(const auto&item:groups)if(&item.second==&g)return item.first;return -1;}
 bool showInterlude(const LyricGroup&g)const{return valid&&!untimed&&g.active&&isInterlude(g);}
 bool indicatorVisible(const LyricGroup&g,int id,double time)const{return valid&&!untimed&&isInterlude(g)&&g.dots.visible(indicatorNow(),time,g.content->model.time,interludeDuration(g.content->model,id));}
 bool indicatorVisible(const LyricGroup&g)const{return indicatorVisible(g,groupIdFor(g),current()+offset);}
 bool groupVisible(const LyricGroup&g,double now,int id,double time)const{if(g.opacity<=.001f)return false;if(emptyGroup(g)&&!indicatorVisible(g,id,time)&&!g.middleInterlude)return false;float y=g.y.value(now);return y+g.renderedHeight(g.zoom.value(now))>=viewportTop&&y<=viewportBottom;}
 bool groupVisible(const LyricGroup&g,double now)const{return groupVisible(g,now,groupIdFor(g),current()+offset);}
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
  lyricLayouts.clear();nextLayoutToWarm=0;graphics.clearRowTextures();
  placeholder=Layout();updatePlaceholders();save();update();scheduleLayoutWarmup();
 }
 void rebuildMeta(){metaLayout=layout(title+(artist.empty()?L"":L"  ·  "+artist),metaFormat.get(),std::max(20.f,width-textX-85));if(metaLayout.value){DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};Com<IDWriteInlineObject>sign;if(SUCCEEDED(textFactory->CreateEllipsisTrimmingSign(metaFormat.get(),sign.put())))metaLayout.value->SetTrimming(&trim,sign.get());}}
 const std::shared_ptr<LyricLayout>& cachedLayout(size_t at){
  if(lyricLayouts.size()!=lines.size())lyricLayouts.resize(lines.size());
  auto&cached=lyricLayouts[at];const auto&line=lines[at];
  if(cached&&cached->model==line)return cached;
  auto built=std::make_shared<LyricLayout>();built->serial=++layoutSerial;built->model=line;
  built->main=layout(line.text,mainFormat.get());built->mainHeight=built->main.height;
  built->glyphs.reserve(line.words.size());
  float x=0;
  for(const auto&word:line.words){
   Glyph glyph;
   glyph.layout=layout(word.text,mainFormat.get());
   if(glyph.layout.value){
    DWRITE_OVERHANG_METRICS ink{};
    if(SUCCEEDED(glyph.layout.value->GetOverhangMetrics(&ink))){
     glyph.inkLeft=-ink.left;
     glyph.inkRight=glyph.layout.value->GetMaxWidth()+ink.right;
    }else glyph.inkRight=glyph.layout.width;
   }
   glyph.time=word.time;
   glyph.duration=word.duration;
   glyph.longNote=word.longNote;
   glyph.emphasize=word.emphasize;
   glyph.x=x;
   x+=glyph.layout.width;
   built->mainHeight=std::max(built->mainHeight,glyph.layout.height);
   built->glyphs.push_back(std::move(glyph));
  }
  Glyph* previousInk=nullptr;
  for(auto&glyph:built->glyphs){
   if(glyph.inkRight<=glyph.inkLeft)continue;
   if(previousInk){
    // Reserve equal space on both sides, including when adjacent words animate together.
    const float room=std::max(0.f,glyph.x+glyph.inkLeft-previousInk->x-previousInk->inkRight)*.5f;
    const float previousReach=previousInk->inkRight-previousInk->layout.width*.5f;
    const float currentReach=glyph.layout.width*.5f-glyph.inkLeft;
    if(previousReach>0.f)previousInk->maxEmphasisScale=std::min(previousInk->maxEmphasisScale,1.f+room/previousReach);
    if(currentReach>0.f)glyph.maxEmphasisScale=std::min(glyph.maxEmphasisScale,1.f+room/currentReach);
   }
   previousInk=&glyph;
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
 void roles(bool captureRelease=true){
  double now=motionNow(),time=current()+offset;
  for(auto&item:groups){auto&g=item.second;bool active=item.first==index;if(active==g.active)continue;
   if(g.active){
    g.resetWords.clear();g.resetGlows.clear();
    if(captureRelease){
     for(size_t i=0;i<g.content->glyphs.size();++i){
      const auto&w=g.content->glyphs[i];
      g.resetWords.push_back({floatWord(time-w.time,w.duration),float(unit((time-w.time)/std::max(1.,w.duration))),wordEmphasisScale(g,i,time,now)});
      if((w.longNote||w.emphasize)&&time>w.time&&time<w.time+w.duration){
       const double elapsed=time-w.time;
       const float tail=w.longNote?longNoteGlow(elapsed,w.duration):0.f;
       const float emphasis=w.emphasize?emphasisGlow(elapsed,w.duration,isLastVocalWord(g.content->model,i)):0.f;
       if(tail>0.f||emphasis>0.f)g.resetGlows.push_back({i,tail,emphasis,elapsed>=w.duration*.35?LongNoteFadeOut:200.});
      }
     }
     g.resetAt=now;
    }
   }else{
    g.resetGlows.clear();
   }
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
  for(auto&item:groups){item.second.resetWords.clear();item.second.resetGlows.clear();}
  if(canFly){int actual=rowAt(target+offset),desired=std::max(0,rowAt(target+offset+(playing&&stagger?200:0)));beginFlight(desired,actual);}else flight=LyricFlight();
  if(browseIndex>=0&&!untimed)stopBrowsing();returnFromBrowse=false;error.clear();snapRow=!canFly;position=target;anchor=nowMs();valid=true;layoutDirty=true;
 }
 void reflow(int desired,bool animate,bool captureRelease=true){
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
   if(animate){double delay=browseIndex>=0||returnFromBrowse||previous==desired?0:rnpDelay(n-desired,direction,stagger),duration=browseIndex>=0?250:500;
    if(lineCurve==LineCurveSpring){
     const bool isBrowse=browseIndex>=0;
     const SpringParams ySpring=isBrowse?SpringParams{.7,160.,21.,.02}:(stagger?SpringParams{.9,72.,15.,.02}:SpringParams{.9,90.,12.,.02});
     const SpringParams zSpring=isBrowse?SpringParams{1.2,160.,24.,.0002}:SpringParams{2.,100.,25.,.0002};
     g.y.targetSpring(target,now,delay,ySpring);g.zoom.targetSpring(groupScale(n,desired),now,delay,zSpring);
    }else{
     g.y.target(target,now,delay,duration,lineCurve);g.zoom.target(groupScale(n,desired),now,delay,duration,lineCurve);
    }
   }else{g.y.snap(target);g.zoom.snap(groupScale(n,desired));}
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
  roles(captureRelease);layoutDirty=false;
 }
 void updatePlaceholders(){
  if(lines.empty())preludeDots=InterludeAnimation();
  const wchar_t*message=L"等待歌词";
  if(lyricMode=="loading")message=L"正在获取歌词";
  else if(lyricMode=="empty")message=L"暂无歌词";
  else if(lyricMode=="instrumental")message=L"纯音乐";
  else if(lyricMode=="unsupported")message=L"暂无在线歌词";
  else if(lyricMode=="error")message=L"歌词获取失败";
  placeholder=layout(message,mainFormat.get());
  preludePreview=-1;for(size_t i=0;i<lines.size();i++){const auto&line=lines[i];if(!line.isInterlude&&(!line.text.empty()||!line.translation.empty()||std::any_of(line.words.begin(),line.words.end(),[](const Word&w){return !w.text.empty();}))){preludePreview=int(i);break;}}
 }
 void recordNormalBounds(){if(!hwnd||IsIconic(hwnd))return;RECT r{};if(GetWindowRect(hwnd,&r)&&r.right>r.left&&r.bottom>r.top){normalWindowRect=r;normalWindowScale=scale();hasNormalRect=true;}}
 bool geometry(){
  RECT rect{};GetClientRect(hwnd,&rect);
  if(geometryPixels.cx==rect.right&&geometryPixels.cy==rect.bottom&&geometryDpi==dpi)return false;
  // Record this before SetWindowRgn, which sends nested window-position messages.
  geometryPixels={rect.right,rect.bottom};geometryDpi=dpi;flight=LyricFlight();recordNormalBounds();
  width=std::max(1.f,float(rect.right)/scale());height=std::max(1.f,float(rect.bottom)/scale());
  auto art=coverGeometry(rect.right,rect.bottom,scale());artSize=art.side;artRadius=art.radius;
  artBounds=D2D1::RectF(art.left,art.top,art.left+art.side,art.top+art.side);textX=artBounds.right+16;transport=transportGeometry(width,height,textX,scale());
  viewportTop=32;viewportBottom=std::min(height-28,transport.y-14);
  check(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(artBounds,artRadius,artRadius),coverClip.put()));
  HRGN region=CreateRoundRectRgn(0,0,rect.right+1,rect.bottom+1,int(34*scale()),int(34*scale()));if(!SetWindowRgn(hwnd,region,FALSE))DeleteObject(region);
  graphics.resize(width,height);rebuildMeta();layoutDirty=true;return true;
 }
 void ensureTarget(){
  RECT rect{};GetClientRect(hwnd,&rect);auto size=D2D1::SizeU(std::max(1L,rect.right),std::max(1L,rect.bottom));
  // Resize discards the back buffer. Pair it with a full draw in the same paint.
  if(target){auto old=target->GetPixelSize();if((old.width!=size.width||old.height!=size.height)&&FAILED(target->Resize(size))){graphics.clear();target.reset();}}
  if(!target){auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE,D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN,D2D1_ALPHA_MODE_UNKNOWN),dpi,dpi);
   HRESULT hr=factory->CreateHwndRenderTarget(props,D2D1::HwndRenderTargetProperties(hwnd,D2D1::SizeU(std::max(1L,rect.right),std::max(1L,rect.bottom)),D2D1_PRESENT_OPTIONS_IMMEDIATELY),target.put());hardware=SUCCEEDED(hr);
   if(FAILED(hr)){props.type=D2D1_RENDER_TARGET_TYPE_SOFTWARE;check(factory->CreateHwndRenderTarget(props,D2D1::HwndRenderTargetProperties(hwnd,D2D1::SizeU(std::max(1L,rect.right),std::max(1L,rect.bottom)),D2D1_PRESENT_OPTIONS_IMMEDIATELY),target.put()));}
   target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  }if(!graphics.target)graphics.init(target.get(),bgA,bgB,cover,width,height);
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
  float zoom=std::max(.01f,g.zoom.value(now)),available=std::max(1.f,width-textX-12),overflow=rowWidth(g)*zoom-available;
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
  const float exitAlpha=animation.exitAlpha(time,start,duration),exitScale=animation.exitScale(time,start,duration);
  float alpha=opacity*animation.opacity.value(now)*exitAlpha;if(alpha<=0)return;
  float breath=animation.scale(now)*exitScale,diameter=currentFontSize*.7f,gap=currentFontSize*.5f;
  g.brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
  for(int i=0;i<3;i++){auto dot=animation.dot(time,start,duration,i,now);float radius=diameter*.5f*dot.scale*breath;g.brush->SetOpacity(alpha*dot.alpha);g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x+(diameter*.5f+i*(diameter+gap))*breath,y+mainRowHeight*.5f),radius,radius),g.brush.get());}
  g.brush->SetOpacity(1);
 }
 static bool hasVocalChar(const std::wstring&text){
  if(text.empty())return false;
  std::vector<WORD> types(text.size(),0);
  if(!GetStringTypeW(CT_CTYPE1,text.data(),int(text.size()),types.data()))return false;
  for(WORD t:types)if(t&(C1_ALPHA|C1_DIGIT))return true;
  return false;
 }
 static bool isLastVocalWord(const Line&line,size_t idx){
  if(idx>=line.words.size())return false;
  if(line.words[idx].lineTail)return true;
  for(const auto&w:line.words)if(w.lineTail)return false;
  for(size_t j=idx+1;j<line.words.size();++j)if(hasVocalChar(line.words[j].text))return false;
  return true;
 }
 double longNoteTailGap(const Line&line,int lineId,size_t wordIdx)const{
  if(lineId<0||lineId>=int(lines.size())||wordIdx>=line.words.size())return 0.;
  if(!isLastVocalWord(line,wordIdx))return 0.;
  const double end=line.words[wordIdx].time+line.words[wordIdx].duration;
  for(int nextId=lineId+1;nextId<int(lines.size());++nextId){
   const auto&next=lines[nextId];
   if(isInterlude(next)||!hasVocalChar(next.text))continue;
   const double gap=next.time-end;
   return (gap>0.&&gap<LongNoteTailMaxGap)?gap:0.;
  }
  return 0.;
 }
 float wordEmphasisScale(const LyricGroup&row,size_t wordIdx,double time,double now)const{
  if(row.active){
   const auto&w=row.content->glyphs[wordIdx];
   const double elapsed=time-w.time;
   if(w.maxEmphasisScale<=1.f||(!w.emphasize&&!w.longNote)||w.duration<LongNoteFullDuration||elapsed<=0.||elapsed>=w.duration)return 1.f;
   return std::min(w.maxEmphasisScale,emphasisScale(elapsed,w.duration,w.emphasize,w.longNote,isLastVocalWord(row.content->model,wordIdx)));
  }
  if(wordIdx<row.resetWords.size()&&now-row.resetAt<200){
   const double p=unit((now-row.resetAt)/200.);
   return mixf(row.resetWords[wordIdx].scale,1.f,lyricEase(p));
  }
  return 1.f;
 }
 float wordGlowIntensity(const LyricGroup&row,int lineId,size_t wordIdx,double time,double now)const{
  if(!row.active){
   for(const auto&gr:row.resetGlows){
    if(gr.wordIndex==wordIdx){
     const double age=now-row.resetAt;
     const float tail=gr.longNote*float(1.-unit(age/gr.longNoteDuration));
     const float emphasis=gr.emphasis*float(1.-unit(age/200.));
     return std::max(tail,emphasis);
    }
   }
  }
  const auto&w=row.content->glyphs[wordIdx];
  const double elapsed=time-w.time;
  if(!row.active&&elapsed<w.duration)return 0.f;
  float glow=0.f;
  if(w.longNote)glow=longNoteGlow(elapsed,w.duration,longNoteTailGap(row.content->model,lineId,wordIdx));
  if(w.emphasize&&row.active)glow=std::max(glow,emphasisGlow(elapsed,w.duration,isLastVocalWord(row.content->model,wordIdx)));
  return glow;
 }
 bool hasLongNoteGlow(const LyricGroup&row,int id,double time,double now)const{
  if(!valid||untimed||id>index)return false;
  const float zoom=std::max(.01f,row.zoom.value(now)),y=row.y.value(now);
  std::optional<float> rowLeft;
  for(size_t i=0;i<row.content->glyphs.size();++i){
   if(wordGlowIntensity(row,id,i,time,now)<=0.f)continue;
   if(!rowLeft)rowLeft=lyricLeft(row,id,time,now,zoom);
   if(glowMayBeVisible(row.content->glyphs[i],wordPlacement(row,i,time,now,*rowLeft,y,zoom)))return true;
  }
  return false;
 }
 bool hasLongNoteGlow(const LyricGroup&row,int id,double time)const{return hasLongNoteGlow(row,id,time,motionNow());}
 FloatWord wordMotion(const LyricGroup&row,size_t index,double time,double now)const{
  const auto&word=row.content->glyphs[index];
  if(row.active)return floatWord(time-word.time,word.duration);
  if(index<row.resetWords.size()&&now-row.resetAt<200){
   const double p=unit((now-row.resetAt)/200);
   return {mixf(row.resetWords[index].motion.alpha,.4f,p),mixf(row.resetWords[index].motion.lift,0,lyricEase(p))};
  }
  return {.4f,0};
 }
 struct WordPlacement {float x,y,scale,alpha;};
 float lyricLeft(const LyricGroup&row,int id,double time,double now,float zoom)const{
  const float available=std::max(1.f,width-textX-12);
  return textX-std::max(0.f,rowWidth(row)*zoom-available)*horizontalPhase(row,time,id,now);
 }
 WordPlacement wordPlacement(const LyricGroup&row,size_t i,double time,double now,float left,float y,float zoom)const{
  const auto&w=row.content->glyphs[i];const auto motion=wordMotion(row,i,time,now);
  const float scale=zoom*wordEmphasisScale(row,i,time,now);
  const float anchorShift=(zoom-scale)*.5f;
  return {left+w.x*zoom+w.layout.width*anchorShift,y-motion.lift*zoom+row.content->mainHeight*anchorShift,scale,motion.alpha};
 }
 float glowClipLeft()const{return std::max({5.f,artBounds.right+2.f,textX-5.f});}
 bool glowMayBeVisible(const Glyph&word,const WordPlacement&p)const{
  // Include the rasterizer's maximum ink overhang and Gaussian padding, even before a bitmap exists.
  const float pad=16.f+std::max(16.f,currentFontSize*.51f+8.f);
  return p.x+(word.layout.width+pad)*p.scale>glowClipLeft()&&p.x-pad*p.scale<width-7.f&&
         p.y+(word.layout.height+pad)*p.scale>viewportTop&&p.y-pad*p.scale<viewportBottom;
 }
 void drawGroupAt(Graphics&g,const LyricGroup&r,int id,double time,double now,bool glowOnly=false){
  if(r.opacity<=0||(glowOnly&&!hasLongNoteGlow(r,id,time,now)))return;
  float zoom=std::max(.01f,r.zoom.value(now)),y=r.y.value(now);
  const float available=std::max(1.f,width-textX-12),rowLeft=lyricLeft(r,id,time,now,zoom),scrollX=textX-rowLeft;
  if(glowOnly){
   for(size_t i=0;i<r.content->glyphs.size();i++){
    const auto&w=r.content->glyphs[i];
    if(!w.longNote&&!w.emphasize&&r.resetGlows.empty())continue;
    const float glow=wordGlowIntensity(r,id,i,time,now)*r.opacity;
    if(glow<=0.f)continue;
    const auto p=wordPlacement(r,i,time,now,rowLeft,y,zoom);
    if(!glowMayBeVisible(w,p))continue;
    GlowTexture* gt=g.acquireGlowTexture(*r.content,i,currentFontSize);
    if(gt&&gt->ready&&gt->bitmap){
     const D2D1_RECT_F dst=D2D1::RectF(
      p.x+gt->left*p.scale,p.y+gt->top*p.scale,
      p.x+(gt->left+gt->width)*p.scale,p.y+(gt->top+gt->height)*p.scale);
     if(dst.right<=glowClipLeft()||dst.left>=width-7.f||dst.bottom<=viewportTop||dst.top>=viewportBottom)continue;
     g.target->DrawBitmap(gt->bitmap.get(),dst,glow,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
   }
   return;
  }
  const float clipLeft=textX-2.f,clipRight=textX+available+2.f;
  const bool useMain=r.content->glyphs.empty()||staticPreview();
  RowTexture* tex=g.acquireRowTexture(*r.content,useMain,!useMain,r.translated);
  auto withSeamClip=[&](const TextSlice&s,const D2D1_RECT_F&dst,float baseX,float drawScale,auto&&fn){
   if(s.clipLeftEdge||s.clipRightEdge){
    const float seamL=s.clipLeftEdge?baseX+s.clipLeft*drawScale:dst.left-2.f;
    const float seamR=s.clipRightEdge?baseX+s.clipRight*drawScale:dst.right+2.f;
    if(seamR<=clipLeft||seamL>=clipRight)return;
    g.target->PushAxisAlignedClip(D2D1::RectF(seamL,dst.top-2.f,seamR,dst.bottom+2.f),D2D1_ANTIALIAS_MODE_ALIASED);
    fn();
    g.target->PopAxisAlignedClip();
   }else{
    fn();
   }
  };
  auto drawAtlasSlice=[&](const TextAtlas&atlas,size_t idx,float baseX,float baseY,float drawScale,float opacity){
   if(idx>=atlas.slices.size()||!atlas.slices[idx].valid)return;
   const auto&s=atlas.slices[idx];
   const D2D1_RECT_F dst=D2D1::RectF(baseX+s.left*drawScale,baseY+s.top*drawScale,baseX+(s.left+s.width)*drawScale,baseY+(s.top+s.height)*drawScale);
   if(dst.right<clipLeft||dst.left>clipRight)return;
   withSeamClip(s,dst,baseX,drawScale,[&]{
    g.target->DrawBitmap(atlas.pages[s.page].get(),dst,opacity,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,s.src);
   });
  };
  auto sweepBrushFor=[&](float x,float y,float scale,float wordWidth,float progress,float highAlpha,float glowWeight){
   auto*brush=g.ensureSweepBrush(highAlpha);
   if(!brush)return brush;
   const float baseFeather=std::min(r.content->mainHeight*.5f,std::max(6.f,wordWidth*.75f));
   const float extraFeather=std::min(r.content->mainHeight*.45f,std::max(4.f,wordWidth*.35f))*glowWeight;
   const float feather=(baseFeather+extraFeather)*scale;
   const float start=x-feather+(std::max(1.f,wordWidth)*scale+feather)*progress;
   brush->SetStartPoint(D2D1::Point2F(start,y));
   brush->SetEndPoint(D2D1::Point2F(start+feather,y));
   brush->SetOpacity(r.opacity);
   return brush;
  };
  auto drawSweepSlice=[&](const TextAtlas&atlas,size_t idx,float baseX,float baseY,float drawScale,float wordWidthDip,float progress,float highAlpha,float glowWeight,float rowOpacity){
   if(idx>=atlas.slices.size()||!atlas.slices[idx].valid)return;
   const auto&s=atlas.slices[idx];
   const D2D1_RECT_F dst=D2D1::RectF(baseX+s.left*drawScale,baseY+s.top*drawScale,baseX+(s.left+s.width)*drawScale,baseY+(s.top+s.height)*drawScale);
   if(dst.right<clipLeft||dst.left>clipRight)return;
   withSeamClip(s,dst,baseX,drawScale,[&]{
    if(progress<=0.f||highAlpha<=.401f){g.target->DrawBitmap(atlas.pages[s.page].get(),dst,.4f*rowOpacity,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,s.src);return;}
    if(progress>=1.f){g.target->DrawBitmap(atlas.pages[s.page].get(),dst,highAlpha*rowOpacity,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,s.src);return;}
    auto*brush=sweepBrushFor(baseX,baseY,drawScale,wordWidthDip,progress,highAlpha,glowWeight);
    if(!brush){g.target->DrawBitmap(atlas.pages[s.page].get(),dst,mixf(.4f,highAlpha,progress)*rowOpacity,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,s.src);return;}
    const auto prevAA=g.target->GetAntialiasMode();
    g.target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    g.target->FillOpacityMask(atlas.pages[s.page].get(),brush,D2D1_OPACITY_MASK_CONTENT_GRAPHICS,&dst,&s.src);
    g.target->SetAntialiasMode(prevAA);
   });
  };
  auto drawFallback=[&](auto&&fn){
   D2D1_MATRIX_3X2_F transform{};const bool scaled=zoom!=1.f;float originX=textX,originY=y;
   if(scaled){g.target->GetTransform(&transform);g.target->SetTransform(D2D1::Matrix3x2F::Scale(zoom,zoom)*D2D1::Matrix3x2F::Translation(textX,y)*transform);originX=originY=0;}
   fn(originX,originY);
   if(scaled)g.target->SetTransform(transform);
  };
  if(useMain){
   const float alpha=(staticPreview()?(id==focusIndex?1.f:.4f):r.mainAlpha.value(now))*r.opacity;
   if(tex&&tex->main.ready){
    for(size_t i=0;i<tex->main.slices.size();++i)drawAtlasSlice(tex->main,i,rowLeft,y,zoom,alpha);
   }else{
    drawFallback([&](float ox,float oy){text(g,r.content->main,ox-scrollX/zoom,oy,D2D1::ColorF(D2D1::ColorF::White),alpha);});
   }
  }else{
   const bool cachedWords=tex&&tex->words.ready&&tex->words.itemSpans.size()==r.content->glyphs.size();
   for(size_t i=0;i<r.content->glyphs.size();++i){
    const auto&w=r.content->glyphs[i];
    if(!w.layout.value)continue;
    const auto placement=wordPlacement(r,i,time,now,rowLeft,y,zoom);
    const float glowWeight=w.longNote?longNoteStrength(w.duration):(w.emphasize?.65f:0.f);
    const bool activeSweep=wordMode==1&&r.active;
    const bool exitSweep=!activeSweep&&wordMode==1&&i<r.resetWords.size()&&now-r.resetAt<200;
    const bool sweeping=activeSweep||exitSweep;
    const float p=activeSweep?float(unit((time-w.time)/std::max(1.,w.duration))):(exitSweep?r.resetWords[i].sweep:0.f);
    const float highAlpha=exitSweep?mixf(1.f,.4f,unit((now-r.resetAt)/200.)):1.f;
    if(cachedWords){
     const auto span=tex->words.itemSpans[i];
     for(size_t k=0;k<span.count;++k){
      const size_t sIdx=span.begin+k;
      if(sweeping)drawSweepSlice(tex->words,sIdx,placement.x,placement.y,placement.scale,w.layout.width,p,highAlpha,glowWeight,r.opacity);
      else drawAtlasSlice(tex->words,sIdx,placement.x,placement.y,placement.scale,placement.alpha*r.opacity);
     }
     continue;
    }
    if(placement.x+w.inkRight*placement.scale<clipLeft||placement.x+w.inkLeft*placement.scale>clipRight)continue;
    auto drawWord=[&](float x,float y){
     float alpha=placement.alpha;
     if(sweeping){
      if(p>0.f&&p<1.f&&highAlpha>.401f){
       if(auto*brush=sweepBrushFor(x,y,1.f,w.layout.width,p,highAlpha,glowWeight)){
        g.target->DrawTextLayout(D2D1::Point2F(x,y),w.layout.value.get(),brush,D2D1_DRAW_TEXT_OPTIONS_NONE);
        return;
       }
      }
      alpha=highAlpha<=.401f?.4f:mixf(.4f,highAlpha,p);
     }
     text(g,w.layout,x,y,D2D1::ColorF(D2D1::ColorF::White),alpha*r.opacity);
    };
    if(placement.scale==1.f){drawWord(placement.x,placement.y);continue;}
    D2D1_MATRIX_3X2_F transform{};
    g.target->GetTransform(&transform);
    g.target->SetTransform(D2D1::Matrix3x2F::Scale(placement.scale,placement.scale)*D2D1::Matrix3x2F::Translation(placement.x,placement.y)*transform);
    drawWord(0,0);
    g.target->SetTransform(transform);
   }
  }
  if(indicatorVisible(r,id,time)){
   drawFallback([&](float ox,float oy){
    drawInterludeAt(g,r.dots,ox,oy,time,r.content->model.time,interludeDuration(r.content->model,id),indicatorNow(),r.mainAlpha.value(now)*r.opacity);
   });
  }
  if(r.translated){
   const float subScroll=std::max(0.f,r.content->subtitle.width*zoom-available)*timedHorizontalPhase(r,time,id);
   const float subLeft=textX-subScroll,subTop=y+r.subtitleOffset(zoom);
   const float subAlpha=(staticPreview()?(id==focusIndex?.8f:.4f):r.subAlpha.value(now))*r.opacity;
   if(tex&&tex->subtitle.ready){
    for(size_t i=0;i<tex->subtitle.slices.size();++i)drawAtlasSlice(tex->subtitle,i,subLeft,subTop,zoom,subAlpha);
   }else{
    drawFallback([&](float ox,float oy){text(g,r.content->subtitle,ox-subScroll/zoom,oy+r.subtitleOffset(1.f),D2D1::ColorF(D2D1::ColorF::White),subAlpha);});
   }
  }
 }
 void drawGroup(Graphics&g,const LyricGroup&r,int id,double time){double now=motionNow();drawGroupAt(g,r,id,time,now,true);drawGroupAt(g,r,id,time,now);}
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
  bool liveWindow=(g.target==target.get())&&backgroundAnimating()&&!resizing;
  if(!resizing||fluid.pixels.empty()){double wall=animationWall();fluid.setSize(width,height);fluid.prepare(background,backgroundClock.time(wall),backgroundClock.position(wall),liveWindow);}
  if(!g.background){check(g.target->CreateBitmap(D2D1::SizeU(FluidBackground::width,FluidBackground::height),fluid.pixels.data(),FluidBackground::width*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),g.background.put()));check(g.target->CreateBitmapBrush(g.background.get(),D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),g.backgroundBrush.put()));g.backgroundRevision=fluid.revision;}
  else if(g.backgroundRevision!=fluid.revision){check(g.background->CopyFromMemory(nullptr,fluid.pixels.data(),FluidBackground::width*4));g.backgroundRevision=fluid.revision;}
  g.backgroundBrush->SetTransform(D2D1::Matrix3x2F::Scale(width/FluidBackground::width,height/FluidBackground::height));g.target->FillRoundedRectangle(card,g.backgroundBrush.get());
 }
 void drawLyricGlowsAt(Graphics&g,double time,double now){
  if(!valid||untimed||index<0)return;
  // Give the halo room in the side gutters. Keep normal text clipping intact.
  g.target->PushAxisAlignedClip(D2D1::RectF(glowClipLeft(),viewportTop,width-7,viewportBottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  for(const auto&item:groups)if(groupVisible(item.second,now,item.first,time))drawGroupAt(g,item.second,item.first,time,now,true);
  g.target->PopAxisAlignedClip();
 }
 void drawLyricsAt(Graphics&g,double time,double now){
  float y=lyricAnchor();double dotsNow=indicatorNow();
  if(!flight.rows.empty()){if(preludeDots.visible(dotsNow,time,0,lines.empty()?0.:lines.front().time))drawInterludeAt(g,preludeDots,textX,y,time,0,lines.empty()?0.:lines.front().time,dotsNow);}
  else switch(lyricView()){
   case LyricView::Waiting:text(g,placeholder,textX,y,D2D1::ColorF(D2D1::ColorF::White));return;
   case LyricView::StaticPreview:break;
   case LyricView::Prelude:if(preludeDots.visible(dotsNow,time,0,lines.empty()?0.:lines.front().time))drawInterludeAt(g,preludeDots,textX,y,time,0,lines.empty()?0.:lines.front().time,dotsNow);break;
   case LyricView::Rows:break;
  }
  for(const auto&item:groups)if(groupVisible(item.second,now,item.first,time))drawGroupAt(g,item.second,item.first,time,now);
 }
 void drawLyrics(Graphics&g,double time){double now=motionNow();drawLyricGlowsAt(g,time,now);drawLyricsAt(g,time,now);}
 void draw(Graphics&g){
  auto*t=g.target;double time=current()+offset,now=motionNow();

  t->Clear(D2D1::ColorF(0,0));auto card=D2D1::RoundedRect(D2D1::RectF(.5f,.5f,width-.5f,height-.5f),17,17);drawBackground(g,card);g.brush->SetColor(D2D1::ColorF(.62f,.65f,.80f,.65f));t->DrawRoundedRectangle(card,g.brush.get(),1);
  float coverAlpha=(g.cover&&playing)?float(clamp((nowMs()-coverSince)/250,0,1)):1.f;
  if(!g.cover){
   g.brush->SetColor(D2D1::ColorF(.33f,.39f,.51f));
   t->FillRoundedRectangle(D2D1::RoundedRect(artBounds,artRadius,artRadius),g.brush.get());
  }else if(g.coverBrush&&coverAlpha>=.999f&&g.coverOpaque){
   float side=float(std::min(cover.width,cover.height)),x=(cover.width-side)/2,y=(cover.height-side)/2;
   float scaleArt=artSize/std::max(1.f,side);
   g.coverBrush->SetTransform(D2D1::Matrix3x2F::Translation(-x,-y)*D2D1::Matrix3x2F::Scale(scaleArt,scaleArt)*D2D1::Matrix3x2F::Translation(artBounds.left,artBounds.top));
   t->FillRoundedRectangle(D2D1::RoundedRect(artBounds,artRadius,artRadius),g.coverBrush.get());
  }else{
   // One rounded mask for both layers avoids accumulated antialiasing halos.
   t->PushLayer(D2D1::LayerParameters(artBounds,coverClip.get()),nullptr);
   g.brush->SetColor(D2D1::ColorF(.33f,.39f,.51f));t->FillRectangle(artBounds,g.brush.get());
   float side=float(std::min(cover.width,cover.height)),x=(cover.width-side)/2,y=(cover.height-side)/2;
   t->DrawBitmap(g.cover.get(),artBounds,coverAlpha,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,D2D1::RectF(x,y,x+side,y+side));
   t->PopLayer();
  }
  text(g,metaLayout,textX,12,D2D1::ColorF(.87f,.89f,.94f));
  drawLyricGlowsAt(g,time,now);
  t->PushAxisAlignedClip(D2D1::RectF(textX,viewportTop,width-12,viewportBottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  drawLyricsAt(g,time,now);
  t->PopAxisAlignedClip();
  if(hover&&!through){
   float y=transport.y;g.brush->SetColor(D2D1::ColorF(.55f,.58f,.69f));t->DrawLine(D2D1::Point2F(textX,y),D2D1::Point2F(transport.right,y),g.brush.get(),3);
   float ratio=float(dragProgress?dragRatio:duration>0?clamp(current()/duration,0,1):0);g.brush->SetColor(D2D1::ColorF(.91f,.93f,.99f));t->DrawLine(D2D1::Point2F(textX,y),D2D1::Point2F(transport.left+transport.length()*ratio,y),g.brush.get(),3);t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(transport.left+transport.length()*ratio,y),3,3),g.brush.get());
   // Transport buttons use vectors, not font glyphs with different baselines.
   float cy=y,previous=transport.previous,next=transport.next,play=transport.play;
   g.brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
   t->DrawLine(D2D1::Point2F(previous+3,cy-5),D2D1::Point2F(previous-3,cy),g.brush.get(),2);
   t->DrawLine(D2D1::Point2F(previous-3,cy),D2D1::Point2F(previous+3,cy+5),g.brush.get(),2);
   t->DrawLine(D2D1::Point2F(next-3,cy-5),D2D1::Point2F(next+3,cy),g.brush.get(),2);
   t->DrawLine(D2D1::Point2F(next+3,cy),D2D1::Point2F(next-3,cy+5),g.brush.get(),2);
   if(playing){float halfGap=std::round(2*scale())/scale(),barWidth=std::round(3*scale())/scale(),halfHeight=std::round(7*scale())/scale();t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(play-halfGap-barWidth,cy-halfHeight,play-halfGap,cy+halfHeight),.6f,.6f),g.brush.get());t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(play+halfGap,cy-halfHeight,play+halfGap+barWidth,cy+halfHeight),.6f,.6f),g.brush.get());}
   else {D2D1_MATRIX_3X2_F transform;t->GetTransform(&transform);t->SetTransform(D2D1::Matrix3x2F::Translation(play,cy)*transform);t->FillGeometry(playGeometry.get(),g.brush.get());t->SetTransform(transform);}
   g.brush->SetColor(D2D1::ColorF(1,1,1,top?1.f:.4f));float x=width-66;t->DrawLine(D2D1::Point2F(x-3,15),D2D1::Point2F(x+3,15),g.brush.get(),2);t->DrawLine(D2D1::Point2F(x,13),D2D1::Point2F(x,25),g.brush.get(),1.5f);
   g.brush->SetColor(D2D1::ColorF(1,1,1));x=width-42;t->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x-4,18,x+4,25),1,1),g.brush.get(),1);t->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x,17),3,4),g.brush.get(),1);
   x=width-18;t->DrawLine(D2D1::Point2F(x-3,16),D2D1::Point2F(x+3,22),g.brush.get(),1.5f);t->DrawLine(D2D1::Point2F(x+3,16),D2D1::Point2F(x-3,22),g.brush.get(),1.5f);
  }
  g.brush->SetColor(D2D1::ColorF(1,1,1,.2f));t->DrawLine(D2D1::Point2F(width-12,height-3),D2D1::Point2F(width-3,height-12),g.brush.get(),1);
 }
 void stopFrames(){if(frameRunning||frameArmed){KillTimer(hwnd,1);if(frameTimer)CancelWaitableTimer(frameTimer);}frameArmed=false;frameRunning=false;frameDeadline=0;}
 void setRenderVisible(bool visible){
  if(renderVisible==visible)return;renderVisible=visible;
  if(!visible){indicatorMotion.setRunning(false,animationWall());flight=LyricFlight();stopBrowsing();snapRow=true;layoutDirty=true;}
  update();scheduleLayoutWarmup();
 }
 void schedule(){
  if(!renderVisible){stopFrames();return;}
  double time=current()+offset,next=200;bool active=manualTransition||(playing&&valid&&(transitionActive||lyricView()==LyricView::Prelude));
  if(playing&&valid&&!untimed&&index+1<int(lines.size())){next=std::min(next,std::max(1.,lines[index+1].time-time));if(browseIndex<0&&stagger&&focusIndex<=index)next=std::min(next,std::max(1.,lines[index+1].time-time-200));}
  if(playing&&valid&&!untimed)for(const auto&item:groups){const auto&g=item.second;if(!groupVisible(g,motionNow(),item.first,time))continue;const auto&line=g.content->model;
   if(indicatorVisible(g,item.first,time))active=true;
   if(g.active)for(const auto&word:line.words){if(time>=word.time&&time<word.time+word.duration+150)active=true;else if(word.time>time)next=std::min(next,word.time-time);}
   if(hasLongNoteGlow(g,item.first,time))active=true;
   if(canScroll(item.first)&&((g.content->glyphs.empty()&&rowWidth(g)*g.zoom.value(motionNow())>width-textX-12)||(g.translated&&g.content->subtitle.width*g.zoom.value(motionNow())>width-textX-12))){double begin=line.time+900,end=begin+std::max(1200.,(line.duration>0?line.duration:4000)-1600);if(time<begin)next=std::min(next,begin-time);else if(time<end)active=true;}
  }
  bool flying=!flight.rows.empty(),coverAnimating=coverSince>=0&&nowMs()-coverSince<250;active=active||coverAnimating||flying;
  bool backgroundActive=backgroundAnimating();bool run=flying||manualTransition||(playing&&valid&&((!untimed&&!lines.empty())||hover||coverAnimating||backgroundActive));double wanted=active?1000./(fps==60?60:30):std::max(1.,next);if(backgroundActive)wanted=std::min(wanted,1000./background.fps);
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
   else{int desired=std::max(0,index<0?preludePreview:rowAt(time+(playing&&valid&&stagger?200:0)));if(desired!=flight.focus||flight.playbackIndex!=index)beginFlight(desired,index);if(actualChanged)roles(false);applyFlight(animationWall());snapRow=false;returnFromBrowse=false;transitionActive=false;
    if(flight.running(animationWall())){updateIndicators();InvalidateRect(hwnd,nullptr,FALSE);schedule();return;}flight=LyricFlight();layoutDirty=true;
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
  const bool allowRelease=!snapRow&&(playing||browseIndex>=0);
  // Prelude and the first active row can share an index but use different anchors.
  bool reflowed=layoutDirty||desired!=focusIndex||groups.empty()||layoutAnchor!=rowAnchor();if(reflowed)reflow(desired,animate,allowRelease);
  if(actualChanged&&!reflowed)roles(allowRelease);
  if(snapRow||(!playing&&browseIndex<0&&actualChanged))for(auto&item:groups){auto&g=item.second;g.mainAlpha.snap(g.mainAlpha.to);g.subAlpha.snap(g.subAlpha.to);g.resetWords.clear();g.resetGlows.clear();}
  snapRow=false;returnFromBrowse=false;transitionActive=false;double now=motionNow();
  updateIndicators();
  if(lyricView()==LyricView::Rows)for(const auto&item:groups){
   const auto&g=item.second;
   bool hasContent=!emptyGroup(g)||indicatorVisible(g,item.first,time);
   if(!hasContent&&!g.middleInterlude)continue;
   bool visible=groupVisible(g,now,item.first,time);
   const float curY=g.y.value(now),curZ=g.zoom.value(now);
   bool crosses=std::min({g.y.from,g.y.to,curY})<=viewportBottom&&std::max({g.y.from,g.y.to,curY})+g.renderedHeight(std::max({g.zoom.from,g.zoom.to,curZ}))>=viewportTop;
   bool contentAnimating=hasContent&&(g.mainAlpha.active(now)||g.subAlpha.active(now)||(!g.active&&!g.resetWords.empty()&&now-g.resetAt<200));
   transitionActive=transitionActive||(g.y.active(now)&&crosses)||(visible&&(g.zoom.active(now)||contentAnimating));
  }
  bool layoutActive=false;for(const auto&item:groups){const auto&g=item.second;layoutActive=layoutActive||g.y.active(now)||g.zoom.active(now);}
  manualTransition=manualTransition&&layoutActive;manualLayoutPending=false;
  motion.setRunning(renderVisible&&((valid&&!untimed&&playing)||manualTransition),animationWall());
  InvalidateRect(hwnd,nullptr,FALSE);schedule();
 }
 void finishDraw(HRESULT hr){
  if(SUCCEEDED(hr)){targetLossRetries=0;return;}
  if(hr!=static_cast<HRESULT>(D2DERR_RECREATE_TARGET)){error="draw-failed";return;}
  graphics.clear();target.reset();
  if(renderVisible&&targetLossRetries<MaxTargetLossRetries){++targetLossRetries;InvalidateRect(hwnd,nullptr,FALSE);}
 }
 void paint(){
  PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);HRESULT hr=S_OK;bool drew=false;
  if(renderVisible)try{ensureTarget();renderCalls++;target->BeginDraw();draw(graphics);hr=target->EndDraw();drew=true;}
  catch(...){graphics.clear();target.reset();error="render-target-failed";}
  EndPaint(hwnd,&ps);
  if(drew)finishDraw(hr);
 }
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
  Json j=Json::obj();j["type"]=type;j["accepted"]=accepted;j["songId"]=songId;j["lyricCount"]=int(lines.size());j["lyricStatus"]=lyricMode;j["hasClock"]=hasClock;j["revision"]=revision;j["positionMs"]=current();j["playing"]=playing;j["clockValid"]=valid;j["seekPending"]=seekPending;j["seekRequest"]=double(seekRequest);j["error"]=error;j["connectionStale"]=connectionStale;j["ownerMonitored"]=ownerProcess.monitored();
  j["backgroundBackend"]=fluid.backend();j["backgroundGpuReady"]=fluid.gpuReady();if(!fluid.gpuError.empty())j["backgroundGpuError"]=fluid.gpuError;
  if(type=="background-state")j["background"]=backgroundState();return j;
 }
 Json diagnostics(const std::string&type,bool accepted=true){Json j=state(type,accepted);j["currentLineIndex"]=index;j["index"]=index;j["frameRunning"]=frameRunning;j["intervalMs"]=int(intervalMs);j["renderCalls"]=double(renderCalls);j["lineBuildCalls"]=double(lineBuildCalls);j["backend"]="Direct2D/DirectWrite";j["hardware"]=hardware;j["renderVisible"]=renderVisible;j["through"]=through;j["top"]=top;j["windowTopmost"]=(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;j["windowNoActivate"]=(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_NOACTIVATE)!=0;j["fps"]=fps;j["fontSize"]=currentFontSize;j["systemMoving"]=systemMoving;j["frameScheduler"]=frameTimer&&(!systemMoving||modalFrames.active())?(systemMoving?"waitable-posted":"waitable-deadline"):"window-timer";j["coverLoaded"]=!cover.pixels.empty();j["dpi"]=double(dpi);j["coverLeftPx"]=std::round(artBounds.left*scale());j["coverTopPx"]=std::round(artBounds.top*scale());j["coverSidePx"]=std::round(artSize*scale());j["coverRadiusPx"]=std::round(artRadius*scale());j["windowHeightPx"]=std::round(height*scale());j["iconBackend"]="vector";j["windowWidthPx"]=std::round(width*scale());j["barLeftPx"]=transport.left*scale();j["barRightPx"]=transport.right*scale();j["controlYPx"]=transport.y*scale();j["previousPx"]=transport.previous*scale();j["playPx"]=transport.play*scale();j["nextPx"]=transport.next*scale();j["rowStartedMs"]=rowSince;j["focusIndex"]=focusIndex;j["browsing"]=browseIndex>=0;j["browseIndex"]=browseIndex;j["browseRemainingMs"]=browseIndex>=0?std::max(0.,browseUntil-animationWall()):0;j["groupCacheLimit"]=32;j["transitionActive"]=transitionActive;j["transitionProgress"]=unit((motionNow()-rowSince-200)/500);j["transitionMode"]=transitionActive?"group-scroll":"none";j["animationTimeMs"]=motionNow();j["retainedRows"]=int(groups.size());j["layoutChanges"]=double(layoutChanges);j["lookaheadMs"]=stagger?200:0;j["stagger"]=stagger;j["curve"]=lineCurve;j["wordMode"]=wordMode;j["rowTextureBuilds"]=double(graphics.rowTextureBuilds);j["viewportTop"]=viewportTop;j["viewportBottom"]=viewportBottom;j["groupGap"]=GroupGap;j["layoutAnchor"]=layoutAnchor;j["groups"].kind=Json::Array;
  for(const auto&item:groups){const auto&g=item.second;Json v=Json::obj();v["index"]=item.first;v["identity"]=double(g.identity);v["text"]=narrow(g.content->model.text);v["translation"]=g.translated?narrow(g.content->model.translation):"";v["top"]=g.y.value(motionNow());v["target"]=g.y.to;v["startAt"]=g.y.start;v["durationMs"]=g.y.duration;v["height"]=g.renderedHeight(1.f);v["scale"]=g.zoom.value(motionNow());v["scaleTarget"]=g.zoom.to;v["scaleStartAt"]=g.zoom.start;v["scaleDurationMs"]=g.zoom.duration;v["renderedHeight"]=g.renderedHeight(g.zoom.value(motionNow()));v["fontSize"]=currentFontSize*g.zoom.value(motionNow());v["translationFontSize"]=g.translated?translationFontSize*g.zoom.value(motionNow()):0.f;v["rendered"]=groupVisible(g,motionNow());v["mainHeight"]=g.content->mainHeight;v["subOffset"]=g.subtitleOffset(g.zoom.value(motionNow()));v["mainAlpha"]=g.mainAlpha.value(motionNow());v["subAlpha"]=g.subAlpha.value(motionNow());v["active"]=g.active;j["groups"].array.push_back(std::move(v));}
  j["seekFlight"]=!flight.rows.empty();if(!flight.rows.empty()){j["transitionMode"]="seek-flight";j["transitionActive"]=true;j["transitionProgress"]=unit((animationWall()-flight.started)/(500+(flight.stagger?400:0)));}j["background"]=backgroundState();j["backgroundAnimating"]=backgroundAnimating();j["backgroundFrameBuilds"]=double(fluid.revision);j["backgroundBackend"]=fluid.backend();j["backgroundGpuReady"]=fluid.gpuReady();j["backgroundGpuError"]=fluid.gpuError;j["backgroundTimeMs"]=backgroundClock.time(animationWall());j["first"]=index>=0&&index<int(lines.size())?narrow(lines[index].text):"";j["second"]=index>=0&&index<int(lines.size())?(translation&&!lines[index].translation.empty()?narrow(lines[index].translation):index+1<int(lines.size())?narrow(lines[index+1].text):""):"";return j;}
 void report(const std::string&type,bool accepted=true,bool detailed=false){if(bridge)bridge->send(detailed?diagnostics(type,accepted):state(type,accepted));}
 void command(const std::string&action,double pos=0){if(!bridge)return;Json j=Json::obj();j["type"]="command";j["action"]=action;j["position"]=pos;if(action=="seek"){j["requestId"]=double(seekRequest);j["songId"]=songId;}bridge->send(j);}
 void cancelSeek(){seekPending=false;seekRequest=0;KillTimer(hwnd,3);}
 void requestSeek(double target){
  if(!bridge)return;
  if(!seekPending)seekFallback=current();seekStarted=nowMs();seekRequest=++seekSerial;seekPending=true;
  SetTimer(hwnd,3,3500,nullptr);
  command("seek",target);
 }
 void stopCoverWorker(){
  if(coverQueue){
   {std::lock_guard<std::mutex> lock(coverQueue->mutex);coverQueue->stop=true;}
   coverQueue->changed.notify_one();
  }
  if(coverWorker.joinable())coverWorker.join();
  coverQueue.reset();
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
     if(queue->stop)return;if(queue->epoch!=epoch||result->status==CoverStatus::Cancelled){superseded=true;break;}
     if(result->status!=CoverStatus::TransientError||attempt==2)break;
     // Retry only transient failures, waking immediately when a new cover or shutdown arrives.
     queue->changed.wait_for(lock,std::chrono::milliseconds(attempt==0?1000:3000),[&]{return queue->stop||queue->epoch!=epoch;});
     if(queue->stop)return;if(queue->epoch!=epoch){superseded=true;break;}
    }
    if(superseded)continue;
    std::lock_guard<std::mutex> lock(queue->mutex);
    if(queue->stop)return;
    if(queue->epoch==epoch&&PostMessageW(window,CoverReady,0,0))queue->ready=std::move(result);
   }
  });
 }
 void loadCover(const std::wstring&url){coverUrl=url;uint64_t epoch=++coverEpoch;if(!coverQueue)return;
  {std::lock_guard<std::mutex> lock(coverQueue->mutex);coverQueue->url=url;coverQueue->epoch=epoch;coverQueue->pending=true;coverQueue->ready.reset();}coverQueue->changed.notify_one();
 }
 void onCover(std::unique_ptr<CoverData> data){if(data->epoch!=coverEpoch||closing)return;if(data->status==CoverStatus::TransientError)coverUrl.clear();cover=std::move(*data);fluid.setCover(cover.pixels,cover.width,cover.height,backgroundClock.time(animationWall()),!backgroundPlaying());coverSince=cover.pixels.empty()?-1000:nowMs();if(!cover.pixels.empty()){uint64_t r=0,g=0,b=0;for(size_t i=0;i<cover.pixels.size();i+=4){b+=cover.pixels[i];g+=cover.pixels[i+1];r+=cover.pixels[i+2];}float count=float(cover.pixels.size()/4);bgA=D2D1::ColorF((r/count*.3f+15)/255,(g/count*.3f+15)/255,(b/count*.3f+22)/255);bgB=D2D1::ColorF(std::min(100/255.f,bgA.r+24/255.f),std::min(100/255.f,bgA.g+17/255.f),std::min(120/255.f,bgA.b+30/255.f));}if(graphics.target){try{graphics.updateCover(cover);graphics.updateGradient(bgA,bgB,width,height);}catch(...){graphics.clear();}}update();}
 void freezeConnection(){
  if(connectionStale)return;
  position=current();anchor=nowMs();playing=false;connectionStale=true;
  cancelSeek();flight=LyricFlight();error="disconnected";update();
 }
 void receive(const Json&m){
  received=nowMs();auto type=m["type"].s();
  if(type=="hello"){cancelSeek();connectionStale=false;if(error=="disconnected")error.clear();update();}
  else if(type=="ping"){if(connectionStale||error=="disconnected"){connectionStale=false;if(error=="disconnected")error.clear();update();}}
  else if(type=="track"){
   auto id=m["id"].s();
   if(id!=songId){
    KillTimer(hwnd,LayoutWarmupTimer);nextLayoutToWarm=0;
    flight=LyricFlight();stopBrowsing();songId=id;groups.clear();lyricLayouts.clear();graphics.clearRowTextures();
    focusIndex=-999;layoutDirty=true;snapRow=true;cancelSeek();lines.clear();lyricMode="waiting";untimed=false;hasClock=false;updatePlaceholders();index=-999;position=0;revision=0;valid=false;error.clear();
   }
   title=widen(m["title"].s());artist=widen(m["artist"].s());duration=std::max(0.,m["duration"].n());rebuildMeta();auto cover=widen(m["cover"].s());if(cover!=coverUrl)loadCover(cover);update();
  }
  else if(type=="lyrics"){
   if(m["songId"].s()!=songId){error="song-mismatch";report("lyrics-ack",false);return;}
   std::vector<Line> next;for(const auto&raw:m["lines"].array){if(next.size()>=5000)break;if(raw.kind!=Json::Object)continue;Line line;line.time=std::max(0.,raw["time"].n());line.duration=std::max(0.,raw["duration"].n());line.text=widen(raw["text"].s());line.isInterlude=raw["isInterlude"].b();line.translation=widen(raw["translation"].s());for(const auto&w:raw["words"].array){if(line.words.size()>=500)break;if(w.kind!=Json::Object)continue;line.words.push_back({widen(w["text"].s()),w["time"].n(),std::max(1.,w["duration"].n(1)),w["longNote"].b(),w["lineTail"].b(),w["emphasize"].b()});}if(line.text.empty())for(const auto&w:line.words)line.text+=w.text;next.push_back(std::move(line));}
   const auto mode=m["mode"].s();
   bool nextUntimed=!next.empty()&&mode=="untimed";
   std::string nextMode=nextUntimed?"untimed":"timed";
   if(next.empty()){
    nextMode="empty";
    if(mode=="loading"||mode=="instrumental"||mode=="unsupported"||mode=="error")nextMode=mode;
   }
   bool modeChanged=nextMode!=lyricMode;lyricMode=std::move(nextMode);
   if(!nextUntimed)std::stable_sort(next.begin(),next.end(),[](const Line&a,const Line&b){return a.time<b.time;});
   bool contentChanged=next!=lines||nextUntimed!=untimed;
   if(contentChanged){if(nextUntimed!=untimed){stopBrowsing();focusIndex=-999;snapRow=true;}untimed=nextUntimed;bool same=index>=0&&index<int(lines.size())&&index<int(next.size())&&lines[index].time==next[index].time&&lines[index].text==next[index].text;
    flight=LyricFlight();lines=std::move(next);layoutDirty=true;if(!same)snapRow=true;
    lyricLayouts.resize(lines.size());
    for(size_t i=0;i<lyricLayouts.size();++i)if(lyricLayouts[i]&&!(lyricLayouts[i]->model==lines[i]))lyricLayouts[i].reset();
    nextLayoutToWarm=0;
   }
   if(contentChanged||modeChanged)updatePlaceholders();
   revision=m["revision"].n();error.clear();update();if(contentChanged)scheduleLayoutWarmup();report("lyrics-ack");
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
  else if(type=="style"){
   int curve=lineCurve;
   if(m["curve"].kind==Json::Number){const double v=m["curve"].n();if(std::isfinite(v)&&std::floor(v)==v&&v>=0&&v<=LineCurveSpring)curve=int(v);}
   const bool wave=m["stagger"].kind==Json::Bool?m["stagger"].b():stagger;
   int highlight=wordMode;
   if(m["wordMode"].kind==Json::Number){const double v=m["wordMode"].n();if(std::isfinite(v)&&std::floor(v)==v&&(v==0||v==1))highlight=int(v);}
   const bool curveChanged=curve!=lineCurve,staggerChanged=wave!=stagger,wordModeChanged=highlight!=wordMode;
   if(curveChanged)lineCurve=curve;
   if(staggerChanged)stagger=wave;
   if(wordModeChanged)wordMode=highlight;
   if(staggerChanged){if(!manualTransition)update();}
   else if(wordModeChanged&&renderVisible&&hwnd)InvalidateRect(hwnd,nullptr,FALSE);
  }
  else if(type=="font"){if(m["size"].kind==Json::Number)setLyricFontSize(m["size"].n());report("host-state");}
  else if(type=="background"){if(readBackground(m)){save();update();}report("background-state");}
  else if(type=="show"){ShowWindow(hwnd,SW_SHOWNOACTIVATE);constrain();}
  else if(type=="unlock"){setThrough(false);update();}
  else if(type=="reset"){setThrough(false);SetWindowPos(hwnd,nullptr,int(80*scale()),int(80*scale()),int(360*scale()),int(130*scale()),SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(hwnd,SW_SHOWNOACTIVATE);save();update();}
  else if(type=="exit")DestroyWindow(hwnd);
  else if(type=="capture")capture();
  else if(type=="test-wheel"&&!capturePath.empty()){POINT p{int(std::round(m["x"].n()*scale())),int(std::round(m["y"].n()*scale()))};ClientToScreen(hwnd,&p);message(WM_MOUSEWHEEL,MAKEWPARAM(0,SHORT(clamp(m["delta"].n(),-12000,12000))),MAKELPARAM(p.x,p.y));report("test-wheel-ack",true,true);}
  else if(type=="test-click"&&!capturePath.empty()){int x=int(std::round(m["x"].n()*scale())),y=int(std::round(m["y"].n()*scale()));message(WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));report("test-click-ack",true,true);}
  else if(type=="inspect"&&!capturePath.empty())report("inspection",true,true);
  else if(type=="test-settings"&&!capturePath.empty()){
   if(m["motionWall"].kind==Json::Number){testWall=true;testWallMs=m["motionWall"].n();}
   if(m["freezeClock"].kind==Json::Bool)freezePlayback=m["freezeClock"].b();
   if(m["width"].kind==Json::Number&&m["height"].kind==Json::Number)SetWindowPos(hwnd,nullptr,0,0,int(clamp(m["width"].n(),360,900)*scale()),int(clamp(m["height"].n(),MinWindowHeight,240)*scale()),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
   if(m["hover"].kind==Json::Bool)hover=m["hover"].b();
   if(m["translation"].kind==Json::Bool&&translation!=m["translation"].b()){translation=m["translation"].b();layoutDirty=true;}
   if(m["through"].kind==Json::Bool)setThrough(m["through"].b());
   if(m["fps"].kind==Json::Number)fps=m["fps"].n()==60?60:30;
   if(m["top"].kind==Json::Bool)setTop(m["top"].b());
   update();report("test-settings-ack",true,true);
  }
 }
 void setTop(bool value){if(setWindowTopmost(hwnd,value,through))top=value;else error="topmost-change-failed";}
 bool syncHover(){
  POINT cursor{};RECT client{};const bool previous=hover;
  hover=!through&&GetCursorPos(&cursor)&&ScreenToClient(hwnd,&cursor)&&GetClientRect(hwnd,&client)&&PtInRect(&client,cursor);
  TRACKMOUSEEVENT tracking{sizeof(tracking),hover?TME_LEAVE:TME_CANCEL|TME_LEAVE,hwnd,0};
  TrackMouseEvent(&tracking);
  return previous!=hover;
 }
 void setThrough(bool value){through=value;LONG_PTR style=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);if(value)style|=WS_EX_TRANSPARENT|WS_EX_LAYERED;else style&=~(WS_EX_TRANSPARENT|WS_EX_LAYERED);SetWindowLongPtrW(hwnd,GWL_EXSTYLE,style);if(!applyWindowActivationMode(hwnd,top,through))error="activation-mode-failed";if(value)SetLayeredWindowAttributes(hwnd,0,255,LWA_ALPHA);syncHover();}
 void constrain(){RECT rect{};GetWindowRect(hwnd,&rect);if(!MonitorFromRect(&rect,MONITOR_DEFAULTTONULL))SetWindowPos(hwnd,nullptr,int(80*scale()),int(80*scale()),0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);}
 void save(){
  if(!hwnd)return;
  recordNormalBounds();
  RECT rect{};float rectScale=scale();
  if(!IsIconic(hwnd)){if(!GetWindowRect(hwnd,&rect))return;}
  else if(hasNormalRect){rect=normalWindowRect;rectScale=normalWindowScale;}
  else{float s=positionInPixels?1.f:rectScale;rect={int(std::lround(savedX*s)),int(std::lround(savedY*s)),0,0};rect.right=rect.left+int(std::lround(savedWidth*rectScale));rect.bottom=rect.top+int(std::lround(savedHeight*rectScale));}
  Json j=Json::obj();j["X"]=int(rect.left);j["Y"]=int(rect.top);j["PositionInPixels"]=true;j["Width"]=(rect.right-rect.left)/rectScale;j["Height"]=(rect.bottom-rect.top)/rectScale;j["Top"]=top;j["Translation"]=translation;j["Fps"]=fps;j["FontSize"]=currentFontSize;j["Background"]=backgroundState();
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
 void finishMenu(int choice){
  if(closing)return;
  if(choice==6){DestroyWindow(hwnd);return;}
  bool persist=false;
  switch(choice){
   case 1:translation=!translation;layoutDirty=true;persist=true;break;
   case 2:{const bool previous=top;setTop(!top);persist=top!=previous;break;}
   case 3:setThrough(!through);break;
   case 4:case 5:{const int nextFps=choice==4?30:60;persist=fps!=nextFps;fps=nextFps;break;}
  }
  if(choice!=3)syncHover();
  update();
  if(persist)save();
 }
 void menu(POINT point){
  HMENU menu=CreatePopupMenu();
  AppendMenuW(menu,MF_STRING|(translation?MF_CHECKED:0),1,L"显示翻译");
  AppendMenuW(menu,MF_STRING|(top?MF_CHECKED:0),2,L"置顶");
  AppendMenuW(menu,MF_STRING|(through?MF_CHECKED:0),3,L"鼠标穿透（在插件设置中恢复）");
  AppendMenuW(menu,MF_STRING|(fps==30?MF_CHECKED:0),4,L"30 FPS");
  AppendMenuW(menu,MF_STRING|(fps==60?MF_CHECKED:0),5,L"60 FPS");
  AppendMenuW(menu,MF_STRING,6,L"关闭小窗");
  SetForegroundWindow(hwnd);
  int choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,hwnd,nullptr);
  DestroyMenu(menu);finishMenu(choice);
 }
 LRESULT message(UINT msg,WPARAM wp,LPARAM lp){
  switch(msg){
   case WM_CREATE:{auto getWindowDpi=reinterpret_cast<UINT(WINAPI*)(HWND)>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow")));if(getWindowDpi){UINT value=getWindowDpi(hwnd);if(value)dpi=float(value);}return 0;}
   case WM_PAINT:paint();return 0;case WM_ERASEBKGND:return 1;case WM_MOUSEACTIVATE:return windowMouseActivate(top,through);
   case Incoming:{std::unique_ptr<Json> value(reinterpret_cast<Json*>(lp));if(!closing)receive(*value);return 0;}
   case BridgeDisconnected:if(!closing&&bridge&&!bridge->connected())freezeConnection();return 0;
   case CoverReady:{
    if(coverQueue){std::unique_ptr<CoverData> value;{std::lock_guard<std::mutex> lock(coverQueue->mutex);value=std::move(coverQueue->ready);}
     if(value)onCover(std::move(value));}
    return 0;
   }
   case FrameReady:if(systemMoving&&modalFrames.accepts(wp)&&frameArmed&&nowMs()+1>=frameDeadline){frameArmed=false;update();if(renderVisible)UpdateWindow(hwnd);}return 0;
   case WM_GETMINMAXINFO:{auto*limits=reinterpret_cast<MINMAXINFO*>(lp);limits->ptMinTrackSize.x=LONG(std::ceil(360.f*scale()));limits->ptMinTrackSize.y=LONG(std::ceil(MinWindowHeight*scale()));return 0;}
   case WM_WINDOWPOSCHANGED:{auto result=DefWindowProcW(hwnd,msg,wp,lp);setRenderVisible(IsWindowVisible(hwnd)&&!IsIconic(hwnd));return result;}
   case WM_SIZE:if(wp==SIZE_MINIMIZED){setRenderVisible(false);return 0;}if(wp!=SIZE_MAXHIDE&&wp!=SIZE_MAXSHOW&&factory&&textFactory&&hwnd&&geometry()){update();if(resizing&&renderVisible)UpdateWindow(hwnd);}return 0;
   case WM_DPICHANGED:{dpi=float(HIWORD(wp));if(target)target->SetDpi(dpi,dpi);graphics.clearRowTextures();auto*rect=reinterpret_cast<RECT*>(lp);SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOCOPYBITS);recordNormalBounds();if(resizing){GetCursorPos(&resizeStart);RECT current{};GetWindowRect(hwnd,&current);resizeSize={current.right-current.left,current.bottom-current.top};}if(geometry())update();if(renderVisible)UpdateWindow(hwnd);return 0;}
   case WM_TIMER:if(wp==1||wp==4){update();if(systemMoving&&renderVisible)UpdateWindow(hwnd);}else if(wp==3&&seekPending){cancelSeek();position=seekFallback+(playing&&valid?nowMs()-seekStarted:0);anchor=nowMs();error="seek-timeout";update();}else if(wp==2&&!demo){if(ownerProcess.exited())DestroyWindow(hwnd);else if(!bridge||!bridge->connected()||nowMs()-received>45000)freezeConnection();}else if(wp==LayoutWarmupTimer)warmLyricLayouts();return 0;
   case WM_MOUSEWHEEL:{
    if(through||dragProgress||resizing||lines.empty())return 0;POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(hwnd,&p);float x=p.x/scale(),y=p.y/scale();
    if(x<textX||x>width-12||y<viewportTop||y>viewportBottom)return 0;
    wheelRemainder+=GET_WHEEL_DELTA_WPARAM(wp);int steps=wheelRemainder/WHEEL_DELTA;wheelRemainder%=WHEEL_DELTA;if(!steps)return 0;
    int origin=browseIndex>=0?browseIndex:(untimed&&focusIndex>=0?focusIndex:index);
    int next=browseStep(origin,steps);if(next<0)return 0;flight=LyricFlight();browseIndex=next;manualTransition=manualLayoutPending=true;browseUntil=animationWall()+4000;if(!untimed)SetTimer(hwnd,4,4000,nullptr);layoutDirty=true;update();return 0;
   }
   case WM_SETCURSOR:{
    if(LOWORD(lp)==HTCLIENT&&!through){
     POINT pt{};GetCursorPos(&pt);ScreenToClient(hwnd,&pt);float x=pt.x/scale(),y=pt.y/scale();LPCWSTR id=IDC_ARROW;
     if(x>width-17&&y>height-17)id=IDC_SIZENWSE;
     else if((hover&&y<32&&x>width-78)||(hover&&y>=transport.y-14&&transport.hit(x,y)!=ControlHit::None))id=IDC_HAND;
     else if(!untimed&&y>=viewportTop&&y<=viewportBottom&&x>=textX&&x<=width-12){
      double now=motionNow();
      for(const auto&item:groups){if(!groupVisible(item.second,now))continue;float top=item.second.y.value(now);if(y>=top-2&&y<=top+item.second.renderedHeight(item.second.zoom.value(now))){id=IDC_HAND;break;}}
     }
     SetCursor(LoadCursorW(nullptr,id));return TRUE;
    }
    break;
   }
   case WM_MOUSEMOVE:{float x=GET_X_LPARAM(lp)/scale();if(resizing){POINT point;GetCursorPos(&point);int w=int(clamp(resizeSize.cx+point.x-resizeStart.x,360*scale(),900*scale())),h=int(clamp(resizeSize.cy+point.y-resizeStart.y,MinWindowHeight*scale(),240*scale()));if(w!=geometryPixels.cx||h!=geometryPixels.cy)SetWindowPos(hwnd,nullptr,0,0,w,h,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);}else if(dragProgress){dragRatio=transport.ratio(x);update();}else if(!hover&&!through&&syncHover())update();return 0;}
   case WM_MOUSELEAVE:if(!dragProgress&&!resizing&&!systemMoving&&hover){hover=false;update();}return 0;
   case WM_LBUTTONDOWN:{if(through)return 0;float x=GET_X_LPARAM(lp)/scale(),y=GET_Y_LPARAM(lp)/scale();if(x>width-17&&y>height-17){resizing=true;GetCursorPos(&resizeStart);RECT r;GetWindowRect(hwnd,&r);resizeSize={r.right-r.left,r.bottom-r.top};SetCapture(hwnd);update();return 0;}if(hover&&y<32&&x>width-78){int i=int((x-(width-78))/24);if(i==0){setTop(!top);save();update();}else if(i==1){setThrough(true);update();}else DestroyWindow(hwnd);return 0;}
    const auto hit=hover?transport.hit(x,y):ControlHit::None;
    if(hit!=ControlHit::None){
     switch(hit){
      case ControlHit::Seek:if(duration>0){dragProgress=true;dragRatio=transport.ratio(x);SetCapture(hwnd);update();}break;
      case ControlHit::Previous:command("previous");break;case ControlHit::Play:command("play");break;case ControlHit::Next:command("next");break;default:break;
     }return 0;
    }
    if(x<textX||y<32){ReleaseCapture();SendMessageW(hwnd,WM_NCLBUTTONDOWN,HTCAPTION,0);return 0;}
    if(!untimed&&y>=viewportTop&&y<=viewportBottom&&x>=textX&&x<=width-12){for(const auto&item:groups){if(!groupVisible(item.second,motionNow()))continue;float top=item.second.y.value(motionNow());if(y>=top-2&&y<=top+item.second.renderedHeight(item.second.zoom.value(motionNow()))){requestSeek(std::max(0.,item.second.content->model.time-offset));update();break;}}}return 0;}
   case WM_LBUTTONUP:if(dragProgress){requestSeek(duration*dragRatio);dragProgress=false;ReleaseCapture();update();}if(resizing)ReleaseCapture();return 0;
   case WM_LBUTTONDBLCLK:if(GET_X_LPARAM(lp)/scale()<textX)command("play");return 0;
   case WM_CAPTURECHANGED:{bool sized=resizing;dragProgress=false;resizing=false;syncHover();update();if(sized)save();return 0;}
   case WM_CONTEXTMENU:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};if(p.x==-1)GetCursorPos(&p);menu(p);return 0;}
   case WM_ENTERSIZEMOVE:stopFrames();if(frameTimer)WaitForSingleObject(frameTimer,0);systemMoving=true;modalFrames.start(hwnd,frameTimer,FrameReady);update();return 0;
   case WM_EXITSIZEMOVE:systemMoving=false;stopFrames();modalFrames.stop();syncHover();update();save();return 0;
   case WM_CLOSE:DestroyWindow(hwnd);return 0;
   case WM_DESTROY:try{if(bridge)bridge->finish();}catch(...){}closing=true;systemMoving=false;stopFrames();modalFrames.stop();save();KillTimer(hwnd,1);KillTimer(hwnd,2);KillTimer(hwnd,3);KillTimer(hwnd,4);KillTimer(hwnd,LayoutWarmupTimer);stopCoverWorker();PostQuitMessage(0);return 0;
  }return DefWindowProcW(hwnd,msg,wp,lp);
 }
 static LRESULT CALLBACK procedure(HWND window,UINT msg,WPARAM wp,LPARAM lp){App*app=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(msg==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);app->hwnd=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}if(app){try{return app->message(msg,wp,lp);}catch(...){app->error="window-message-failed";}}return DefWindowProcW(window,msg,wp,lp);}
 public:
 ~App(){stopCoverWorker();}
 App(){settingsPath=env(L"MINILYRICS_SETTINGS");if(settingsPath.empty())settingsPath=env(L"LOCALAPPDATA")+L"\\MiniLyrics\\settings.json";
  try{std::ifstream f(std::filesystem::path(settingsPath),std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(f)),{});if(bytes.size()>65536)throw std::runtime_error("settings size");auto settings=JsonParser(bytes).parse();savedX=settings["X"].n(80);savedY=settings["Y"].n(80);positionInPixels=settings["PositionInPixels"].b();savedWidth=clamp(settings["Width"].n(360),360,900);savedHeight=clamp(settings["Height"].n(130),MinWindowHeight,240);top=settings["Top"].b(true);translation=settings["Translation"].b(true);fps=settings["Fps"].n(60)==60?60:30;readBackground(settings["Background"]);currentFontSize=float(std::round(clamp(settings["FontSize"].n(::CurrentFontSize),14,36)));contextFontSize=::ContextFontSize*currentFontSize/::CurrentFontSize;translationFontSize=::TranslationFontSize*currentFontSize/::CurrentFontSize;}catch(...){}
 }
 int run(HINSTANCE instance,int port,const std::string&token,const std::wstring&captureFile,bool demoMode){
  capturePath=captureFile;demo=demoMode;if(!demo)ownerProcess.capture();check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.put()));check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(textFactory.put())));check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(imaging.put())));
  makeFormat(mainFormat,currentFontSize,DWRITE_FONT_WEIGHT_SEMI_BOLD);makeFormat(secondaryFormat,translationFontSize,DWRITE_FONT_WEIGHT_NORMAL);makeFormat(metaFormat,11.5,DWRITE_FONT_WEIGHT_NORMAL);mainRowHeight=layout(L"Ag",mainFormat.get()).height;updatePlaceholders();
  check(factory->CreatePathGeometry(playGeometry.put()));Com<ID2D1GeometrySink> sink;check(playGeometry->Open(sink.put()));sink->BeginFigure(D2D1::Point2F(-5,-7),D2D1_FIGURE_BEGIN_FILLED);sink->AddLine(D2D1::Point2F(-5,7));sink->AddLine(D2D1::Point2F(6,0));sink->EndFigure(D2D1_FIGURE_END_CLOSED);check(sink->Close());
  frameTimer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_MODIFY_STATE|SYNCHRONIZE);if(!frameTimer)frameTimer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
  WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.style=CS_DBLCLKS;cls.lpfnWndProc=procedure;cls.hInstance=instance;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.lpszClassName=L"MiniLyrics.Direct2D";RegisterClassExW(&cls);
  auto getSystemDpi=reinterpret_cast<UINT(WINAPI*)()>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForSystem")));if(getSystemDpi)dpi=float(getSystemDpi());
  float positionScale=positionInPixels?1.f:scale();
  hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|(top?WS_EX_TOPMOST|WS_EX_NOACTIVATE:0),cls.lpszClassName,L"MiniLyrics",WS_POPUP,int(std::round(savedX*positionScale)),int(std::round(savedY*positionScale)),int(std::round(savedWidth*scale())),int(std::round(savedHeight*scale())),nullptr,nullptr,instance,this);if(!hwnd)throw std::runtime_error("window creation failed");
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
  modalFrames.stop();if(frameTimer){CancelWaitableTimer(frameTimer);CloseHandle(frameTimer);frameTimer=nullptr;}bridge.reset();graphics.clear();target.reset();return 0;
 }
};

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int){
 auto awareness=reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext")));if(awareness)awareness(reinterpret_cast<HANDLE>(-4));else SetProcessDPIAware();
 HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa)!=0)return 2;
 int result=1;try{int argc=0;LPWSTR*argv=CommandLineToArgvW(GetCommandLineW(),&argc);int port=0;std::string token;std::wstring capture;bool demo=false;for(int i=1;i<argc;i++){std::wstring arg=argv[i];if(arg==L"--demo")demo=true;else if(i+1<argc&&arg==L"--port")port=_wtoi(argv[++i]);else if(i+1<argc&&arg==L"--token")token=narrow(argv[++i]);else if(i+1<argc&&arg==L"--capture")capture=argv[++i];}LocalFree(argv);if(!demo&&(port<1024||port>65535||token.size()<32))result=2;else{App app;result=app.run(instance,port,token,capture,demo);}}
 catch(const std::exception&error){try{std::ofstream f(std::filesystem::path(env(L"TEMP")+L"\\MiniLyrics-error.log"),std::ios::binary);f<<error.what();}catch(...){}result=1;}
 WSACleanup();if(SUCCEEDED(com))CoUninitialize();return result;
}
