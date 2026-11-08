// MiniLyrics native Win32 / Direct2D / DirectWrite host.
// Copyright (C) 2026 Yushun Wang. GPL-3.0-or-later.
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
static std::wstring widen(const std::string&s){if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);if(n<=0)return {};std::wstring out(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),&out[0],n);return out;}
static std::string narrow(const std::wstring&s){if(s.empty())return {};int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);std::string out(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),&out[0],n,nullptr,nullptr);return out;}
static std::wstring env(const wchar_t*name){DWORD n=GetEnvironmentVariableW(name,nullptr,0);if(!n)return {};std::wstring out(n,0);GetEnvironmentVariableW(name,&out[0],n);out.resize(n-1);return out;}
static void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Windows graphics initialization failed ("+std::to_string(uint32_t(hr))+")");}
struct Word {std::wstring text;double time=0,duration=1;bool operator==(const Word&o)const{return text==o.text&&time==o.time&&duration==o.duration;}};
struct Line {std::wstring text,translation;double time=0,duration=0;std::vector<Word> words;bool isInterlude=false;bool operator==(const Line&o)const{return text==o.text&&translation==o.translation&&time==o.time&&duration==o.duration&&words==o.words&&isInterlude==o.isInterlude;}};
struct Layout {Com<IDWriteTextLayout> value;float width=0,height=0;};
struct Glyph {Layout layout;double time=0,duration=1;float x=0;};
struct LyricGroup {
 Line model;Layout main,subtitle;std::vector<Glyph> glyphs;std::vector<FloatWord> resetWords;InterludeAnimation dots;
 LineTrack y,zoom,mainAlpha,subAlpha;float mainHeight=0,opacity=1;double resetAt=-1000;uint64_t identity=0;bool translated=false,placed=false,active=false,browsePlaceholder=false;
 float subtitleOffset(float scale)const{return (mainHeight+TranslationGap)*scale;}
 float renderedHeight(float scale)const{return (mainHeight+(translated?TranslationGap+subtitle.height:0))*scale;}
};
// Whole-row visibility is independent of plain-text emphasis and word progress.
struct FlightPoint {float y=0,zoom=1,mainAlpha=.4f,subAlpha=.4f,opacity=1;};
struct FlightRow {FlightPoint from,to,point;float height=0;int bucket=0;};
struct LyricFlight {
 std::vector<FlightRow> rows;double started=0;int focus=0,playbackIndex=-1,curve=0;bool stagger=true,prelude=false;
 bool running(double now)const{return !rows.empty()&&now<started+500+(stagger?400:0);}
 struct Phase {double p[9]{},alpha=0;};
 Phase phase(double now)const{Phase out;for(int i=0;i<9;i++)out.p[i]=lineEase(unit((now-started-(stagger?i*50:0))/500),curve);out.alpha=lyricEase(unit((now-started)/500));return out;}
 FlightPoint sample(const FlightRow&r,const Phase&p)const{return {mixf(r.from.y,r.to.y,p.p[r.bucket]),mixf(r.from.zoom,r.to.zoom,p.p[r.bucket]),mixf(r.from.mainAlpha,r.to.mainAlpha,p.alpha),mixf(r.from.subAlpha,r.to.subAlpha,p.alpha),mixf(r.from.opacity,r.to.opacity,p.alpha)};}
};
struct CoverData {uint64_t epoch=0;UINT width=0,height=0;std::vector<BYTE> pixels;};
struct CoverQueue {std::mutex mutex;std::condition_variable changed;bool stop=false,pending=false;std::wstring url;uint64_t epoch=0;};

static std::unique_ptr<CoverData> downloadCover(const std::wstring&url,uint64_t epoch){
 auto out=std::make_unique<CoverData>();out->epoch=epoch;
 URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=DWORD(-1);
 if(!WinHttpCrackUrl(url.c_str(),0,0,&parts)||(parts.nScheme!=INTERNET_SCHEME_HTTP&&parts.nScheme!=INTERNET_SCHEME_HTTPS))return out;
 std::wstring host(parts.lpszHostName,parts.dwHostNameLength),path(parts.lpszUrlPath,parts.dwUrlPathLength);
 if(parts.dwExtraInfoLength)path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);if(path.empty())path=L"/";
 HINTERNET session=WinHttpOpen(L"MiniLyrics/0.2",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);if(!session)return out;
 WinHttpSetTimeouts(session,3000,3000,8000,8000);
 HINTERNET connection=WinHttpConnect(session,host.c_str(),parts.nPort,0),request=nullptr;std::vector<BYTE> bytes;
 if(connection)request=WinHttpOpenRequest(connection,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);
 if(request&&WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(request,nullptr)){
  DWORD status=0,size=sizeof(status);WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr);
  if(status==200){BYTE buf[8192];DWORD got=0;while(WinHttpReadData(request,buf,sizeof(buf),&got)&&got){if(bytes.size()+got>8*1024*1024){bytes.clear();break;}bytes.insert(bytes.end(),buf,buf+got);}}
 }
 if(request)WinHttpCloseHandle(request);if(connection)WinHttpCloseHandle(connection);WinHttpCloseHandle(session);if(bytes.empty())return out;
 HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
 try{
  Com<IWICImagingFactory> factory;check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.put())));
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
 if(SUCCEEDED(init))CoUninitialize();return out;
}

struct Graphics {
 ID2D1RenderTarget*target=nullptr;Com<ID2D1SolidColorBrush> brush;Com<ID2D1LinearGradientBrush> gradient;Com<ID2D1Bitmap> cover,background;Com<ID2D1BitmapBrush> backgroundBrush;uint64_t backgroundRevision=0;
 void clear(){backgroundBrush.reset();background.reset();backgroundRevision=0;cover.reset();gradient.reset();brush.reset();target=nullptr;}
 void resize(float w,float h){if(gradient)gradient->SetEndPoint(D2D1::Point2F(w,h*.5f));}
 void updateGradient(D2D1_COLOR_F a,D2D1_COLOR_F b,float w,float h){
  D2D1_GRADIENT_STOP stops[]={{0,a},{1,b}};Com<ID2D1GradientStopCollection> collection;Com<ID2D1LinearGradientBrush> next;
  check(target->CreateGradientStopCollection(stops,2,collection.put()));check(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(w,h*.5f)),collection.get(),next.put()));gradient=std::move(next);
 }
 void updateCover(const CoverData&image){
  if(image.pixels.empty()){cover.reset();return;}
  if(cover&&cover->GetPixelSize().width==image.width&&cover->GetPixelSize().height==image.height){check(cover->CopyFromMemory(nullptr,image.pixels.data(),image.width*4));return;}
  Com<ID2D1Bitmap> next;check(target->CreateBitmap(D2D1::SizeU(image.width,image.height),image.pixels.data(),image.width*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),next.put()));cover=std::move(next);
 }
 void init(ID2D1RenderTarget*t,D2D1_COLOR_F a,D2D1_COLOR_F b,const CoverData&image,float w,float h){
  clear();target=t;check(t->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),brush.put()));
  updateGradient(a,b,w,h);updateCover(image);
 }
};

