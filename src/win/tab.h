// tab.h - one open document (DESIGN 15.1). Owner after Wave 0: overseer (implementation in tabs.cpp, T07).
#ifndef PICA_TAB_H
#define PICA_TAB_H

#include <windows.h>
#include "etypes.h"
#include "doc.h"
#include "lexer.h"
#include "docbind.h"
#include "marks.h"
#include "edops.h"
#include "fsys.h"
#include "fileops.h"
#include "notifybar.h"

enum { VIEW_TEXT = 0, VIEW_HEX = 1 };
struct TextViewState {
  u32 top_line, top_subrow, hscroll, desired_cell;  // hscroll in cells; desired_cell for Up/Down
  u8 wrap, tab_width, use_spaces, overwrite;
};
struct HexViewState { u32 top_row; HexCaret caret; };
struct Tab {
  u32 id;                                 // unique per session (backup file names, session)
  Document* doc;                          // owned
  Highlighter* hl;                        // owned
  DocLines lines;                         // binds doc -> hl
  LineMarks bookmarks;
  RangeMarks marks;                       // Mark All / incremental search highlights
  const LangInfo* lang;                   // NULL = plain text
  u8 lang_forced, enc_forced, untitled, view;   // view: VIEW_TEXT / VIEW_HEX
  u32 untitled_no;                        // "new 3"
  char path[260];                         // full path; empty for untitled
  Selection sel;                          // byte offsets (shared by both views)
  TextViewState tv;
  HexViewState hv;
  // disk state (change detection, 13.9)
  FsStat disk; u8 disk_missing, shared_ro, change_pending, ondemand;
  FileSource* fsrc;                       // on-demand source (owned by doc), NULL for in-memory documents
  u8 large, indexing, saving, ro_attr;    // large mode; indexer running; save running; read-only attribute on disk
  u32 index_pct, save_pct;
  NotifyState notify;
  // backups (13.5)
  u32 backup_gen, backup_seq, dirty_since_ms; u8 has_backup, backup_note_shown, pad[2];
  u32 io_pending;                         // I/O jobs queued for this tab
};
// Takes ownership of doc on success; creates the Highlighter, binds lines/marks; new id. On failure (no memory) it
// returns NULL and doc stays the caller's (delete it).
Tab* tab_new(Document* doc);
void tab_free(Tab* t);                   // unbinds and frees everything (tab must be out of the bar, no pending I/O)
Tab* tab_find_path(const char* path);    // open tab with this full path (case-insensitive), or NULL
void tab_title(Tab* t, char* out, u32 cap);   // "name.ext" or "new 3"
// Call after setting t->lang: when cfg.use_lang_tabs is on and the tab has a language, copies the language's tab width
// and tabs/spaces choice into t->tv (use_tabs 2 = the global use_spaces) and repaints the text view if t is active.
// Returns 1 if t->tv changed. Does nothing for plain text or with the option off.
int  tab_apply_lang_tabs(Tab* t);

#endif
