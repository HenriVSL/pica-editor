// session.cpp - session model to/from INI (DESIGN 6.19, 13.6). STUB from T00 (init/free work). Owner after Wave 0: T06.
#include "session.h"

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
int session_to_ini(const Session& s, Ini* ini) { (void)s; (void)ini; return E_NOTIMPL; }
int session_from_ini(const Ini& ini, Session* s) { (void)ini; (void)s; return E_NOTIMPL; }
int session_validate(const u8* p, u32 n, u32* seq) { (void)p; (void)n; if (seq) *seq = 0; return E_NOTIMPL; }
