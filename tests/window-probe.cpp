#define UNICODE
#define _UNICODE
#include <windows.h>
#include <cstdio>
#include <cstdlib>
struct Search{DWORD pid;HWND hwnd;};
BOOL CALLBACK findWindow(HWND hwnd,LPARAM lp){auto*s=reinterpret_cast<Search*>(lp);DWORD id;GetWindowThreadProcessId(hwnd,&id);wchar_t name[128];GetClassNameW(hwnd,name,128);if(id==s->pid&&wcscmp(name,L"MiniLyrics.Direct2D")==0){s->hwnd=hwnd;return FALSE;}return TRUE;}
int main(int argc,char**argv){if(argc<5)return 2;SetProcessDPIAware();Search s{DWORD(strtoul(argv[1],nullptr,10)),nullptr};EnumWindows(findWindow,reinterpret_cast<LPARAM>(&s));if(!s.hwnd)return 3;int x=atoi(argv[3]),y=atoi(argv[4]);if(strcmp(argv[2],"place")==0){POINT cursor;GetCursorPos(&cursor);SetWindowPos(s.hwnd,HWND_TOPMOST,cursor.x-x,cursor.y-y,0,0,SWP_NOSIZE|SWP_NOACTIVATE);SendMessageW(s.hwnd,WM_MOUSEMOVE,0,MAKELPARAM(x,y));}else if(strcmp(argv[2],"down")==0)SendMessageW(s.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));else if(strcmp(argv[2],"up")==0)SendMessageW(s.hwnd,WM_LBUTTONUP,0,MAKELPARAM(x,y));else return 4;printf("ok\n");return 0;}
