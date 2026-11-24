/* stackdump <pid>: prints every thread's stack as module+offset (resolve offsets with nm) */
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

static void modname(HANDLE p, DWORD64 a, char* out, int n){
  IMAGEHLP_MODULE64 m; memset(&m,0,sizeof m); m.SizeOfStruct=sizeof m;
  if(SymGetModuleInfo64(p,a,&m)) snprintf(out,n,"%s+%llx",m.ModuleName,(unsigned long long)(a-m.BaseOfImage));
  else snprintf(out,n,"%llx",(unsigned long long)a); }

int main(int argc, char** argv){
  DWORD pid=atoi(argv[1]); int maxf= argc>2? atoi(argv[2]) : 24;
  HANDLE p=OpenProcess(PROCESS_ALL_ACCESS,FALSE,pid); if(!p){ printf("open failed %lu\n",GetLastError()); return 1; }
  SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS);
  if(!SymInitialize(p,NULL,TRUE)) printf("SymInitialize failed\n");
  HANDLE sn=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0); THREADENTRY32 te; te.dwSize=sizeof te;
  for(BOOL ok=Thread32First(sn,&te); ok; ok=Thread32Next(sn,&te)){
    if(te.th32OwnerProcessID!=pid) continue;
    HANDLE t=OpenThread(THREAD_ALL_ACCESS,FALSE,te.th32ThreadID); if(!t) continue;
    SuspendThread(t);
    CONTEXT c; memset(&c,0,sizeof c); c.ContextFlags=CONTEXT_FULL;
    if(GetThreadContext(t,&c)){
      wchar_t* desc=NULL; char dn[128]="";
      HRESULT (WINAPI *gtd)(HANDLE,PWSTR*)=(void*)GetProcAddress(GetModuleHandleA("kernel32"),"GetThreadDescription");
      if(gtd && SUCCEEDED(gtd(t,&desc)) && desc){ WideCharToMultiByte(CP_UTF8,0,desc,-1,dn,sizeof dn,NULL,NULL); LocalFree(desc); }
      printf("thread %lu %s\n",te.th32ThreadID,dn);
      STACKFRAME64 f; memset(&f,0,sizeof f);
      f.AddrPC.Offset=c.Rip; f.AddrPC.Mode=AddrModeFlat; f.AddrFrame.Offset=c.Rbp; f.AddrFrame.Mode=AddrModeFlat; f.AddrStack.Offset=c.Rsp; f.AddrStack.Mode=AddrModeFlat;
      for(int i=0;i<maxf;i++){
        if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,p,t,&f,&c,NULL,SymFunctionTableAccess64,SymGetModuleBase64,NULL)) break;
        if(!f.AddrPC.Offset) break;
        char b[300]; modname(p,f.AddrPC.Offset,b,sizeof b);
        char sb[sizeof(SYMBOL_INFO)+256]; SYMBOL_INFO* si=(SYMBOL_INFO*)sb; si->SizeOfStruct=sizeof(SYMBOL_INFO); si->MaxNameLen=255; DWORD64 d=0;
        if(SymFromAddr(p,f.AddrPC.Offset,&d,si)) printf("   %s  %s+%llx\n",b,si->Name,(unsigned long long)d); else printf("   %s\n",b); } }
    ResumeThread(t); CloseHandle(t); }
  return 0; }
