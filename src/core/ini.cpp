// ini.cpp - tolerant INI reader/writer (DESIGN 6.19). Owner: T06.
//
// Every line is kept in order with its original text; only lines changed through set() are rewritten. Lines are
// classified as section headers "[name]" (text after the ']' is kept but ignored), key lines "key=value" (key and
// value trimmed) and everything else (comments starting with ';' or '#', blank lines, garbage), which is written
// back verbatim. Sections and keys match case-insensitively; a section may appear several times (all occurrences
// are searched, the first matching key wins; new keys go to the last occurrence). Keys before the first header
// belong to the section "". Values are not unescaped, with one exception: a value written as "..." loses one pair
// of surrounding double quotes, and set() adds quotes when a value has leading/trailing blanks or is itself quoted,
// so every string round-trips. Inline comments are NOT stripped (paths and masks contain ';').
#include "ini.h"
#include <string.h>

enum { IK_OTHER = 0, IK_SECTION = 1, IK_KEY = 2 };

static int ini_ws(char c) { return c == ' ' || c == '\t'; }

static void span_trim(const char* s, u32* a, u32* b)
{
  while (*a < *b && ini_ws(s[*a])) (*a)++;
  while (*b > *a && ini_ws(s[*b - 1])) (*b)--;
}

static const char* nz(const char* s) { return s ? s : ""; }

Ini::Ini() {}
Ini::~Ini() { clear_(); }

void Ini::clear_()
{
  u32 i;
  for (i = 0; i < lines_.size(); i++) e_free(lines_[i].raw);
  lines_.release();
  secs_.release();
}

// Rebuilds the header index. Allocates only when the number of headers grows beyond the capacity: set() reserves
// room before adding a header, so only parse() can see E_NOMEM here.
int Ini::reindex_()
{
  u32 i, n = 0;
  for (i = 0; i < lines_.size(); i++) if (lines_[i].kind == IK_SECTION) n++;
  if (secs_.reserve(n) != E_OK) return E_NOMEM;
  secs_.clear();
  for (i = 0; i < lines_.size(); i++) if (lines_[i].kind == IK_SECTION) secs_.push(i);
  return E_OK;
}

int Ini::range_(u32 h, u32* first, u32* end) const
{
  if (h >= secs_.size()) {
    *first = 0;
    *end = secs_.size() ? secs_[0] : lines_.size();
  } else {
    *first = secs_[h] + 1;
    *end = (h + 1 < secs_.size()) ? secs_[h + 1] : lines_.size();
  }
  return E_OK;
}

int Ini::find_(const char* sec, const char* key) const
{
  u32 h, first, end, j;
  sec = nz(sec);
  key = nz(key);
  for (h = 0; h <= secs_.size(); h++) {
    if (h < secs_.size()) { if (es_icmp(lines_[secs_[h]].name, sec) != 0) continue; }
    else if (sec[0]) break;
    range_(h, &first, &end);
    for (j = first; j < end; j++)
      if (lines_[j].kind == IK_KEY && es_icmp(lines_[j].name, key) == 0) return (int)j;
  }
  return -1;
}

int Ini::put_line_(u32 at, int replace, const char* raw, u32 rlen, u8 kind, const char* name, u32 nlen,
                   const char* val, u32 vlen)
{
  IniLine ln;
  char* b;
  u32 i;
  if (rlen > 0x3FFFFFFFu || nlen > 0x3FFFFFFFu || vlen > 0x3FFFFFFFu) return E_NOMEM;
  b = (char*)e_alloc(rlen + nlen + vlen + 3);
  if (!b) return E_NOMEM;
  memcpy(b, raw, rlen);
  for (i = 0; i < rlen; i++) if (b[i] == 0) b[i] = ' ';
  b[rlen] = 0;
  ln.raw = b;
  ln.name = b + rlen + 1;
  if (nlen) memcpy(ln.name, name, nlen);
  ln.name[nlen] = 0;
  ln.val = ln.name + nlen + 1;
  if (vlen) memcpy(ln.val, val, vlen);
  ln.val[vlen] = 0;
  for (i = 0; i < nlen + vlen + 1; i++) if (ln.name[i] == 0 && i != nlen) ln.name[i] = ' ';
  ln.kind = kind;
  ln.pad[0] = ln.pad[1] = ln.pad[2] = 0;
  if (replace) {
    e_free(lines_[at].raw);
    lines_[at] = ln;
    return E_OK;
  }
  if (lines_.insert(at, &ln, 1) != E_OK) { e_free(b); return E_NOMEM; }
  return E_OK;
}

