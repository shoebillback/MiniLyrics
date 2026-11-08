// MiniLyrics shared responsive transport layout. GPL-3.0-or-later.
#pragma once
#include <cmath>
enum class ControlHit {None,Seek,Previous,Play,Next};
struct TransportGeometry {
 float left,right,y,previous,play,next;
 float length()const{return right-left;}
 float ratio(float x)const{return std::fmax(0.f,std::fmin(1.f,(x-left)/length()));}
 ControlHit hit(float x,float py)const{
  if(std::fabs(py-y)>12)return ControlHit::None;
  if(x>=left-4&&x<=right+4)return ControlHit::Seek;
  if(std::fabs(x-previous)<=12)return ControlHit::Previous;
  if(std::fabs(x-play)<=12)return ControlHit::Play;
  if(std::fabs(x-next)<=12)return ControlHit::Next;
  return ControlHit::None;
 }
};
inline TransportGeometry transportGeometry(float width,float height,float textX,float scale){
 auto snap=[scale](float n){return std::round(n*scale)/scale;};
 float next=snap(width-24),play=snap(width-54),previous=snap(width-84);
 return {snap(textX),snap(previous-22),snap(height-18),previous,play,next};
}
