#include "../native/line_motion.hpp"
#include <cassert>
#include <iostream>
int main(){
 int samples=0;
 for(double d:{1.,50.,500.,2000.}){float alpha=.4f,lift=0;for(int i=-10;i<=110;i++){
  auto f=floatWord((d+150)*i/100,d);assert(f.alpha>=.4f&&f.alpha<=1&&f.lift>=0&&f.lift<=2);assert(f.alpha+1e-6>=alpha&&f.lift+1e-6>=lift);alpha=f.alpha;lift=f.lift;++samples;
 }}
 for(int curve=0;curve<4;curve++){double last=0;for(int i=0;i<=500;i++){double y=lineEase(i/500.,curve);assert(y>=last-1e-6&&y>=0&&y<=1);last=y;}}
 assert(std::abs(lineEase(.5,0)-.8024)<.001);
 assert(rnpDelay(-1,1,true)==150&&rnpDelay(0,1,true)==200&&rnpDelay(1,1,true)==250);
 assert(rnpDelay(-1,-1,true)==250&&rnpDelay(1,-1,true)==150);assert(rnpDelay(99,1,true)==400&&rnpDelay(-99,1,true)==0&&rnpDelay(2,1,false)==0);
 LineTrack y;y.snap(100);y.target(0,1000,200,500);assert(y.value(1199)==100&&y.value(1200)==100&&y.value(1700)==0);
 assert(y.value(1450)>0&&y.value(1450)<100);float at=y.value(1450);y.target(-100,1450,150,500);assert(y.value(1450)==at&&y.value(1600)==at&&y.value(2100)==-100);
 LyricClock clock;clock.setRunning(true,1000);assert(clock.now(1125)==125);clock.setRunning(false,1125);assert(clock.now(9999)==125);clock.setRunning(true,9999);assert(clock.now(10124)==250);
 std::cout<<"PASS: "<<samples<<" float samples, 2004 curve samples, RNP delay/retarget/pause checks\n";
}
