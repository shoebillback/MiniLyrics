// MiniLyrics modal frame forwarding. GPL-3.0-or-later.
#pragma once
#include <windows.h>

// The system move loop dispatches messages, but cannot wait on App::run's
// timer handle. Forward that same timer to the UI thread only while moving.
class ModalFrames {
 HANDLE waiter=nullptr;
 HWND window=nullptr;
 UINT message=0;
 UINT_PTR serial=0;
 static void CALLBACK ready(void* context,BOOLEAN){
  auto*self=static_cast<ModalFrames*>(context);
  PostMessageW(self->window,self->message,self->serial,0);
 }
 public:
 ModalFrames()=default;
 ModalFrames(const ModalFrames&)=delete;
 ModalFrames&operator=(const ModalFrames&)=delete;
 ~ModalFrames(){stop();}
 bool start(HWND target,HANDLE timer,UINT notification){
  stop();window=target;message=notification;
  if(timer&&RegisterWaitForSingleObject(&waiter,timer,ready,this,INFINITE,WT_EXECUTEDEFAULT))return true;
  waiter=nullptr;return false;
 }
 void stop(){
  if(waiter){HANDLE previous=waiter;waiter=nullptr;UnregisterWaitEx(previous,INVALID_HANDLE_VALUE);}
  // Callback fields are immutable until all callbacks have completed. Queued
  // messages from a previous move are invalid after this point.
  ++serial;
 }
 bool active()const{return waiter!=nullptr;}
 bool accepts(WPARAM ticket)const{return active()&&ticket==serial;}
};
