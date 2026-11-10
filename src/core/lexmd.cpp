// lexmd.cpp - hand-written Markdown lexer (DESIGN 6.13). Owner: T04.
//
// Line-oriented. Styles: ATX and setext headings HEADING; list markers, block quote '>', table pipes and horizontal
// rules OPERATOR; block quote text COMMENT; fenced and indented code CODE; `inline code` CODE; *emphasis* /
// **strong** / _x_ / __x__ EMPH; [link text] KEYWORD3 and (url) / <autolink> STRING; inline HTML tags TAG;
// <!-- comments --> COMMENT (may span lines).
//
// State word: bits 0-1 mode (0 normal, 1 fenced code, 2 HTML comment), bit 2 fence uses '~', bits 3-6 fence length
// (3-15), bit 7 previous line was paragraph text (setext underline, no indented code), bit 8 inside a list,
// bit 9 previous line was indented code, bit 10 previous line was blank.
#include "lexer_int.h"

enum { MD_NORMAL = 0, MD_FENCE = 1, MD_HCOMMENT = 2 };
enum { MB_TILDE = 4, MB_PARA = 0x80, MB_LIST = 0x100, MB_CODE = 0x200, MB_BLANK = 0x400 };

static int md_punct(ecp c)
{
  return c < 128 && c > 32 && !lex_is_alnum(c);
}

static u32 md_run(const ecp* s, u32 n, u32 i, ecp c)
{
  u32 e = i;
  while (e < n && s[e] == c) e++;
  return e - i;
}

static u32 md_find(const ecp* s, u32 n, u32 i, const char* t)   // n if not found
{
  u32 k;
  for (; i < n; i++) {
    for (k = 0; t[k] && i + k < n && s[i + k] == (u8)t[k]; k++) {}
    if (!t[k]) return i;
  }
  return n;
}

static u32 md_match(const ecp* s, u32 n, u32 i, ecp open, ecp close)   // s[i] == open; index of the match or n
{
  u32 d = 0;
  for (; i < n; i++) {
    if (s[i] == '\\') { i++; continue; }
    if (s[i] == open) d++;
    else if (s[i] == close && --d == 0) return i;
  }
  return n;
}

// inline spans from i to n on a base style; may switch *state to an open HTML comment
void MdLexer::inl(const ecp* s, u32 n, u32 i, u8 base, u8* st, u16* state) const
{
  while (i < n) {
    ecp c = s[i];
    u32 k, e;
    if (c == '\\' && i + 1 < n && md_punct(s[i + 1])) { lex_fill(st, i, i + 2, base); i += 2; continue; }
    if (c == '`') {
      k = md_run(s, n, i, '`');
      for (e = i + k; e < n; e++) {
        if (s[e] == '`') {
          u32 r = md_run(s, n, e, '`');
          if (r == k) break;
          e += r - 1;
        }
      }
      if (e < n) { lex_fill(st, i, e + k, ST_CODE); i = e + k; }
      else { lex_fill(st, i, i + k, base); i += k; }
      continue;
    }
    if (c == '*' || c == '_') {
      k = md_run(s, n, i, c);
      if (k > 3) k = 3;
      if (i + k < n && !lex_is_space(s[i + k]) && !(c == '_' && i > 0 && lex_word_char(s[i - 1]))) {
        for (e = i + k + 1; e + k <= n; e++) {
          u32 m;
          for (m = 0; m < k && s[e + m] == c; m++) {}
          if (m == k && !lex_is_space(s[e - 1]) && (e + k >= n || s[e + k] != c) &&
              !(c == '_' && e + k < n && lex_word_char(s[e + k]))) break;
        }
        if (e + k <= n) { lex_fill(st, i, e + k, ST_EMPH); i = e + k; continue; }
      }
      e = i + md_run(s, n, i, c);
      lex_fill(st, i, e, base);
      i = e;
      continue;
    }
    if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {
      u32 b = c == '!' ? i + 1 : i;
      e = md_match(s, n, b, '[', ']');
      if (e < n && e + 1 < n && s[e + 1] == '(') {
        u32 m = md_match(s, n, e + 1, '(', ')');
        if (m < n) {
          lex_fill(st, i, e + 1, ST_KEYWORD3);
          lex_fill(st, e + 1, m + 1, ST_STRING);
          i = m + 1;
          continue;
        }
      }
      if (e < n && e + 1 < n && s[e + 1] == '[') {
        u32 m = md_match(s, n, e + 1, '[', ']');
        if (m < n) { lex_fill(st, i, m + 1, ST_KEYWORD3); i = m + 1; continue; }
      }
      if (st) st[i] = base;
      i++;
      continue;
    }
    if (c == '<') {
      if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
        e = md_find(s, n, i + 4, "-->");
        if (e < n) { lex_fill(st, i, e + 3, ST_COMMENT); i = e + 3; continue; }
        lex_fill(st, i, n, ST_COMMENT);
        *state = (u16)((*state & ~3u) | MD_HCOMMENT);
        return;
      }
      if (i + 1 < n && (lex_is_alpha(s[i + 1]) || (s[i + 1] == '/' && i + 2 < n && lex_is_alpha(s[i + 2])))) {
        int url = 0;
        for (e = i + 1; e < n && s[e] != '>' && s[e] != '<' && !(s[e] == ' ' && url); e++) {
          if (s[e] == ':' || s[e] == '@') url = 1;
        }
        if (e < n && s[e] == '>') {
          for (k = i + 1; k < e && (lex_is_alnum(s[k]) || s[k] == '+' || s[k] == '.' || s[k] == '-'); k++) {}
          lex_fill(st, i, e + 1, (k < e && (s[k] == ':' || s[k] == '@')) ? ST_STRING : ST_TAG);
          i = e + 1;
          continue;
        }
      }
      if (st) st[i] = base;
      i++;
      continue;
    }
    if (c == '|') { if (st) st[i] = ST_OPERATOR; i++; continue; }
    if (st) st[i] = base;
    i++;
  }
}

