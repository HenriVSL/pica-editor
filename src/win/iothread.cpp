// iothread.cpp - I/O worker thread (DESIGN 4.3, 13.3, 13.5, 13.6). STUB from T00. Owner after Wave 0: T09 (T13 adds backup/session/delete jobs).
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "cmds.h"
#include "ui.h"
#include "iothread.h"

int iothread_start() { return E_OK; }
void iothread_stop() {}
u32 iothread_submit(IoJob* job) { delete job; return 0; }
void iothread_cancel_tab(u32 tabid) { (void)tabid; }
int iothread_wait_tab(u32 tabid, u32 timeout_ms) { (void)tabid; (void)timeout_ms; return 1; }
int iothread_flush(u32 timeout_ms) { (void)timeout_ms; return 1; }
void iothread_on_done(WPARAM w, LPARAM l) { (void)w; (void)l; }
