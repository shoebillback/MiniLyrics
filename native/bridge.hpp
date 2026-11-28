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
#include <string_view>
#include "json.hpp"
class Bridge {
 std::atomic<SOCKET> listener{INVALID_SOCKET},client{INVALID_SOCKET};
 std::atomic<bool> stopping{false},authenticated{false};std::thread worker;std::mutex writeMutex;
 std::string token;std::function<void(Json)> receive;std::function<void()> disconnected;
 static bool readBytes(SOCKET s,void* p,size_t n){char*b=static_cast<char*>(p);while(n){int r=recv(s,b,int(std::min<size_t>(n,65536)),0);if(r<=0)return false;b+=r;n-=r;}return true;}
 static bool writeBytes(SOCKET s,const void*p,size_t n){const char*b=static_cast<const char*>(p);while(n){int r=::send(s,b,int(std::min<size_t>(n,65536)),0);if(r<=0)return false;b+=r;n-=r;}return true;}
 static void closeListener(std::atomic<SOCKET>&slot){SOCKET s=slot.exchange(INVALID_SOCKET);if(s!=INVALID_SOCKET){shutdown(s,SD_BOTH);closesocket(s);}}
 void closeClient(){
  SOCKET s=client.load();
  if(s!=INVALID_SOCKET)shutdown(s,SD_BOTH);
  std::lock_guard<std::mutex> lock(writeMutex);
  s=client.exchange(INVALID_SOCKET);
  if(s!=INVALID_SOCKET)closesocket(s);
 }
 static bool constantTimeEqual(const std::string&a,const std::string&b){
  if(a.size()!=b.size())return false;
  unsigned char diff=0;
  for(size_t i=0;i<a.size();i++)diff|=static_cast<unsigned char>(a[i])^static_cast<unsigned char>(b[i]);
  return diff==0;
 }
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
  std::string header;char peek[2048];
  while(header.size()<16384){
   int avail=recv(s,peek,int(std::min<size_t>(sizeof(peek),16384-header.size())),MSG_PEEK);
   if(avail<=0)return false;
   size_t prevLen=header.size();
   header.append(peek,size_t(avail));
   size_t searchFrom=prevLen>=3?prevLen-3:0;
   size_t found=header.find("\r\n\r\n",searchFrom);
   if(found!=std::string::npos){
    size_t toConsume=(found+4)-prevLen;
    header.resize(found+4);
    if(!readBytes(s,peek,toConsume))return false;
    break;
   }
   if(!readBytes(s,peek,size_t(avail)))return false;
  }
  if(header.size()<4||header.compare(header.size()-4,4,"\r\n\r\n")!=0)return false;
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
  size_t n=payload.size(),headerSize=2+(n>=126?(n<=65535?2:8):0);
  std::string out;out.reserve(headerSize+n);
  out.push_back(char(static_cast<unsigned char>(128|opcode)));
  if(n<126)out.push_back(char(static_cast<unsigned char>(n)));
  else if(n<=65535){out.push_back(char(126));out.push_back(char(static_cast<unsigned char>(n>>8)));out.push_back(char(static_cast<unsigned char>(n)));}
  else{out.push_back(char(127));for(int i=7;i>=0;i--)out.push_back(char(static_cast<unsigned char>((uint64_t(n)>>(8*i))&255)));}
  out.append(payload);
  writeBytes(s,out.data(),out.size());
 }
 void run(){
  while(!stopping){SOCKET s=accept(listener.load(),nullptr,nullptr);if(s==INVALID_SOCKET)break;
   {std::lock_guard<std::mutex> lock(writeMutex);if(stopping){closesocket(s);break;}client=s;}
   authenticated=false;
   DWORD timeout=20000;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));timeout=3000;setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));BOOL noDelay=TRUE;setsockopt(s,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<char*>(&noDelay),sizeof(noDelay));
   if(handshake(s)){
    bool assembling=false;std::string fragment;
    while(!stopping){
     unsigned char first[2];if(!readBytes(s,first,2))break;
     bool fin=(first[0]&128)!=0,masked=(first[1]&128)!=0;
     int rsv=first[0]&112,opcode=first[0]&15;
     if(!masked||rsv!=0)break;
     uint64_t length=first[1]&127;if(length==126){unsigned char b[2];if(!readBytes(s,b,2))break;length=uint64_t(b[0])*256+b[1];}
     else if(length==127){unsigned char b[8];if(!readBytes(s,b,8))break;length=0;for(auto c:b)length=(length<<8)|c;}
     if(length>4*1024*1024)break;unsigned char mask[4];if(!readBytes(s,mask,4))break;
     std::string body(size_t(length),'\0');if(length&&!readBytes(s,&body[0],size_t(length)))break;
     for(size_t i=0;i<body.size();i++)body[i]=char(static_cast<unsigned char>(body[i])^mask[i%4]);
     if(opcode>=8){if(!fin||length>125)break;if(opcode==8)break;if(opcode==9)frame(10,body);continue;}
     if(opcode==1){if(assembling)break;if(!fin){assembling=true;fragment=std::move(body);continue;}}
     else if(opcode==0){if(!assembling)break;if(fragment.size()+body.size()>4*1024*1024)break;fragment+=body;if(!fin)continue;body=std::move(fragment);fragment.clear();assembling=false;}
     else{if(assembling)break;continue;}
     try{Json m=JsonParser(body).parse();if(m.kind!=Json::Object)break;
      if(!authenticated){if(m["type"].s()!="hello"||!constantTimeEqual(m["token"].s(),token))break;authenticated=true;
       // Authenticated silence does not imply that the player has exited.
       DWORD noTimeout=0;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&noTimeout),sizeof(noTimeout));}
      else if(m["type"].s()=="hello")continue; // Only the handshake starts a session.
      receive(std::move(m));
     }catch(...){break;}
    }
   }bool hadSession=authenticated.exchange(false);closeClient();
   if(hadSession&&!stopping&&disconnected)disconnected();
  }
 }
 public:Bridge(int port,std::string secret,std::function<void(Json)> callback,std::function<void()> onDisconnect={}):token(std::move(secret)),receive(std::move(callback)),disconnected(std::move(onDisconnect)){
  SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(s==INVALID_SOCKET)throw std::runtime_error("socket failed");listener=s;
#ifdef SO_EXCLUSIVEADDRUSE
  BOOL exclusive=TRUE;setsockopt(s,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive));
#endif
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(static_cast<unsigned short>(port));address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  if(bind(s,reinterpret_cast<sockaddr*>(&address),sizeof(address))==SOCKET_ERROR||listen(s,1)==SOCKET_ERROR){closeListener(listener);throw std::runtime_error("loopback port unavailable");}
  worker=std::thread([this]{run();});
 }
 void send(const Json&m){if(authenticated)frame(1,m.dump());}
 bool connected()const{return authenticated.load();}
 int boundPort()const{SOCKET s=listener.load();if(s==INVALID_SOCKET)return 0;sockaddr_in addr{};int len=sizeof(addr);return getsockname(s,reinterpret_cast<sockaddr*>(&addr),&len)==0?ntohs(addr.sin_port):0;}
 void finish(){if(authenticated)frame(8,std::string("\x03\xe8",2)+"host-closed");}
 void stop(){stopping=true;authenticated=false;closeClient();closeListener(listener);if(worker.joinable())worker.join();}
 ~Bridge(){stop();}
};
