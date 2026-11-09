// lexer.cpp - language registry and Highlighter (DESIGN 6.13). STUB from T00. Owner after Wave 0: T04.
#include "lexer.h"

int lang_init(LangSource* builtin, LangSource* overrides, EStr* errors) { (void)builtin; (void)overrides; (void)errors; return E_NOTIMPL; }
void lang_shutdown() {}
u32 lang_count() { return 0; }
const LangInfo* lang_at(u32 i) { (void)i; return 0; }
const LangInfo* lang_find(const char* id) { (void)id; return 0; }
const LangInfo* lang_for_file(const char* basename) { (void)basename; return 0; }
const LangInfo* lang_results() { return 0; }
int lang_parse(const char* text, u32 len, LangInfo* out, EStr* err) { (void)text; (void)len; (void)out; (void)err; return E_NOTIMPL; }

Highlighter::Highlighter() : lines_(0), lang_(0), enabled_(true), valid_upto_(0) {}
Highlighter::~Highlighter() {}
int Highlighter::attach(LineProvider* lines, const LangInfo* lang) { lines_ = lines; lang_ = lang; return E_OK; }
void Highlighter::detach() { lines_ = 0; lang_ = 0; }
void Highlighter::set_enabled(bool on) { enabled_ = on; }
bool Highlighter::enabled() const { return enabled_; }
void Highlighter::lines_changed(u32 first, u32 removed, u32 added) { (void)first; (void)removed; (void)added; }
void Highlighter::lines_reset() {}
void Highlighter::set_target(u32 line) { (void)line; }
int Highlighter::work(u32 budget_lines) { (void)budget_lines; return 0; }
bool Highlighter::state_known(u32 line) const { (void)line; return false; }
int Highlighter::style_line(u32 line, const ecp* s, u32 n, u8* styles)
{
  u32 i;
  (void)line; (void)s;
  if (styles) for (i = 0; i < n; i++) styles[i] = ST_DEFAULT;
  return 0;
}
int Highlighter::take_dirty(u32* first, u32* last) { (void)first; (void)last; return 0; }
u32 Highlighter::valid_upto() const { return valid_upto_; }
