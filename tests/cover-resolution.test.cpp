// Two decode cases through the production HTTP/WIC path, without a player.
#include "../native/Host.cpp"
#include <iostream>
static void require(bool value,const char*message){if(!value)throw std::runtime_error(message);}
int main(int argc,char**argv){try{
 require(argc>=3,"expected fixture URL and results directory");
 auto large=downloadCover(widen(std::string(argv[1])+"/large.bmp"),1);
 if(large->pixels.empty())throw std::runtime_error("production large cover decode returned empty; Win32 error "+std::to_string(GetLastError()));
 require(large->width==512&&large->height==512&&large->pixels.size()==512*512*4,"large artwork retains a 512px cover through production decoding");
 auto small=downloadCover(widen(std::string(argv[1])+"/small.bmp"),2);
 require(small->width==64&&small->height==64&&small->pixels.size()==64*64*4,"small artwork keeps its native dimensions");
 FluidBackground background;background.setCover(large->pixels,large->width,large->height,0,true);BackgroundSettings settings;
 background.prepare(settings,0,0);require(background.gpuReady(),background.gpuError.c_str());
 require(background.pixels.size()==192*72*4,"higher resolution cover keeps the existing background output size");
 std::ofstream report(std::filesystem::path(argv[2])/"cover-resolution-results.json");
 report<<"{\"largeDecoded\":[512,512],\"smallDecoded\":[64,64],\"backgroundOutput\":[192,72],\"gpuReady\":true}";
 std::cout<<"PASS: production large/small cover decoding and independent GPU background size\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
