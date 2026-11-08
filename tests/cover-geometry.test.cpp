#include "../native/cover_geometry.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
bool integral(float value){return std::abs(value-std::round(value))<.001f;}
int main(){int cases=0;
 for(float scale:{1.f,1.25f,1.5f,1.75f,2.f,2.25f})for(int width:{360,460,465,700})for(int height:{110,130,169,170,240})for(int delta:{0,1}){
  int w=int(std::lround(width*scale)),h=int(std::lround(height*scale))+delta;auto g=coverGeometry(w,h,scale);
  assert(integral(g.left*scale));assert(integral(g.top*scale));assert(integral((g.left+g.side)*scale));assert(integral((g.top+g.side)*scale));
  assert(integral(g.radius*scale));assert(g.radius>0&&g.radius<=g.side/2);
  assert(std::abs(g.top*scale-(h-(g.top+g.side)*scale))<.001f);
  assert(g.left>=0&&g.top>=0&&(g.left+g.side)*scale<=w+.001f&&(g.top+g.side)*scale<=h+.001f);++cases;
 }
 auto screenshot=coverGeometry(698,254,1.5f);assert(std::lround(screenshot.top*1.5f)==26);assert(std::lround(screenshot.side*1.5f)==202);
 std::cout<<"PASS: "<<cases+1<<" DPI/size cover geometry cases\n";
}
