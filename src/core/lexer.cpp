// lexer.cpp - language registry, find-results lexer and the incremental Highlighter (DESIGN 6.13). Owner: T04.
//
// Highlighter: one u16 start state per line in a gap buffer (bit 15 = "line changed since the next line's state was
// computed", 0x7FFF = unknown). valid_upto_ = number of leading lines whose start state is correct. An edit at line L
// sets valid_upto_ <= L + 1, flags line L and inserts unknown states for new lines; the old states after the edit stay
// as candidates. Lexing forward stores each computed end state; when it equals the stored one the rest is unchanged,
// and the valid prefix jumps over every following line that is neither flagged nor unknown (convergence). Idle work
// only lexes up to the target line (the viewport); style_line catches up synchronously when the gap is small.
#include "lexer_int.h"

enum { HL_SENT = 0x7FFF, HL_FLAG = 0x8000, HL_SYNC_MAX = 200, HL_FAST_UNIT = 4096 };

// ---------------------------------------------------------------------------------------------------------------
// find-results lexer: header "Search ..." HEADING, file lines SECTION, "    NNN:" line numbers NUMBER

class ResultsLexer : public Lexer {
public:
  u16 lex_line(const ecp* s, u32 n, u16 state, u8* st) const
  {
    u32 j, e;
    (void)state;
    if (n > LEX_LINE_MAX) { lex_fill(st, LEX_LINE_MAX, n, ST_DEFAULT); n = LEX_LINE_MAX; }
    lex_fill(st, 0, n, ST_DEFAULT);
    if (n >= 7 && s[0] == 'S' && s[1] == 'e' && s[2] == 'a' && s[3] == 'r' && s[4] == 'c' && s[5] == 'h' &&
        s[6] == ' ') {
      lex_fill(st, 0, n, ST_HEADING);
      return 0;
    }
    if (n && (s[0] == ' ' || s[0] == '\t')) {
      j = 0;
      while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
      e = j;
      while (e < n && lex_is_digit(s[e])) e++;
      if (e > j && e < n && s[e] == ':') lex_fill(st, j, e + 1, ST_NUMBER);
      return 0;
    }
    lex_fill(st, 0, n, ST_SECTION);
    return 0;
  }
};

static ResultsLexer g_results_lexer;
static LangInfo g_results_info;
static int g_results_init = 0;

const LangInfo* lang_results()
{
  if (!g_results_init) {
    memset(&g_results_info, 0, sizeof(g_results_info));
    es_copy(g_results_info.id, sizeof(g_results_info.id), "results");
    es_copy(g_results_info.name, sizeof(g_results_info.name), "Find results");
    g_results_info.use_tabs = 2;
    g_results_info.case_sensitive = 1;
    g_results_info.lexer = &g_results_lexer;
    g_results_init = 1;
  }
  return &g_results_info;
}

// ---------------------------------------------------------------------------------------------------------------
// registry

struct LangRec { LangInfo info; u32 kind; char script[16]; char style[16]; };

static Vec<LangRec*> g_recs;      // menu order: built-ins in source order, then new override languages
static Vec<Lexer*> g_lexers;      // every lexer created by lang_init / lang_parse (freed by lang_shutdown)

void lang_shutdown()
{
  u32 i;
  for (i = 0; i < g_recs.size(); i++) e_free(g_recs[i]);
  for (i = 0; i < g_lexers.size(); i++) delete g_lexers[i];
  g_recs.release();
  g_lexers.release();
}

static const char* base_name(const char* p)
{
  const char* b = p;
  if (!p) return "";
  for (; *p; p++) if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
  return b;
}

static void id_from(char* id, const char* s, int stem)    // lowercase alnum/_/- of s (up to the last '.' if stem)
{
  u32 n = 0;
  const char* dot = 0;
  const char* p;
  if (stem) for (p = s; *p; p++) if (*p == '.') dot = p;
  for (p = s; *p && p != dot && n < 15; p++) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if (lex_is_alnum((u8)c) || c == '_' || c == '-') id[n++] = c;
  }
  id[n] = 0;
  if (!n) es_copy(id, 16, "lang");
}

