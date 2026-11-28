// RNP Interlude timing and CSS breath/visibility rules. GPL-3.0-or-later.
#pragma once
#include "line_motion.hpp"
inline double interludeEase(double x){return cubicBezierEase(x,.42,0,.58,1);}
inline double easeInOutBack(double x){
 x=unit(x);
 constexpr double c1=1.70158,c2=c1*1.525;
 return x<.5
  ?(2*x)*(2*x)*((c2+1)*2*x-c2)*.5
  :((2*x-2)*(2*x-2)*((c2+1)*(x*2-2)+c2)+2)*.5;
}
struct InterludeDot {float alpha=0.2f,scale=0.9f;};
inline InterludeDot interludeDot(double time,double start,double duration,int dot,bool current){
 if(!current)return {};
 double perDot=std::floor(std::max(0.,duration)/3),p=unit((time-start-perDot*dot)/std::max(1.,perDot));
 return {float(.2+.7*p),float(.9+.1*p)};
}
struct InterludeAnimation {
 LyricClock breath;LineTrack opacity;bool current=false;InterludeDot reset[3];double resetAt=-1000;
 float exitScaleHold=1.f,exitAlphaHold=1.f;
 static float exitScaleFor(double time,double start,double duration){
  if(duration<1500.)return 1.f;
  const double remaining=(start+duration)-time;
  if(remaining>=750.)return 1.f;
  const double p=unit((750.-remaining)/750.);
  return float(std::max(0.,1.-easeInOutBack(p*.5)));
 }
 static float exitAlphaFor(double time,double start,double duration){
  if(duration<1500.)return 1.f;
  const double remaining=(start+duration)-time;
  if(remaining>=375.)return 1.f;
  return float(unit(remaining/375.));
 }
 void update(bool selected,bool playing,double wall,double time=0,double start=0,double duration=0){
  breath.setRunning(selected&&playing,wall);
  if(selected!=current){
   if(!selected){
    for(int i=0;i<3;i++)reset[i]=interludeDot(time,start,duration,i,true);
    resetAt=wall;
    exitScaleHold=exitScaleFor(time,start,duration);
    exitAlphaHold=exitAlphaFor(time,start,duration);
   }else{
    exitScaleHold=1.f;exitAlphaHold=1.f;
   }
   opacity.target(selected?1.f:0.f,wall,selected?500:0,500);current=selected;
  }
 }
 InterludeDot dot(double time,double start,double duration,int index,double wall)const{
  if(current)return interludeDot(time,start,duration,index,true);
  double p=lyricEase(unit((wall-resetAt)/200));return {mixf(reset[index].alpha,.2f,p),mixf(reset[index].scale,.9f,p)};
 }
 bool visible(double wall)const{return opacity.value(wall)>.001f||opacity.active(wall);}
 bool visible(double wall,double time,double start,double duration)const{
  return visible(wall)&&exitAlpha(time,start,duration)>.001f;
 }
 float scale(double wall)const{double p=std::fmod(breath.now(wall),2000.)/1000.;return float(1+.1*interludeEase(p<=1?p:2-p));}
 float exitScale(double time,double start,double duration)const{return current?exitScaleFor(time,start,duration):exitScaleHold;}
 float exitAlpha(double time,double start,double duration)const{return current?exitAlphaFor(time,start,duration):exitAlphaHold;}
};
