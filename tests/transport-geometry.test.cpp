#include "../native/cover_geometry.hpp"
#include "../native/transport_geometry.hpp"
#include <cassert>
#include <iostream>
int main(){
 int count=0;
 for(float scale:{1.f,1.25f,1.5f,1.75f,2.f,2.25f})
 for(int w:{360,460,546,900})for(int h:{110,130,164,240}){
  int wp=int(std::round(w*scale)),hp=int(std::round(h*scale));
  auto art=coverGeometry(wp,hp,scale);
  auto t=transportGeometry(wp/scale,hp/scale,art.left+art.side+16,scale);
  assert(t.length()>60);assert(t.right+4<t.previous-12);
  assert(t.previous+12<t.play-12&&t.play+12<t.next-12);
  assert(t.next+12<wp/scale&&t.y+12<hp/scale);
  assert(t.hit(t.left,t.y)==ControlHit::Seek);assert(t.hit(t.right,t.y)==ControlHit::Seek);
  assert(t.hit(t.previous,t.y)==ControlHit::Previous);
  assert(t.hit(t.play,t.y)==ControlHit::Play);assert(t.hit(t.next,t.y)==ControlHit::Next);
  assert(t.hit(t.play,t.y-14)==ControlHit::None);
  assert(t.ratio(t.left-100)==0&&t.ratio(t.right+100)==1);
  assert(std::fabs(t.ratio(t.left+t.length()*.7f)-.7f)<.0001f);
  for(float x:{t.left,t.right,t.y,t.previous,t.play,t.next})assert(std::fabs(x*scale-std::round(x*scale))<.002f);
  auto wide=transportGeometry(wp/scale+100,hp/scale,art.left+art.side+16,scale);
  assert(std::fabs((wide.length()-t.length())-100)<1.f/scale+.01f);++count;
 }
 std::cout<<"PASS: "<<count<<" responsive layout and hit-test cases\n";
}
