// Loopback WebSocket bridge, preserving the MiniLyrics message protocol.
// GPL-3.0-or-later. Uses only Windows APIs and the C++ standard library.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <algorithm>
#include <cctype>
#include "json.hpp"
class Bridge {
 std::atomic<SOCKET> listener{INVALID_SOCKET},client{INVALID_SOCKET};
 std::atomic<bool> stopping{false},authenticated{false};std::thread worker;std::mutex writeMutex;
 std::string token;std::function<void(Json)> receive;
 static bool readBytes(SOCKET s,void* p,size_t n){char*b=static_cast<char*>(p);while(n){int r=recv(s,b,int(std::min<size_t>(n,65536)),0);if(r<=0)return false;b+=r;n-=r;}return true;}
 static bool writeBytes(SOCKET s,const void*p,size_t n){const char*b=static_cast<const char*>(p);while(n){int r=::send(s,b,int(std::min<size_t>(n,65536)),0);if(r<=0)return false;b+=r;n-=r;}return true;}
 static void closeSocket(std::atomic<SOCKET>&slot){SOCKET s=slot.exchange(INVALID_SOCKET);if(s!=INVALID_SOCKET){shutdown(s,SD_BOTH);closesocket(s);}}
 static std::string acceptKey(const std::string&key){
  BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;DWORD length=0,done=0;std::string result;
  if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA1_ALGORITHM,nullptr,0)<0)return result;
  if(BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&done,0)>=0){
   std::vector<unsigned char> object(length);unsigned char digest[20];
   if(BCryptCreateHash(alg,&hash,object.data(),length,nullptr,0,0)>=0){
    auto input=key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    if(BCryptHashData(hash,reinterpret_cast<PUCHAR>(&input[0]),DWORD(input.size()),0)>=0&&BCryptFinishHash(hash,digest,20,0)>=0){
     DWORD size=0;CryptBinaryToStringA(digest,20,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&size);result.resize(size);
     if(CryptBinaryToStringA(digest,20,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,&result[0],&size))result.resize(size);else result.clear();
     while(!result.empty()&&result.back()==0)result.pop_back();
    }BCryptDestroyHash(hash);
   }
  }BCryptCloseAlgorithmProvider(alg,0);return result;
 }
 bool handshake(SOCKET s){
  std::string header;char c;while(header.size()<16384){if(!readBytes(s,&c,1))return false;header+=c;if(header.size()>=4&&header.compare(header.size()-4,4,"\r\n\r\n")==0)break;}
  if(header.rfind("GET / HTTP/1.1\r\n",0)!=0)return false;
  std::string lower=header;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(std::tolower(c));});
  auto begin=lower.find("\r\nsec-websocket-key:");if(begin==std::string::npos)return false;begin+=20;
  auto end=header.find("\r\n",begin);if(end==std::string::npos)return false;auto key=header.substr(begin,end-begin);
  key.erase(0,key.find_first_not_of(" \t"));auto kEnd=key.find_last_not_of(" \t");if(kEnd==std::string::npos)return false;key.resize(kEnd+1);
  auto accept=acceptKey(key);if(accept.empty())return false;
  auto response="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "+accept+"\r\n\r\n";
  return writeBytes(s,response.data(),response.size());
 }
 void frame(int opcode,const std::string&payload){
  std::lock_guard<std::mutex> lock(writeMutex);SOCKET s=client.load();if(s==INVALID_SOCKET)return;
  std::vector<unsigned char> h{static_cast<unsigned char>(128|opcode)};size_t n=payload.size();
  if(n<126)h.push_back(static_cast<unsigned char>(n));else if(n<=65535){h.push_back(126);h.push_back(static_cast<unsigned char>(n>>8));h.push_back(static_cast<unsigned char>(n));}
  else{h.push_back(127);for(int i=7;i>=0;i--)h.push_back(static_cast<unsigned char>((uint64_t(n)>>(8*i))&255));}
  if(writeBytes(s,h.data(),h.size()))writeBytes(s,payload.data(),payload.size());
 }
 void run(){
  while(!stopping){SOCKET s=accept(listener.load(),nullptr,nullptr);if(s==INVALID_SOCKET)break;client=s;authenticated=false;
   DWORD timeout=20000;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));timeout=3000;setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));BOOL noDelay=TRUE;setsockopt(s,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<char*>(&noDelay),sizeof(noDelay));
   if(handshake(s)){while(!stopping){
    unsigned char first[2];if(!readBytes(s,first,2))break;int opcode=first[0]&15;
    if(!(first[0]&128)||!(first[1]&128)||(first[0]&112))break;
    uint64_t length=first[1]&127;if(length==126){unsigned char b[2];if(!readBytes(s,b,2))break;length=uint64_t(b[0])*256+b[1];}
    else if(length==127){unsigned char b[8];if(!readBytes(s,b,8))break;length=0;for(auto c:b)length=(length<<8)|c;}
    if(length>4*1024*1024)break;unsigned char mask[4];if(!readBytes(s,mask,4))break;
    std::string body(size_t(length),'\0');if(length&&!readBytes(s,&body[0],size_t(length)))break;
    for(size_t i=0;i<body.size();i++)body[i]=char(static_cast<unsigned char>(body[i])^mask[i%4]);
    if(opcode==8)break;if(opcode==9){if(length>125)break;frame(10,body);continue;}if(opcode!=1)continue;
    try{Json m=JsonParser(body).parse();if(m.kind!=Json::Object)break;
     if(!authenticated){if(m["type"].s()!="hello"||m["token"].s()!=token)break;authenticated=true;}
     receive(std::move(m));
    }catch(...){break;}
   }}authenticated=false;closeSocket(client);
  }
 }
 public:Bridge(int port,std::string secret,std::function<void(Json)> callback):token(std::move(secret)),receive(std::move(callback)){
  SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(s==INVALID_SOCKET)throw std::runtime_error("socket failed");listener=s;
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(static_cast<unsigned short>(port));address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(bind(s,reinterpret_cast<sockaddr*>(&address),sizeof(address))==SOCKET_ERROR||listen(s,1)==SOCKET_ERROR){closeSocket(listener);throw std::runtime_error("loopback port unavailable");}
  worker=std::thread([this]{run();});
 }
 void send(const Json&m){if(authenticated)frame(1,m.dump());}
 void stop(){stopping=true;authenticated=false;closeSocket(client);closeSocket(listener);if(worker.joinable())worker.join();}
 ~Bridge(){stop();}
};
