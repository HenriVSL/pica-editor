// fifworker.h - find-in-files worker, results panel, incremental search bar (DESIGN 11.5, 11.6).
// fifworker.cpp (T11): fif_start/stop/on_msg. fifpanel.cpp (T11): results_*. isearch.cpp (T11): isearch_*.
// Owner after Wave 0: overseer.
#ifndef PICA_FIFWORKER_H
#define PICA_FIFWORKER_H

#include <windows.h>
#include "etypes.h"
#include "fif.h"

int  fif_start(const FifQuery& q);       // starts the worker (stops a running search first)
void fif_stop();
int  fif_running();
void fif_on_msg(WPARAM w, LPARAM l);     // WM_APP_FIF
HWND results_create(HWND parent);        // the results panel window (created hidden by the frame)
void results_show(int on);
int  results_visible();
HWND isearch_create(HWND parent);        // the incremental search bar (created hidden by the frame)
void isearch_show();
void isearch_hide();

#endif