void Ini::erase_line_(u32 i)
{
  e_free(lines_[i].raw);
  lines_.erase(i, 1);
}

int Ini::parse(const char* text, u32 n)
{
  u32 i = 0;
  char* copy = 0;
  clear_();
  if (!text) n = 0;
  if (n && memchr(text, 0, n)) {                    // NUL bytes become blanks before anything is classified
    u32 k;
    copy = (char*)e_alloc(n);
    if (!copy) return E_NOMEM;
    for (k = 0; k < n; k++) copy[k] = text[k] ? text[k] : ' ';
    text = copy;
  }
  while (n - i >= 3 && (u8)text[i] == 0xEF && (u8)text[i + 1] == 0xBB && (u8)text[i + 2] == 0xBF) i += 3;
  while (i < n) {
    u32 j = i, next, a, b;
    u8 kind = IK_OTHER;
    u32 na = 0, nb = 0, va = 0, vb = 0;
    const char* s = text + i;
    while (j < n && text[j] != '\r' && text[j] != '\n') j++;
    next = j + 1;
    if (j < n && text[j] == '\r' && j + 1 < n && text[j + 1] == '\n') next = j + 2;
    // classify [0, j - i)
    a = 0; b = j - i;
    span_trim(s, &a, &b);
    if (a < b && s[a] == '[') {
      u32 c = a + 1;
      while (c < b && s[c] != ']') c++;
      if (c < b) {
        na = a + 1; nb = c;
        span_trim(s, &na, &nb);
        if (na < nb) kind = IK_SECTION;
      }
    } else if (a < b && s[a] != ';' && s[a] != '#') {
      u32 eq = a;
      while (eq < b && s[eq] != '=') eq++;
      if (eq < b) {
        na = a; nb = eq;
        span_trim(s, &na, &nb);
        va = eq + 1; vb = b;
        span_trim(s, &va, &vb);
        if (vb - va >= 2 && s[va] == '"' && s[vb - 1] == '"') { va++; vb--; }
        if (na < nb) kind = IK_KEY;
      }
    }
    if (kind == IK_OTHER) { na = nb = va = vb = 0; }
    if (put_line_(lines_.size(), 0, s, j - i, kind, s + na, nb - na, s + va, vb - va) != E_OK) {
      clear_();
      e_free(copy);
      return E_NOMEM;
    }
    i = next;
  }
  e_free(copy);
  if (reindex_() != E_OK) { clear_(); return E_NOMEM; }
  return E_OK;
}

const char* Ini::get(const char* sec, const char* key) const
{
  int i = find_(sec, key);
  return i < 0 ? 0 : lines_[(u32)i].val;
}

const char* Ini::get_s(const char* sec, const char* key, const char* def) const
{
  const char* v = get(sec, key);
  return v ? v : def;
}

