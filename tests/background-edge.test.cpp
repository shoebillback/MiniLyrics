// Targeted native regression for compressed hard bands after mesh deformation.
// Uses generated artwork; no player connection or screenshot assets required.
#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <fstream>
#include "../native/background.hpp"

static void require(bool value,const char*message){if(!value)throw std::runtime_error(message);}
static int edgeDelta(const std::vector<uint8_t>&p){
 int maximum=0;
 for(unsigned y=2;y<FluidBackground::height-2;y++)for(unsigned x=2;x<FluidBackground::width-2;x++){
  size_t at=(y*FluidBackground::width+x)*4;
  for(size_t offset:{size_t(4),size_t(FluidBackground::width*4)})for(int k=0;k<3;k++)maximum=std::max(maximum,std::abs(int(p[at+k])-int(p[at-offset+k])));
 }
 return maximum;
}
static double contrast(const std::vector<uint8_t>&p){
 double sum[3]{},square[3]{};int count=0;
 for(unsigned y=2;y<FluidBackground::height-2;y++)for(unsigned x=2;x<FluidBackground::width-2;x++){count++;for(int k=0;k<3;k++){double v=p[(y*FluidBackground::width+x)*4+k];sum[k]+=v;square[k]+=v*v;}}
 double maximum=0;for(int k=0;k<3;k++)maximum=std::max(maximum,std::sqrt(std::max(0.,square[k]/count-sum[k]*sum[k]/count/count)));return maximum;
}
static double motionDifference(const std::vector<uint8_t>&a,const std::vector<uint8_t>&b){
 double total=0;size_t count=0;for(size_t i=0;i<a.size();i++)if(i%4!=3){total+=std::abs(int(a[i])-int(b[i]));count++;}return total/count;
}
int main(int argc,char**argv){try{
 // Broad light/dark patches expose a narrow band when the landscape mesh
 // compresses the blurred texture near the end of its deformation cycle.
 std::vector<uint8_t>art(192*192*4);
 for(int y=0;y<192;y++)for(int x=0;x<192;x++){
  size_t at=(y*192+x)*4;float light=(x>96&&y<110)?220.f:42.f;
  art[at]=uint8_t(light);art[at+1]=uint8_t(light*.9f);art[at+2]=uint8_t(light*.85f);art[at+3]=255;
 }
 FluidBackground background;background.setSize(360,130);background.setCover(art,192,192,0,true);
 BackgroundSettings settings;settings.mode=1;settings.speed=2;settings.fps=15;
 BackgroundClock clock;clock.update(true,settings.speed,0);
 int maximum=0;double minimumContrast=1e9,totalContrast=0,motion=0;int samples=0;std::vector<uint8_t>first;
 for(double wall:{0.,1250.,2500.,3750.,5000.}){
  background.prepare(settings,clock.time(wall),clock.position(wall));
  require(background.gpuReady(),background.gpuError.c_str());maximum=std::max(maximum,edgeDelta(background.pixels));
  if(first.empty())first=background.pixels;
  double fieldContrast=contrast(background.pixels);minimumContrast=std::min(minimumContrast,fieldContrast);totalContrast+=fieldContrast;samples++;motion=std::max(motion,motionDifference(background.pixels,first));
 }
 std::cout<<"Measured: max edge "<<maximum<<", minimum field contrast "<<minimumContrast<<", mean field contrast "<<totalContrast/samples<<", motion difference "<<motion<<"\n";
 require(maximum<=6,"standard/fast/15 FPS must not compress the fixture into a sharp band");
 require(totalContrast/samples>=3,"softening must preserve color-field contrast across the cycle, not just pixel inequality");
 require(motion>=2,"background motion must exceed small quantization changes");
 clock.update(false,settings.speed,5000);auto frozen=background.pixels;auto revision=background.revision;
 require(!background.prepare(settings,clock.time(9000),clock.position(9000))&&background.revision==revision&&background.pixels==frozen,"paused background reuses the same texture");
 background.setSize(540,195);require(background.prepare(settings,clock.time(9000),clock.position(9000))&&background.revision==revision+1,"same-aspect resize must refresh blur for actual dimensions");
 require(background.pixels!=frozen,"larger window must use its own blur conversion");
 background.setSize(540,195);require(!background.prepare(settings,clock.time(9000),clock.position(9000)),"unchanged size must retain the cached texture");
 std::cout<<"PASS: native hard-band regression, visible color fields, motion, pause cache and same-aspect resize\n";
 if(argc>1){std::ofstream report(argv[1]);report<<"{\"generatedCoverMaxDelta\":"<<maximum<<",\"minimumContrast\":"<<minimumContrast<<",\"meanContrast\":"<<totalContrast/samples<<",\"motionDifference\":"<<motion<<",\"gpuReady\":true,\"pausedCache\":true,\"sameAspectResize\":true}";}
 return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
