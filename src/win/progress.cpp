// progress.cpp - modal progress window and status-bar progress (DESIGN 15.10). STUB from T00. Owner after Wave 0: T07.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "progress.h"
#include "estr.h"

UiProgress::UiProgress(const char* title, const char* text, int can_cancel)
  : can_cancel_(can_cancel), cancelled_(0), start_ms_(GetTickCount()), wnd_(0)
{
  es_copy(title_, sizeof(title_), title);
  es_copy(text_, sizeof(text_), text);
}
UiProgress::~UiProgress() {}
int UiProgress::progress(u32 done, u32 total) { (void)done; (void)total; return cancelled_; }
void UiProgress::set_text(const char* text) { es_copy(text_, sizeof(text_), text); }
int UiProgress::cancelled() const { return cancelled_; }
int progress_status_begin(const char* label) { (void)label; return 1; }
void progress_status_step(int h, u32 pct) { (void)h; (void)pct; }
void progress_status_end(int h) { (void)h; }