i32 Ini::get_i(const char* sec, const char* key, i32 def, i32 lo, i32 hi) const
{
  const char* v = get(sec, key);
  i32 x;
  int rc;
  if (!v) return def;
  rc = es_parse_i32(v, &x);
  if (rc == E_RANGE) {
    const char* p = v;
    while (ini_ws(*p)) p++;
    return *p == '-' ? lo : hi;
  }
  if (rc != E_OK) return def;
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

u32 Ini::get_x(const char* sec, const char* key, u32 def) const
{
  const char* v = get(sec, key);
  u32 x;
  if (!v || es_parse_u32(v, &x, 0) != E_OK) return def;
  return x;
}

int Ini::get_b(const char* sec, const char* key, int def) const
{
  const char* v = get(sec, key);
  if (!v) return def;
  if (es_icmp(v, "1") == 0 || es_icmp(v, "yes") == 0 || es_icmp(v, "true") == 0 || es_icmp(v, "on") == 0) return 1;
  if (es_icmp(v, "0") == 0 || es_icmp(v, "no") == 0 || es_icmp(v, "false") == 0 || es_icmp(v, "off") == 0) return 0;
  return def;
}

static int valid_name(const char* s, int is_section)
{
  u32 n;
  const char* p;
  if (!s) return 0;
  n = es_len(s);
  if (n == 0 || ini_ws(s[0]) || ini_ws(s[n - 1])) return 0;
  for (p = s; *p; p++) {
    if (*p == '\r' || *p == '\n') return 0;
    if (is_section ? (*p == ']') : (*p == '=')) return 0;
  }
  if (!is_section && (s[0] == '[' || s[0] == ';' || s[0] == '#')) return 0;
  return 1;
}

int Ini::set(const char* sec, const char* key, const char* val)
{
  EStr clean, raw;
  const char* name;
  u32 i, n, first, end, h, found_h;
  int idx, rc, quote;
  sec = nz(sec);
  if (!valid_name(key, 0) || (sec[0] && !valid_name(sec, 1))) return E_SYNTAX;
  // value: CR/LF become spaces (one line per key)
  val = nz(val);
  n = es_len(val);
  for (i = 0; i < n; i++) {
    char ch = (val[i] == '\r' || val[i] == '\n') ? ' ' : val[i];
    if (clean.appendn(&ch, 1) != E_OK) return E_NOMEM;
  }
  n = clean.len();
  quote = n > 0 && (ini_ws(clean.c_str()[0]) || ini_ws(clean.c_str()[n - 1]) ||
                    (n >= 2 && clean.c_str()[0] == '"' && clean.c_str()[n - 1] == '"'));
  idx = find_(sec, key);
  name = idx >= 0 ? lines_[(u32)idx].name : key;
  if (raw.append(name) != E_OK || raw.append("=") != E_OK || (quote && raw.append("\"") != E_OK) ||
      raw.append(clean.c_str()) != E_OK || (quote && raw.append("\"") != E_OK)) return E_NOMEM;
  if (idx >= 0) {
    // name points into the old block: put_line_ copies it before freeing (raw holds a copy anyway)
    EStr nm;
    if (nm.append(name) != E_OK) return E_NOMEM;
    return put_line_((u32)idx, 1, raw.c_str(), raw.len(), IK_KEY, nm.c_str(), nm.len(), clean.c_str(), n);
  }
  // new key: after the last key line of the last occurrence of the section
  found_h = 0xFFFFFFFFu;
  if (sec[0] == 0) found_h = secs_.size();
  else {
    for (h = 0; h < secs_.size(); h++) if (es_icmp(lines_[secs_[h]].name, sec) == 0) found_h = h;
  }
  if (found_h != 0xFFFFFFFFu) {
    u32 pos;
    range_(found_h, &first, &end);
    pos = first;
    for (i = first; i < end; i++) if (lines_[i].kind == IK_KEY) pos = i + 1;
    rc = put_line_(pos, 0, raw.c_str(), raw.len(), IK_KEY, key, es_len(key), clean.c_str(), n);
    if (rc == E_OK) reindex_();
    return rc;
  }
  {
    EStr hdr;
    u32 at = lines_.size();
    int blank = at > 0 && lines_[at - 1].raw[0] != 0;
    if (hdr.append("[") != E_OK || hdr.append(sec) != E_OK || hdr.append("]") != E_OK) return E_NOMEM;
    if (secs_.reserve(secs_.size() + 1) != E_OK) return E_NOMEM;   // reindex_ below then cannot fail
    if (blank && put_line_(at++, 0, "", 0, IK_OTHER, "", 0, "", 0) != E_OK) return E_NOMEM;
    rc = put_line_(at, 0, hdr.c_str(), hdr.len(), IK_SECTION, sec, es_len(sec), "", 0);
    if (rc == E_OK) rc = put_line_(at + 1, 0, raw.c_str(), raw.len(), IK_KEY, key, es_len(key), clean.c_str(), n);
    if (rc != E_OK) {                                // roll back the partial section
      while (lines_.size() > (blank ? at - 1 : at)) erase_line_(lines_.size() - 1);
    }
    reindex_();
    return rc;
  }
}

int Ini::set_i(const char* sec, const char* key, i32 v)
{
  char buf[16];
  es_fmt(buf, sizeof(buf), "%d", (int)v);
  return set(sec, key, buf);
}

int Ini::set_x(const char* sec, const char* key, u32 v)
{
  char buf[16];
  es_fmt(buf, sizeof(buf), "0x%X", v);
  return set(sec, key, buf);
}

int Ini::remove(const char* sec, const char* key)
{
  int removed = 0, i;
  while ((i = find_(sec, key)) >= 0) {
    erase_line_((u32)i);
    reindex_();
    removed = 1;
  }
  return removed ? E_OK : E_NOTFOUND;
}

int Ini::remove_section(const char* sec)
{
  int removed = 0;
  u32 h, first, end;
  sec = nz(sec);
  for (;;) {
    int hit = 0;
    for (h = 0; h <= secs_.size(); h++) {
      if (h < secs_.size()) { if (es_icmp(lines_[secs_[h]].name, sec) != 0) continue; }
      else if (sec[0]) break;
      range_(h, &first, &end);
      if (h < secs_.size()) first--;               // include the header line
      if (first == end) break;                       // empty global part
      while (end > first) erase_line_(--end);
      reindex_();
      hit = removed = 1;
      break;
    }
    if (!hit) break;
  }
  return removed ? E_OK : E_NOTFOUND;
}

u32 Ini::section_count() const { return secs_.size(); }

const char* Ini::section(u32 i) const
{
  return i < secs_.size() ? lines_[secs_[i]].name : 0;
}

u32 Ini::key_count(const char* sec) const
{
  u32 h, first, end, j, n = 0;
  sec = nz(sec);
  for (h = 0; h <= secs_.size(); h++) {
    if (h < secs_.size()) { if (es_icmp(lines_[secs_[h]].name, sec) != 0) continue; }
    else if (sec[0]) break;
    range_(h, &first, &end);
    for (j = first; j < end; j++) if (lines_[j].kind == IK_KEY) n++;
  }
  return n;
}

const char* Ini::key_at(const char* sec, u32 i, const char** val) const
{
  u32 h, first, end, j, n = 0;
  sec = nz(sec);
  if (val) *val = 0;
  for (h = 0; h <= secs_.size(); h++) {
    if (h < secs_.size()) { if (es_icmp(lines_[secs_[h]].name, sec) != 0) continue; }
    else if (sec[0]) break;
    range_(h, &first, &end);
    for (j = first; j < end; j++) {
      if (lines_[j].kind != IK_KEY) continue;
      if (n++ == i) {
        if (val) *val = lines_[j].val;
        return lines_[j].name;
      }
    }
  }
  return 0;
}

int Ini::write(EStr* out) const
{
  u32 i;
  if (!out) return E_RANGE;
  out->clear();
  for (i = 0; i < lines_.size(); i++) {
    if (out->append(lines_[i].raw) != E_OK || out->append("\r\n") != E_OK) return E_NOMEM;
  }
  return E_OK;
}
