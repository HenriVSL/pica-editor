// statusbar.cpp - status bar with the 9 parts of DESIGN 15.3, transient notes. T00: parts and notes work; T07 fills
// the texts (app_update_status) and click targets. Owner after Wave 0: T07.
#include <windows.h>
#include <commctrl.h>
#include <stdarg.h>
#include "app.h"
#include "tab.h"
#include "selftest.h"
#include "frame_int.h"
#include "estr.h"

static const int k_part_w[SB_PARTS - 1] = { 130, 90, 70, 85, 90, 55, 40, 45 };   // flags part takes the rest
static char g_note[300];

HWND statusbar_create(HWND parent)
{
  g_app.statusbar = CreateWindowExA(0, STATUSCLASSNAMEA, "", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                                    parent, (HMENU)ID_STATUSBAR, g_app.hinst, 0);
  statusbar_layout();
  return g_app.statusbar;
}

void statusbar_layout()
{
  int edges[SB_PARTS];
  int i, x = 0;
  if (!g_app.statusbar) return;
  for (i = 0; i < SB_PARTS - 1; i++) { x += k_part_w[i]; edges[i] = x; }
  edges[SB_PARTS - 1] = -1;
  SendMessageA(g_app.statusbar, SB_SETPARTS, SB_PARTS, (LPARAM)edges);
}

void statusbar_set(int part, const char* text)
{
  if (g_app.statusbar) SendMessageA(g_app.statusbar, SB_SETTEXTA, (WPARAM)part, (LPARAM)(text ? text : ""));
}

void app_status_note(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  es_vfmt(g_note, sizeof(g_note), fmt, ap);
  va_end(ap);
  statusbar_set(SB_PART_FLAGS, g_note);
  selftest_log("note: %s", g_note);
  if (g_app.frame) SetTimer(g_app.frame, TIMER_NOTE, 5000, 0);
}

void statusbar_note_expired()
{
  if (g_app.frame) KillTimer(g_app.frame, TIMER_NOTE);
  g_note[0] = 0;
  app_update_status();
}

void app_update_status()
{
  int i;
  // T07: position, selection, char, language, encoding, EOL, INS/OVR, TEXT/HEX, flags (15.3)
  for (i = 0; i < SB_PARTS - 1; i++) statusbar_set(i, "");
  statusbar_set(SB_PART_FLAGS, g_note);
}
