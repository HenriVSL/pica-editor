// emem.cpp - allocation wrappers (DESIGN 5.2, 6.2). Owner after Wave 0: overseer.
// Core never includes windows.h: VirtualAlloc/VirtualFree are declared by hand for Win32 builds.
#include "emem.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
extern "C" __declspec(dllimport) void* __stdcall VirtualAlloc(void* addr, unsigned long size, unsigned long type,
                                                              unsigned long protect);
extern "C" __declspec(dllimport) int __stdcall VirtualFree(void* addr, unsigned long size, unsigned long type);
#define E_MEM_COMMIT 0x1000
#define E_MEM_RESERVE 0x2000
#define E_PAGE_READWRITE 0x04
#define E_MEM_RELEASE 0x8000
#endif

static u32 g_live = 0;
static i32 g_fail_after = -1;   // countdown: 0 = the next allocation fails

static int should_fail()
{
  if (g_fail_after < 0) return 0;
  if (g_fail_after == 0) { g_fail_after = -1; return 1; }
  g_fail_after--;
  return 0;
}

void* e_alloc(u32 n)
{
  void* p;
  if (should_fail()) return 0;
  p = malloc(n ? n : 1);
  if (p) g_live++;
  return p;
}

void* e_realloc(void* p, u32 n)
{
  void* q;
  if (!p) return e_alloc(n);
  if (should_fail()) return 0;
  q = realloc(p, n ? n : 1);
  return q;
}

void e_free(void* p)
{
  if (!p) return;
  free(p);
  g_live--;
}

void* e_valloc(u32 n)
{
  void* p;
  if (should_fail()) return 0;
#if defined(_WIN32)
  p = VirtualAlloc(0, n ? n : 1, E_MEM_COMMIT | E_MEM_RESERVE, E_PAGE_READWRITE);
#else
  p = malloc(n ? n : 1);
#endif
  if (p) g_live++;
  return p;
}

void e_vfree(void* p, u32 n)
{
  (void)n;
  if (!p) return;
#if defined(_WIN32)
  VirtualFree(p, 0, E_MEM_RELEASE);
#else
  free(p);
#endif
  g_live--;
}

void e_test_fail_after(i32 n)
{
  // "n-th following allocation fails": n = 1 (or 0) makes the very next one fail; one-shot; n < 0 disables.
  g_fail_after = n < 0 ? -1 : (n == 0 ? 0 : n - 1);
}

u32 e_alloc_count()
{
  return g_live;
}

int EBytes::append(const void* p, u32 n)
{
  return insert(size(), (const u8*)p, n);
}
