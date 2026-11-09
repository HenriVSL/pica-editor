// progress.h - modal progress window and status-bar progress (DESIGN 15.10). Implemented in progress.cpp (T07).
// Owner after Wave 0: overseer.
#ifndef PICA_PROGRESS_H
#define PICA_PROGRESS_H

#include <windows.h>
#include "etypes.h"

class UiProgress : public ProgressCb {   // appears 500 ms after construction; pumps paint/timer/mouse messages
public:
  UiProgress(const char* title, const char* text, int can_cancel);
  ~UiProgress();
  int  progress(u32 done, u32 total);    // nonzero = cancelled
  void set_text(const char* text);
  int  cancelled() const;
private:                                 // private state: T07 may change freely
  char title_[64]; char text_[260]; int can_cancel_; int cancelled_; u32 start_ms_; HWND wnd_;
  UiProgress(const UiProgress&); UiProgress& operator=(const UiProgress&);
};
int  progress_status_begin(const char* label);   // non-modal status-bar progress; returns a handle (> 0)
void progress_status_step(int h, u32 pct);
void progress_status_end(int h);

#endif
