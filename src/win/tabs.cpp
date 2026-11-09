// tabs.cpp - Tab lifecycle and the tab list (DESIGN 15.1). T00: working basics (no tab bar control yet); T07 adds
// the tab bar, view attach details and state save/restore. Owner after Wave 0: T07.
#include <windows.h>
#include "app.h"
#include "tab.h"
#include "ui.h"
#include "views.h"
#include "notifybar.h"
#include "sessmgr.h"
#include "estr.h"
#include <string.h>

static void tab_zero(Tab* t)
{
  t->id = 0; t->doc = 0; t->hl = 0; t->lang = 0;
  t->lang_forced = t->enc_forced = t->untitled = 0; t->view = VIEW_TEXT;
  t->untitled_no = 0; t->path[0] = 0;
  t->sel.anchor = t->sel.caret = 0;
  memset(&t->tv, 0, sizeof(t->tv));
  memset(&t->hv, 0, sizeof(t->hv));
  memset(&t->disk, 0, sizeof(t->disk));
  t->disk_missing = t->shared_ro = t->change_pending = t->ondemand = 0;
  t->fsrc = 0;
  t->large = t->indexing = t->saving = t->ro_attr = 0;
  t->index_pct = t->save_pct = 0;
  memset(&t->notify, 0, sizeof(t->notify));
  t->backup_gen = t->backup_seq = t->dirty_since_ms = 0;
  t->has_backup = t->backup_note_shown = 0; t->pad[0] = t->pad[1] = 0;
  t->io_pending = 0;
}

Tab* tab_new(Document* doc)
{
  Tab* t = new Tab;
  if (!t) return 0;
  tab_zero(t);
  t->hl = new Highlighter;
  if (!t->hl) { delete t; return 0; }
  t->doc = doc;
  t->id = g_app.next_tabid++;
  t->tv.tab_width = (u8)g_app.cfg.tab_width;
  t->tv.use_spaces = g_app.cfg.use_spaces;
  t->tv.wrap = g_app.cfg.wrap_default;
  t->hv.caret.insert = (u8)(g_app.cfg.hex_overwrite ? 0 : 1);
  if (doc) {
    t->lines.bind(doc, t->hl);
    t->bookmarks.attach(doc);
    t->marks.attach(doc);
    t->hl->attach(&t->lines, 0);
  }
  return t;
}

void tab_free(Tab* t)
{
  if (!t) return;
  t->marks.detach();
  t->bookmarks.detach();
  t->lines.unbind();
  if (t->hl) { t->hl->detach(); delete t->hl; }
  if (t->doc) delete t->doc;
  delete t;
}

Tab* tab_find_path(const char* path)
{
  u32 i;
  if (!path || !path[0]) return 0;
  for (i = 0; i < g_app.tabs.size(); i++) {
    Tab* t = g_app.tabs[i];
    if (!t->untitled && t->path[0] && ui_path_equal(t->path, path)) return t;
  }
  return 0;
}

void tab_title(Tab* t, char* out, u32 cap)
{
  if (t->untitled || !t->path[0]) es_fmt(out, cap, "new %u", t->untitled_no);
  else es_copy(out, cap, ui_path_name(t->path));
}

int app_tab_count() { return (int)g_app.tabs.size(); }
Tab* app_tab(int i) { return (i >= 0 && i < (int)g_app.tabs.size()) ? g_app.tabs[(u32)i] : 0; }
Tab* app_active() { return app_tab(g_app.active); }
int app_active_index() { return g_app.active; }

int app_tab_index(Tab* t)
{
  u32 i;
  for (i = 0; i < g_app.tabs.size(); i++) if (g_app.tabs[i] == t) return (int)i;
  return -1;
}

void app_activate(int i)
{
  Tab* t;
  if (i < 0 || i >= (int)g_app.tabs.size()) i = g_app.tabs.size() ? 0 : -1;
  g_app.active = i;
  t = app_active();
  if (g_app.textview) textview_attach(g_app.textview, t && t->view == VIEW_TEXT ? t : 0);
  if (g_app.hexview) hexview_attach(g_app.hexview, t && t->view == VIEW_HEX ? t : 0);
  notifybar_update();
  app_layout();
  app_update_title();
  app_update_status();
  sessmgr_state_changed();
}

int app_add_tab(Tab* t, int activate)
{
  if (g_app.tabs.push(t) != E_OK) return E_NOMEM;
  if (activate || g_app.active < 0) app_activate((int)g_app.tabs.size() - 1);
  return E_OK;
}

void app_remove_tab(Tab* t)
{
  int i = app_tab_index(t);
  if (i < 0) return;
  g_app.tabs.erase((u32)i, 1);
  if (g_app.active > i) g_app.active--;
  else if (g_app.active == i) {
    int n = (int)g_app.tabs.size();
    app_activate(n == 0 ? -1 : (i < n ? i : n - 1));
    return;
  }
  app_update_title();
}

void app_update_tab(Tab* t)
{
  if (t == app_active()) app_update_title();   // T07: tab label text (modified marker, (saving), (indexing))
}

void app_on_doc_changed(Tab* t)
{
  app_update_tab(t);
  sessmgr_doc_changed(t);
}
