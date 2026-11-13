// Read-only lifetime tracking of the CloudMusic instance that launched MiniLyrics.
// GPL-3.0-or-later. A retained process handle prevents PID-reuse false positives.
#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
class OwnerProcess {
 HANDLE owner=nullptr;
 static unsigned long long created(HANDLE process){
  FILETIME start{},end{},kernel{},user{};
  if(!GetProcessTimes(process,&start,&end,&kernel,&user))return 0;
  return (static_cast<unsigned long long>(start.dwHighDateTime)<<32)|start.dwLowDateTime;
 }
 public:
 OwnerProcess()=default;
 OwnerProcess(const OwnerProcess&)=delete;
 OwnerProcess&operator=(const OwnerProcess&)=delete;
 ~OwnerProcess(){if(owner)CloseHandle(owner);}
 void capture(){
  if(owner)return;
  HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
  if(snapshot==INVALID_HANDLE_VALUE)return;
  std::vector<PROCESSENTRY32W> entries;PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
  if(Process32FirstW(snapshot,&entry))do{entries.push_back(entry);}while(Process32NextW(snapshot,&entry));
  CloseHandle(snapshot);
  // Find the outermost CloudMusic ancestor in the snapshot first. Do not open
  // unrelated desktop/system ancestors: their exit or permissions are irrelevant.
  std::vector<DWORD> lineage;DWORD cursor=GetCurrentProcessId(),ownerId=0;
  for(int depth=0;depth<64;++depth){
   const PROCESSENTRY32W* child=nullptr;
   for(const auto& item:entries)if(item.th32ProcessID==cursor){child=&item;break;}
   if(!child||!child->th32ParentProcessID)break;
   DWORD parentId=child->th32ParentProcessID;
   if(parentId==cursor)return;
   for(auto id:lineage)if(id==parentId)return;
   const PROCESSENTRY32W* parent=nullptr;
   for(const auto& item:entries)if(item.th32ProcessID==parentId){parent=&item;break;}
   if(!parent)break;
   lineage.push_back(parentId);
   if(_wcsicmp(parent->szExeFile,L"cloudmusic.exe")==0)ownerId=parentId;
   cursor=parentId;
  }
  if(!ownerId)return;
  auto childCreated=created(GetCurrentProcess());if(!childCreated)return;
  for(auto parentId:lineage){
   HANDLE parent=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parentId);
   if(!parent)return;
   auto parentCreated=created(parent);
   if(!parentCreated||parentCreated>childCreated){CloseHandle(parent);return;}
   if(parentId==ownerId){
    wchar_t path[32768];DWORD length=32768;
    if(!QueryFullProcessImageNameW(parent,0,path,&length)){CloseHandle(parent);return;}
    std::wstring image(path,length);auto separator=image.find_last_of(L"\\/");
    std::wstring name=image.substr(separator==std::wstring::npos?0:separator+1);
    if(_wcsicmp(name.c_str(),L"cloudmusic.exe")!=0){CloseHandle(parent);return;}
    owner=parent;return;
   }
   CloseHandle(parent);childCreated=parentCreated;
  }
  // Identification failure leaves the window alive rather than guessing an exit.
 }
 bool exited()const{return owner&&WaitForSingleObject(owner,0)==WAIT_OBJECT_0;}
 bool monitored()const{return owner!=nullptr;}
};
