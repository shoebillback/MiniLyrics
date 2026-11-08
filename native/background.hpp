// Native Lyricify-style D3D11 pipeline; bounded CPU color-field fallback.
// GPL-3.0-or-later.
#pragma once
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
struct BackgroundSettings {
 int mode=1;bool animated=true;double speed=1;int fps=30;
 bool operator==(const BackgroundSettings&o)const{return mode==o.mode&&animated==o.animated&&speed==o.speed&&fps==o.fps;}
};

inline int backgroundFrameRate(int value){switch(value){case 10:case 15:case 30:case 45:case 60:return value;default:return 30;}}

struct BackgroundClock {
 double elapsed=0,phase=0,anchor=0,speed=1;bool running=false;
 double time(double wall)const{return elapsed+(running?std::max(0.,wall-anchor):0);}
 double position(double wall)const{return phase+(running?std::max(0.,wall-anchor)*speed:0);}
 void update(bool run,double rate,double wall){elapsed=time(wall);phase=position(wall);anchor=wall;running=run;speed=rate;}
};
struct BackgroundColor {double r,g,b;};
using BackgroundPalette=std::array<BackgroundColor,4>;
inline BackgroundPalette defaultBackgroundPalette(){return {{{.06,.12,.25},{.20,.35,.46},{.65,.37,.35},{.84,.59,.40}}};}
inline BackgroundPalette coverBackgroundPalette(const std::vector<uint8_t>&pixels,unsigned width,unsigned height){
 auto out=defaultBackgroundPalette();if(!width||!height||uint64_t(width)*height>pixels.size()/4)return out;
 double sums[4][4]{};
 for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++){
  unsigned q=(y>=height/2?2:0)+(x>=width/2?1:0);size_t p=(size_t(y)*width+x)*4;
  sums[q][0]+=pixels[p+2];sums[q][1]+=pixels[p+1];sums[q][2]+=pixels[p];sums[q][3]+=pixels[p+3];
 }
 for(int q=0;q<4;q++)if(sums[q][3]>0){
  BackgroundColor c{sums[q][0]/sums[q][3],sums[q][1]/sums[q][3],sums[q][2]/sums[q][3]};
  double gray=.2126*c.r+.7152*c.g+.0722*c.b;
  c={gray+(c.r-gray)*1.25,gray+(c.g-gray)*1.25,gray+(c.b-gray)*1.25};
  auto bound=[](double n){return std::max(.04,std::min(.9,n));};out[q]={bound(c.r),bound(c.g),bound(c.b)};
 }
 return out;
}
class ColorFieldFallback {
 static BackgroundColor mix(BackgroundColor a,BackgroundColor b,double p){p=std::max(0.,std::min(1.,p));return {a.r+(b.r-a.r)*p,a.g+(b.g-a.g)*p,a.b+(b.b-a.b)*p};}
 static double wave(double x,double y,double t){return .48*std::sin(x*3.2+y*1.7+t)+.29*std::sin(x*5.8-y*2.6-t*.7)+.23*std::cos(x*2.1-y*4.2+t*.36);}
 BackgroundPalette palette=defaultBackgroundPalette();bool dirty=true;int lastMode=-1,lastFps=0;int64_t lastBucket=-1;
 public:
 static constexpr unsigned width=192,height=72;
 std::vector<uint8_t>pixels;uint64_t revision=0;
 void invalidate(){dirty=true;}
 void setPalette(const BackgroundPalette&colors){palette=colors;dirty=true;}
 bool prepare(const BackgroundSettings&s,double elapsed,double position){
  if(s.mode<=0||s.mode>3)return false;
  int64_t bucket=int64_t(std::floor((std::max(0.,elapsed)+.000001)*s.fps/1000));
  if(!dirty&&lastMode==s.mode&&lastFps==s.fps&&lastBucket==bucket)return false;
  dirty=false;lastMode=s.mode;lastFps=s.fps;lastBucket=bucket;pixels.resize(width*height*4);
  double phase=1.3+std::max(0.,position)/1000*.12;
  double sx[4]={-.8+.25*std::sin(phase*.7),.7+.18*std::cos(phase),-.1+.3*std::cos(phase*.5),.9+.2*std::sin(phase)};
  double sy[4]={-.5,.5,-.5+.3*std::sin(phase),-.5+.2*std::cos(phase)};
  for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++){
   double u=double(x)/(width-1),v=double(y)/(height-1),px=u*2-1,py=(v*2-1)*.6;
   double wx=px+.30*wave(px+1,py,phase),wy=py+.24*wave(py-1,px,phase+.7);BackgroundColor c{};
   if(s.mode==2){
    double a=.5+.5*std::sin(wx*3.4+wy*3.6+wave(px,py,phase)*1.4),b=.5+.5*std::cos(wx*2.3-wy*4.1-phase*.35);
    c=mix(palette[0],palette[1],a*.9);c=mix(c,palette[2],b*b*.8);c=mix(c,palette[3],a*a*b*b*.65);
   }else{
    double total=0;for(int k=0;k<4;k++){double dx=wx-sx[k],dy=wy-sy[k],w=std::exp(-(dx*dx*1.8+dy*dy*1.3)/(k==2?.65:1.));total+=w;c.r+=palette[k].r*w;c.g+=palette[k].g*w;c.b+=palette[k].b*w;}
    c={c.r/total,c.g/total,c.b/total};
   }
   double dim=s.mode==3?.48:.74;if(s.mode==1)dim*=1-.12*u;if(s.mode==2)dim*=1-.20*u;c=mix({.035,.05,.09},c,dim);
   auto channel=[](double n){return uint8_t(std::max(0.,std::min(255.,n*255)));};size_t p=(y*width+x)*4;
   pixels[p]=channel(c.b);pixels[p+1]=channel(c.g);pixels[p+2]=channel(c.r);pixels[p+3]=255;
  }
  ++revision;return true;
 }
};