static int parse_keep(const char* text, u32 len, LangRec* rec, EStr* err)
{
  Lexer* lex = 0;
  LngExtra ex;
  int r;
  memset(rec, 0, sizeof(*rec));
  r = lng_parse(text, len, &rec->info, &ex, &lex, err);
  if (r != E_OK) return r;
  if (g_lexers.push(lex) != E_OK) { delete lex; return E_NOMEM; }
  rec->info.lexer = lex;
  rec->kind = ex.kind;
  es_copy(rec->script, sizeof(rec->script), ex.script);
  es_copy(rec->style, sizeof(rec->style), ex.style);
  return E_OK;
}

int lang_parse(const char* text, u32 len, LangInfo* out, EStr* err)
{
  LangRec rec;
  int r = parse_keep(text, len, &rec, err);
  if (r != E_OK) { memset(out, 0, sizeof(*out)); return r; }
  if (!rec.info.id[0]) id_from(rec.info.id, rec.info.name, 0);
  *out = rec.info;
  return E_OK;
}

static LangRec* find_rec(const char* id)
{
  u32 i;
  if (!id) return 0;
  for (i = 0; i < g_recs.size(); i++) if (es_icmp(g_recs[i]->info.id, id) == 0) return g_recs[i];
  return 0;
}

static int add_rec(const LangRec& r)
{
  LangRec* p = (LangRec*)e_alloc(sizeof(LangRec));
  if (!p) return E_NOMEM;
  *p = r;
  if (g_recs.push(p) != E_OK) { e_free(p); return E_NOMEM; }
  return E_OK;
}

static void err_line(EStr* errors, const char* prefix, const char* name, const char* msg, const char* suffix)
{
  if (!errors) return;
  errors->appendf("%s%.80s: %.300s%s\n", prefix, name, msg, suffix);
}

int lang_init(LangSource* builtin, LangSource* overrides, EStr* errors)
{
  Vec<u8> used;
  u32 nb = builtin ? builtin->count() : 0, no = overrides ? overrides->count() : 0, i, j;
  int rc = E_OK, r;
  const char* name;
  const char* text;
  u32 len;

  lang_shutdown();
  if (used.resize(no) != E_OK) return E_NOMEM;
  for (i = 0; i < nb; i++) {
    LangRec rec;
    int have = 0;
    if (builtin->get(i, &name, &text, &len) != E_OK || !name) { rc = E_FORMAT; err_line(errors, "built-in ", "?", "cannot read", ""); continue; }
    for (j = 0; j < no && !have; j++) {
      const char* on;
      const char* ot;
      u32 ol;
      if (used[j] || overrides->get(j, &on, &ot, &ol) != E_OK || !on) continue;
      if (es_icmp(base_name(on), base_name(name)) != 0) continue;
      used[j] = 1;
      {
        EStr e;
        r = parse_keep(ot, ol, &rec, &e);
        if (r == E_NOMEM) return E_NOMEM;
        if (r == E_OK) {
          if (!rec.info.id[0]) id_from(rec.info.id, base_name(name), 1);
          if (find_rec(rec.info.id)) { err_line(errors, "", on, "duplicate language id", " (using the built-in definition)"); continue; }
          have = 1;
        } else {
          err_line(errors, "", on, e.c_str(), " (using the built-in definition)");
        }
      }
    }
    if (!have) {
      EStr e;
      r = parse_keep(text, len, &rec, &e);
      if (r == E_NOMEM) return E_NOMEM;
      if (r != E_OK) { rc = E_FORMAT; err_line(errors, "built-in ", name, e.c_str(), ""); continue; }
      if (!rec.info.id[0]) id_from(rec.info.id, base_name(name), 1);
      if (find_rec(rec.info.id)) { rc = E_FORMAT; err_line(errors, "built-in ", name, "duplicate language id", ""); continue; }
    }
    if (add_rec(rec) != E_OK) return E_NOMEM;
  }
  for (j = 0; j < no; j++) {                               // new languages from the override folder
    LangRec rec;
    EStr e;
    if (used[j] || overrides->get(j, &name, &text, &len) != E_OK || !name) continue;
    r = parse_keep(text, len, &rec, &e);
    if (r == E_NOMEM) return E_NOMEM;
    if (r != E_OK) { err_line(errors, "", name, e.c_str(), " (skipped)"); continue; }
    if (!rec.info.id[0]) id_from(rec.info.id, base_name(name), 1);
    if (find_rec(rec.info.id)) { err_line(errors, "", name, "duplicate language id", " (skipped)"); continue; }
    if (g_recs.size() >= 40) { err_line(errors, "", name, "too many languages (max 40)", " (skipped)"); continue; }
    if (add_rec(rec) != E_OK) return E_NOMEM;
  }
  for (i = 0; i < g_recs.size(); i++) {                    // HTML: link the <script> / <style> languages
    LangRec* p = g_recs[i];
    if (p->kind == LK_HTML) {
      LangRec* js = find_rec(p->script);
      LangRec* css = find_rec(p->style);
      ((HtmlLexer*)p->info.lexer)->set_sub(js && js->kind == LK_GENERIC ? (const GenLexer*)js->info.lexer : 0,
                                           css && css->kind == LK_GENERIC ? (const GenLexer*)css->info.lexer : 0);
    }
  }
  return rc;
}

