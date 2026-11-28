// Two decode cases through the production HTTP/WIC path, without a player.
#include "../native/Host.cpp"
#include <iostream>
static void require(bool value,const char*message){if(!value)throw std::runtime_error(message);}
int main(int argc,char**argv){try{
 require(argc>=3,"expected fixture URL and results directory");
 auto large=downloadCover(widen(std::string(argv[1])+"/large.bmp"),1);
 if(large->pixels.empty())throw std::runtime_error("production large cover decode returned empty; Win32 error "+std::to_string(GetLastError()));
 require(large->status==CoverStatus::Ok&&large->width==512&&large->height==512&&large->pixels.size()==512*512*4,"large artwork retains a 512px cover through production decoding");
 auto small=downloadCover(widen(std::string(argv[1])+"/small.bmp"),2);
 require(small->status==CoverStatus::Ok&&small->width==64&&small->height==64&&small->pixels.size()==64*64*4,"small artwork keeps its native dimensions");
 const auto dir=std::filesystem::path(argv[2]);
 const auto marker=dir/"slow-cover-started";
 const auto base=widen(argv[1]);
 CoverQueue queue;queue.epoch=3;
 std::unique_ptr<CoverData> stale,latest;
 std::thread worker([&]{
  CoverDownloader downloader;
  stale=downloader.fetch(base+L"/slow.bmp",3,&queue);
  latest=downloader.fetch(base+L"/small.bmp",4,&queue);
 });
 auto start=nowMs();
 while(!std::filesystem::exists(marker)&&nowMs()-start<2500)std::this_thread::sleep_for(std::chrono::milliseconds(10));
 {std::lock_guard<std::mutex> lock(queue.mutex);queue.epoch=4;}
 worker.join();
 require(std::filesystem::exists(marker),"the superseded cover request reached the loopback server");
 require(stale&&stale->status==CoverStatus::Cancelled&&stale->pixels.empty()&&stale->epoch==3,"superseded response is discarded");
 require(latest&&latest->status==CoverStatus::Ok&&latest->width==64&&latest->epoch==4,"worker can decode the next cover after cancelling the old request");
 require(downloadCover(L"",5)->status==CoverStatus::Empty,"empty URL returns Empty status");
 require(downloadCover(L"ftp://example.com/bad.png",6)->status==CoverStatus::PermanentError,"non-HTTP URL is a permanent failure");
 require(downloadCover(base+L"/missing.bmp",7)->status==CoverStatus::PermanentError,"HTTP 404 is a permanent failure");
 require(downloadCover(base+L"/corrupt.bmp",8)->status==CoverStatus::PermanentError,"corrupt image payload is a permanent failure");
 require(downloadCover(base+L"/transient.bmp",9)->status==CoverStatus::TransientError,"HTTP 503 is a transient failure");
 FluidBackground background;background.setCover(large->pixels,large->width,large->height,0,true);BackgroundSettings settings;
 background.prepare(settings,0,0);require(background.gpuReady(),background.gpuError.c_str());
 require(background.pixels.size()==192*72*4,"higher resolution cover keeps the existing background output size");
 std::ofstream report(std::filesystem::path(argv[2])/"cover-resolution-results.json");
 report<<"{\"largeDecoded\":[512,512],\"smallDecoded\":[64,64],\"backgroundOutput\":[192,72],\"gpuReady\":true}";
 std::cout<<"PASS: cover decoding, superseded HTTP request and independent GPU background size\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
