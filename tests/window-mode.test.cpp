#define UNICODE
#define _UNICODE
#include "../native/window_mode.hpp"
#include <cassert>
#include <iostream>
static bool top=false,through=false;
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_MOUSEACTIVATE)return windowMouseActivate(top,through);return DefWindowProcW(h,m,w,l);}
int main(){
 WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"MiniLyrics.WindowModeTest";assert(RegisterClassW(&c));
 HWND h=CreateWindowExW(WS_EX_TOOLWINDOW,c.lpszClassName,L"Isolated regression",WS_POPUP,0,0,100,100,nullptr,nullptr,c.hInstance,nullptr);assert(h);
 // Hidden, independent window: inspect real OS styles, not a mirrored boolean.
 for(int pass=0;pass<3;pass++)for(bool t:{true,false})for(bool p:{false,true}){
  top=t;through=p;assert(setWindowTopmost(h,top,through));auto style=GetWindowLongPtrW(h,GWL_EXSTYLE);
  assert(bool(style&WS_EX_TOPMOST)==top);assert(bool(style&WS_EX_NOACTIVATE)==(top||through));
  assert(SendMessageW(h,WM_MOUSEACTIVATE,0,MAKELPARAM(HTCLIENT,WM_LBUTTONDOWN))==(top||through?MA_NOACTIVATE:MA_ACTIVATE));
  SetWindowPos(h,nullptr,20,20,120,120,SWP_NOZORDER|SWP_NOACTIVATE);assert(bool(GetWindowLongPtrW(h,GWL_EXSTYLE)&WS_EX_TOPMOST)==top);
 }
 top=false;through=false;assert(setWindowTopmost(h,top,through));
 SetWindowLongPtrW(h,GWL_EXSTYLE,GetWindowLongPtrW(h,GWL_EXSTYLE)|WS_EX_LAYERED|WS_EX_TRANSPARENT);assert(applyWindowActivationMode(h,top,true));
 assert((GetWindowLongPtrW(h,GWL_EXSTYLE)&(WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT))==(WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT));
 SetWindowLongPtrW(h,GWL_EXSTYLE,GetWindowLongPtrW(h,GWL_EXSTYLE)&~LONG_PTR(WS_EX_LAYERED|WS_EX_TRANSPARENT));assert(applyWindowActivationMode(h,top,false));assert(!(GetWindowLongPtrW(h,GWL_EXSTYLE)&WS_EX_NOACTIVATE));
 assert(!setWindowTopmost(nullptr,true,false));DestroyWindow(h);assert(!setWindowTopmost(h,true,false));
 std::cout<<"PASS: real Win32 topmost/activation flags, repeated toggles, pass-through, resize, mouse activation and invalid handles\n";
}