class App {
 friend struct NativeTest;
 static constexpr UINT Incoming=WM_APP+1,CoverReady=WM_APP+2,FrameReady=WM_APP+3;
 HWND hwnd=nullptr;HANDLE frameTimer=nullptr;bool frameArmed=false;double frameDeadline=0,framePeriod=0;std::unique_ptr<Bridge> bridge;
 ModalFrames modalFrames;bool systemMoving=false;
 std::shared_ptr<CoverQueue> coverQueue;
 Com<ID2D1Factory> factory;Com<IDWriteFactory> textFactory;Com<IWICImagingFactory> imaging;
 Com<ID2D1HwndRenderTarget> target;Graphics graphics;Com<ID2D1RoundedRectangleGeometry> coverClip;
 Com<IDWriteTextFormat> mainFormat,secondaryFormat,metaFormat;
 Layout metaLayout,placeholder,syncing;int preludePreview=-1;std::map<int,LyricGroup>groups;InterludeAnimation preludeDots;LyricFlight flight;float secondaryRowHeight=0;
 LyricClock motion;bool transitionActive=false,snapRow=false,testWall=false,freezePlayback=false,layoutDirty=true,stagger=true;double testWallMs=0;int focusIndex=-999,lineCurve=0,browseIndex=-1,wheelRemainder=0;double browseUntil=0;bool returnFromBrowse=false;float viewportTop=32,viewportBottom=98,layoutAnchor=44,mainRowHeight=28;uint64_t groupSerial=0,layoutChanges=0;
 Com<ID2D1PathGeometry> playGeometry;
 std::vector<Line> lines;CoverData cover;BackgroundSettings background;BackgroundClock backgroundClock;FluidBackground fluid;
 D2D1_COLOR_F bgA=D2D1::ColorF(33/255.f,42/255.f,66/255.f),bgB=D2D1::ColorF(76/255.f,67/255.f,99/255.f);
 D2D1_RECT_F artBounds{};float artRadius=10;TransportGeometry transport{};
 bool seekPending=false;uint64_t seekSerial=0,seekRequest=0;double seekStarted=0,seekFallback=0;
 std::wstring settingsPath,capturePath,title=L"MiniLyrics",artist,coverUrl;std::string songId,error;
 double position=0,anchor=0,duration=0,offset=0,received=0,revision=0,rowSince=-1000,coverSince=-1000;
 double savedX=80,savedY=80,savedWidth=460,savedHeight=130;float dpi=96,width=460,height=130,artSize=96,textX=124;
 bool positionInPixels=false;
 SIZE geometryPixels{};float geometryDpi=0;
 bool playing=false,valid=false,through=false,top=true,translation=true,hover=false,dragProgress=false,resizing=false,closing=false,hardware=false,frameRunning=false,hotkeyRegistered=false;
 bool demo=false,renderVisible=false;int fps=30,index=-999;UINT intervalMs=200;uint64_t renderCalls=0,lineBuildCalls=0,coverEpoch=0;double dragRatio=0;
 POINT resizeStart{};SIZE resizeSize{};
 double current()const{return position+(playing&&valid&&!freezePlayback?std::max(0.,nowMs()-anchor):0.);}
 double animationWall()const{return testWall?testWallMs:nowMs();}
 double motionNow()const{return motion.now(animationWall());}
 void stopBrowsing(){browseIndex=-1;wheelRemainder=0;browseUntil=0;returnFromBrowse=true;KillTimer(hwnd,4);layoutDirty=true;}
 void finishTransition(){for(auto&item:groups){auto&g=item.second;g.y.snap(g.y.to);g.zoom.snap(g.zoom.to);g.mainAlpha.snap(g.mainAlpha.to);g.subAlpha.snap(g.subAlpha.to);}transitionActive=false;}
 float scale()const{return dpi/96.f;}
 float lyricAnchor()const{return height/2+4-(mainRowHeight*(1+ContextFontSize/CurrentFontSize)+11)/2;}
 enum class LyricView {Waiting,Syncing,Prelude,Rows};
 LyricView lyricView()const{if(lines.empty())return LyricView::Waiting;if(browseIndex>=0)return LyricView::Rows;if(!valid)return LyricView::Syncing;return index<0?LyricView::Prelude:LyricView::Rows;}
 float rowAnchor()const{return lyricAnchor()+(lyricView()==LyricView::Prelude?mainRowHeight+GroupGap:0);}
 bool emptyGroup(const LyricGroup&g)const{return g.model.text.empty()&&g.glyphs.empty()&&!g.translated;}
 bool isInterlude(const Line&line)const{return line.isInterlude||(line.text.empty()&&line.words.empty()&&(!translation||line.translation.empty()));}
 bool isInterlude(const LyricGroup&g)const{return isInterlude(g.model);}
 std::pair<int,int> browseBounds()const{int first=0,last=int(lines.size())-1;while(first<=last&&isInterlude(lines[first]))++first;while(last>=first&&isInterlude(lines[last]))--last;return first<=last?std::make_pair(first,last):std::make_pair(-1,-1);}
 int browseNeighbor(int at,int direction)const{auto bounds=browseBounds();if(bounds.first<0)return -1;int next=at+direction;next=direction>0?std::max(next,bounds.first):std::min(next,bounds.second);return next>=bounds.first&&next<=bounds.second?next:-1;}
 int browseFocus(int at)const{auto bounds=browseBounds();return bounds.first<0?-1:std::max(bounds.first,std::min(bounds.second,at));}
 int browseStep(int at,int steps)const{
  auto bounds=browseBounds();if(bounds.first<0)return -1;int direction=steps>0?-1:1,count=std::abs(steps);if(!count)return browseFocus(at);
  if(at<bounds.first||at>bounds.second){at=std::max(bounds.first,std::min(bounds.second,at));--count;}
  return std::max(bounds.first,std::min(bounds.second,at+direction*count));
 }
 bool staticInterlude(const LyricGroup&g)const{return browseIndex>=0&&g.browsePlaceholder;}
 bool showInterlude(const LyricGroup&g)const{return valid&&browseIndex<0&&g.active&&isInterlude(g);}
 bool indicatorVisible(const LyricGroup&g)const{return valid&&browseIndex<0&&isInterlude(g)&&g.dots.visible(motionNow());}
 bool groupVisible(const LyricGroup&g,double now)const{if(g.opacity<=.001f)return false;if(browseIndex<0){if(!valid)return false;if(index<0&&flight.rows.empty()){auto preview=groups.find(preludePreview);if(preview==groups.end()||&preview->second!=&g)return false;}}if(browseIndex>=0&&isInterlude(g)&&!staticInterlude(g))return false;if(emptyGroup(g)&&!indicatorVisible(g)&&!staticInterlude(g))return false;float y=g.y.value(now);return y+g.renderedHeight(g.zoom.value(now))>=viewportTop&&y<=viewportBottom;}
 void updateIndicators(){double now=motionNow(),time=current()+offset;bool prelude=lyricView()==LyricView::Prelude;preludeDots.update(prelude,playing&&valid,now,time,0,lines.empty()?0:lines.front().time);if(prelude&&!playing&&valid)preludeDots.opacity.snap(1);for(auto&item:groups){auto&g=item.second;bool selected=showInterlude(g);g.dots.update(selected,playing&&valid,now,time,g.model.time,interludeDuration(g.model,item.first));if(selected&&!playing)g.dots.opacity.snap(1);}}
 double interludeDuration(const Line&line,int id)const{if(line.duration>0)return line.duration;if(id>=0&&id+1<int(lines.size()))return std::max(0.,lines[id+1].time-line.time);return std::max(0.,duration-line.time);}
 int rowAt(double time)const {int lo=0,hi=int(lines.size())-1,result=-1;while(lo<=hi){int mid=(lo+hi)/2;if(lines[mid].time<=time){result=mid;lo=mid+1;}else hi=mid-1;}return result;}
 Layout layout(const std::wstring&text,IDWriteTextFormat*format,float maxWidth=50000){Layout l;if(text.empty())return l;check(textFactory->CreateTextLayout(text.c_str(),UINT32(text.size()),format,maxWidth,64,l.value.put()));DWRITE_TEXT_METRICS metrics{};check(l.value->GetMetrics(&metrics));l.width=metrics.widthIncludingTrailingWhitespace;l.height=metrics.height;return l;}
 void makeFormat(Com<IDWriteTextFormat>&format,float size,DWRITE_FONT_WEIGHT weight){check(textFactory->CreateTextFormat(L"Segoe UI",nullptr,weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"zh-cn",format.put()));format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);}
 void rebuildMeta(){metaLayout=layout(title+(artist.empty()?L"":L"  ·  "+artist),metaFormat.get(),std::max(20.f,width-textX-85));if(metaLayout.value){DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};Com<IDWriteInlineObject>sign;if(SUCCEEDED(textFactory->CreateEllipsisTrimmingSign(metaFormat.get(),sign.put())))metaLayout.value->SetTrimming(&trim,sign.get());}}
 LyricGroup& group(int at){
  auto result=groups.try_emplace(at);auto&g=result.first->second;const auto&line=lines[at];
  if(!result.second&&g.model==line&&g.translated==(translation&&!line.translation.empty()))return g;
  bool same=!result.second&&g.model.time==line.time&&g.model.text==line.text;
  auto y=g.y,z=g.zoom,ma=g.mainAlpha,sa=g.subAlpha;auto dots=g.dots;float opacity=g.opacity;bool placed=g.placed;uint64_t id=same?g.identity:++groupSerial;
  g=LyricGroup();g.model=line;g.identity=id;g.translated=translation&&!line.translation.empty();g.y=y;g.zoom=z;g.placed=placed;g.mainAlpha=ma;g.subAlpha=sa;g.opacity=opacity;
  if(same)g.dots=dots;
  g.main=layout(line.text,mainFormat.get());g.mainHeight=g.main.height;
  float x=0;for(const auto&word:line.words){Glyph glyph;glyph.layout=layout(word.text,mainFormat.get());glyph.x=x;glyph.time=word.time;glyph.duration=word.duration;x+=glyph.layout.width;g.mainHeight=std::max(g.mainHeight,glyph.layout.height);g.glyphs.push_back(std::move(glyph));}
  if(g.mainHeight<=0)g.mainHeight=mainRowHeight;
  if(g.translated)g.subtitle=layout(line.translation,secondaryFormat.get());
  g.mainAlpha.snap(at==index?1.f:.4f);g.subAlpha.snap(at==index?.8f:.4f);g.active=at==index;lineBuildCalls++;return g;
 }
 float groupScale(int at,int focus){return at==focus&&lyricView()!=LyricView::Prelude?1.f:ContextFontSize/CurrentFontSize;}
 int layoutDistance(int at,int focus,int limit)const{
  if(at<0||focus<0||at>=int(lines.size())||focus>=int(lines.size()))return limit;int distance=0;
  auto bounds=browseIndex>=0?browseBounds():std::make_pair(-1,-1);for(int n=std::min(at,focus);n<std::max(at,focus);n++)if((browseIndex<0||(n>=bounds.first&&n<=bounds.second))&&++distance>=limit)break;return distance;
 }
 float positionFor(int at,int focus,float anchorY){
  if(at<0||focus<0||at>=int(lines.size())||focus>=int(lines.size()))return anchorY+(at>focus?1:-1)*(height+200);
  float y=anchorY;int from=std::min(at,focus),to=std::max(at,focus);
  auto bounds=browseIndex>=0?browseBounds():std::make_pair(-1,-1);auto included=[&](int n){return browseIndex<0||(n>=bounds.first&&n<=bounds.second);};
  if(layoutDistance(at,focus,7)>6)return anchorY+(at>focus?1:-1)*(height+200);
  for(int n=from;n<to;n++)if(included(n))y+=(at>focus?1:-1)*(group(n).renderedHeight(groupScale(n,focus))+GroupGap);
  return y;
 }
 void roles(){
  double now=motionNow(),time=current()+offset;
  for(auto&item:groups){auto&g=item.second;bool active=item.first==index;if(active==g.active)continue;
   if(g.active){g.resetWords.clear();for(const auto&w:g.glyphs)g.resetWords.push_back(floatWord(time-w.time,w.duration));g.resetAt=now;}
   g.active=active;g.mainAlpha.target(active?1.f:.4f,now);g.subAlpha.target(active?.8f:.4f,now);
  }
 }
 void beginFlight(int desired,int playbackIndex){
  if(lines.empty()||focusIndex<0)return;if(playbackIndex<0&&preludePreview>=0)desired=preludePreview;desired=std::max(0,std::min(int(lines.size())-1,desired));double now=animationWall(),motionTime=motionNow();bool sourcePrelude=flight.rows.empty()&&lyricView()==LyricView::Prelude,sourceBrowse=browseIndex>=0;int sourceFocus=std::max(0,std::min(int(lines.size())-1,focusIndex));
  for(int i=std::max(0,desired-6);i<=std::min(int(lines.size())-1,desired+6);i++)group(i);
  if(secondaryRowHeight<=0)secondaryRowHeight=layout(L"Ag",secondaryFormat.get()).height;
  std::vector<float> heights(lines.size()),prefix(lines.size()+1),sourcePrefix(lines.size()+1);auto bounds=sourceBrowse?browseBounds():std::make_pair(-1,-1);float context=ContextFontSize/CurrentFontSize;
  for(size_t i=0;i<lines.size();i++){auto found=groups.find(int(i));heights[i]=found!=groups.end()&&found->second.model==lines[i]?found->second.renderedHeight(1):mainRowHeight+(translation&&!lines[i].translation.empty()?TranslationGap+secondaryRowHeight:0);float step=heights[i]*context+GroupGap;prefix[i+1]=prefix[i]+step;sourcePrefix[i+1]=sourcePrefix[i]+(!sourceBrowse||(int(i)>=bounds.first&&int(i)<=bounds.second)?step:0);}
  auto previous=std::move(flight);auto previousPhase=previous.phase(now);flight=LyricFlight();flight.rows.resize(lines.size());flight.started=now;flight.focus=desired;flight.playbackIndex=playbackIndex;flight.curve=lineCurve;flight.stagger=stagger&&desired!=sourceFocus;flight.prelude=playbackIndex<0;int direction=desired>=sourceFocus?1:-1;
  float destinationAnchor=lyricAnchor()+(flight.prelude?mainRowHeight+GroupGap:0),sourceAnchor=layoutAnchor;
  for(size_t i=0;i<lines.size();i++){
   auto&r=flight.rows[i];r.height=heights[i];r.bucket=std::max(-4,std::min(4,int(i)-desired))*direction+4;
   r.from={sourceAnchor+sourcePrefix[i]-sourcePrefix[sourceFocus]+(!sourcePrelude&&int(i)>sourceFocus?heights[sourceFocus]*(1-context):0),!sourcePrelude&&int(i)==sourceFocus?1.f:context,int(i)==index?1.f:.4f,int(i)==index?.8f:.4f,!sourcePrelude||int(i)==preludePreview?1.f:0.f};
   if(previous.rows.size()==lines.size())r.from=previous.sample(previous.rows[i],previousPhase);
   else{auto found=groups.find(int(i));if(found!=groups.end()&&found->second.placed&&((!sourcePrelude&&std::abs(int(i)-sourceFocus)<=6)||groupVisible(found->second,motionTime))){const auto&g=found->second;r.from={g.y.value(motionTime),g.zoom.value(motionTime),g.mainAlpha.value(motionTime),g.subAlpha.value(motionTime),g.opacity};}}
   r.to={destinationAnchor+prefix[i]-prefix[desired]+(!flight.prelude&&int(i)>desired?heights[desired]*(1-context):0),!flight.prelude&&int(i)==desired?1.f:context,int(i)==playbackIndex?1.f:.4f,int(i)==playbackIndex?.8f:.4f,!flight.prelude||int(i)==preludePreview?1.f:0.f};
  }
 }
 void applyFlight(double now){
  auto phase=flight.phase(now);std::vector<int>wanted;wanted.reserve(32);auto add=[&](int id){if(id>=0&&id<int(lines.size())&&wanted.size()<32&&std::find(wanted.begin(),wanted.end(),id)==wanted.end())wanted.push_back(id);};
  for(size_t i=0;i<flight.rows.size();i++){auto&r=flight.rows[i];r.point=flight.sample(r,phase);if(r.point.y+r.height*r.point.zoom>=viewportTop&&r.point.y<=viewportBottom)add(int(i));}
  for(size_t i=0;i<flight.rows.size();i++){const auto&r=flight.rows[i];if(r.point.y+r.height*r.point.zoom>=viewportTop-mainRowHeight*2&&r.point.y<=viewportBottom+mainRowHeight*2)add(int(i));}
  for(int i=flight.focus-2;i<=flight.focus+2;i++)add(i);
  for(auto it=groups.begin();it!=groups.end();)if(std::find(wanted.begin(),wanted.end(),it->first)==wanted.end())it=groups.erase(it);else ++it;
  for(int id:wanted){auto&g=group(id);const auto&p=flight.rows[id].point;g.y.snap(p.y);g.zoom.snap(p.zoom);g.mainAlpha.snap(p.mainAlpha);g.subAlpha.snap(p.subAlpha);g.opacity=p.opacity;g.placed=true;g.active=id==index;g.browsePlaceholder=false;}
  focusIndex=flight.focus;layoutAnchor=lyricAnchor()+(flight.prelude?mainRowHeight+GroupGap:0);layoutDirty=false;
 }
 void seekTo(double target){
  target=std::max(0.,duration>0?std::min(duration,target):target);bool canFly=valid&&!lines.empty()&&focusIndex>=0&&!groups.empty();
  if(canFly){int actual=rowAt(target+offset),desired=std::max(0,rowAt(target+offset+(playing&&stagger?200:0)));beginFlight(desired,actual);}else flight=LyricFlight();
  if(browseIndex>=0)stopBrowsing();returnFromBrowse=false;error.clear();snapRow=!canFly;position=target;anchor=nowMs();valid=true;layoutDirty=true;
 }
 void reflow(int desired,bool animate){
  if(lines.empty()){groups.clear();focusIndex=-1;transitionActive=false;layoutDirty=false;return;}
  desired=std::max(0,std::min(int(lines.size())-1,desired));int previous=focusIndex;
  std::vector<int>wanted;auto add=[&](int n){if(n>=0&&n<int(lines.size())&&std::find(wanted.begin(),wanted.end(),n)==wanted.end())wanted.push_back(n);};
  if(browseIndex>=0){add(desired);for(int direction:{-1,1}){int at=desired;for(int d=0;d<6;d++){at=browseNeighbor(at,direction);if(at<0)break;add(at);}}}
  else for(int d=-6;d<=6;d++)add(desired+d);
  for(int d=-2;d<=2;d++)add(index+d);
  for(int n:wanted)group(n);
  float newAnchor=rowAnchor();
  double now=motionNow();int direction=desired>=previous?1:-1;
  for(int n:wanted){auto&g=group(n);g.opacity=1;
   if(!g.placed){int initial=previous>=0&&animate?previous:desired;g.y.snap(positionFor(n,initial,previous>=0&&animate?layoutAnchor:newAnchor));g.zoom.snap(groupScale(n,initial));g.placed=true;}
   float target=positionFor(n,desired,newAnchor);
   if(animate){double delay=browseIndex>=0||returnFromBrowse||previous==desired?0:rnpDelay(n-desired,direction,stagger),duration=browseIndex>=0?250:500;g.y.target(target,now,delay,duration,lineCurve);g.zoom.target(groupScale(n,desired),now,delay,duration,lineCurve);}else{g.y.snap(target);g.zoom.snap(groupScale(n,desired));}
  }
  layoutAnchor=newAnchor;focusIndex=desired;
  for(auto it=groups.begin();it!=groups.end();){
   bool keep=std::find(wanted.begin(),wanted.end(),it->first)!=wanted.end();float y=it->second.y.value(now);
   if(!keep&&(!animate||!it->second.y.active(now)||y+it->second.renderedHeight(it->second.zoom.value(now))<viewportTop-8||y>viewportBottom+8))it=groups.erase(it);else ++it;
  }
  while(groups.size()>32){auto farthest=groups.end();for(auto it=groups.begin();it!=groups.end();++it)if(it->first!=index&&(farthest==groups.end()||std::abs(it->first-desired)>std::abs(farthest->first-desired)))farthest=it;if(farthest==groups.end())break;groups.erase(farthest);}
  auto bounds=browseIndex>=0?browseBounds():std::make_pair(-1,-1);for(auto&item:groups)item.second.browsePlaceholder=browseIndex>=0&&item.first>bounds.first&&item.first<bounds.second&&isInterlude(item.second);
  if(previous!=desired){rowSince=now;layoutChanges++;}
  roles();layoutDirty=false;
 }
 void updatePlaceholders(){
  if(lines.empty())preludeDots=InterludeAnimation();
  if(!placeholder.value)placeholder=layout(L"等待歌词",mainFormat.get());
  if(!syncing.value)syncing=layout(L"同步中",mainFormat.get());
  preludePreview=-1;for(size_t i=0;i<lines.size();i++){const auto&line=lines[i];if(!line.isInterlude&&(!line.text.empty()||std::any_of(line.words.begin(),line.words.end(),[](const Word&w){return !w.text.empty();}))){preludePreview=int(i);break;}}
 }
 bool geometry(){
  RECT rect{};GetClientRect(hwnd,&rect);
  if(geometryPixels.cx==rect.right&&geometryPixels.cy==rect.bottom&&geometryDpi==dpi)return false;
  // Record this before SetWindowRgn, which sends nested window-position messages.
  geometryPixels={rect.right,rect.bottom};geometryDpi=dpi;flight=LyricFlight();
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
   target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
  }if(!graphics.target)graphics.init(target.get(),bgA,bgB,cover,width,height);
 }
 void text(Graphics&g,const Layout&l,float x,float y,D2D1_COLOR_F color,float alpha=1){if(!l.value)return;g.brush->SetColor(color);g.brush->SetOpacity(alpha);g.target->DrawTextLayout(D2D1::Point2F(x,y),l.value.get(),g.brush.get(),D2D1_DRAW_TEXT_OPTIONS_NONE);g.brush->SetOpacity(1);}
 float rowWidth(const LyricGroup&g)const{return g.glyphs.empty()?g.main.width:g.glyphs.back().x+g.glyphs.back().layout.width;}
 bool canScroll(int id)const{return browseIndex<0&&id<=index;}
 float timedHorizontalPhase(const LyricGroup&g,double time,int id)const{
  if(!canScroll(id))return 0;
  const Line*line=&g.model;
  return float(clamp((time-line->time-900)/std::max(1200.,(line->duration>0?line->duration:4000)-1600),0,1));
 }
 float horizontalPhase(const LyricGroup&g,double time,int id)const{
  if(g.glyphs.empty())return timedHorizontalPhase(g,time,id);
  if(!canScroll(id))return 0;
  float zoom=std::max(.01f,g.zoom.value(motionNow())),available=std::max(1.f,width-textX-12),overflow=rowWidth(g)*zoom-available;
  if(overflow<=0)return 0;
  const auto&last=g.glyphs.back();if(time>=last.time+last.duration)return 1;
  // Track the sung position in measured text space. Gaps hold this cursor;
  // seeking is stateless, and contiguous word boundaries stay continuous.
  float cursor=0;
  for(const auto&w:g.glyphs)if(time>=w.time)cursor=std::max(cursor,w.x+w.layout.width*float(unit((time-w.time)/w.duration)));
  return float(clamp((cursor*zoom-available*.5f)/overflow,0,1));
 }
 void drawInterlude(Graphics&g,const InterludeAnimation&animation,float x,float y,double time,double start,double duration,float opacity=1,bool stationary=false){
  float alpha=opacity*(stationary?1.f:animation.opacity.value(motionNow()));if(alpha<=0)return;
  float breath=stationary?1.f:animation.scale(motionNow()),diameter=CurrentFontSize*.7f,gap=CurrentFontSize*.5f;
  g.brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
  for(int i=0;i<3;i++){auto dot=stationary?InterludeDot{.4f,.9f}:animation.dot(time,start,duration,i,motionNow());float radius=diameter*.5f*dot.scale*breath;g.brush->SetOpacity(alpha*dot.alpha);g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x+(diameter*.5f+i*(diameter+gap))*breath,y+mainRowHeight*.5f),radius,radius),g.brush.get());}
  g.brush->SetOpacity(1);
 }
 void drawGroup(Graphics&g,const LyricGroup&r,int id,double time){
  if(r.opacity<=0)return;double now=motionNow();float zoom=std::max(.01f,r.zoom.value(now)),y=r.y.value(now);
  float available=std::max(1.f,width-textX-12),phase=horizontalPhase(r,time,id);
  D2D1_MATRIX_3X2_F transform;bool scaled=zoom!=1.f;float originX=textX,originY=y;
  if(scaled){g.target->GetTransform(&transform);g.target->SetTransform(D2D1::Matrix3x2F::Scale(zoom,zoom)*D2D1::Matrix3x2F::Translation(textX,y)*transform);originX=originY=0;}
  if(staticInterlude(r)){drawInterlude(g,r.dots,originX,originY,0,0,0,r.opacity,true);if(scaled)g.target->SetTransform(transform);return;}
  float x=originX-std::max(0.f,rowWidth(r)*zoom-available)*phase/zoom;
  if(r.glyphs.empty())text(g,r.main,x,originY,D2D1::ColorF(D2D1::ColorF::White),r.mainAlpha.value(now)*r.opacity);
  else for(size_t i=0;i<r.glyphs.size();i++){
   const auto&w=r.glyphs[i];FloatWord f{.4f,0};
   if(r.active)f=floatWord(time-w.time,w.duration);
   else if(i<r.resetWords.size()&&now-r.resetAt<200){double p=unit((now-r.resetAt)/200);f={mixf(r.resetWords[i].alpha,.4f,p),mixf(r.resetWords[i].lift,0,lyricEase(p))};}
   text(g,w.layout,x+w.x,originY-f.lift,D2D1::ColorF(D2D1::ColorF::White),f.alpha*r.opacity);
  }
  if(indicatorVisible(r))drawInterlude(g,r.dots,originX,originY,time,r.model.time,interludeDuration(r.model,id),r.mainAlpha.value(now)*r.opacity);
  if(r.translated){float subX=originX-std::max(0.f,r.subtitle.width*zoom-available)*timedHorizontalPhase(r,time,id)/zoom;text(g,r.subtitle,subX,originY+r.subtitleOffset(1.f),D2D1::ColorF(D2D1::ColorF::White),r.subAlpha.value(now)*r.opacity);}
  if(scaled)g.target->SetTransform(transform);
 }
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
  if(!resizing||fluid.pixels.empty()){double wall=animationWall();fluid.setSize(width,height);fluid.prepare(background,backgroundClock.time(wall),backgroundClock.position(wall));}
  if(!g.background){check(g.target->CreateBitmap(D2D1::SizeU(FluidBackground::width,FluidBackground::height),fluid.pixels.data(),FluidBackground::width*4,D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)),g.background.put()));check(g.target->CreateBitmapBrush(g.background.get(),D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),g.backgroundBrush.put()));g.backgroundRevision=fluid.revision;}
  else if(g.backgroundRevision!=fluid.revision){check(g.background->CopyFromMemory(nullptr,fluid.pixels.data(),FluidBackground::width*4));g.backgroundRevision=fluid.revision;}
  g.backgroundBrush->SetTransform(D2D1::Matrix3x2F::Scale(width/FluidBackground::width,height/FluidBackground::height));g.target->FillRoundedRectangle(card,g.backgroundBrush.get());
 }
 void drawLyrics(Graphics&g,double time){
  float y=lyricAnchor();if(!flight.rows.empty()){if(preludeDots.visible(motionNow()))drawInterlude(g,preludeDots,textX,y,time,0,lines.front().time);}
  else switch(lyricView()){
   case LyricView::Waiting:text(g,placeholder,textX,y,D2D1::ColorF(D2D1::ColorF::White));return;
   case LyricView::Syncing:text(g,syncing,textX,y,D2D1::ColorF(D2D1::ColorF::White));return;
   case LyricView::Prelude:drawInterlude(g,preludeDots,textX,y,time,0,lines.front().time);break;
   case LyricView::Rows:break;
  }
  for(const auto&item:groups)if(groupVisible(item.second,motionNow()))drawGroup(g,item.second,item.first,time);
 }
 void draw(Graphics&g){
  auto*t=g.target;double time=current()+offset;

  t->Clear(D2D1::ColorF(0,0));auto card=D2D1::RoundedRect(D2D1::RectF(.5f,.5f,width-.5f,height-.5f),17,17);drawBackground(g,card);g.brush->SetColor(D2D1::ColorF(.62f,.65f,.80f,.65f));t->DrawRoundedRectangle(card,g.brush.get(),1);
  // One rounded mask for both layers avoids accumulated antialiasing halos.
  t->PushLayer(D2D1::LayerParameters(artBounds,coverClip.get()),nullptr);
  g.brush->SetColor(D2D1::ColorF(.33f,.39f,.51f));t->FillRectangle(artBounds,g.brush.get());
  if(g.cover){float side=float(std::min(cover.width,cover.height)),x=(cover.width-side)/2,y=(cover.height-side)/2;t->DrawBitmap(g.cover.get(),artBounds,playing?float(clamp((nowMs()-coverSince)/250,0,1)):1,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,D2D1::RectF(x,y,x+side,y+side));}
  t->PopLayer();
  text(g,metaLayout,textX,12,D2D1::ColorF(.87f,.89f,.94f));
  t->PushAxisAlignedClip(D2D1::RectF(textX,viewportTop,width-12,viewportBottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  drawLyrics(g,time);
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
  if(!visible){flight=LyricFlight();stopBrowsing();snapRow=true;layoutDirty=true;}
  update();
 }
 void schedule(){
  if(!renderVisible){stopFrames();return;}
  double time=current()+offset,next=200;bool active=playing&&valid&&(transitionActive||lyricView()==LyricView::Prelude);
  if(playing&&valid&&index+1<int(lines.size())){next=std::min(next,std::max(1.,lines[index+1].time-time));if(browseIndex<0&&stagger&&focusIndex<=index)next=std::min(next,std::max(1.,lines[index+1].time-time-200));}
  if(playing&&valid)for(const auto&item:groups){const auto&g=item.second;if(!groupVisible(g,motionNow())||staticInterlude(g))continue;const auto&line=g.model;
   if(indicatorVisible(g))active=true;
   if(g.active)for(const auto&word:line.words){if(time>=word.time&&time<word.time+word.duration+150)active=true;else if(word.time>time)next=std::min(next,word.time-time);}
   if(canScroll(item.first)&&((g.glyphs.empty()&&rowWidth(g)*g.zoom.value(motionNow())>width-textX-12)||(g.translated&&g.subtitle.width*g.zoom.value(motionNow())>width-textX-12))){double begin=line.time+900,end=begin+std::max(1200.,(line.duration>0?line.duration:4000)-1600);if(time<begin)next=std::min(next,begin-time);else if(time<end)active=true;}
  }
  bool flying=!flight.rows.empty(),coverAnimating=coverSince>=0&&nowMs()-coverSince<250;active=active||coverAnimating||flying;
  bool backgroundActive=backgroundAnimating();bool run=flying||(playing&&valid&&(!lines.empty()||hover||coverAnimating||backgroundActive));double wanted=active?1000./(fps==60?60:30):std::max(1.,next);if(backgroundActive)wanted=std::min(wanted,1000./background.fps);
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
  motion.setRunning(renderVisible&&playing&&valid,animationWall());backgroundClock.update(backgroundAnimating(),background.speed,animationWall());
  if(!renderVisible){stopFrames();return;}
  if(!valid&&!flight.rows.empty()){flight=LyricFlight();layoutDirty=true;}
  if(browseIndex>=0&&(lines.empty()||animationWall()>=browseUntil))stopBrowsing();
  if(browseIndex>=0){int normalized=browseFocus(browseIndex);if(normalized<0)stopBrowsing();else if(normalized!=browseIndex){browseIndex=normalized;layoutDirty=true;}}
  double time=current()+offset;int next=rowAt(time);bool actualChanged=next!=index;if(next<index&&flight.rows.empty())snapRow=true;index=next;
  if(!flight.rows.empty()){
   if(flight.rows.size()!=lines.size())flight=LyricFlight();
   else{int desired=std::max(0,index<0?preludePreview:rowAt(time+(playing&&valid&&stagger?200:0)));if(desired!=flight.focus||flight.playbackIndex!=index)beginFlight(desired,index);if(actualChanged)roles();applyFlight(animationWall());snapRow=false;returnFromBrowse=false;transitionActive=false;
    if(flight.running(animationWall())){updateIndicators();InvalidateRect(hwnd,nullptr,FALSE);schedule();return;}flight=LyricFlight();layoutDirty=true;
   }
  }
  if(snapRow&&browseIndex>=0)stopBrowsing();
  int desired=focusIndex;
  if(browseIndex>=0)desired=browseIndex=std::min(browseIndex,int(lines.size())-1);
  else if(lyricView()==LyricView::Prelude)desired=std::max(0,preludePreview);
  else if(snapRow||focusIndex<0||groups.empty()||(!playing&&actualChanged))desired=std::max(0,index);
  else if(returnFromBrowse)desired=std::max(0,playing&&valid?rowAt(time+(stagger?200:0)):index);
  else if(playing&&valid)desired=std::max(0,rowAt(time+(stagger?200:0)));
  bool animate=playing&&valid&&!snapRow&&focusIndex>=0&&layoutDistance(desired,focusIndex,4)<=3;
  if(snapRow)layoutDirty=true;
  // Prelude and the first active row can share an index but use different anchors.
  bool reflowed=layoutDirty||desired!=focusIndex||groups.empty()||layoutAnchor!=rowAnchor();if(reflowed)reflow(desired,animate);
  if(actualChanged&&!reflowed)roles();
  if(snapRow||(!playing&&browseIndex<0&&actualChanged))for(auto&item:groups){auto&g=item.second;g.mainAlpha.snap(g.mainAlpha.to);g.subAlpha.snap(g.subAlpha.to);if(snapRow)g.resetWords.clear();}
  snapRow=false;returnFromBrowse=false;transitionActive=false;double now=motionNow();
  updateIndicators();
  if(lyricView()==LyricView::Rows)for(auto&item:groups){auto&g=item.second;bool stationary=staticInterlude(g);if((browseIndex>=0&&isInterlude(g)&&!stationary)||(emptyGroup(g)&&!indicatorVisible(g)&&!stationary))continue;bool visible=groupVisible(g,now);bool crosses=std::min(g.y.from,g.y.to)<=viewportBottom&&std::max(g.y.from,g.y.to)+g.renderedHeight(std::max(g.zoom.from,g.zoom.to))>=viewportTop;transitionActive=transitionActive||(g.y.active(now)&&crosses)||(visible&&(g.zoom.active(now)||(!stationary&&(g.mainAlpha.active(now)||g.subAlpha.active(now)||(!g.active&&!g.resetWords.empty()&&now-g.resetAt<200)))));}
  InvalidateRect(hwnd,nullptr,FALSE);schedule();
 }
 void paint(){PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);if(renderVisible)try{ensureTarget();renderCalls++;target->BeginDraw();draw(graphics);HRESULT hr=target->EndDraw();if(hr==D2DERR_RECREATE_TARGET){graphics.clear();target.reset();}else if(FAILED(hr))error="draw-failed";}catch(...){graphics.clear();target.reset();error="render-target-failed";}EndPaint(hwnd,&ps);}
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
  Json j=Json::obj();j["type"]=type;j["accepted"]=accepted;j["songId"]=songId;j["lyricCount"]=int(lines.size());j["revision"]=revision;j["positionMs"]=current();j["playing"]=playing;j["clockValid"]=valid;j["seekPending"]=seekPending;j["seekRequest"]=double(seekRequest);j["error"]=error;
  j["backgroundBackend"]=fluid.backend();j["backgroundGpuReady"]=fluid.gpuReady();if(!fluid.gpuError.empty())j["backgroundGpuError"]=fluid.gpuError;
  if(type=="background-state")j["background"]=backgroundState();return j;
 }
 Json diagnostics(const std::string&type,bool accepted=true){Json j=state(type,accepted);j["currentLineIndex"]=index;j["index"]=index;j["frameRunning"]=frameRunning;j["intervalMs"]=int(intervalMs);j["renderCalls"]=double(renderCalls);j["lineBuildCalls"]=double(lineBuildCalls);j["backend"]="Direct2D/DirectWrite";j["hardware"]=hardware;j["renderVisible"]=renderVisible;j["through"]=through;j["top"]=top;j["windowTopmost"]=(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;j["windowNoActivate"]=(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)&WS_EX_NOACTIVATE)!=0;j["fps"]=fps;j["systemMoving"]=systemMoving;j["frameScheduler"]=frameTimer&&(!systemMoving||modalFrames.active())?(systemMoving?"waitable-posted":"waitable-deadline"):"window-timer";j["coverLoaded"]=!cover.pixels.empty();j["dpi"]=double(dpi);j["coverLeftPx"]=std::round(artBounds.left*scale());j["coverTopPx"]=std::round(artBounds.top*scale());j["coverSidePx"]=std::round(artSize*scale());j["coverRadiusPx"]=std::round(artRadius*scale());j["windowHeightPx"]=std::round(height*scale());j["iconBackend"]="vector";j["windowWidthPx"]=std::round(width*scale());j["barLeftPx"]=transport.left*scale();j["barRightPx"]=transport.right*scale();j["controlYPx"]=transport.y*scale();j["previousPx"]=transport.previous*scale();j["playPx"]=transport.play*scale();j["nextPx"]=transport.next*scale();j["rowStartedMs"]=rowSince;j["focusIndex"]=focusIndex;j["browsing"]=browseIndex>=0;j["browseIndex"]=browseIndex;j["browseRemainingMs"]=browseIndex>=0?std::max(0.,browseUntil-animationWall()):0;j["groupCacheLimit"]=32;j["transitionActive"]=transitionActive;j["transitionProgress"]=unit((motionNow()-rowSince-200)/500);j["transitionMode"]=transitionActive?"group-scroll":"none";j["animationTimeMs"]=motionNow();j["retainedRows"]=int(groups.size());j["layoutChanges"]=double(layoutChanges);j["lookaheadMs"]=stagger?200:0;j["stagger"]=stagger;j["curve"]=lineCurve;j["viewportTop"]=viewportTop;j["viewportBottom"]=viewportBottom;j["groupGap"]=GroupGap;j["layoutAnchor"]=layoutAnchor;j["groups"].kind=Json::Array;
  for(const auto&item:groups){const auto&g=item.second;Json v=Json::obj();v["index"]=item.first;v["identity"]=double(g.identity);v["text"]=narrow(g.model.text);v["translation"]=g.translated?narrow(g.model.translation):"";v["top"]=g.y.value(motionNow());v["target"]=g.y.to;v["startAt"]=g.y.start;v["durationMs"]=g.y.duration;v["height"]=g.renderedHeight(1.f);v["scale"]=g.zoom.value(motionNow());v["scaleTarget"]=g.zoom.to;v["scaleStartAt"]=g.zoom.start;v["scaleDurationMs"]=g.zoom.duration;v["renderedHeight"]=g.renderedHeight(g.zoom.value(motionNow()));v["fontSize"]=CurrentFontSize*g.zoom.value(motionNow());v["translationFontSize"]=g.translated?TranslationFontSize*g.zoom.value(motionNow()):0.f;v["rendered"]=groupVisible(g,motionNow());v["mainHeight"]=g.mainHeight;v["subOffset"]=g.subtitleOffset(g.zoom.value(motionNow()));v["mainAlpha"]=g.mainAlpha.value(motionNow());v["subAlpha"]=g.subAlpha.value(motionNow());v["active"]=g.active;j["groups"].array.push_back(std::move(v));}
  j["seekFlight"]=!flight.rows.empty();if(!flight.rows.empty()){j["transitionMode"]="seek-flight";j["transitionActive"]=true;j["transitionProgress"]=unit((animationWall()-flight.started)/(500+(flight.stagger?400:0)));}j["background"]=backgroundState();j["backgroundAnimating"]=backgroundAnimating();j["backgroundFrameBuilds"]=double(fluid.revision);j["backgroundBackend"]=fluid.backend();j["backgroundGpuReady"]=fluid.gpuReady();j["backgroundGpuError"]=fluid.gpuError;j["backgroundTimeMs"]=backgroundClock.time(animationWall());j["hotkeyRegistered"]=hotkeyRegistered;j["first"]=index>=0&&index<int(lines.size())?narrow(lines[index].text):"";j["second"]=index>=0&&index<int(lines.size())?(translation&&!lines[index].translation.empty()?narrow(lines[index].translation):index+1<int(lines.size())?narrow(lines[index+1].text):""):"";return j;}
 void report(const std::string&type,bool accepted=true,bool detailed=false){if(bridge)bridge->send(detailed?diagnostics(type,accepted):state(type,accepted));}
 void command(const std::string&action,double pos=0){if(!bridge)return;Json j=Json::obj();j["type"]="command";j["action"]=action;j["position"]=pos;if(action=="seek"){j["requestId"]=double(seekRequest);j["songId"]=songId;}bridge->send(j);}
 void cancelSeek(){seekPending=false;KillTimer(hwnd,3);}
 void requestSeek(double target){
  if(!bridge)return;
  if(!seekPending)seekFallback=current();seekStarted=nowMs();seekRequest=++seekSerial;seekPending=true;
  seekTo(target);SetTimer(hwnd,3,3500,nullptr);
  command("seek",position);
 }
 void startCoverWorker(){
  coverQueue=std::make_shared<CoverQueue>();auto queue=coverQueue;HWND window=hwnd;
  std::thread([queue,window]{for(;;){std::wstring url;uint64_t epoch;
   {std::unique_lock<std::mutex> lock(queue->mutex);queue->changed.wait(lock,[&]{return queue->stop||queue->pending;});if(queue->stop)return;url=queue->url;epoch=queue->epoch;queue->pending=false;}
   auto result=downloadCover(url,epoch);
   {std::lock_guard<std::mutex> lock(queue->mutex);if(queue->stop)return;if(queue->pending&&queue->epoch!=epoch)continue;}
   if(PostMessageW(window,CoverReady,0,reinterpret_cast<LPARAM>(result.get())))result.release();
  }}).detach();
 }
 void loadCover(const std::wstring&url){coverUrl=url;uint64_t epoch=++coverEpoch;if(!coverQueue)return;
  {std::lock_guard<std::mutex> lock(coverQueue->mutex);coverQueue->url=url;coverQueue->epoch=epoch;coverQueue->pending=true;}coverQueue->changed.notify_one();
 }
 void onCover(std::unique_ptr<CoverData> data){if(data->epoch!=coverEpoch||closing)return;cover=std::move(*data);fluid.setCover(cover.pixels,cover.width,cover.height,backgroundClock.time(animationWall()),!backgroundPlaying());coverSince=nowMs();if(!cover.pixels.empty()){uint64_t r=0,g=0,b=0;for(size_t i=0;i<cover.pixels.size();i+=4){b+=cover.pixels[i];g+=cover.pixels[i+1];r+=cover.pixels[i+2];}float count=float(cover.pixels.size()/4);bgA=D2D1::ColorF((r/count*.3f+15)/255,(g/count*.3f+15)/255,(b/count*.3f+22)/255);bgB=D2D1::ColorF(std::min(100/255.f,bgA.r+24/255.f),std::min(100/255.f,bgA.g+17/255.f),std::min(120/255.f,bgA.b+30/255.f));}if(graphics.target){try{graphics.updateCover(cover);graphics.updateGradient(bgA,bgB,width,height);}catch(...){graphics.clear();}}update();}
 void receive(const Json&m){
  received=nowMs();auto type=m["type"].s();
  if(type=="track"){auto id=m["id"].s();if(id!=songId){flight=LyricFlight();stopBrowsing();songId=id;groups.clear();focusIndex=-999;layoutDirty=true;snapRow=true;cancelSeek();seekRequest=0;lines.clear();updatePlaceholders();index=-999;position=0;revision=0;valid=false;error.clear();}title=widen(m["title"].s());artist=widen(m["artist"].s());duration=std::max(0.,m["duration"].n());rebuildMeta();auto cover=widen(m["cover"].s());if(cover!=coverUrl)loadCover(cover);update();}
  else if(type=="lyrics"){
   if(m["songId"].s()!=songId){error="song-mismatch";report("lyrics-ack",false);return;}
   std::vector<Line> next;for(const auto&raw:m["lines"].array){if(next.size()>=5000)break;if(raw.kind!=Json::Object)continue;Line line;line.time=std::max(0.,raw["time"].n());line.duration=std::max(0.,raw["duration"].n());line.text=widen(raw["text"].s());line.isInterlude=raw["isInterlude"].b()||line.text.empty();line.translation=widen(raw["translation"].s());for(const auto&w:raw["words"].array){if(line.words.size()>=500)break;if(w.kind!=Json::Object)continue;line.words.push_back({widen(w["text"].s()),w["time"].n(),std::max(1.,w["duration"].n(1))});}next.push_back(std::move(line));}
   std::stable_sort(next.begin(),next.end(),[](const Line&a,const Line&b){return a.time<b.time;});
   if(next!=lines){bool same=index>=0&&index<int(lines.size())&&index<int(next.size())&&lines[index].time==next[index].time&&lines[index].text==next[index].text;
    flight=LyricFlight();lines=std::move(next);layoutDirty=true;if(!same)snapRow=true;updatePlaceholders();}
   revision=m["revision"].n();error.clear();update();report("lyrics-ack");
  }else if(type=="clock"){
   if(m["songId"].kind==Json::String&&m["songId"].s()!=songId)return;
   if(seekRequest&&uint64_t(m["seekRequest"].n())!=seekRequest)return;
   auto seekState=m["seekState"].s();if(seekPending&&seekState!="pending"&&seekState!="confirmed"&&seekState!="failed")return;
   if(seekState=="confirmed"||seekState=="failed")cancelSeek();
   double incoming=std::max(0.,m["position"].n());if(valid&&std::abs(incoming-current())>1500&&flight.rows.empty())snapRow=true;
   position=incoming;anchor=nowMs();playing=m["playing"].b();valid=m["valid"].b();offset=m["offset"].n();if(m["duration"].n()>0)duration=m["duration"].n();update();report("host-state");}
  else if(type=="style"){int curve=int(clamp(m["curve"].n(),0,3));bool wave=m["stagger"].b(true);if(curve!=lineCurve||wave!=stagger){lineCurve=curve;stagger=wave;layoutDirty=true;update();}}
  else if(type=="background"){if(readBackground(m)){save();update();}report("background-state");}
  else if(type=="show"){ShowWindow(hwnd,SW_SHOWNOACTIVATE);constrain();}
  else if(type=="unlock"){setThrough(false);update();}
  else if(type=="reset"){setThrough(false);SetWindowPos(hwnd,nullptr,int(80*scale()),int(80*scale()),int(460*scale()),int(130*scale()),SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(hwnd,SW_SHOWNOACTIVATE);save();update();}
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
 void setThrough(bool value){through=value;LONG_PTR style=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);if(value)style|=WS_EX_TRANSPARENT|WS_EX_LAYERED;else style&=~(WS_EX_TRANSPARENT|WS_EX_LAYERED);SetWindowLongPtrW(hwnd,GWL_EXSTYLE,style);if(!applyWindowActivationMode(hwnd,top,through))error="activation-mode-failed";if(value)SetLayeredWindowAttributes(hwnd,0,255,LWA_ALPHA);hover=false;}
 void constrain(){RECT rect{};GetWindowRect(hwnd,&rect);if(!MonitorFromRect(&rect,MONITOR_DEFAULTTONULL))SetWindowPos(hwnd,nullptr,int(80*scale()),int(80*scale()),0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);}
 void save(){if(!hwnd)return;RECT rect{};GetWindowRect(hwnd,&rect);Json j=Json::obj();j["X"]=int(rect.left);j["Y"]=int(rect.top);j["PositionInPixels"]=true;j["Width"]=(rect.right-rect.left)/scale();j["Height"]=(rect.bottom-rect.top)/scale();j["Top"]=top;j["Translation"]=translation;j["Fps"]=fps;j["Background"]=backgroundState();try{auto path=std::filesystem::path(settingsPath);std::filesystem::create_directories(path.parent_path());std::ofstream file(path,std::ios::binary);file<<j.dump();}catch(...){}}
 void menu(POINT point){HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING|(translation?MF_CHECKED:0),1,L"显示翻译");AppendMenuW(menu,MF_STRING|(top?MF_CHECKED:0),2,L"置顶");AppendMenuW(menu,MF_STRING|(through?MF_CHECKED:0),3,L"鼠标穿透（Ctrl+Alt+L 恢复）");AppendMenuW(menu,MF_STRING|(fps==30?MF_CHECKED:0),4,L"30 FPS");AppendMenuW(menu,MF_STRING|(fps==60?MF_CHECKED:0),5,L"60 FPS");AppendMenuW(menu,MF_STRING,6,L"关闭小窗");SetForegroundWindow(hwnd);int choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,hwnd,nullptr);DestroyMenu(menu);if(choice==1){translation=!translation;layoutDirty=true;}else if(choice==2)setTop(!top);else if(choice==3)setThrough(!through);else if(choice==4)fps=30;else if(choice==5)fps=60;else if(choice==6){DestroyWindow(hwnd);return;}save();update();}
 LRESULT message(UINT msg,WPARAM wp,LPARAM lp){
  switch(msg){
   case WM_CREATE:{auto getWindowDpi=reinterpret_cast<UINT(WINAPI*)(HWND)>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));if(getWindowDpi){UINT value=getWindowDpi(hwnd);if(value)dpi=float(value);}return 0;}
   case WM_PAINT:paint();return 0;case WM_ERASEBKGND:return 1;case WM_MOUSEACTIVATE:return windowMouseActivate(top,through);
   case Incoming:{std::unique_ptr<Json> value(reinterpret_cast<Json*>(lp));if(!closing)receive(*value);return 0;}
   case CoverReady:{std::unique_ptr<CoverData> value(reinterpret_cast<CoverData*>(lp));onCover(std::move(value));return 0;}
   case FrameReady:if(systemMoving&&modalFrames.accepts(wp)&&frameArmed&&nowMs()+1>=frameDeadline){frameArmed=false;update();if(renderVisible)UpdateWindow(hwnd);}return 0;
   case WM_GETMINMAXINFO:{auto*limits=reinterpret_cast<MINMAXINFO*>(lp);limits->ptMinTrackSize.y=LONG(std::ceil(MinWindowHeight*scale()));return 0;}
   case WM_WINDOWPOSCHANGED:{auto result=DefWindowProcW(hwnd,msg,wp,lp);setRenderVisible(IsWindowVisible(hwnd)&&!IsIconic(hwnd));return result;}
   case WM_SIZE:if(wp==SIZE_MINIMIZED){setRenderVisible(false);return 0;}if(wp!=SIZE_MAXHIDE&&wp!=SIZE_MAXSHOW&&factory&&textFactory&&hwnd&&geometry()){update();if(resizing&&renderVisible)UpdateWindow(hwnd);}return 0;
   case WM_DPICHANGED:{dpi=float(HIWORD(wp));if(target)target->SetDpi(dpi,dpi);auto*rect=reinterpret_cast<RECT*>(lp);SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOCOPYBITS);if(resizing){GetCursorPos(&resizeStart);RECT current{};GetWindowRect(hwnd,&current);resizeSize={current.right-current.left,current.bottom-current.top};}if(geometry())update();if(renderVisible)UpdateWindow(hwnd);return 0;}
   case WM_TIMER:if(wp==1||wp==4){update();if(systemMoving&&renderVisible)UpdateWindow(hwnd);}else if(wp==3){cancelSeek();position=seekFallback+(playing&&valid?nowMs()-seekStarted:0);anchor=nowMs();error="seek-timeout";update();}else if(wp==2&&!demo&&nowMs()-received>45000)DestroyWindow(hwnd);return 0;
   case WM_MOUSEWHEEL:{
    if(through||dragProgress||resizing||lines.empty())return 0;POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(hwnd,&p);float x=p.x/scale(),y=p.y/scale();
    if(x<textX||x>width-12||y<viewportTop||y>viewportBottom)return 0;
    wheelRemainder+=GET_WHEEL_DELTA_WPARAM(wp);int steps=wheelRemainder/WHEEL_DELTA;wheelRemainder%=WHEEL_DELTA;if(!steps)return 0;
    int next=browseStep(browseIndex>=0?browseIndex:index,steps);if(next<0)return 0;flight=LyricFlight();browseIndex=next;browseUntil=animationWall()+4000;SetTimer(hwnd,4,4000,nullptr);layoutDirty=true;update();return 0;
   }
   case WM_HOTKEY:if(wp==0x312){setThrough(!through);update();}return 0;
   case WM_MOUSEMOVE:{float x=GET_X_LPARAM(lp)/scale(),y=GET_Y_LPARAM(lp)/scale();if(resizing){POINT point;GetCursorPos(&point);int w=int(clamp(resizeSize.cx+point.x-resizeStart.x,360*scale(),900*scale())),h=int(clamp(resizeSize.cy+point.y-resizeStart.y,MinWindowHeight*scale(),240*scale()));if(w!=geometryPixels.cx||h!=geometryPixels.cy)SetWindowPos(hwnd,nullptr,0,0,w,h,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);}else if(dragProgress){dragRatio=transport.ratio(x);update();}else if(!hover&&!through){hover=true;TRACKMOUSEEVENT event{sizeof(event),TME_LEAVE,hwnd,0};TrackMouseEvent(&event);update();}return 0;}
   case WM_MOUSELEAVE:if(!dragProgress&&!resizing){hover=false;update();}return 0;
   case WM_LBUTTONDOWN:{if(through)return 0;float x=GET_X_LPARAM(lp)/scale(),y=GET_Y_LPARAM(lp)/scale();if(x>width-17&&y>height-17){resizing=true;GetCursorPos(&resizeStart);RECT r;GetWindowRect(hwnd,&r);resizeSize={r.right-r.left,r.bottom-r.top};SetCapture(hwnd);update();return 0;}if(hover&&y<32&&x>width-78){int i=int((x-(width-78))/24);if(i==0){setTop(!top);save();update();}else if(i==1){setThrough(true);update();}else DestroyWindow(hwnd);return 0;}
    if(hover&&y>=transport.y-14){
     switch(transport.hit(x,y)){
      case ControlHit::Seek:if(duration>0){dragProgress=true;dragRatio=transport.ratio(x);SetCapture(hwnd);update();}break;
      case ControlHit::Previous:command("previous");break;case ControlHit::Play:command("play");break;case ControlHit::Next:command("next");break;default:break;
     }return 0;
    }
    if(x<textX||y<32){ReleaseCapture();SendMessageW(hwnd,WM_NCLBUTTONDOWN,HTCAPTION,0);return 0;}
    if(y>=viewportTop&&y<=viewportBottom&&x>=textX&&x<=width-12){for(const auto&item:groups){if(!groupVisible(item.second,motionNow()))continue;float top=item.second.y.value(motionNow());if(y>=top-2&&y<=top+item.second.renderedHeight(item.second.zoom.value(motionNow()))){requestSeek(std::max(0.,item.second.model.time-offset));update();break;}}}return 0;}
   case WM_LBUTTONUP:if(dragProgress){requestSeek(duration*dragRatio);dragProgress=false;ReleaseCapture();update();}if(resizing)ReleaseCapture();return 0;
   case WM_LBUTTONDBLCLK:if(GET_X_LPARAM(lp)/scale()<textX)command("play");return 0;
   case WM_CAPTURECHANGED:{bool sized=resizing;dragProgress=false;resizing=false;POINT p;GetCursorPos(&p);ScreenToClient(hwnd,&p);hover=!through&&p.x>=0&&p.y>=0&&p.x<width*scale()&&p.y<height*scale();update();if(sized)save();return 0;}
   case WM_CONTEXTMENU:{POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};if(p.x==-1)GetCursorPos(&p);menu(p);return 0;}
   case WM_ENTERSIZEMOVE:stopFrames();if(frameTimer)WaitForSingleObject(frameTimer,0);systemMoving=true;modalFrames.start(hwnd,frameTimer,FrameReady);update();return 0;
   case WM_EXITSIZEMOVE:systemMoving=false;stopFrames();modalFrames.stop();update();save();return 0;
   case WM_CLOSE:DestroyWindow(hwnd);return 0;
   case WM_DESTROY:closing=true;systemMoving=false;stopFrames();modalFrames.stop();save();KillTimer(hwnd,1);KillTimer(hwnd,2);KillTimer(hwnd,3);KillTimer(hwnd,4);UnregisterHotKey(hwnd,0x312);if(coverQueue){{std::lock_guard<std::mutex> lock(coverQueue->mutex);coverQueue->stop=true;}coverQueue->changed.notify_one();}PostQuitMessage(0);return 0;
  }return DefWindowProcW(hwnd,msg,wp,lp);
 }
 static LRESULT CALLBACK procedure(HWND window,UINT msg,WPARAM wp,LPARAM lp){App*app=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(msg==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);app->hwnd=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}if(app){try{return app->message(msg,wp,lp);}catch(...){app->error="window-message-failed";}}return DefWindowProcW(window,msg,wp,lp);}
 public:
 App(){settingsPath=env(L"MINILYRICS_SETTINGS");if(settingsPath.empty())settingsPath=env(L"LOCALAPPDATA")+L"\\MiniLyrics\\settings.json";
  try{std::ifstream f(std::filesystem::path(settingsPath),std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(f)),{});if(bytes.size()>65536)throw std::runtime_error("settings size");auto settings=JsonParser(bytes).parse();savedX=settings["X"].n(80);savedY=settings["Y"].n(80);positionInPixels=settings["PositionInPixels"].b();savedWidth=clamp(settings["Width"].n(460),360,900);savedHeight=clamp(settings["Height"].n(130),MinWindowHeight,240);top=settings["Top"].b(true);translation=settings["Translation"].b(true);fps=settings["Fps"].n(30)==60?60:30;readBackground(settings["Background"]);}catch(...){}
 }
 int run(HINSTANCE instance,int port,const std::string&token,const std::wstring&captureFile,bool demoMode){
  capturePath=captureFile;demo=demoMode;check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.put()));check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(textFactory.put())));check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(imaging.put())));
  makeFormat(mainFormat,CurrentFontSize,DWRITE_FONT_WEIGHT_SEMI_BOLD);makeFormat(secondaryFormat,TranslationFontSize,DWRITE_FONT_WEIGHT_NORMAL);makeFormat(metaFormat,11.5,DWRITE_FONT_WEIGHT_NORMAL);mainRowHeight=layout(L"Ag",mainFormat.get()).height;updatePlaceholders();
  check(factory->CreatePathGeometry(playGeometry.put()));Com<ID2D1GeometrySink> sink;check(playGeometry->Open(sink.put()));sink->BeginFigure(D2D1::Point2F(-5,-7),D2D1_FIGURE_BEGIN_FILLED);sink->AddLine(D2D1::Point2F(-5,7));sink->AddLine(D2D1::Point2F(6,0));sink->EndFigure(D2D1_FIGURE_END_CLOSED);check(sink->Close());
  frameTimer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_MODIFY_STATE|SYNCHRONIZE);if(!frameTimer)frameTimer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
  WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.style=CS_DBLCLKS;cls.lpfnWndProc=procedure;cls.hInstance=instance;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.lpszClassName=L"MiniLyrics.Direct2D";RegisterClassExW(&cls);
  auto getSystemDpi=reinterpret_cast<UINT(WINAPI*)()>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForSystem"));if(getSystemDpi)dpi=float(getSystemDpi());
  float positionScale=positionInPixels?1.f:scale();
  hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|(top?WS_EX_TOPMOST|WS_EX_NOACTIVATE:0),cls.lpszClassName,L"MiniLyrics",WS_POPUP,int(std::round(savedX*positionScale)),int(std::round(savedY*positionScale)),int(std::round(savedWidth*scale())),int(std::round(savedHeight*scale())),nullptr,nullptr,instance,this);if(!hwnd)throw std::runtime_error("window creation failed");
  // WM_CREATE has the actual monitor DPI. Correct the initial physical size
  // before showing, keeping saved dimensions in DIP and screen positions in px.
  SetWindowPos(hwnd,nullptr,0,0,int(std::round(savedWidth*scale())),int(std::round(savedHeight*scale())),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
  geometry();startCoverWorker();hotkeyRegistered=RegisterHotKey(hwnd,0x312,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'L');received=nowMs();SetTimer(hwnd,2,5000,nullptr);
  if(!demo)bridge=std::make_unique<Bridge>(port,token,[window=hwnd](Json message){auto data=std::make_unique<Json>(std::move(message));if(PostMessageW(window,Incoming,0,reinterpret_cast<LPARAM>(data.get())))data.release();});
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
 auto awareness=reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext"));if(awareness)awareness(reinterpret_cast<HANDLE>(-4));else SetProcessDPIAware();
 HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa)!=0)return 2;
 int result=1;try{int argc=0;LPWSTR*argv=CommandLineToArgvW(GetCommandLineW(),&argc);int port=0;std::string token;std::wstring capture;bool demo=false;for(int i=1;i<argc;i++){std::wstring arg=argv[i];if(arg==L"--demo")demo=true;else if(i+1<argc&&arg==L"--port")port=_wtoi(argv[++i]);else if(i+1<argc&&arg==L"--token")token=narrow(argv[++i]);else if(i+1<argc&&arg==L"--capture")capture=argv[++i];}LocalFree(argv);if(!demo&&(port<1024||port>65535||token.size()<32))result=2;else{App app;result=app.run(instance,port,token,capture,demo);}}
 catch(const std::exception&error){try{std::ofstream f(std::filesystem::path(env(L"TEMP")+L"\\MiniLyrics-error.log"),std::ios::binary);f<<error.what();}catch(...){}result=1;}
 WSACleanup();if(SUCCEEDED(com))CoUninitialize();return result;
}
