// MiniLyrics pixel-aligned album bounds. GPL-3.0-or-later.
#pragma once
#include <algorithm>
#include <cmath>
struct CoverGeometry {float left,top,side,radius;};
inline CoverGeometry coverGeometry(int clientWidthPx,int clientHeightPx,float scale){
 scale=std::max(.25f,scale);
 float logicalWidth=clientWidthPx/scale,logicalHeight=clientHeightPx/scale;
 float desired=std::max(72.f,std::min(160.f,std::min(logicalHeight-34,logicalWidth-270)));
 int side=std::max(1,int(std::lround(desired*scale)));
 if((side&1)!=(clientHeightPx&1))--side;
 side=std::max(1,std::min(side,clientHeightPx));
 int left=int(std::lround(12*scale)),top=(clientHeightPx-side)/2;
 int radius=std::max(1,std::min(side/2,int(std::lround(10*scale))));
 return {left/scale,top/scale,side/scale,radius/scale};
}