u32 lang_count() { return g_recs.size(); }
const LangInfo* lang_at(u32 i) { return i < g_recs.size() ? &g_recs[i]->info : 0; }
const LangInfo* lang_find(const char* id)
{
  LangRec* r = find_rec(id);
  return r ? &r->info : 0;
}

static int list_has(const char* list, const char* item, int wild)
{
  char buf[100];
  const char* p = list;
  while (*p) {
    u32 n = 0;
    while (*p && *p != ';') { if (n + 1 < sizeof(buf)) buf[n++] = *p; p++; }
    buf[n] = 0;
    if (*p) p++;
    if (n && (wild ? es_wildcard(buf, item) : es_icmp(buf, item) == 0)) return 1;
  }
  return 0;
}

const LangInfo* lang_for_file(const char* basename)
{
  const char* b = base_name(basename);
  const char* ext = 0;
  const char* p;
  u32 i;
  if (!*b) return 0;
  for (i = g_recs.size(); i > 0; i--) if (g_recs[i - 1]->info.names[0] && list_has(g_recs[i - 1]->info.names, b, 1)) return &g_recs[i - 1]->info;
  for (p = b; *p; p++) if (*p == '.') ext = p + 1;
  if (!ext || !*ext) return 0;
  for (i = g_recs.size(); i > 0; i--) if (list_has(g_recs[i - 1]->info.exts, ext, 0)) return &g_recs[i - 1]->info;
  return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Highlighter

Highlighter::Highlighter()
  : lines_(0), lang_(0), enabled_(true), valid_upto_(0), gb_(0), gcap_(0), gs_(0), gl_(0), n_(0), target_(0), lexed_(0),
    probed_(0xFFFFFFFFu), dfirst_(0), dlast_(0), ufirst_(0), ulast_(0), dirty_(false), unk_(false), broken_(false),
    buf_(0) {}

Highlighter::~Highlighter() { detach(); }

bool Highlighter::active_() const
{
  return lines_ && lang_ && lang_->lexer && enabled_ && !broken_ && gb_ && buf_;
}

u16 Highlighter::gget_(u32 i) const { return i < gs_ ? gb_[i] : gb_[i + gl_]; }
void Highlighter::gset_(u32 i, u16 v) { if (i < gs_) gb_[i] = v; else gb_[i + gl_] = v; }

void Highlighter::gmove_(u32 at)
{
  if (at < gs_) memmove(gb_ + at + gl_, gb_ + at, (gs_ - at) * sizeof(u16));
  else if (at > gs_) memmove(gb_ + gs_, gb_ + gs_ + gl_, (at - gs_) * sizeof(u16));
  gs_ = at;
}

int Highlighter::greserve_(u32 extra)
{
  u32 nc, suf;
  u16* nb;
  if (gl_ >= extra) return E_OK;
  if (extra > 0x3FFFFFFFu - n_) return E_NOMEM;
  nc = emax(gcap_ * 2, n_ + extra + 256);
  if (nc > 0x3FFFFFFFu) nc = n_ + extra + 256;
  nb = (u16*)e_alloc(nc * (u32)sizeof(u16));
  if (!nb) return E_NOMEM;
  suf = n_ - gs_;
  memcpy(nb, gb_, gs_ * sizeof(u16));
  memcpy(nb + nc - suf, gb_ + gs_ + gl_, suf * sizeof(u16));
  e_free(gb_);
  gb_ = nb; gcap_ = nc; gl_ = nc - n_;
  return E_OK;
}

int Highlighter::gresize_(u32 n)
{
  u32 cap = n + 256, i;
  u16* nb;
  if (n > 0x3FFFFFFFu) return E_NOMEM;
  nb = (u16*)e_alloc(cap * (u32)sizeof(u16));
  if (!nb) return E_NOMEM;
  e_free(gb_);
  gb_ = nb; gcap_ = cap; gs_ = n; gl_ = cap - n; n_ = n;
  for (i = 0; i < n; i++) gb_[i] = HL_SENT | HL_FLAG;
  if (n) gb_[0] = 0 | HL_FLAG;
  return E_OK;
}

void Highlighter::mark_dirty_(u32 a, u32 b)
{
  if (a > b) return;
  if (!dirty_) { dfirst_ = a; dlast_ = b; dirty_ = true; return; }
  if (a < dfirst_) dfirst_ = a;
  if (b > dlast_) dlast_ = b;
}

void Highlighter::advance_(u32 to)
{
  if (unk_ && ufirst_ < to) {                     // lines painted while unknown are known now: repaint them
    mark_dirty_(ufirst_, emin(ulast_, to - 1));
    if (ulast_ < to) unk_ = false; else ufirst_ = to;
  }
  valid_upto_ = to;
}

// lexes line valid_upto_ - 1 and stores the start state of the next line; returns the lines skipped by convergence.
// probe = 1: only compare (store nothing unless it converges); a failed probe returns 0xFFFFFFFF.
u32 Highlighter::lex_step_(int probe)
{
  u32 i = valid_upto_ - 1, len = 0, nv, old = valid_upto_;
  u16 start, e, cur;
  if (lines_->lp_get(i, buf_, LEX_LINE_MAX, &len) != E_OK || len > LEX_LINE_MAX) len = 0;
  start = (u16)(gget_(i) & HL_SENT);
  e = lang_->lexer->lex_line(buf_, len, start, 0);
  if (e >= HL_SENT) e = HL_SENT - 1;
  lexed_++;
  cur = gget_(i + 1);
  if ((cur & HL_SENT) == e) {                     // converged: skip lines that are neither changed nor unknown
    gset_(i, start);                              // line i is now consistent with its successor
    nv = valid_upto_ + 1;
    while (nv < n_ && !(gget_(nv - 1) & HL_FLAG) && (gget_(nv) & HL_SENT) != HL_SENT) nv++;
    advance_(nv);
    return nv - old - 1;
  }
  if (probe) return 0xFFFFFFFFu;
  gset_(i, start);
  gset_(i + 1, (u16)(e | HL_FLAG));
  mark_dirty_(i + 1, i + 1);
  advance_(valid_upto_ + 1);
  return 0;
}

int Highlighter::catch_up_(u32 line, u32 limit)
{
  if (line < valid_upto_) return 1;
  if (line + 1 - valid_upto_ > limit) return 0;
  while (valid_upto_ <= line) lex_step_(0);
  return 1;
}

int Highlighter::attach(LineProvider* lines, const LangInfo* lang)
{
  detach();
  lines_ = lines; lang_ = lang;
  if (!lines_ || !lang_ || !lang_->lexer) return E_OK;
  lines_reset();
  return broken_ ? E_NOMEM : E_OK;
}

void Highlighter::detach()
{
  e_free(gb_); e_free(buf_);
  gb_ = 0; buf_ = 0; gcap_ = gs_ = gl_ = n_ = 0;
  lines_ = 0; lang_ = 0; valid_upto_ = 0; probed_ = 0xFFFFFFFFu;
  dirty_ = unk_ = broken_ = false;
}

void Highlighter::set_enabled(bool on)
{
  if (on == enabled_) return;
  enabled_ = on;
  if (!on) {
    u32 n = lines_ ? lines_->lp_count() : 0;
    e_free(gb_); e_free(buf_);
    gb_ = 0; buf_ = 0; gcap_ = gs_ = gl_ = n_ = 0; valid_upto_ = 0; unk_ = false;
    if (n) mark_dirty_(0, n - 1);
    return;
  }
  lines_reset();
}

bool Highlighter::enabled() const { return enabled_; }

void Highlighter::lines_reset()
{
  u32 n;
  if (!lines_) return;
  n = lines_->lp_count();
  unk_ = false;
  probed_ = 0xFFFFFFFFu;
  if (n) mark_dirty_(0, n - 1);
  if (!lang_ || !lang_->lexer || !enabled_) return;
  broken_ = false;
  if (!buf_) buf_ = (ecp*)e_alloc(LEX_LINE_MAX * (u32)sizeof(ecp));
  if (!buf_ || gresize_(n) != E_OK) {
    broken_ = true;
    e_free(gb_); e_free(buf_);
    gb_ = 0; buf_ = 0; gcap_ = gs_ = gl_ = n_ = 0; valid_upto_ = 0;
    return;
  }
  valid_upto_ = n ? 1 : 0;
}

static u32 shift_line(u32 x, u32 first, u32 removed, u32 added)
{
  if (x <= first) return x;
  if (x <= first + removed) return first + added;
  return x - removed + added;
}

void Highlighter::lines_changed(u32 first, u32 removed, u32 added)
{
  u32 i;
  if (!active_()) return;                         // plain text / disabled: nothing to track
  if (first >= n_ || removed > n_ - 1 - first || n_ - removed + added != lines_->lp_count()) { lines_reset(); return; }
  if (dirty_) {
    dfirst_ = shift_line(dfirst_, first, removed, added);
    dlast_ = emax(shift_line(dlast_, first, removed, added), dfirst_);
  }
  if (unk_) {
    ufirst_ = shift_line(ufirst_, first, removed, added);
    ulast_ = emax(shift_line(ulast_, first, removed, added), ufirst_);
  }
  if (valid_upto_ > first + 1) valid_upto_ = first + 1;
  probed_ = 0xFFFFFFFFu;
  if (removed) {
    gmove_(first + 1);
    gl_ += removed;
    n_ -= removed;
  }
  if (added) {
    gmove_(first + 1);
    if (greserve_(added) != E_OK) { lines_reset(); return; }
    for (i = 0; i < added; i++) gb_[gs_ + i] = HL_SENT | HL_FLAG;
    gs_ += added; gl_ -= added; n_ += added;
  }
  gset_(first, (u16)(gget_(first) | HL_FLAG));
}

void Highlighter::set_target(u32 line) { target_ = line; }

int Highlighter::work(u32 budget_lines)
{
  u32 goal;
  if (!active_()) return 0;
  if (lines_->lp_count() != n_) { lines_reset(); if (!active_()) return 0; }
  goal = target_ >= n_ ? n_ : target_ + 1;
  while (valid_upto_ < goal && budget_lines > 0) {
    u32 adv = lex_step_(0);
    budget_lines--;
    adv /= HL_FAST_UNIT;
    budget_lines -= emin(budget_lines, adv);
  }
  // at the target: check once whether the old state after it still holds (an edit that was undone, a closed
  // comment). Comparing without storing keeps that old state intact for the next check.
  if (valid_upto_ >= goal && valid_upto_ < n_ && budget_lines > 0 && probed_ != valid_upto_) {
    if (lex_step_(1) == 0xFFFFFFFFu) probed_ = valid_upto_;
  }
  return valid_upto_ < goal ? 1 : 0;
}

bool Highlighter::state_known(u32 line) const
{
  if (!active_()) return true;
  return line < valid_upto_;
}

int Highlighter::style_line(u32 line, const ecp* s, u32 n, u8* styles)
{
  u16 st;
  if (!active_()) { lex_fill(styles, 0, n, ST_DEFAULT); return 1; }
  if (line >= n_ || !catch_up_(line, HL_SYNC_MAX)) {
    lex_fill(styles, 0, n, ST_DEFAULT);
    if (line < n_) {
      if (!unk_) { ufirst_ = ulast_ = line; unk_ = true; }
      else { ufirst_ = emin(ufirst_, line); ulast_ = emax(ulast_, line); }
    }
    return 0;
  }
  if (!s) { lex_fill(styles, 0, n, ST_DEFAULT); return 1; }
  st = (u16)(gget_(line) & HL_SENT);
  lang_->lexer->lex_line(s, n, st, styles);
  return 1;
}

int Highlighter::take_dirty(u32* first, u32* last)
{
  u32 n = lines_ ? lines_->lp_count() : 0;
  if (!dirty_) return 0;
  dirty_ = false;
  if (!n || dfirst_ >= n) return 0;
  *first = dfirst_;
  *last = emin(dlast_, n - 1);
  return 1;
}

u32 Highlighter::valid_upto() const
{
  if (!active_()) return lines_ ? lines_->lp_count() : 0;
  return valid_upto_;
}

int Highlighter::debug_state(u32 line, u16* state) const
{
  if (!gb_ || line >= n_) return 0;
  *state = (u16)(gget_(line) & HL_SENT);
  return line < valid_upto_ ? 1 : 0;
}

u32 Highlighter::debug_lexed() const { return lexed_; }
