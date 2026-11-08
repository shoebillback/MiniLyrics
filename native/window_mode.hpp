// Window activation policy shared by the host and Win32 regression tests.
// GPL-3.0-or-later.
#pragma once
#include <windows.h>
inline LRESULT windowMouseActivate(bool top,bool through){return top||through?MA_NOACTIVATE:MA_ACTIVATE;}
inline bool applyWindowActivationMode(HWND hwnd,bool top,bool through){
 if(!IsWindow(hwnd)){SetLastError(ERROR_INVALID_WINDOW_HANDLE);return false;}
 LONG_PTR before=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
 LONG_PTR after=top||through?before|WS_EX_NOACTIVATE:before&~LONG_PTR(WS_EX_NOACTIVATE);
 if(after==before)return true;
 SetLastError(0);
 return SetWindowLongPtrW(hwnd,GWL_EXSTYLE,after)!=0||GetLastError()==0;
}
inline bool setWindowTopmost(HWND hwnd,bool top,bool through){
 if(!IsWindow(hwnd)){SetLastError(ERROR_INVALID_WINDOW_HANDLE);return false;}
 LONG_PTR before=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
 if(!applyWindowActivationMode(hwnd,top,through))return false;
 if(SetWindowPos(hwnd,top?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_FRAMECHANGED))return true;
 DWORD error=GetLastError();SetWindowLongPtrW(hwnd,GWL_EXSTYLE,before);SetLastError(error);return false;
}
