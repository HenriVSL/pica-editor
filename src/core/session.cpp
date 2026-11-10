// session.cpp - session model to/from INI (DESIGN 6.19, 13.6). Owner: T06.
//
// Format (13.6): [Session] version=1 seq active next_tabid next_untitled clean_exit count, then [Tab0]..[TabN-1],
// then "[end] ok=1" as the last section. session_from_ini is strict about structure (sentinel, version, every
// [TabK] with an id) because the A/B scheme has an older copy to fall back to, and tolerant about values (clamped
// or defaulted). session_validate runs the full parse, so whatever ab_read accepts also loads.
#include "session.h"
#include "estr.h"
#include <string.h>

enum { SESSION_VERSION = 1, SESSION_MAX_TABS = 4096, SESSION_MAX_BOOKMARKS = 100000 };

void session_init(Session* s)
{
  s->seq = 0; s->active = 0; s->next_tabid = 1; s->next_untitled = 1; s->clean_exit = 0;
  s->pad[0] = s->pad[1] = s->pad[2] = 0;
  s->tabs.clear();
}

void session_free(Session* s)
{
  u32 i;
  for (i = 0; i < s->tabs.size(); i++) delete s->tabs[i];
  s->tabs.release();
}

static void tab_defaults(SessionTab* t)
{
  t->id = 0; t->path[0] = 0; t->untitled = 0; t->view = 0; t->hex_pane = 0; t->hex_insert = 0; t->untitled_no = 0;
  t->enc = enc_ansi(); t->enc_forced = 0; t->eol = EOL_CRLF; t->lang_forced = 0; t->wrap = 0; t->lang[0] = 0;
  t->tab_width = 4; t->use_spaces = 0; t->read_only = 0; t->modified = 0; t->has_backup = 0; t->disk_missing = 0;
  t->pad[0] = t->pad[1] = 0;
  t->caret = 0; t->anchor = 0; t->top_line = 0; t->top_subrow = 0; t->hex_top = 0;
  t->disk_size = 0; t->disk_mtime_lo = 0; t->disk_mtime_hi = 0;
  t->bookmarks.clear();
}

static const char* const k_eol[3] = { "crlf", "lf", "cr" };

static int put_u(Ini* ini, const char* sec, const char* key, u32 v)
{
  char buf[16];
  es_fmt(buf, sizeof(buf), "%u", v);
  return ini->set(sec, key, buf);
}

static int put_tab(Ini* ini, const char* sec, const SessionTab& t)
{
  char buf[64];
  EStr bm;
  u32 i;
  int rc = E_OK;
#define PUT(x) do { if (rc == E_OK) rc = (x); } while (0)
  PUT(put_u(ini, sec, "id", t.id));
  PUT(ini->set(sec, "path", t.path));
  PUT(put_u(ini, sec, "untitled", t.untitled ? 1 : 0));
  PUT(put_u(ini, sec, "untitled_no", t.untitled_no));
  PUT(ini->set(sec, "view", t.view ? "hex" : "text"));
  PUT(put_u(ini, sec, "hex_pane", t.hex_pane ? 1 : 0));
  PUT(put_u(ini, sec, "hex_insert", t.hex_insert ? 1 : 0));
  enc_token(t.enc, buf, sizeof(buf));
  PUT(ini->set(sec, "enc", buf));
  PUT(put_u(ini, sec, "enc_forced", t.enc_forced ? 1 : 0));
  PUT(ini->set(sec, "eol", k_eol[t.eol < 3 ? t.eol : 0]));
  PUT(ini->set(sec, "lang", t.lang));
  PUT(put_u(ini, sec, "lang_forced", t.lang_forced ? 1 : 0));
  PUT(put_u(ini, sec, "wrap", t.wrap ? 1 : 0));
  PUT(put_u(ini, sec, "tab_width", t.tab_width));
  PUT(put_u(ini, sec, "use_spaces", t.use_spaces ? 1 : 0));
  PUT(put_u(ini, sec, "read_only", t.read_only ? 1 : 0));
  PUT(put_u(ini, sec, "caret", t.caret));
  PUT(put_u(ini, sec, "anchor", t.anchor));
  PUT(put_u(ini, sec, "top_line", t.top_line));
  PUT(put_u(ini, sec, "top_subrow", t.top_subrow));
  PUT(put_u(ini, sec, "hex_top", t.hex_top));
  for (i = 0; i < t.bookmarks.size() && rc == E_OK; i++) {
    es_fmt(buf, sizeof(buf), i ? ",%u" : "%u", t.bookmarks[i]);
    if (bm.append(buf) != E_OK) rc = E_NOMEM;
  }
  PUT(ini->set(sec, "bookmarks", bm.c_str()));
  PUT(put_u(ini, sec, "modified", t.modified ? 1 : 0));
  PUT(put_u(ini, sec, "has_backup", t.has_backup ? 1 : 0));
  PUT(put_u(ini, sec, "disk_size", t.disk_size));
  es_fmt(buf, sizeof(buf), "%08X%08X", t.disk_mtime_hi, t.disk_mtime_lo);
  PUT(ini->set(sec, "disk_mtime", buf));
  PUT(put_u(ini, sec, "disk_missing", t.disk_missing ? 1 : 0));
#undef PUT
  return rc;
}