#include "lyricify_gpu.hpp"
class FluidBackground {
 friend struct NativeTest;
 std::unique_ptr<lyricify::Renderer>gpu;
 ColorFieldFallback fallback;
 std::vector<uint8_t>current,previous;
 bool dirty=true,upload=true,failed=false;int lastMode=-1,lastFps=0;int64_t lastBucket=-1;
 double transitionAt=-1000;float viewWidth=460,viewHeight=130;
 float blend(double elapsed)const{return float(std::max(0.,std::min(1.,(elapsed-transitionAt)/500)));}
 void defaultArtwork(const BackgroundPalette&p){current.resize(192*192*4);for(int y=0;y<192;y++)for(int x=0;x<192;x++){float u=x/191.f,v=y/191.f;int k=(y>=96?2:0)+(x>=96?1:0);auto a=p[k];size_t i=(y*192+x)*4;current[i]=uint8_t(a.b*255);current[i+1]=uint8_t(a.g*255);current[i+2]=uint8_t(a.r*255);current[i+3]=255;}previous=current;upload=true;}
 public:
 static constexpr unsigned width=192,height=72;
 std::vector<uint8_t>pixels;uint64_t revision=0;std::string gpuError;
 FluidBackground(){defaultArtwork(defaultBackgroundPalette());}
 const char*backend()const{return failed?"cpu-color-field-fallback":"d3d11-lyricify";}
 bool gpuReady()const{return bool(gpu)&&!failed;}
 void invalidate(){dirty=true;fallback.invalidate();}
 void setSize(float w,float h){if(!std::isfinite(w)||!std::isfinite(h)||w<=0||h<=0)return;if(std::abs(w-viewWidth)>.001f||std::abs(h-viewHeight)>.001f){viewWidth=w;viewHeight=h;invalidate();}}
 void setPalette(const BackgroundPalette&p){fallback.setPalette(p);defaultArtwork(p);invalidate();}
 void setCover(const std::vector<uint8_t>&p,unsigned w,unsigned h,double elapsed,bool immediate=false){
  float f=blend(elapsed);if(previous.size()==current.size())for(size_t i=0;i<previous.size();i++)previous[i]=uint8_t(previous[i]+(current[i]-previous[i])*f);else previous=current;
  if(!w||!h||uint64_t(w)*h>p.size()/4){auto source=std::move(previous);defaultArtwork(defaultBackgroundPalette());previous=std::move(source);}
  else{current.resize(192*192*4);unsigned side=std::min(w,h),left=(w-side)/2,top=(h-side)/2;for(unsigned y=0;y<192;y++)for(unsigned x=0;x<192;x++){unsigned sx=left+std::min(side-1,x*side/192),sy=top+std::min(side-1,y*side/192);size_t src=(size_t(sy)*w+sx)*4,dst=(y*192+x)*4;float alpha=p[src+3]/255.f;for(int k=0;k<3;k++)current[dst+k]=uint8_t(std::min(255.f,p[src+k]+(1-alpha)*24));current[dst+3]=255;}}
  transitionAt=immediate?elapsed-500:elapsed;if(immediate)previous=current;
  fallback.setPalette(coverBackgroundPalette(p,w,h));upload=true;invalidate();
 }
 bool prepare(const BackgroundSettings&s,double elapsed,double position){
  if(s.mode<=0||s.mode>3)return false;int64_t bucket=int64_t(std::floor((std::max(0.,elapsed)+.000001)*s.fps/1000));
  if(!dirty&&lastMode==s.mode&&lastFps==s.fps&&lastBucket==bucket)return false;
  if(!failed)try{if(!gpu)gpu=std::make_unique<lyricify::Renderer>();if(upload){gpu->artwork(current,previous);upload=false;}gpu->draw(std::max(0.,position)/1000,blend(elapsed),s.mode,viewWidth,viewHeight,pixels);}
  catch(const std::exception&e){failed=true;gpuError=e.what();gpu.reset();}
  if(failed){fallback.prepare(s,elapsed,position);pixels=fallback.pixels;}
  dirty=false;lastMode=s.mode;lastFps=s.fps;lastBucket=bucket;++revision;return true;
 }
};
