// MiniLyrics native D3D11 adapter for Lyricify Backgrounds (Apache-2.0 shader/mesh).
// Adapter Copyright (C) 2026 Yushun Wang, GPL-3.0-or-later.
#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstring>
#include <stdexcept>
#include <string>
#include <memory>
#include "lyricify_shader.hpp"
struct BackgroundPoint {
 float x=0,y=0;
 BackgroundPoint operator+(BackgroundPoint p)const{return {x+p.x,y+p.y};}
 BackgroundPoint operator*(float v)const{return {x*v,y*v};}
};
#include "lyricify_presets.hpp"
namespace lyricify {
template<class T>struct Ptr {
 T*p=nullptr;Ptr()=default;~Ptr(){reset();}Ptr(const Ptr&)=delete;Ptr&operator=(const Ptr&)=delete;
 Ptr(Ptr&&o)noexcept:p(o.p){o.p=nullptr;}Ptr&operator=(Ptr&&o)noexcept{if(this!=&o){reset();p=o.p;o.p=nullptr;}return *this;}
 T*get()const{return p;}T**put(){reset();return &p;}T*operator->()const{return p;}
 void reset(){if(p)p->Release();p=nullptr;}
};
inline void ok(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D11 background HRESULT "+std::to_string(uint32_t(hr)));}
// C++ translation of AppleMusicIosMesh.Subdivide: boundary-aware Catmull-Clark.
inline std::vector<BackgroundPoint> subdivide(const std::vector<BackgroundPoint>&s,int n){
 int m=n*2-1;std::vector<BackgroundPoint>f((n-1)*(n-1)),r(m*m);
 auto a=[&](int y,int x){return s[y*n+x];};auto face=[&](int y,int x){return f[y*(n-1)+x];};
 for(int y=0;y<n-1;y++)for(int x=0;x<n-1;x++)f[y*(n-1)+x]=(a(y,x)+a(y,x+1)+a(y+1,x)+a(y+1,x+1))*.25f;
 for(int y=0;y<n;y++)for(int x=0;x<n;x++){
  auto p=a(y,x);bool by=y==0||y==n-1,bx=x==0||x==n-1;
  if(by&&bx)r[y*2*m+x*2]=p;
  else if(by)r[y*2*m+x*2]=(a(y,x-1)+p*6+a(y,x+1))*.125f;
  else if(bx)r[y*2*m+x*2]=(a(y-1,x)+p*6+a(y+1,x))*.125f;
  else{auto favg=(face(y-1,x-1)+face(y-1,x)+face(y,x-1)+face(y,x))*.25f;auto eavg=(p*4+a(y-1,x)+a(y+1,x)+a(y,x-1)+a(y,x+1))*.125f;r[y*2*m+x*2]=(favg+eavg*2+p)*.25f;}
 }
 for(int y=0;y<n;y++)for(int x=0;x<n-1;x++){auto p=a(y,x)+a(y,x+1);r[y*2*m+x*2+1]=(y==0||y==n-1)?p*.5f:(p+face(y-1,x)+face(y,x))*.25f;}
 for(int y=0;y<n-1;y++)for(int x=0;x<n;x++){auto p=a(y,x)+a(y+1,x);r[(y*2+1)*m+x*2]=(x==0||x==n-1)?p*.5f:(p+face(y,x-1)+face(y,x))*.25f;}
 for(int y=0;y<n-1;y++)for(int x=0;x<n-1;x++)r[(y*2+1)*m+x*2+1]=face(y,x);
 return r;
}
struct Surface{Ptr<ID3D11Texture2D>texture;Ptr<ID3D11RenderTargetView>rtv;Ptr<ID3D11ShaderResourceView>srv;};
struct Constants{float time=0,transition=1,view[2]={1,1},scrim=.4f,dither=1,blur[2]={1,1},image[4]={1,1,1,1},pinch[4]={1,1,0,0},lyrics=1,rotation=1,padding[2]{};};
static_assert(sizeof(Constants)==80,"HLSL constant buffer packing");
class Renderer {
 static constexpr UINT W=192,H=72;
 Ptr<ID3D11Device>device;Ptr<ID3D11DeviceContext>context;
 Ptr<ID3D11VertexShader>fullVS,fillVS,rotateVS,meshVS;
 Ptr<ID3D11PixelShader>rotatePS,horizontalPS,verticalPS,meshPS;
 Ptr<ID3D11InputLayout>quadLayout,meshLayout;
 Ptr<ID3D11Buffer>quad,mesh,indices,constants;
 Ptr<ID3D11SamplerState>clampSampler,borderSampler;
 Ptr<ID3D11RasterizerState>rasterizer;
 Surface rotated,horizontal,blurred,output;
 Ptr<ID3D11Texture2D>readback,current,previous;Ptr<ID3D11ShaderResourceView>currentView,previousView;
 UINT indexCount=0;bool portrait=false,meshReady=false;
 Ptr<ID3DBlob> compile(const char*entry,const char*profile){Ptr<ID3DBlob>code,error;auto hr=D3DCompile(LyricifyShaderSource,std::strlen(LyricifyShaderSource),"Lyricify",nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,code.put(),error.put());if(FAILED(hr))throw std::runtime_error(error.get()?std::string((char*)error->GetBufferPointer(),error->GetBufferSize()):"Background shader compilation failed");return code;}
 void vertex(Ptr<ID3D11VertexShader>&shader,const char*entry,Ptr<ID3D11InputLayout>*layout=nullptr,bool isMesh=false){auto code=compile(entry,"vs_4_0");ok(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,shader.put()));if(layout){D3D11_INPUT_ELEMENT_DESC q[]={{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0}};D3D11_INPUT_ELEMENT_DESC m[]={{"FROMPOS",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TOPOS",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0}};ok(device->CreateInputLayout(isMesh?m:q,isMesh?3:2,code->GetBufferPointer(),code->GetBufferSize(),layout->put()));}}
 void pixel(Ptr<ID3D11PixelShader>&shader,const char*entry){auto code=compile(entry,"ps_4_0");ok(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,shader.put()));}
 void surface(Surface&s){D3D11_TEXTURE2D_DESC d{};d.Width=W;d.Height=H;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;ok(device->CreateTexture2D(&d,nullptr,s.texture.put()));ok(device->CreateRenderTargetView(s.texture.get(),nullptr,s.rtv.put()));ok(device->CreateShaderResourceView(s.texture.get(),nullptr,s.srv.put()));}
 void buffer(Ptr<ID3D11Buffer>&b,const void*data,UINT size,UINT bind){D3D11_BUFFER_DESC d{};d.ByteWidth=size;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=bind;D3D11_SUBRESOURCE_DATA s{};s.pSysMem=data;ok(device->CreateBuffer(&d,data?&s:nullptr,b.put()));}
 void texture(Ptr<ID3D11Texture2D>&t,Ptr<ID3D11ShaderResourceView>&view,const std::vector<uint8_t>&p){if(t.get()&&view.get()){context->UpdateSubresource(t.get(),0,nullptr,p.data(),192*4,0);return;}D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=192;d.MipLevels=d.ArraySize=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA s{};s.pSysMem=p.data();s.SysMemPitch=192*4;ok(device->CreateTexture2D(&d,&s,t.put()));ok(device->CreateShaderResourceView(t.get(),nullptr,view.put()));}
 void buildMesh(bool vertical){
  if(meshReady&&portrait==vertical)return;portrait=vertical;int n=vertical?6:9;
  std::vector<BackgroundPoint>from(n*n),to(n*n);
  for(int y=0;y<n;y++)for(int x=0;x<n;x++){int i=y*n+x;from[i]=vertical?Preset0From[i]:BackgroundPoint{float(x)/(n-1),float(y)/(n-1)};to[i]=vertical?Preset0To[i]:LandscapePreset0To[i];}
  // Keep the moving mesh away from its most compressed shapes. This softens
  // narrow color bands while preserving the low-frequency artwork detail.
  constexpr float deformationStrength=.7f;
  for(int y=0;y<n;y++)for(int x=0;x<n;x++){int i=y*n+x;BackgroundPoint identity{float(x)/(n-1),float(y)/(n-1)};from[i]=identity*(1-deformationStrength)+from[i]*deformationStrength;to[i]=identity*(1-deformationStrength)+to[i]*deformationStrength;}
  for(int level=0;level<2;level++){from=subdivide(from,n);to=subdivide(to,n);n=n*2-1;}
  struct V{float fx,fy,tx,ty,u,v;};std::vector<V>vs;std::vector<uint16_t>ix;
  for(int y=0;y<n;y++)for(int x=0;x<n;x++){int i=y*n+x;vs.push_back({from[i].x*2-1,from[i].y*2-1,to[i].x*2-1,to[i].y*2-1,float(x)/(n-1),1-float(y)/(n-1)});}
  for(int y=0;y<n-1;y++)for(int x=0;x<n-1;x++){uint16_t a=y*n+x,b=a+1,c=a+n,d=c+1;ix.insert(ix.end(),{a,c,d,d,b,a});}
  buffer(mesh,vs.data(),UINT(vs.size()*sizeof(V)),D3D11_BIND_VERTEX_BUFFER);buffer(indices,ix.data(),UINT(ix.size()*sizeof(uint16_t)),D3D11_BIND_INDEX_BUFFER);indexCount=UINT(ix.size());meshReady=true;
 }
 void pass(Surface&dest,ID3D11VertexShader*vs,ID3D11PixelShader*ps,ID3D11ShaderResourceView*a,ID3D11ShaderResourceView*b=nullptr){
  ID3D11ShaderResourceView*empty[2]{};context->PSSetShaderResources(0,2,empty);auto rtv=dest.rtv.get();context->OMSetRenderTargets(1,&rtv,nullptr);const float black[4]={0,0,0,1};context->ClearRenderTargetView(rtv,black);ID3D11ShaderResourceView*views[]={a,b};context->PSSetShaderResources(0,2,views);context->VSSetShader(vs,nullptr,0);context->PSSetShader(ps,nullptr,0);
 }
 void bindQuad(){UINT stride=24,offset=0;auto vb=quad.get();context->IASetInputLayout(quadLayout.get());context->IASetVertexBuffers(0,1,&vb,&stride,&offset);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);}
 public:
 Renderer(){
  D3D_FEATURE_LEVEL level;ok(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,device.put(),&level,context.put()));
  vertex(fullVS,"FullscreenVertex",&quadLayout);vertex(fillVS,"ArtworkFillVertex");vertex(rotateVS,"RotationVertex");vertex(meshVS,"PinchVertex",&meshLayout,true);
  pixel(rotatePS,"RotationPixel");pixel(horizontalPS,"BlurHorizontalPixel");pixel(verticalPS,"BlurVerticalPixel");pixel(meshPS,"PinchPixel");
  const float q[]={-1,-1,0,1,0,1,-1,1,0,1,0,0,1,1,0,1,1,0,-1,-1,0,1,0,1,1,1,0,1,1,0,1,-1,0,1,1,1};buffer(quad,q,sizeof(q),D3D11_BIND_VERTEX_BUFFER);buffer(constants,nullptr,sizeof(Constants),D3D11_BIND_CONSTANT_BUFFER);
  D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;s.MaxLOD=D3D11_FLOAT32_MAX;ok(device->CreateSamplerState(&s,clampSampler.put()));s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_BORDER;ok(device->CreateSamplerState(&s,borderSampler.put()));
  D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;ok(device->CreateRasterizerState(&rs,rasterizer.put()));
  surface(rotated);surface(horizontal);surface(blurred);surface(output);D3D11_TEXTURE2D_DESC rd{};output.texture->GetDesc(&rd);rd.BindFlags=0;rd.Usage=D3D11_USAGE_STAGING;rd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ok(device->CreateTexture2D(&rd,nullptr,readback.put()));
 }
 void artwork(const std::vector<uint8_t>&now,const std::vector<uint8_t>&old){texture(current,currentView,now);texture(previous,previousView,old);}
 void draw(double seconds,float transition,int mode,float viewWidth,float viewHeight,std::vector<uint8_t>&pixels){
  float aspect=viewWidth/viewHeight;buildMesh(aspect<1);
  Constants c;c.time=float(seconds);c.transition=transition;c.view[0]=aspect<1?1/aspect:1;c.view[1]=aspect>1?aspect:1;c.scrim=mode==3?.60f:mode==2?.30f:.40f;c.rotation=mode==2?1.7f:1;
  // Adapt the fullscreen material to a compact card. A fixed 170-DIP sigma
  // averages away the color fields in a 130-DIP-tall window.
  const float sigma=std::clamp(std::min(viewWidth,viewHeight)*.35f,24.f,64.f);
  c.blur[0]=sigma/42.5f*W/viewWidth;c.blur[1]=sigma/42.5f*H/viewHeight;
  float textureScale=aspect<1?1.f:.8f;
  c.pinch[0]=c.pinch[1]=textureScale;c.pinch[2]=c.pinch[3]=(1-textureScale)*.5f;
  context->UpdateSubresource(constants.get(),0,nullptr,&c,0,0);auto cb=constants.get();context->VSSetConstantBuffers(0,1,&cb);context->PSSetConstantBuffers(0,1,&cb);ID3D11SamplerState*samplers[]={clampSampler.get(),borderSampler.get()};context->PSSetSamplers(0,2,samplers);D3D11_VIEWPORT vp{0,0,float(W),float(H),0,1};context->RSSetViewports(1,&vp);context->RSSetState(rasterizer.get());
  bindQuad();pass(rotated,fillVS.get(),rotatePS.get(),currentView.get(),previousView.get());context->Draw(6,0);context->VSSetShader(rotateVS.get(),nullptr,0);context->DrawInstanced(6,3,0,0);
  pass(horizontal,fullVS.get(),horizontalPS.get(),rotated.srv.get());context->Draw(6,0);pass(blurred,fullVS.get(),verticalPS.get(),horizontal.srv.get());context->Draw(6,0);
  pass(output,meshVS.get(),meshPS.get(),blurred.srv.get());UINT stride=24,offset=0;auto vb=mesh.get();context->IASetInputLayout(meshLayout.get());context->IASetVertexBuffers(0,1,&vb,&stride,&offset);context->IASetIndexBuffer(indices.get(),DXGI_FORMAT_R16_UINT,0);context->DrawIndexed(indexCount,0,0);
  ID3D11ShaderResourceView*empty[2]{};context->PSSetShaderResources(0,2,empty);context->OMSetRenderTargets(0,nullptr,nullptr);context->CopyResource(readback.get(),output.texture.get());D3D11_MAPPED_SUBRESOURCE mapped{};ok(context->Map(readback.get(),0,D3D11_MAP_READ,0,&mapped));pixels.resize(W*H*4);for(UINT y=0;y<H;y++)std::memcpy(pixels.data()+y*W*4,static_cast<uint8_t*>(mapped.pData)+y*mapped.RowPitch,W*4);context->Unmap(readback.get(),0);ok(device->GetDeviceRemovedReason());
 }
};
}
