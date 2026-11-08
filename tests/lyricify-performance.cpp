// Focused native pipeline benchmark. No visible window or player connection.
#define NOMINMAX
#include <windows.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <numeric>
#include "../native/background.hpp"
static double wall(){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static double cpu(){FILETIME c,e,k,u;GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);ULARGE_INTEGER a,b;a.LowPart=k.dwLowDateTime;a.HighPart=k.dwHighDateTime;b.LowPart=u.dwLowDateTime;b.HighPart=u.dwHighDateTime;return (a.QuadPart+b.QuadPart)/10000.;}
template<class F>void sample(const char*label,F draw){
 for(int i=0;i<5;i++)draw(i*1000./15);
 std::vector<double>times;double start=wall(),before=cpu();
 for(int i=0;i<60;i++){double remaining=start+i*1000./15-wall();if(remaining>0)std::this_thread::sleep_for(std::chrono::duration<double,std::milli>(remaining));double t=wall();draw(1000+i*1000./15);times.push_back(wall()-t);}
 double remaining=start+4000-wall();if(remaining>0)std::this_thread::sleep_for(std::chrono::duration<double,std::milli>(remaining));
 double used=cpu()-before,elapsed=wall()-start;std::sort(times.begin(),times.end());
 std::cout<<"{\"label\":\""<<label<<"\",\"frames\":60,\"elapsedMs\":"<<elapsed<<",\"cpuOneCorePercent\":"<<used/elapsed*100<<",\"frameWallMeanMs\":"<<std::accumulate(times.begin(),times.end(),0.)/60<<",\"frameWallP95Ms\":"<<times[57]<<"}\n";
}
int main(){try{
 BackgroundSettings s;s.mode=1;FluidBackground f;std::vector<uint8_t>art(192*192*4);for(int y=0;y<192;y++)for(int x=0;x<192;x++){size_t i=(y*192+x)*4;art[i]=40+x;art[i+1]=20+y;art[i+2]=230-x/2;art[i+3]=255;}f.setCover(art,192,192,0,true);
 double init=wall();f.prepare(s,0,0);std::cout<<"{\"initializationMs\":"<<wall()-init<<",\"gpuReady\":"<<(f.gpuReady()?"true":"false")<<"}\n";if(!f.gpuReady()){std::cerr<<f.gpuError<<"\n";return 1;}
 sample("lyricify-d3d11-with-readback",[&](double t){f.prepare(s,t,t);});
 ColorFieldFallback old;old.setPalette(coverBackgroundPalette(art,192,192));sample("previous-native-cpu-color-field",[&](double t){old.prepare(s,t,t);});
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
