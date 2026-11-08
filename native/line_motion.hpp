// RNP line position transitions. GPL-3.0-or-later.
#pragma once
#include "lyric_motion.hpp"
inline double lineEase(double x,int curve){
 if(curve==0)return lyricEase(x);if(x<=0)return 0;if(x>=1)return 1;
 double x1=.25,y1=.46,x2=.45,y2=.94;
 if(curve==1){x1=.45;y1=0;x2=.07;y2=1;}
 if(curve==3){x1=.18;y1=.77;x2=.58;y2=.99;}
 return cubicBezierEase(x,x1,y1,x2,y2);
}
inline double rnpDelay(int offset,int direction,bool stagger){
 return stagger?50*(std::max(-4,std::min(4,offset))*direction+4):0;
}
struct LineTrack {
 float from=0,to=0;double start=0,duration=0;int curve=0;
 float value(double now)const{
  if(duration<=0||now>=start+duration)return to;if(now<=start)return from;
  return mixf(from,to,lineEase((now-start)/duration,curve));
 }
 bool active(double now)const{return duration>0&&std::abs(to-from)>.001f&&now<start+duration;}
 void snap(float p){from=to=p;duration=0;}
 void target(float p,double now,double delay=0,double ms=500,int easing=0){
  if(std::abs(to-p)<.001f)return;from=value(now);to=p;start=now+delay;duration=ms;curve=easing;
 }
};