static int is_tab_section(const char* name)
{
  const char* p;
  if (es_nicmp(name, "Tab", 3) != 0 || !name[3]) return 0;
  for (p = name + 3; *p; p++) if (*p < '0' || *p > '9') return 0;
  return 1;
}

int session_to_ini(const Session& s, Ini* ini)
{
  u32 i;
  int rc = E_OK;
  char sec[16];
  if (!ini) return E_RANGE;
  // replace any previous session content; other sections are kept
  i = 0;
  while (i < ini->section_count()) {
    const char* nm = ini->section(i);
    if (es_icmp(nm, "Session") == 0 || es_icmp(nm, "end") == 0 || is_tab_section(nm)) {
      char copy[64];
      es_copy(copy, sizeof(copy), nm);
      ini->remove_section(copy);
      i = 0;
    } else {
      i++;
    }
  }
#define PUT(x) do { if (rc == E_OK) rc = (x); } while (0)
  PUT(put_u(ini, "Session", "version", SESSION_VERSION));
  PUT(put_u(ini, "Session", "seq", s.seq));
  PUT(put_u(ini, "Session", "active", s.active));
  PUT(put_u(ini, "Session", "next_tabid", s.next_tabid));
  PUT(put_u(ini, "Session", "next_untitled", s.next_untitled));
  PUT(put_u(ini, "Session", "clean_exit", s.clean_exit ? 1 : 0));
  PUT(put_u(ini, "Session", "count", s.tabs.size()));
  for (i = 0; i < s.tabs.size() && rc == E_OK; i++) {
    es_fmt(sec, sizeof(sec), "Tab%u", i);
    rc = s.tabs[i] ? put_tab(ini, sec, *s.tabs[i]) : E_STATE;
  }
  PUT(ini->set("end", "ok", "1"));
#undef PUT
  return rc;
}

// ---- reading ----

static int get_u(const Ini& ini, const char* sec, const char* key, u32 def, u32 lo, u32 hi, u32* out)
{
  const char* v = ini.get(sec, key);
  u32 x;
  if (!v || es_parse_u32(v, &x, 10) != E_OK) { *out = def; return 0; }
  if (x < lo) x = lo;
  if (x > hi) x = hi;
  *out = x;
  return 1;
}

static u8 get_flag(const Ini& ini, const char* sec, const char* key)
{
  return (u8)ini.get_b(sec, key, 0);
}

static int parse_hex64(const char* v, u32* hi, u32* lo)
{
  u32 n, i;
  u32 h = 0, l = 0;
  if (!v) return E_SYNTAX;
  n = es_len(v);
  if (n == 0 || n > 16) return E_SYNTAX;
  for (i = 0; i < n; i++) {
    char c = v[i];
    u32 d;
    if (c >= '0' && c <= '9') d = (u32)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
    else return E_SYNTAX;
    h = (h << 4) | (l >> 28);
    l = (l << 4) | d;
  }
  *hi = h; *lo = l;
  return E_OK;
}

// "10,20,30": sorted unique line numbers (hand edits tolerated: garbage skipped, order and duplicates fixed).
static int parse_bookmarks(const char* v, Vec<u32>* out)
{
  const char* p = v;
  out->clear();
  if (!v) return E_OK;
  while (*p) {
    u32 x = 0, lo, hi;
    int ok = 1;
    if (*p < '0' || *p > '9') { p++; continue; }
    while (*p >= '0' && *p <= '9') {
      u32 d = (u32)(*p - '0');
      if (x > (0xFFFFFFFFu - d) / 10) ok = 0;
      else x = x * 10 + d;
      p++;
    }
    if (!ok) continue;
    if (out->size() >= SESSION_MAX_BOOKMARKS) break;
    lo = 0; hi = out->size();
    while (lo < hi) { u32 mid = lo + (hi - lo) / 2; if ((*out)[mid] < x) lo = mid + 1; else hi = mid; }
    if (lo < out->size() && (*out)[lo] == x) continue;
    if (out->insert(lo, &x, 1) != E_OK) return E_NOMEM;
  }
  return E_OK;
}

