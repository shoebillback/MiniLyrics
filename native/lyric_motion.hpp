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
inline constexpr double LongNoteFadeOut=500;
inline constexpr double LongNoteTailMaxGap=4000;
inline constexpr double LongNoteTailRelease=250;
inline constexpr double LongNoteShortGap=LongNoteFadeOut-LongNoteTailRelease;
inline constexpr double LongNoteMinTailDuration=380;
inline constexpr double LongNoteFullDuration=1000;
inline constexpr float LongNoteTailEmber=0.12f;
inline double glowAttackEase(double x){return cubicBezierEase(x,0.,0.,.58,1.);}
inline float longNoteStrength(double duration){
 if(duration<LongNoteMinTailDuration)return 0.f;
 if(duration>=LongNoteFullDuration)return 1.f;
 if(duration<650.)return float(.30+.25*unit((duration-LongNoteMinTailDuration)/(650.-LongNoteMinTailDuration)));
 return float(.55+.45*unit((duration-650.)/(LongNoteFullDuration-650.)));
}
inline float longNoteGlow(double elapsed,double duration){
 const float peak=longNoteStrength(duration);
 if(peak<=0.f||elapsed<=0.)return 0.f;
 if(elapsed<duration)return peak*float(glowAttackEase(unit(elapsed/std::max(1.,duration*.35))));
 return peak*float(1.-unit((elapsed-duration)/LongNoteFadeOut));
}
inline float longNoteGlow(double elapsed,double duration,double tailGap){
 if(elapsed<duration||tailGap<=LongNoteShortGap||tailGap>=LongNoteTailMaxGap)return longNoteGlow(elapsed,duration);
 const float peak=longNoteStrength(duration);
 const double after=elapsed-duration;
 const float handoff=after<tailGap
  ?float(1.-(1.-LongNoteTailEmber)*glowAttackEase(unit(after/tailGap)))
  :LongNoteTailEmber*float(1.-unit((after-tailGap)/LongNoteTailRelease));
 // Blend into the longer handoff without an abrupt change at the short-gap boundary.
 const double weight=unit((tailGap-LongNoteShortGap)/(LongNoteFadeOut-LongNoteShortGap));
 return peak*mixf(float(1.-unit(after/LongNoteFadeOut)),handoff,weight);
}
inline double longNoteGlowEnd(double duration,double tailGap){
 return duration+(tailGap>LongNoteShortGap&&tailGap<LongNoteTailMaxGap?tailGap+LongNoteTailRelease:LongNoteFadeOut);
}
inline double emphasisBell(double x){
 if(x<=0.||x>=1.)return 0.;
 if(x<.5)return cubicBezierEase(x*2.,.2,.4,.58,1.);
 return 1.-cubicBezierEase((x-.5)*2.,.3,0.,.58,1.);
}
inline float emphasisScale(double elapsed,double duration,bool isEmphasize,bool isLongNote,bool isLineTail){
 if(duration<LongNoteFullDuration||elapsed<=0.||elapsed>=duration)return 1.f;
 if(!isEmphasize&&!isLongNote)return 1.f;
 double amount=duration/2000.;
 amount=amount>1.?std::sqrt(amount):amount*amount*amount;
 amount*=.6;
 if(isLineTail)amount*=1.35;
 amount=std::min(1.,amount);
 const double maxBoost=isEmphasize?.075:.05;
 const double boost=maxBoost*(.35+.65*amount);
 return float(1.+emphasisBell(elapsed/duration)*boost);
}
inline float emphasisGlow(double elapsed,double duration,bool isLineTail){
 if(duration<LongNoteFullDuration||elapsed<=0.||elapsed>=duration)return 0.f;
 double blur=duration/3000.;
 blur=blur>1.?std::sqrt(blur):blur*blur*blur;
 blur=std::min(.85,.35+blur*(isLineTail?.75:.5));
 return float(emphasisBell(elapsed/duration)*blur);
}
class LyricClock {
 double elapsed=0,anchor=0;bool running=false;
 public:
 double now(double wall)const{return elapsed+(running?std::max(0.,wall-anchor):0);}
 void setRunning(bool run,double wall){elapsed=now(wall);anchor=wall;running=run;}
};
