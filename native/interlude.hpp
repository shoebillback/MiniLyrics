// RNP Interlude timing and CSS breath/visibility rules. GPL-3.0-or-later.
#pragma once
#include "line_motion.hpp"
inline double interludeEase(double x){return cubicBezierEase(x,.42,0,.58,1);}
struct InterludeDot {float alpha=0.2f,scale=0.9f;};
inline InterludeDot interludeDot(double time,double start,double duration,int dot,bool current){
 if(!current)return {};
 double perDot=std::floor(std::max(0.,duration)/3),p=unit((time-start-perDot*dot)/std::max(1.,perDot));
 return {float(.2+.7*p),float(.9+.1*p)};
}
struct InterludeAnimation {
 LyricClock breath;LineTrack opacity;bool current=false;InterludeDot reset[3];double resetAt=-1000;
 void update(bool selected,bool playing,double wall,double time=0,double start=0,double duration=0){
  breath.setRunning(selected&&playing,wall);
  if(selected!=current){if(!selected){for(int i=0;i<3;i++)reset[i]=interludeDot(time,start,duration,i,true);resetAt=wall;}opacity.target(selected?1.f:0.f,wall,selected?500:0,500);current=selected;}
 }
 InterludeDot dot(double time,double start,double duration,int index,double wall)const{
  if(current)return interludeDot(time,start,duration,index,true);
  double p=lyricEase(unit((wall-resetAt)/200));return {mixf(reset[index].alpha,.2f,p),mixf(reset[index].scale,.9f,p)};
 }
 bool visible(double wall)const{return opacity.value(wall)>.001f||opacity.active(wall);}
 float scale(double wall)const{double p=std::fmod(breath.now(wall),2000.)/1000.;return float(1+.1*interludeEase(p<=1?p:2-p));}
};