static int read_tab(const Ini& ini, const char* sec, SessionTab* t)
{
  const char* v;
  u32 x;
  tab_defaults(t);
  if (!get_u(ini, sec, "id", 0, 0, 0xFFFFFFFFu, &t->id) || t->id == 0) return E_FORMAT;
  es_copy(t->path, sizeof(t->path), ini.get_s(sec, "path", ""));
  t->untitled = get_flag(ini, sec, "untitled");
  if (!t->untitled && !t->path[0]) return E_FORMAT;   // a named tab needs its path
  get_u(ini, sec, "untitled_no", 0, 0, 0xFFFFFFFFu, &t->untitled_no);
  v = ini.get(sec, "view");
  t->view = (u8)(v && es_icmp(v, "hex") == 0 ? 1 : 0);
  t->hex_pane = get_flag(ini, sec, "hex_pane");
  t->hex_insert = get_flag(ini, sec, "hex_insert");
  v = ini.get(sec, "enc");
  if (v) { EncSpec e; if (enc_parse(v, &e) == E_OK) t->enc = e; }
  t->enc_forced = get_flag(ini, sec, "enc_forced");
  v = ini.get(sec, "eol");
  if (v) for (x = 0; x < 3; x++) if (es_icmp(v, k_eol[x]) == 0) t->eol = (u8)x;
  es_copy(t->lang, sizeof(t->lang), ini.get_s(sec, "lang", ""));
  t->lang_forced = get_flag(ini, sec, "lang_forced");
  t->wrap = get_flag(ini, sec, "wrap");
  get_u(ini, sec, "tab_width", 4, 1, 16, &x); t->tab_width = (u8)x;
  t->use_spaces = get_flag(ini, sec, "use_spaces");
  t->read_only = get_flag(ini, sec, "read_only");
  get_u(ini, sec, "caret", 0, 0, EPOS_MAX, &t->caret);
  get_u(ini, sec, "anchor", t->caret, 0, EPOS_MAX, &t->anchor);
  get_u(ini, sec, "top_line", 0, 0, 0xFFFFFFFFu, &t->top_line);
  get_u(ini, sec, "top_subrow", 0, 0, 0xFFFFFFFFu, &t->top_subrow);
  get_u(ini, sec, "hex_top", 0, 0, EPOS_MAX, &t->hex_top);
  if (parse_bookmarks(ini.get(sec, "bookmarks"), &t->bookmarks) != E_OK) return E_NOMEM;
  t->modified = get_flag(ini, sec, "modified");
  t->has_backup = get_flag(ini, sec, "has_backup");
  get_u(ini, sec, "disk_size", 0, 0, 0xFFFFFFFFu, &t->disk_size);
  if (parse_hex64(ini.get(sec, "disk_mtime"), &t->disk_mtime_hi, &t->disk_mtime_lo) != E_OK) {
    t->disk_mtime_hi = t->disk_mtime_lo = 0;
  }
  t->disk_missing = get_flag(ini, sec, "disk_missing");
  return E_OK;
}

int session_from_ini(const Ini& ini, Session* s)
{
  u32 count, version, i;
  int rc;
  char sec[16];
  if (!s) return E_RANGE;
  session_free(s);
  session_init(s);
  if (!ini.get_b("end", "ok", 0)) return E_FORMAT;
  if (!get_u(ini, "Session", "version", 0, 0, 0xFFFFFFFFu, &version) || version != SESSION_VERSION) return E_FORMAT;
  if (!get_u(ini, "Session", "count", 0, 0, 0xFFFFFFFFu, &count) || count > SESSION_MAX_TABS) return E_FORMAT;
  get_u(ini, "Session", "seq", 0, 0, 0xFFFFFFFFu, &s->seq);
  get_u(ini, "Session", "active", 0, 0, 0xFFFFFFFFu, &s->active);
  get_u(ini, "Session", "next_tabid", 1, 1, 0xFFFFFFFFu, &s->next_tabid);
  get_u(ini, "Session", "next_untitled", 1, 1, 0xFFFFFFFFu, &s->next_untitled);
  s->clean_exit = (u8)ini.get_b("Session", "clean_exit", 0);
  if (s->tabs.reserve(count) != E_OK) return E_NOMEM;
  for (i = 0; i < count; i++) {
    SessionTab* t;
    u32 j;
    es_fmt(sec, sizeof(sec), "Tab%u", i);
    t = new SessionTab;
    if (!t) { rc = E_NOMEM; goto fail; }
    rc = read_tab(ini, sec, t);
    if (rc == E_OK) {
      for (j = 0; j < s->tabs.size(); j++) if (s->tabs[j]->id == t->id) { rc = E_FORMAT; break; }   // ids unique
    }
    if (rc != E_OK) { delete t; goto fail; }
    s->tabs.push(t);                                  // reserved: cannot fail
    if (t->id >= s->next_tabid && t->id != 0xFFFFFFFFu) s->next_tabid = t->id + 1;   // never reuse a tab id
    if (t->untitled && t->untitled_no >= s->next_untitled && t->untitled_no != 0xFFFFFFFFu)
      s->next_untitled = t->untitled_no + 1;
  }
  if (s->active >= count) s->active = 0;
  return E_OK;
fail:
  session_free(s);
  session_init(s);
  return rc;
}

int session_validate(const u8* p, u32 n, u32* seq)
{
  Ini ini;
  Session s;
  int rc;
  if (seq) *seq = 0;
  if (!p && n) return E_FORMAT;
  rc = ini.parse((const char*)p, n);
  if (rc != E_OK) return rc;
  session_init(&s);
  rc = session_from_ini(ini, &s);
  if (rc == E_OK && seq) *seq = s.seq;
  session_free(&s);
  return rc;
}
