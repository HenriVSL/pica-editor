// frame_int.h - private to the frame modules (main, frame, tabs, statusbar, notifybar, progress, util). Owner: T07.
#ifndef PICA_FRAME_INT_H
#define PICA_FRAME_INT_H

#include <windows.h>
#include "etypes.h"
#include "emem.h"

struct Tab;
struct Config;

// Frame-private window messages (sent/posted only to the frame window).
enum {
  WM_APP_TABCLOSE = WM_APP + 0x40,       // wParam = tab id: close that tab (asks to save)
  WM_APP_FRAME_LAST
};
enum { FRAME_SPLIT_H = 5 };              // results panel splitter height (px)

// frame.cpp
int  frame_register(HINSTANCE h);
HWND frame_create(int show_cmd);
void frame_close();                      // WM_CLOSE: exit flow, placement, settings, DestroyWindow
void frame_ui_dirty();                   // toolbar state / tab labels / title refresh on the next idle slice
void frame_idle();                       // deferred UI refresh (called from app_idle_work)
int  frame_close_tab(Tab* t, int ask);   // docio_close(t, ask) with a direct fallback while docio is a stub
void frame_rebuild_menu(HMENU popup);    // MRU / Window lists of a popup (WM_INITMENUPOPUP), then menu state
void frame_toolbar_update();             // enable/check toolbar buttons from cmd_state
int  frame_toolbar_state(int id);        // test: TB_GETSTATE of a button (-1 if none)
void frame_ctx_tab(Tab* t);              // tab the IDM_TAB_* commands act on (NULL = active tab)
void frame_splitter_rect(RECT* r);       // empty when the results panel is hidden

// tabs.cpp (tab list, tab bar control)
HWND tabbar_create(HWND parent);
int  tabbar_height();                    // 0 when the tab bar is hidden (no tabs)
void tabbar_rebuild();                   // all items from g_app.tabs (order, labels, selection)
int  tabbar_move(int from, int to);      // reorder (drag): moves the tab, keeps the active tab active
void tabbar_draw(const DRAWITEMSTRUCT* d);
const char* tabbar_tooltip(int i);       // full path of tab i ("new 3 (not saved yet)" for untitled)
void tab_label(Tab* t, char* out, u32 cap);   // "name.ext *", "(saving)", "(indexing 43%)" suffixes
void tabs_idle();                        // labels of tabs whose documents changed
int  tabbar_close_rect(int i, RECT* r);  // close box of tab i (only the active tab has one)
Tab* tab_by_id(u32 id);

// statusbar.cpp
HWND statusbar_create(HWND parent);
void statusbar_layout();                 // part widths (text or hex set) after a resize / view switch
void statusbar_set(int part, const char* text);
void statusbar_note_expired();           // TIMER_NOTE
void statusbar_note_ms(u32 ms, const char* fmt, ...);   // note with a custom lifetime
int  statusbar_part_at(int x);           // part index at a client x of the status bar, -1 if none
void statusbar_click(int part);          // NM_CLICK: Go To, Syntax/Encoding/EOL popups, INS/OVR, TEXT/HEX
HMENU statusbar_popup_menu(int part);    // the main-menu submenu a part pops up (NULL for other parts)
void statusbar_get(int part, char* out, u32 cap);   // current text (tests)

// notifybar.cpp
void notifybar_on_msg(WPARAM w, LPARAM l);   // WM_APP_NOTIFY: run the callback of the clicked button
void notifybar_tick();                       // 1 s tick: auto-hide
HWND notifybar_button(int index);            // test: button window (index 0..nbuttons-1, NB_MAX_BUTTONS = close)
void notifybar_close_click();                // the [x] (tests and the bar's own close box)

// progress.cpp
int  progress_status_text(char* out, u32 cap);   // "Saving 20%, Indexing 3%" of the running status progresses
int  progress_window_visible();                  // test: a UiProgress window is on screen
HWND progress_window();

// main.cpp
const char* main_ini_path(char* out, u32 cap);   // <cfg_dir>\pica.ini
int  main_save_config_to(const char* path);      // config_store into the file at path (keeps unknown keys)
int  main_load_config_from(const char* path, Config* out);
// WM_COPYDATA from a second instance: block = "PICA1\0<cwd>\0<arg>\0...<arg>\0\0"
LRESULT main_on_copydata(HWND from, const COPYDATASTRUCT* cds);
void main_on_copydata_open(WPARAM w, LPARAM l);  // WM_APP_COPYDATA_OPEN (posted by main_on_copydata)
struct MainFileArg { char path[260]; u32 line; u8 hex, ro, pad[2]; };
int  main_parse_block(const char* block, u32 size, Vec<MainFileArg>* out);   // test hook; resolves paths
void main_accept_copydata_in_selftest(int on);   // st_frame only
const char* main_lang_errors();                  // errors from lang_init ("" if none)

#endif
