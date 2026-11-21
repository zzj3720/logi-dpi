// win-cc: lets pionc (which drives a GCC-style `cc`) build and link with LLVM clang for the MSVC target, the
// toolchain mbui's Windows host is written for (WRL, WinRT, Windows SDK headers and import libraries).
//
// It runs `$PION_CLANG` (clang.exe, or clang++.exe when invoked as win-c++) with the same arguments except:
//   -Wl,--stack,N   -> -Wl,/STACK:N
//   -mwindows       -> -Wl,/SUBSYSTEM:WINDOWS -Wl,/ENTRY:mainCRTStartup
//   -lm, -lpthread  -> dropped (part of the UCRT)
// and adds -fuse-ld=lld when linking. Build it with that clang: clang -O2 tools/win-cc.c -o out/win-cc.exe
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One argument quoted as CreateProcess/the CRT parse it back.
static char *quote(const char *a) {
  size_t n = strlen(a);
  int need = n == 0 || strpbrk(a, " \t\"") != NULL;
  if (!need) return _strdup(a);
  char *out = (char *)malloc(n * 2 + 3);
  char *o = out;
  *o++ = '"';
  size_t bs = 0;
  for (const char *p = a;; p++) {
    if (*p == '\\') {
      bs++;
      continue;
    }
    if (*p == '\0') {
      for (size_t i = 0; i < bs * 2; i++) *o++ = '\\';
      break;
    }
    if (*p == '"') {
      for (size_t i = 0; i < bs * 2 + 1; i++) *o++ = '\\';
    } else {
      for (size_t i = 0; i < bs; i++) *o++ = '\\';
    }
    bs = 0;
    *o++ = *p;
  }
  *o++ = '"';
  *o = '\0';
  return out;
}

static int ends_with(const char *s, const char *tail) {
  size_t n = strlen(s), m = strlen(tail);
  return n >= m && _stricmp(s + n - m, tail) == 0;
}

int main(int argc, char **argv) {
  const char *clang = getenv("PION_CLANG");
  if (!clang || !*clang) {
    fprintf(stderr, "win-cc: set PION_CLANG to clang.exe\n");
    return 2;
  }
  char exe[1024];
  snprintf(exe, sizeof exe, "%s", clang);
  int cxx = ends_with(argv[0], "c++.exe") || ends_with(argv[0], "c++");
  if (cxx) {
    char *slash = strrchr(exe, '\\');
    char *fslash = strrchr(exe, '/');
    if (fslash > slash) slash = fslash;
    snprintf(slash ? slash + 1 : exe, sizeof exe - (size_t)(slash ? slash + 1 - exe : 0), "clang++.exe");
  }
  const char **out = (const char **)calloc((size_t)argc * 2 + 12, sizeof(char *));
  int k = 0, compile_only = 0;
  out[k++] = exe;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "-c") == 0 || strcmp(a, "-S") == 0 || strcmp(a, "-E") == 0 || strcmp(a, "-fsyntax-only") == 0 ||
        strcmp(a, "--version") == 0)
      compile_only = 1;
    if (strcmp(a, "-lm") == 0 || strcmp(a, "-lpthread") == 0) continue;
    if (strncmp(a, "-Wl,--stack,", 12) == 0) {
      char *s = (char *)malloc(strlen(a) + 16);
      sprintf(s, "-Wl,/STACK:%s", a + 12);
      out[k++] = s;
      continue;
    }
    if (strcmp(a, "-mwindows") == 0) {
      out[k++] = "-Wl,/SUBSYSTEM:WINDOWS";
      out[k++] = "-Wl,/ENTRY:mainCRTStartup";
      continue;
    }
    out[k++] = a;
  }
  if (!compile_only) out[k++] = "-fuse-ld=lld";
  // WIN_CC_DEBUG=1: CodeView debug info and a PDB beside the program (stacks in a debugger)
  if (getenv("WIN_CC_DEBUG")) {
    out[k++] = "-g";
    out[k++] = "-gcodeview";
    if (!compile_only) out[k++] = "-Wl,/DEBUG";
  }
  out[k] = NULL;
  const char **q = (const char **)calloc((size_t)k + 1, sizeof(char *));
  for (int i = 0; i < k; i++) q[i] = quote(out[i]);
  if (getenv("WIN_CC_VERBOSE")) {
    for (int i = 0; i < k; i++) fprintf(stderr, "%s ", q[i]);
    fprintf(stderr, "\n");
  }
  intptr_t rc = _spawnv(_P_WAIT, exe, q);
  if (rc == -1) {
    perror("win-cc: spawn");
    return 127;
  }
  return (int)rc;
}
