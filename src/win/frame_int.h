// frame_int.h - private to the frame modules (main, frame, tabs, statusbar, notifybar, progress, util). Owner: T07.
#ifndef PICA_FRAME_INT_H
#define PICA_FRAME_INT_H

#include <windows.h>
#include "etypes.h"

// frame.cpp
int  frame_register(HINSTANCE h);
HWND frame_create(int show_cmd);
void frame_close();                      // WM_CLOSE: exit flow, placement, settings, DestroyWindow
// statusbar.cpp
HWND statusbar_create(HWND parent);
void statusbar_layout();                 // part widths after a resize
void statusbar_set(int part, const char* text);
void statusbar_note_expired();           // TIMER_NOTE
// main.cpp
const char* main_ini_path(char* out, u32 cap);   // <cfg_dir>\pica.ini

#endif
