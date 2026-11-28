// RNP line position transitions and closed-form spring motion. GPL-3.0-or-later.
#pragma once
#include "lyric_motion.hpp"
inline constexpr int LineCurveSpring=4;
struct SpringParams {double mass=1,stiffness=100,damping=15,epsilon=.01;};
inline void springEvaluate(double x0,double v0,double s,const SpringParams&p,double&outX,double&outV){
 if(s<=0){outX=x0;outV=v0;return;}
 const double m=std::max(.01,p.mass),k=std::max(.01,p.stiffness),c=std::max(.01,p.damping);
 const double g=c/(2.*m),w0Sq=k/m,disc=g*g-w0Sq;
 if(disc<-1e-9){
  const double wd=std::sqrt(-disc),A=x0,B=(v0+g*x0)/wd,e=std::exp(-g*s),cs=std::cos(wd*s),sn=std::sin(wd*s);
  outX=e*(A*cs+B*sn);
  outV=e*((B*wd-A*g)*cs-(A*wd+B*g)*sn);
 }else if(std::abs(disc)<=1e-9){
  const double A=x0,B=v0+g*x0,e=std::exp(-g*s);
  outX=e*(A+B*s);
  outV=e*(B-g*(A+B*s));
 }else{
  const double sq=std::sqrt(disc),r1=-g+sq,r2=-g-sq;
  const double c1=(v0-r2*x0)/(r1-r2),c2=x0-c1,e1=std::exp(r1*s),e2=std::exp(r2*s);
  outX=c1*e1+c2*e2;
  outV=c1*r1*e1+c2*r2*e2;
 }
}
inline double springSettleMs(double x0,double v0,const SpringParams&p){
 const double eps=std::max(1e-6,p.epsilon);
 if(std::abs(x0)<=eps&&std::abs(v0)<=eps*10.)return 0.;
 const double m=std::max(.01,p.mass),k=std::max(.01,p.stiffness),c=std::max(.01,p.damping);
 const double g=c/(2.*m),w0Sq=k/m,disc=g*g-w0Sq;
 if(disc<-1e-9){
  const double wd=std::sqrt(-disc),A=x0,B=(v0+g*x0)/wd;
  const double ampX=std::hypot(A,B),ampV=std::hypot(B*wd-A*g,A*wd+B*g);
  const double ratio=std::max(ampX/eps,ampV/(eps*10.));
  const double t=ratio>1.?std::log(ratio)/g:0.;
  return std::min(3000.,std::max(16.,t*1000.));
 }
 for(double s=.02;s<=3.;s+=.02){
  double ox=0,ov=0,nx=0,nv=0;
  springEvaluate(x0,v0,s,p,ox,ov);
  springEvaluate(x0,v0,s+.02,p,nx,nv);
  if(std::abs(ox)<=eps&&std::abs(ov)<=eps*10.&&std::abs(nx)<=eps&&std::abs(nv)<=eps*10.)return std::max(16.,s*1000.);
 }
 return 3000.;
}
inline double lineEase(double x,int curve){
 if(curve==0)return lyricEase(x);if(x<=0)return 0;if(x>=1)return 1;
 if(curve==LineCurveSpring){
  double ox=0,ov=0,endX=0,endV=0;
  const SpringParams sp{.9,90.,15.,.001};
  springEvaluate(-1.,0.,unit(x)*.85,sp,ox,ov);
  springEvaluate(-1.,0.,.85,sp,endX,endV);
  const double denom=1.+endX;
  return denom>1e-6?(1.+ox)/denom:unit(x);
 }
 double x1=.25,y1=.46,x2=.45,y2=.94;
 if(curve==1){x1=.45;y1=0;x2=.07;y2=1;}
 if(curve==3){x1=.18;y1=.77;x2=.58;y2=.99;}
 return cubicBezierEase(x,x1,y1,x2,y2);
}
inline double rnpDelay(int offset,int direction,bool stagger){
 return stagger?50*(std::max(-4,std::min(4,offset))*direction+4):0;
}
struct LineTrack {
 struct CarrySegment {
  bool valid=false,spring=false;
  float from=0,to=0;double start=0,duration=0,v0=0;int curve=0;SpringParams params{};
 };
 float from=0,to=0;double start=0,duration=0;int curve=0;
 bool spring=false;double v0=0;SpringParams params{};CarrySegment carry{};
 static float evalSegment(bool isSpring,float f,float t,double st,double dur,double vel0,int crv,const SpringParams&sp,double now,double&outVel){
  if(dur<=0||now>=st+dur){outVel=0;return t;}
  if(now<=st){outVel=isSpring?vel0:0;return f;}
  if(isSpring){
   double ox=0,ov=0;
   springEvaluate(double(f)-double(t),vel0,(now-st)/1000.,sp,ox,ov);
   outVel=ov;return float(double(t)+ox);
  }
  const double u=(now-st)/dur,h=.5;
  const float pos=mixf(f,t,lineEase(u,crv));
  const double uNext=std::min(1.,(now+h-st)/dur),uPrev=std::max(0.,(now-h-st)/dur);
  const double dt=std::max(1e-6,(uNext-uPrev)*dur/1000.);
  outVel=(mixf(f,t,lineEase(uNext,crv))-mixf(f,t,lineEase(uPrev,crv)))/dt;
  return pos;
 }
 float value(double now)const{
  double vel=0;
  if(carry.valid&&now<start)return evalSegment(carry.spring,carry.from,carry.to,carry.start,carry.duration,carry.v0,carry.curve,carry.params,now,vel);
  return evalSegment(spring,from,to,start,duration,v0,curve,params,now,vel);
 }
 double velocity(double now)const{
  double vel=0;
  if(carry.valid&&now<start){evalSegment(carry.spring,carry.from,carry.to,carry.start,carry.duration,carry.v0,carry.curve,carry.params,now,vel);return vel;}
  evalSegment(spring,from,to,start,duration,v0,curve,params,now,vel);return vel;
 }
 bool active(double now)const{
  if(carry.valid&&now<start)return true;
  if(spring)return duration>0&&now<start+duration;
  return duration>0&&std::abs(to-from)>.001f&&now<start+duration;
 }
 void snap(float p){from=to=p;duration=0;spring=false;v0=0;carry={};}
 void target(float p,double now,double delay=0,double ms=500,int easing=0){
  if(std::abs(to-p)<.001f)return;
  from=value(now);to=p;start=now+delay;duration=ms;curve=easing;spring=false;v0=0;carry={};
 }
 void targetSpring(float p,double now,double delay,const SpringParams&sp){
  if(spring&&std::abs(to-p)<sp.epsilon*.5&&std::abs(params.stiffness-sp.stiffness)<.01&&std::abs(params.damping-sp.damping)<.01&&std::abs(params.mass-sp.mass)<.01&&(!active(now)||delay<=0))return;
  if(delay>0&&active(now)){
   CarrySegment nextCarry=carry.valid&&now<start?carry:CarrySegment{true,spring,from,to,start,duration,v0,curve,params};
   const double launchAt=now+delay;
   double launchVel=0;
   const float launchPos=evalSegment(nextCarry.spring,nextCarry.from,nextCarry.to,nextCarry.start,nextCarry.duration,nextCarry.v0,nextCarry.curve,nextCarry.params,launchAt,launchVel);
   carry=nextCarry;from=launchPos;v0=launchVel;to=p;start=launchAt;spring=true;curve=LineCurveSpring;params=sp;
   duration=springSettleMs(double(from)-double(to),v0,sp);
   return;
  }
  const double curVel=active(now)?velocity(now):0.;
  const float curVal=value(now);
  carry={};from=curVal;v0=curVel;to=p;start=now+std::max(0.,delay);spring=true;curve=LineCurveSpring;params=sp;
  duration=springSettleMs(double(from)-double(to),v0,sp);
  if(duration<=0){from=to=p;v0=0;}
 }
};
