// MiniLyrics lyric motion, based on RNP's float timing. GPL-3.0-or-later.
#pragma once
#include <algorithm>
#include <cmath>
inline double unit(double p){return std::max(0.,std::min(1.,p));}
inline float mixf(float a,float b,double p){return float(a+(b-a)*p);}
// Shared CSS cubic-bezier solver, evaluated by time rather than curve parameter.
inline double cubicBezierEase(double x,double x1,double y1,double x2,double y2){
 if(x<=0)return 0;if(x>=1)return 1;double lo=0,hi=1,t=x;
 for(int i=0;i<18;i++){t=(lo+hi)*.5;double v=3*(1-t)*(1-t)*t*x1+3*(1-t)*t*t*x2+t*t*t;if(v<x)lo=t;else hi=t;}
 return 3*(1-t)*(1-t)*t*y1+3*(1-t)*t*t*y2+t*t*t;
}
inline double lyricEase(double x){return cubicBezierEase(x,.25,.1,.25,1);}
struct FloatWord {float alpha,lift;};
inline FloatWord floatWord(double elapsed,double duration){
 duration=std::max(1.,duration);
 return {float(.4+.6*unit(elapsed/duration)),float(2*lyricEase(elapsed/(duration+150)))};
}
class LyricClock {
 double elapsed=0,anchor=0;bool running=false;
 public:
 double now(double wall)const{return elapsed+(running?std::max(0.,wall-anchor):0);}
 void setRunning(bool run,double wall){elapsed=now(wall);anchor=wall;running=run;}
};