static int md_only(const ecp* s, u32 n, u32 j, ecp c, u32* count)   // only c and spaces from j; counts c
{
  u32 k = 0;
  for (; j < n; j++) {
    if (s[j] == c) k++;
    else if (!lex_is_space(s[j])) return 0;
  }
  *count = k;
  return 1;
}

u16 MdLexer::lex_line(const ecp* s, u32 n, u16 state, u8* st) const
{
  u32 mode = state & 3, j = 0, indent = 0, k, e;
  u16 out;
  u32 list = state & MB_LIST;

  if (n > LEX_LINE_MAX) { lex_fill(st, LEX_LINE_MAX, n, ST_DEFAULT); n = LEX_LINE_MAX; }
  while (j < n && lex_is_space(s[j])) { indent += s[j] == '\t' ? 4 - indent % 4 : 1; j++; }

  if (mode == MD_FENCE) {
    ecp fc = (state & MB_TILDE) ? '~' : '`';
    u32 flen = (state >> 3) & 15;
    lex_fill(st, 0, n, ST_CODE);
    k = md_run(s, n, j, fc);
    if (k >= flen && md_only(s, n, j + k, ' ', &e)) return (u16)list;
    return state;
  }
  if (mode == MD_HCOMMENT) {
    e = md_find(s, n, 0, "-->");
    if (e >= n) { lex_fill(st, 0, n, ST_COMMENT); return state; }
    lex_fill(st, 0, e + 3, ST_COMMENT);
    out = (u16)(MB_PARA | list);
    inl(s, n, e + 3, ST_DEFAULT, st, &out);
    return out;
  }
  if (j >= n) {                                             // blank line
    lex_fill(st, 0, n, ST_DEFAULT);
    return (u16)(list | (state & MB_CODE) | MB_BLANK);
  }
  lex_fill(st, 0, j, ST_DEFAULT);
  if (indent >= 4 && !(state & MB_PARA) && (!list || (state & MB_CODE))) {
    lex_fill(st, j, n, ST_CODE);
    return (u16)(list | MB_CODE);
  }
  if (s[j] == '`' || s[j] == '~') {                         // fence
    k = md_run(s, n, j, s[j]);
    if (k >= 3) {
      int ok = 1;
      if (s[j] == '`') for (e = j + k; e < n; e++) if (s[e] == '`') { ok = 0; break; }
      if (ok) {
        lex_fill(st, j, n, ST_CODE);
        return (u16)(MD_FENCE | (s[j] == '~' ? MB_TILDE : 0) | (emin(k, (u32)15) << 3) | list);
      }
    }
  }
  if (s[j] == '#') {                                        // ATX heading
    k = md_run(s, n, j, '#');
    if (k <= 6 && (j + k >= n || lex_is_space(s[j + k]))) { lex_fill(st, j, n, ST_HEADING); return 0; }
  }
  if ((state & MB_PARA) && (s[j] == '=' || s[j] == '-') && indent < 4 && md_only(s, n, j, s[j], &k) && k >= 1 &&
      !(s[j] == '-' && k == 1)) {                           // setext underline
    lex_fill(st, j, n, ST_HEADING);
    return 0;
  }
  if ((s[j] == '-' || s[j] == '*' || s[j] == '_') && md_only(s, n, j, s[j], &k) && k >= 3) {   // horizontal rule
    lex_fill(st, j, n, ST_OPERATOR);
    return 0;
  }
  if (s[j] == '>') {                                        // block quote
    e = j;
    while (e < n && (s[e] == '>' || lex_is_space(s[e]))) { if (st) st[e] = s[e] == '>' ? ST_OPERATOR : ST_DEFAULT; e++; }
    out = (u16)(MB_PARA | list);
    inl(s, n, e, ST_COMMENT, st, &out);
    return out;
  }
  e = j;
  if ((s[j] == '-' || s[j] == '*' || s[j] == '+') && (j + 1 >= n || lex_is_space(s[j + 1]))) e = j + 1;
  else if (lex_is_digit(s[j])) {
    k = j;
    while (k < n && k - j < 9 && lex_is_digit(s[k])) k++;
    if (k < n && (s[k] == '.' || s[k] == ')') && (k + 1 >= n || lex_is_space(s[k + 1]))) e = k + 1;
  }
  if (e > j) {                                              // list item
    lex_fill(st, j, e, ST_OPERATOR);
    out = (u16)MB_LIST;
    inl(s, n, e, ST_DEFAULT, st, &out);
    return out;
  }
  if (s[j] == '[') {                                        // reference definition [id]: url
    e = md_match(s, n, j, '[', ']');
    if (e < n && e + 1 < n && s[e + 1] == ':') {
      lex_fill(st, j, e + 2, ST_KEYWORD3);
      lex_fill(st, e + 2, n, ST_STRING);
      return (u16)list;
    }
  }
  if (list && indent == 0 && (state & MB_BLANK)) list = 0;  // unindented paragraph after a blank line ends a list
  out = (u16)(MB_PARA | list);
  inl(s, n, j, ST_DEFAULT, st, &out);
  return out;
}
