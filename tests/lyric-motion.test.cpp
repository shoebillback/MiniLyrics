#include "../native/interlude.hpp"
#include <cassert>
#include <iostream>
int main(){
 int samples=0;
 for(double d:{1.,50.,500.,2000.}){float alpha=.4f,lift=0;for(int i=-10;i<=110;i++){
  auto f=floatWord((d+150)*i/100,d);assert(f.alpha>=.4f&&f.alpha<=1&&f.lift>=0&&f.lift<=2);assert(f.alpha+1e-6>=alpha&&f.lift+1e-6>=lift);alpha=f.alpha;lift=f.lift;++samples;
 }}
 for(int curve=0;curve<=LineCurveSpring;curve++){double last=0;for(int i=0;i<=500;i++){double y=lineEase(i/500.,curve);if(curve<LineCurveSpring)assert(y>=last-1e-6&&y>=0&&y<=1);last=y;}}
 assert(lineEase(0,LineCurveSpring)==0&&std::abs(lineEase(1,LineCurveSpring)-1)<1e-6);
 assert(std::abs(lineEase(.5,0)-.8024)<.001);
 assert(longNoteGlow(100,379)==0&&longNoteGlow(-1,1000)==0&&longNoteGlow(0,1000)==0);
 assert(std::abs(longNoteStrength(380)-.30f)<1e-4f&&std::abs(longNoteStrength(650)-.55f)<1e-4f&&longNoteStrength(1000)==1.f);
 assert(longNoteGlow(175,1000)>.6f&&longNoteGlow(175,1000)<.8f&&longNoteGlow(350,1000)==1.f&&longNoteGlow(1000,1000)==1.f);
 assert(longNoteGlow(1250,1000)==.5f&&longNoteGlow(1500,1000)==0);
 // Long-note tail handoff across <4s vocal gap, CJK short tail tiering, and fallback for >=4s or 0 gap.
 assert(longNoteGlow(1000,1000,2000)==1.f);
 assert(longNoteGlow(2000,1000,2000)>.12f&&longNoteGlow(2000,1000,2000)<1.f);
 assert(std::abs(longNoteGlow(3000,1000,2000)-.12f)<1e-4f);
 assert(std::abs(longNoteGlow(3125,1000,2000)-.06f)<1e-4f);
 assert(longNoteGlow(3250,1000,2000)==0.f);
 assert(longNoteGlow(1250,1000,4000)==.5f&&longNoteGlow(1500,1000,4000)==0.f);
 assert(std::abs(longNoteGlow(1050,1000,50)-.9f)<1e-4f);
 for(double gap:{0.,1.,50.,250.}){
  assert(longNoteGlow(1250,1000,gap)==.5f&&longNoteGlow(1500,1000,gap)==0.f);
  assert(longNoteGlowEnd(1000,gap)==1500.);
 }
 assert(std::abs(longNoteGlow(1250,1000,250.1)-longNoteGlow(1250,1000,250.))<.001f);
 const float s500=longNoteStrength(500);
 assert(s500>.35f&&s500<.5f&&std::abs(longNoteGlow(175,500,2000)-s500)<1e-4f);
 assert(std::abs(longNoteGlow(2500,500,2000)-s500*.12f)<1e-4f&&longNoteGlow(2750,500,2000)==0.f);
 assert(longNoteGlowEnd(1000,2000)==3250.&&longNoteGlowEnd(1000,4000)==1500.);
 // Whole-word emphasis bell curve, swell scale, and bell glow.
 assert(emphasisBell(0)==0&&emphasisBell(1)==0&&emphasisBell(.5)>.99);
 assert(emphasisScale(0,2000,true,false,false)==1.f&&emphasisScale(2000,2000,true,false,false)==1.f);
 assert(emphasisScale(1000,999,true,false,false)==1.f);
 const float midEmp=emphasisScale(1000,2000,true,false,false),tailEmp=emphasisScale(1000,2000,true,true,true);
 assert(midEmp>1.03f&&tailEmp>=midEmp&&tailEmp<=1.08f);
 assert(emphasisGlow(0,2000,false)==0.f&&emphasisGlow(1000,2000,false)>.35f&&emphasisGlow(2000,2000,false)==0.f);
 assert(rnpDelay(-1,1,true)==150&&rnpDelay(0,1,true)==200&&rnpDelay(1,1,true)==250);
 assert(rnpDelay(-1,-1,true)==250&&rnpDelay(1,-1,true)==150);assert(rnpDelay(99,1,true)==400&&rnpDelay(-99,1,true)==0&&rnpDelay(2,1,false)==0);
 LineTrack y;y.snap(100);y.target(0,1000,200,500);assert(y.value(1199)==100&&y.value(1200)==100&&y.value(1700)==0);
 assert(y.value(1450)>0&&y.value(1450)<100);float at=y.value(1450);y.target(-100,1450,150,500);assert(y.value(1450)==at&&y.value(1600)==at&&y.value(2100)==-100);
 // Closed-form spring: convergence, velocity inheritance on retarget, and staggered carry.
 LineTrack sp;sp.snap(100);sp.targetSpring(0,1000,0,{.9,90.,15.,.02});
 assert(sp.active(1000)&&sp.duration>0&&sp.duration<=3000);
 double midPos=sp.value(1150),midVel=sp.velocity(1150);
 assert(midPos<100&&midVel<-10.);
 sp.targetSpring(-80,1150,0,{.9,90.,15.,.02});
 assert(std::abs(sp.value(1150)-midPos)<.01&&std::abs(sp.velocity(1150)-midVel)<1e-3);
 assert(!sp.active(1000+sp.duration+200)&&sp.value(1000+sp.duration+200)==-80.f);
 LineTrack carried;carried.snap(0);carried.targetSpring(100,1000,0,{.9,72.,15.,.02});
 float cPos=carried.value(1120);double cVel=carried.velocity(1120);
 carried.targetSpring(200,1120,100,{.9,72.,15.,.02});
 assert(std::abs(carried.value(1120)-cPos)<.01&&std::abs(carried.velocity(1120)-cVel)<1e-3);
 assert(carried.value(1170)>cPos&&!carried.active(1120+carried.duration+200)&&carried.value(1120+carried.duration+200)==200.f);
 // Interlude exit anticipation and alpha fade.
 InterludeAnimation ia;ia.update(true,true,0,0,0,6000);
 assert(ia.exitScale(5250,0,6000)==1.f&&ia.exitAlpha(5250,0,6000)==1.f);
 assert(ia.exitScale(5550,0,6000)>1.f&&ia.exitScale(5950,0,6000)<1.f);
 assert(ia.exitAlpha(5812.5,0,6000)>0.f&&ia.exitAlpha(5812.5,0,6000)<1.f&&ia.exitAlpha(6000,0,6000)==0.f);
 assert(ia.exitScale(900,0,1000)==1.f&&ia.exitAlpha(900,0,1000)==1.f);
 assert(ia.visible(1000,1500,0,2000)&&ia.exitAlpha(2000,0,2000)==0.f&&!ia.visible(1000,3000,0,2000));
 sp.target(sp.to,1200,0,500,0);
 assert(sp.spring&&sp.active(1200));
 LyricClock clock;clock.setRunning(true,1000);assert(clock.now(1125)==125);clock.setRunning(false,1125);assert(clock.now(9999)==125);clock.setRunning(true,9999);assert(clock.now(10124)==250);
 std::cout<<"PASS: "<<samples<<" float samples, 2505 curve samples, spring/tail-handoff/interlude-exit/RNP checks\n";
}
