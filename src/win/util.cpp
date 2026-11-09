// util.cpp - ui_ask with the selftest answer queue, ui_error, ANSI <-> ecp, path helpers (DESIGN 15.1).
// T00: working. Owner after Wave 0: T07.
#include <windows.h>
#include <stdarg.h>
#include "app.h"
#include "ui.h"
#include "selftest.h"
#include "winenc.h"
#include "codec.h"
#include "estr.h"

struct UiAnswer { int qid; int button; };
static Vec<UiAnswer> g_answers;
static int g_unexpected = 0;

void ui_answer_push(int qid, int button)
{
  UiAnswer a;
  a.qid = qid; a.button = button;
  g_answers.push(a);
}

void ui_answer_clear()
{
  g_answers.clear();
  g_unexpected = 0;
}

u32 ui_answer_pending()
{
  return g_answers.size();
}

int ui_unexpected_questions()
{
  return g_unexpected;
}

int ui_ask(int qid, const char* text, int buttons)
{
  if (g_app.selftest) {
    u32 i;
    for (i = 0; i < g_answers.size(); i++) {
      if (g_answers[i].qid == qid || g_answers[i].qid == Q_ANY) {
        int b = g_answers[i].button;
        g_answers.erase(i, 1);
        selftest_log("ask %d: \"%s\" -> %d", qid, text ? text : "", b);
        return b;
      }
    }
    g_unexpected++;
    selftest_log("unexpected question %d: \"%s\" (answered IDCANCEL)", qid, text ? text : "");
    return IDCANCEL;
  }
  if (!(buttons & MB_ICONMASK)) buttons |= MB_ICONQUESTION;
  return MessageBoxA(g_app.frame, text ? text : "", APP_NAME, (UINT)buttons);
}

void ui_error(const char* fmt, ...)
{
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  es_vfmt(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_app.selftest) { selftest_log("error box: %s", buf); return; }
  MessageBoxA(g_app.frame, buf, APP_NAME, MB_OK | MB_ICONERROR);
}

void ui_notimpl(const char* what)
{
  app_status_note("Not implemented yet (%s)", what ? what : "?");
}

int ui_ansi_to_ecp(const char* s, Vec<ecp>* out)
{
  u32 n = es_len(s), i;
  const Codec* c;
  int rc = winenc_ansi_to_ecp(s, n, out);
  if (rc != E_NOTIMPL) return rc;
  c = codec_get(enc_ansi());                     // fallback until winenc lands: SBCS decode (or Latin-1)
  for (i = 0; i < n; i++) {
    ecp cp = (u8)s[i];
    if (c) c->decode((const u8*)s + i, 1, 1, &cp);
    if (out->push(cp) != E_OK) return E_NOMEM;
  }
  return E_OK;
}

int ui_ecp_to_ansi(const ecp* s, u32 n, char* out, u32 cap)
{
  EBytes b;
  u32 lost = 0, i, k = 0;
  const Codec* c;
  if (!out || cap == 0) return 0;
  if (winenc_ecp_to_ansi(s, n, &b, &lost) == E_OK) {
    for (i = 0; i < b.size() && k + 1 < cap; i++) out[k++] = (char)b[i];
    out[k] = 0;
    return (int)lost;
  }
  c = codec_get(enc_ansi());
  for (i = 0; i < n && k + 1 < cap; i++) {
    u8 tmp[4];
    u32 len = c ? c->encode(s[i], tmp) : (s[i] < 0x100 ? (tmp[0] = (u8)s[i], 1u) : 0u);
    if (len != 1) { tmp[0] = '?'; lost++; }
    out[k++] = (char)tmp[0];
  }
  out[k] = 0;
  return (int)lost;
}

// ---- paths ----

const char* ui_path_name(const char* path)
{
  const char* p;
  const char* name = path;
  if (!path) return "";
  for (p = path; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') name = p + 1;
  return name;
}

void ui_path_dir(const char* path, char* out, u32 cap)
{
  u32 n;
  if (!out || cap == 0) return;
  if (!path) path = "";
  n = (u32)(ui_path_name(path) - path);
  es_copy(out, cap, path);
  if (n < cap) out[n] = 0;
  n = es_len(out);
  // drop the trailing backslash except for "C:\" and "\"
  if (n > 1 && (out[n - 1] == '\\' || out[n - 1] == '/') && !(n == 3 && out[1] == ':')) out[n - 1] = 0;
}

void ui_path_join(const char* dir, const char* name, char* out, u32 cap)
{
  u32 n;
  es_copy(out, cap, dir);
  n = es_len(out);
  if (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/') es_cat(out, cap, "\\");
  es_cat(out, cap, name);
}

int ui_path_full(const char* in, char* out, u32 cap)
{
  char* part;
  DWORD n = GetFullPathNameA(in, cap, out, &part);
  if (n == 0) return E_NOTFOUND;
  if (n >= cap) return E_RANGE;
  return E_OK;
}

int ui_path_equal(const char* a, const char* b)
{
  char sa[MAX_PATH], sb[MAX_PATH];
  if (es_icmp(a, b) == 0) return 1;
  if (GetShortPathNameA(a, sa, sizeof(sa)) && GetShortPathNameA(b, sb, sizeof(sb))) return es_icmp(sa, sb) == 0;
  return 0;
}

int ui_file_exists(const char* path)
{
  DWORD a = GetFileAttributesA(path);
  return a != 0xFFFFFFFF && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int ui_dir_exists(const char* path)
{
  DWORD a = GetFileAttributesA(path);
  return a != 0xFFFFFFFF && (a & FILE_ATTRIBUTE_DIRECTORY);
}
