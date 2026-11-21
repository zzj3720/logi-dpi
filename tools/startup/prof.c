// prof <pid> <ms>: sample the main (first) thread from outside every ~1 ms, aggregate by function (self = RIP,
// total = RIP + return-address-looking stack words), symbolized with the program's PDB.
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { DWORD64 a; int self, total; } E;
static E es[20000]; static int ne;
static E *get(DWORD64 a) { for (int i = 0; i < ne; i++) if (es[i].a == a) return &es[i]; if (ne >= 20000) return 0; es[ne].a = a; es[ne].self = es[ne].total = 0; return &es[ne++]; }
static int cmp(const void *x, const void *y) { return ((E *)y)->total - ((E *)x)->total; }
int main(int argc, char **argv) {
  DWORD pid = (DWORD)atoi(argv[1]); int ms = atoi(argv[2]);
  HANDLE p = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(p, NULL, TRUE);
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te = {sizeof te}; DWORD tid = 0;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) if (te.th32OwnerProcessID == pid) { tid = te.th32ThreadID; break; }
  HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
  HMODULE exe; DWORD need; EnumProcessModules(p, &exe, sizeof exe, &need);
  MODULEINFO mi; GetModuleInformation(p, exe, &mi, sizeof mi);
  DWORD64 lo = (DWORD64)mi.lpBaseOfDll, hi = lo + mi.SizeOfImage;
  timeBeginPeriod(1);
  static DWORD64 stk[4096]; int samples = 0, busy = 0;
  ULONGLONG end = GetTickCount64() + ms;
  while (GetTickCount64() < end) {
    CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_FULL; SIZE_T got = 0;
    SuspendThread(th); GetThreadContext(th, &c); ReadProcessMemory(p, (void *)c.Rsp, stk, sizeof stk, &got); ResumeThread(th);
    samples++;
    char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf; DWORD64 d;
    si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
    DWORD64 f = SymFromAddr(p, c.Rip, &d, si) ? si->Address : c.Rip;
    if (!strstr(si->Name, "Wait")) busy++;
    DWORD64 seen[300]; int ns = 0; E *e = get(f); if (e) { e->self++; e->total++; } seen[ns++] = f;
    for (size_t i = 0; i < got / 8 && ns < 300; i++) {
      DWORD64 v = stk[i]; if (v < lo || v >= hi) continue;
      si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
      if (!SymFromAddr(p, v, &d, si)) continue;
      DWORD64 g = si->Address; int dup = 0; for (int j = 0; j < ns; j++) if (seen[j] == g) { dup = 1; break; }
      if (dup) continue; seen[ns++] = g; E *e2 = get(g); if (e2) e2->total++;
    }
    Sleep(1);
  }
  qsort(es, ne, sizeof(E), cmp);
  printf("samples=%d busy=%.1f%%\n", samples, 100.0 * busy / samples);
  for (int i = 0; i < ne && i < 70; i++) {
    char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO *si = (SYMBOL_INFO *)buf; DWORD64 d;
    si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
    printf("total %5.1f%% self %5.1f%%  %s\n", 100.0 * es[i].total / samples, 100.0 * es[i].self / samples, SymFromAddr(p, es[i].a, &d, si) ? si->Name : "?");
  }
  return 0;
}
