// t_edtype.cpp - suite "edtype": typing ops, word movement, smart home, ed_lang_from (DESIGN 6.18). Owner: T08.
//
// Golden cases are written as abstract text (UTF-8 in the source, ASCII escapes) with markers: '|' = caret, '^' =
// anchor (selection [anchor, caret]); '\n' = one line break in the document's EOL style. Every case runs in 6 document
// kinds: ANSI (cp1252), UTF-8 with BOM, UTF-16 LE with BOM, each with CRLF and LF. The document is turned back into the
// abstract form (a CRLF/LF that is not the document's style shows as <CRLF>/<LF>, a lone CR as <CR>) and compared.
#include "tf.h"
#include "edops.h"
#include "codec.h"
#include "estr.h"
#include "lexer.h"
#include <string.h>

struct EdtKind { EncSpec enc; EolStyle eol; const char* name; };

static void edt_kinds(EdtKind* k)   // 6 kinds
{
  k[0].enc = enc_ansi();      k[0].eol = EOL_CRLF; k[0].name = "ansi-crlf";
  k[1].enc = enc_ansi();      k[1].eol = EOL_LF;   k[1].name = "ansi-lf";
  k[2].enc = enc_utf8(1);     k[2].eol = EOL_CRLF; k[2].name = "utf8bom-crlf";
  k[3].enc = enc_utf8(1);     k[3].eol = EOL_LF;   k[3].name = "utf8bom-lf";
  k[4].enc = enc_utf16le(1);  k[4].eol = EOL_CRLF; k[4].name = "utf16le-crlf";
  k[5].enc = enc_utf16le(1);  k[5].eol = EOL_LF;   k[5].name = "utf16le-lf";
}

// Abstract text -> document bytes. Returns the document (owned by the caller) and the selection.
static Document* edt_make(TF* tf, const char* text, const EncSpec& enc, EolStyle eol, Selection* sel)
{
  Vec<ecp> cps;
  EBytes bytes;
  u8 buf[8];
  u32 i, bom;
  epos caret = EPOS_NONE, anchor = EPOS_NONE;
  const Codec* c = codec_get(enc);
  Document* d = new Document();
  MemSource* src = new MemSource();
  es_utf8_to_ecp(text, es_len(text), &cps);
  bom = enc_bom(enc, buf);
  bytes.append(buf, bom);
  for (i = 0; i < cps.size(); i++) {
    ecp ch = cps[i];
    if (ch == '|') { caret = bytes.size(); continue; }
    if (ch == '^') { anchor = bytes.size(); continue; }
    if (ch == '\n') {
      if (eol == EOL_CRLF) { u32 n = c->encode('\r', buf); bytes.append(buf, n); }
      { u32 n = c->encode(eol == EOL_CR ? '\r' : '\n', buf); bytes.append(buf, n); }
      continue;
    }
    { u32 n = c->encode(ch, buf); TF_CHECK(n > 0); bytes.append(buf, n); }
  }
  if (caret == EPOS_NONE) caret = bytes.size();
  if (anchor == EPOS_NONE) anchor = caret;
  src->copy_from(bytes.data(), bytes.size());
  TF_EQ_I(d->init_source(src, enc, bom, eol, 0), E_OK);
  sel->anchor = anchor; sel->caret = caret;
  return d;
}

static void edt_put(EStr* out, const char* s) { out->append(s); }

// Document -> abstract text with markers.
static void edt_render(Document* d, const Selection& sel, EStr* out)
{
  epos pos = d->text_origin(), len = d->length();
  int crlf_style = d->eol_style() == EOL_CRLF;
  out->clear();
  for (;;) {
    ecp ch = 0;
    u32 used;
    char u[8];
    if (pos == sel.caret) edt_put(out, "|");
    else if (pos == sel.anchor) edt_put(out, "^");
    if (pos >= len) break;
    used = d->decode(pos, &ch);
    if (!used) break;
    if (ch == '\r') {
      ecp nx = 0;
      u32 u2 = pos + used < len ? d->decode(pos + used, &nx) : 0;
      if (u2 && nx == '\n' && d->eol_style() != EOL_CR) {
        edt_put(out, crlf_style ? "\n" : "<CRLF>");
        pos += used + u2;
        continue;
      }
      edt_put(out, d->eol_style() == EOL_CR ? "\n" : "<CR>");
      pos += used;
      continue;
    }
    if (ch == '\n') { edt_put(out, crlf_style || d->eol_style() == EOL_CR ? "<LF>" : "\n"); pos += used; continue; }
    if (ECP_IS_BAD(ch)) { char b[16]; es_fmt(b, sizeof(b), "<%X>", ch); edt_put(out, b); pos += used; continue; }
    u[es_utf8_encode(ch, u)] = 0;
    edt_put(out, u);
    pos += used;
  }
}

static void edt_ctx(EdCtx* e, Document* d, const Selection& s)
{
  memset(e, 0, sizeof(*e));
  e->doc = d; e->sel = s;
  ed_lang_from(0, 4, 0, &e->lang);
  e->auto_indent = 1;
  e->now_ms = 1000;
}

struct EdtCase { const char* before; const char* op; const char* after; };

// ops: "t<char>" type (UTF-8), "o<char>" type in overwrite mode, "T" Tab, "n" Enter, "b" Backspace, "d" Del,
// "w" Ctrl+Del, "W" Ctrl+Backspace, "r"/"l" word right/left (caret move), "h" smart home, "a" word at caret,
// "p<text>" paste (ed_insert_text).
static const EdtCase k_cases[] = {
  { "ab|cd", "tX", "abX|cd" },
  { "a^bc|d", "tX", "aX|d" },
  { "a|bc^d", "tX", "aX|d" },
  { "abc|", "t\xC3\xA9", "abc\xC3\xA9|" },
  { "|", "tq", "q|" },
  { "  ab|cd", "n", "  ab\n  |cd" },
  { "|  ab", "n", "\n|  ab" },
  { " | ab", "n", " \n | ab" },
  { "\t x|", "n", "\t x\n\t |" },
  { "a^bc|d", "n", "a\n|d" },
  { "x\n  y|", "n", "x\n  y\n  |" },
  { "ab|c", "b", "a|c" },
  { "ab\n|cd", "b", "ab|cd" },
  { "|ab", "b", "|ab" },
  { "a^bc|d", "b", "a|d" },
  { "a\xC3\xA9|b", "b", "a|b" },
  { "\n|", "b", "|" },
  { "a|bc", "d", "a|c" },
  { "ab|\ncd", "d", "ab|cd" },
  { "ab|", "d", "ab|" },
  { "a|\xC3\xA9" "b", "d", "a|b" },
  { "|\n\n", "d", "|\n" },
  { "|foo bar", "w", "|bar" },
  { "|foo.bar", "w", "|.bar" },
  { "foo|\nbar", "w", "foo|bar" },
  { "|   foo", "w", "|foo" },
  { "foo bar|", "W", "foo |" },
  { "foo   |bar", "W", "|bar" },
  { "ab\n|cd", "W", "ab|cd" },
  { "|ab", "W", "|ab" },
  { "f^oo b|ar", "W", "f|ar" },
  { "|foo bar", "r", "foo |bar" },
  { "foo|\nbar", "r", "foo\n|bar" },
  { "|foo(bar)", "r", "foo|(bar)" },
  { "foo(|bar)", "r", "foo(bar|)" },
  { "foo|", "r", "foo|" },
  { "x\xC3\xA9|y z", "r", "x\xC3\xA9y |z" },
  { "foo bar|", "l", "foo |bar" },
  { "foo\n|bar", "l", "foo|\nbar" },
  { "foo   |bar", "l", "|foo   bar" },
  { "|foo", "l", "|foo" },
  { "a.|.b", "l", "a|..b" },
  { "fo|o bar", "a", "^foo| bar" },
  { "foo| bar", "a", "^foo| bar" },
  { "|foo bar", "a", "^foo| bar" },
  { "foo |  bar", "a", "foo^   |bar" },
  { "foo bar|\nx", "a", "foo ^bar|\nx" },
  { "a+|+b", "a", "a^++|b" },
  { "|", "a", "|" },
  { "  ab|c", "h", "  |abc" },
  { "  |abc", "h", "|  abc" },
  { " | abc", "h", "  |abc" },
  { "x\n\t|", "h", "x\n|\t" },
  { "abc|", "h", "|abc" },
  { "a|bc", "oX", "aX|c" },
  { "ab|\ncd", "oX", "abX|\ncd" },
  { "a|\xC3\xA9" "b", "oX", "aX|b" },
  { "a^b|c", "oX", "aX|c" },
  { "ab|", "oX", "abX|" },
  { "ab|", "T", "ab\t|" },
  { "a|b", "pX\nY", "aX\nY|b" },
  { "a|b", "p1\r\n2\r3\n", "a1\n2\n3\n|b" },
  { "a^xx|b", "p", "a|b" },
};

static int edt_run_op(EdCtx* e, const char* op)
{
  Vec<ecp> cps;
  u32 rep = 0;
  char k = op[0];
  es_utf8_to_ecp(op + 1, es_len(op + 1), &cps);
  switch (k) {
  case 't': return ed_type(e, cps[0]);
  case 'o': e->overwrite = 1; return ed_type(e, cps[0]);
  case 'T': return ed_type(e, '\t');
  case 'n': return ed_newline(e);
  case 'b': return ed_backspace(e);
  case 'd': return ed_delete(e);
  case 'w': return ed_delete_word(e, 1);
  case 'W': return ed_delete_word(e, 0);
  case 'r': e->sel.anchor = e->sel.caret = ed_word_right(e->doc, e->sel.caret); return E_OK;
  case 'l': e->sel.anchor = e->sel.caret = ed_word_left(e->doc, e->sel.caret); return E_OK;
  case 'h': e->sel.anchor = e->sel.caret = ed_smart_home(e->doc, e->sel.caret); return E_OK;
  case 'a': ed_word_at(e->doc, e->sel.caret, &e->sel); return E_OK;
  case 'p': return ed_insert_text(e, cps.data(), cps.size(), &rep);
  default: return E_STATE;
  }
}

static void test_golden(TF* tf)
{
  EdtKind kinds[6];
  u32 i, k;
  edt_kinds(kinds);
  for (k = 0; k < 6; k++) {
    for (i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
      const EdtCase& c = k_cases[i];
      Selection s;
      EdCtx e;
      EStr got;
      Document* d = edt_make(tf, c.before, kinds[k].enc, kinds[k].eol, &s);
      int rc;
      edt_ctx(&e, d, s);
      rc = edt_run_op(&e, c.op);
      edt_render(d, e.sel, &got);
      if (rc != E_OK || strcmp(got.c_str(), c.after) != 0)
        tf_fail(tf, __FILE__, __LINE__, "%s case %u '%s' op '%s': rc %d got '%s' want '%s'", kinds[k].name, i, c.before,
                c.op, rc, got.c_str(), c.after);
      else TF_CHECK(1);
      // undo restores the original text and selection, redo the result
      if (d->can_undo() && c.op[0] != 'r' && c.op[0] != 'l' && c.op[0] != 'h' && c.op[0] != 'a') {
        Selection us;
        EStr orig;
        Selection s0;
        Document* d0 = edt_make(tf, c.before, kinds[k].enc, kinds[k].eol, &s0);
        TF_EQ_I(d->undo(&us), E_OK);
        edt_render(d, us, &got);
        edt_render(d0, s0, &orig);
        TF_EQ_S(got.c_str(), orig.c_str());
        TF_EQ_I(d->redo(&us), E_OK);
        edt_render(d, us, &got);
        TF_EQ_S(got.c_str(), c.after);
        delete d0;
      }
      delete d;
    }
  }
}

// Tab with "insert spaces": spaces to the next tab stop (display cells, so tabs and wide chars count).
static void test_tab_spaces(TF* tf)
{
  static const char* const cases[][2] = {
    { "ab|", "ab  |" }, { "|", "    |" }, { "abcd|", "abcd    |" }, { "\t|", "\t    |" }, { "a\tb|", "a\tb   |" },
    { "a^bc|d", "a   |d" }, { "x\nab|", "x\nab  |" },
  };
  EdtKind kinds[6];
  u32 i, k;
  edt_kinds(kinds);
  for (k = 0; k < 6; k++) {
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      Selection s;
      EdCtx e;
      EStr got;
      Document* d = edt_make(tf, cases[i][0], kinds[k].enc, kinds[k].eol, &s);
      edt_ctx(&e, d, s);
      e.lang.use_spaces = 1;
      TF_EQ_I(ed_type(&e, '\t'), E_OK);
      edt_render(d, e.sel, &got);
      TF_EQ_S(got.c_str(), cases[i][1]);
      delete d;
    }
  }
  {   // tab width 8 and a wide char (2 cells)
    Selection s;
    EdCtx e;
    EStr got;
    Document* d = edt_make(tf, "\xE4\xB8\xAD|", enc_utf8(0), EOL_LF, &s);
    edt_ctx(&e, d, s);
    e.lang.use_spaces = 1; e.lang.tab_width = 8;
    TF_EQ_I(ed_type(&e, '\t'), E_OK);
    edt_render(d, e.sel, &got);
    TF_EQ_S(got.c_str(), "\xE4\xB8\xAD      |");
    delete d;
  }
}

static void test_errors(TF* tf)
{
  Selection s;
  EdCtx e;
  EStr got;
  ecp txt[4];
  u32 rep = 9;
  Document* d = edt_make(tf, "ab|c", enc_ansi(), EOL_CRLF, &s);
  edt_ctx(&e, d, s);
  TF_EQ_I(ed_type(&e, 0x416), E_RANGE);              // Cyrillic Zhe: not in cp1252
  edt_render(d, e.sel, &got);
  TF_EQ_S(got.c_str(), "ab|c");
  TF_CHECK(!d->can_undo());
  txt[0] = 'x'; txt[1] = 0x416; txt[2] = 0x4E2D; txt[3] = 'y';
  TF_EQ_I(ed_insert_text(&e, txt, 4, &rep), E_OK);    // paste: '?' for each unrepresentable char
  TF_EQ_U(rep, 2);
  edt_render(d, e.sel, &got);
  TF_EQ_S(got.c_str(), "abx??y|c");
  d->set_read_only(true);
  TF_EQ_I(ed_type(&e, 'z'), E_READONLY);
  TF_EQ_I(ed_backspace(&e), E_READONLY);
  TF_EQ_I(ed_delete(&e), E_READONLY);
  TF_EQ_I(ed_newline(&e), E_READONLY);
  TF_EQ_I(ed_delete_word(&e, 1), E_READONLY);
  TF_EQ_I(ed_insert_bytes(&e, (const u8*)"q", 1), E_READONLY);
  d->set_read_only(false);
  d->set_busy(true);
  TF_EQ_I(ed_type(&e, 'z'), E_BUSY);
  TF_EQ_I(ed_insert_text(&e, txt, 1, &rep), E_BUSY);
  d->set_busy(false);
  edt_render(d, e.sel, &got);
  TF_EQ_S(got.c_str(), "abx??y|c");
  delete d;

  // UTF-16: raw bytes must be whole units; an odd trailing byte blocks inserts after it but can be deleted
  d = edt_make(tf, "ab|", enc_utf16le(1), EOL_LF, &s);
  edt_ctx(&e, d, s);
  TF_EQ_I(ed_insert_bytes(&e, (const u8*)"x", 1), E_ALIGN);
  TF_EQ_I(ed_insert_bytes(&e, (const u8*)"x\0", 2), E_OK);
  edt_render(d, e.sel, &got);
  TF_EQ_S(got.c_str(), "abx|");
  TF_EQ_I(d->replace(d->length(), 0, (const u8*)"\x41", 1), E_ALIGN);
  delete d;
  {
    MemSource* src = new MemSource();
    d = new Document();
    src->copy_from((const u8*)"\xFF\xFE" "a\0" "\x42", 5);   // BOM, 'a', odd byte 0x42
    TF_EQ_I(d->init_source(src, enc_utf16le(1), 2, EOL_LF, 0), E_OK);
    s.anchor = s.caret = 5;
    edt_ctx(&e, d, s);
    TF_EQ_I(ed_type(&e, 'z'), E_ALIGN);
    TF_EQ_I(ed_backspace(&e), E_OK);                    // deletes the dangling byte
    TF_EQ_U(d->length(), 4);
    TF_EQ_U(e.sel.caret, 4);
    TF_EQ_I(ed_type(&e, 'z'), E_OK);
    TF_EQ_U(d->length(), 6);
    delete d;
  }
}

// Bad bytes, lone CRs, combining marks, CR-mode documents.
static void test_special(TF* tf)
{
  Selection s;
  EdCtx e;
  EStr got;
  MemSource* src;
  Document* d = new Document();
  src = new MemSource();
  src->copy_from((const u8*)"a\xFF" "b\r" "c\r\nd", 8);   // UTF-8: bad byte FF, lone CR, then CRLF
  TF_EQ_I(d->init_source(src, enc_utf8(0), 0, EOL_CRLF, 0), E_OK);
  s.anchor = s.caret = 1;
  edt_ctx(&e, d, s);
  TF_EQ_I(ed_delete(&e), E_OK);                         // one bad byte = one character
  edt_render(d, e.sel, &got);
  TF_EQ_S(got.c_str(), "a|b<CR>c\nd");
  e.sel.anchor = e.sel.caret = 2;
  TF_EQ_I(ed_delete(&e), E_OK);                         // the lone CR is an ordinary character
  edt_render(d, e.sel, &got);
  TF_EQ_S(got.c_str(), "ab|c\nd");
  TF_EQ_U(ed_word_right(d, 0), 3);                       // "abc" then the EOL stop
  TF_EQ_U(ed_word_right(d, 3), 5);
  delete d;

  // combining mark: Backspace removes the mark only, Del from before the base removes the base only
  d = edt_make(tf, "e\xCC\x81|x", enc_utf8(0), EOL_LF, &s);   // e + U+0301
  edt_ctx(&e, d, s);
  TF_EQ_I(ed_backspace(&e), E_OK);
  edt_render(d, e.sel, &got);
  TF_EQ_S(got.c_str(), "e|x");
  delete d;

  // CR mode (classic Mac): Enter inserts CR, Backspace at a line start removes it
  d = edt_make(tf, "ab|\ncd", enc_ansi(), EOL_CR, &s);
  TF_EQ_U(d->line_count(), 2);
  edt_ctx(&e, d, s);
  TF_EQ_I(ed_newline(&e), E_OK);
  edt_render(d, e.sel, &got);
  TF_EQ_S(got.c_str(), "ab\n|\ncd");
  TF_EQ_U(d->line_count(), 3);
  TF_EQ_I(ed_backspace(&e), E_OK);
  TF_EQ_I(ed_delete(&e), E_OK);
  edt_render(d, e.sel, &got);
  TF_EQ_S(got.c_str(), "ab|cd");
  delete d;

  // the BOM is never touched: Backspace/Ctrl+Backspace at the origin, a selection reaching below the origin
  d = edt_make(tf, "|ab", enc_utf8(1), EOL_LF, &s);
  edt_ctx(&e, d, s);
  TF_EQ_I(ed_backspace(&e), E_OK);
  TF_EQ_I(ed_delete_word(&e, 0), E_OK);
  TF_EQ_U(d->length(), 5);
  e.sel.anchor = 0; e.sel.caret = 4;
  TF_EQ_I(ed_backspace(&e), E_OK);
  TF_EQ_U(d->length(), 4);
  TF_EQ_U(e.sel.caret, 3);
  TF_EQ_U(ed_word_left(d, 3), 3);
  TF_EQ_U(ed_smart_home(d, 4), 3);
  delete d;

  // mixed EOLs: Backspace/Del remove the line's own EOL whatever its kind
  d = new Document();
  src = new MemSource();
  src->copy_from((const u8*)"a\r\nb\nc", 6);
  TF_EQ_I(d->init_source(src, enc_ansi(), 0, EOL_CRLF, 0), E_OK);
  s.anchor = s.caret = 3;
  edt_ctx(&e, d, s);
  TF_EQ_I(ed_backspace(&e), E_OK);                      // removes CRLF
  TF_EQ_U(d->length(), 4);
  TF_EQ_U(e.sel.caret, 1);
  e.sel.anchor = e.sel.caret = 2;
  TF_EQ_I(ed_delete(&e), E_OK);                         // removes the bare LF
  edt_render(d, e.sel, &got);
  TF_EQ_S(got.c_str(), "ab|c");
  delete d;
}

// Undo grouping: consecutive typing merges (one undo), Enter breaks it, Backspace runs merge, a 2 s pause splits.
static void test_undo_groups(TF* tf)
{
  Selection s, us;
  EdCtx e;
  EStr got;
  Document* d = edt_make(tf, "|", enc_utf8(0), EOL_LF, &s);
  edt_ctx(&e, d, s);
  TF_EQ_I(ed_type(&e, 'a'), E_OK); e.now_ms += 100;
  TF_EQ_I(ed_type(&e, 'b'), E_OK); e.now_ms += 100;
  TF_EQ_I(ed_type(&e, 'c'), E_OK); e.now_ms += 100;
  TF_EQ_I(ed_newline(&e), E_OK); e.now_ms += 100;
  TF_EQ_I(ed_type(&e, 'd'), E_OK); e.now_ms += 100;
  TF_EQ_I(ed_type(&e, 'e'), E_OK); e.now_ms += 3000;
  TF_EQ_I(ed_type(&e, 'f'), E_OK); e.now_ms += 100;
  TF_EQ_I(ed_backspace(&e), E_OK); e.now_ms += 100;
  TF_EQ_I(ed_backspace(&e), E_OK); e.now_ms += 100;
  edt_render(d, e.sel, &got);
  TF_EQ_S(got.c_str(), "abc\nd|");
  TF_EQ_I(d->undo(&us), E_OK);                          // both backspaces
  edt_render(d, us, &got);
  TF_EQ_S(got.c_str(), "abc\ndef|");
  TF_EQ_I(d->undo(&us), E_OK);                          // 'f' (after the pause)
  edt_render(d, us, &got);
  TF_EQ_S(got.c_str(), "abc\nde|");
  TF_EQ_I(d->undo(&us), E_OK);                          // "de"
  edt_render(d, us, &got);
  TF_EQ_S(got.c_str(), "abc\n|");
  TF_EQ_I(d->undo(&us), E_OK);                          // Enter
  edt_render(d, us, &got);
  TF_EQ_S(got.c_str(), "abc|");
  TF_EQ_I(d->undo(&us), E_OK);                          // "abc"
  edt_render(d, us, &got);
  TF_EQ_S(got.c_str(), "|");
  TF_CHECK(!d->can_undo());
  delete d;
}

static void test_lang_from(TF* tf)
{
  EdLang l;
  LangInfo li;
  EStr err;
  static const char* const k_lng = "id=tst\nname=Test\nextensions=tst\ncomment=REM\\s\ncomment_block=(* *)\ntab_width=3\nuse_tabs=1\n"
                                   "braces=<>\n";
  ed_lang_from(0, 8, 1, &l);
  TF_EQ_U(l.tab_width, 8);
  TF_EQ_U(l.use_spaces, 1);
  TF_EQ_U(l.lc_len, 0);
  TF_EQ_S(l.braces, "()[]{}");
  memset(&li, 0, sizeof(li));
  if (lang_parse(k_lng, es_len(k_lng), &li, &err) == E_OK) {
    ed_lang_from(&li, 4, 1, &l);
    TF_EQ_U(l.lc_len, 4);
    TF_EQ_U(l.lc[0], 'R');
    TF_EQ_U(l.lc[3], ' ');
    TF_EQ_U(l.bo_len, 2);
    TF_EQ_U(l.bc[1], ')');
    TF_EQ_U(l.tab_width, 3);
    TF_EQ_U(l.use_spaces, 0);
    TF_EQ_S(l.braces, "<>");
  } else tf_fail(tf, __FILE__, __LINE__, "lang_parse: %s", err.c_str());
  lang_shutdown();                                      // frees the lexer lang_parse created
}

// Differential random test against a code-point model (insert/overwrite typing, Enter with auto-indent, Backspace,
// Del, paste), all six document kinds, with undo/redo of everything at the end.
struct EdtModel { Vec<ecp> t; u32 anchor, caret; };

static u32 edt_line_start(const EdtModel& m, u32 i) { while (i > 0 && m.t[i - 1] != '\n') i--; return i; }

static void edt_model_replace(EdtModel& m, u32 lo, u32 hi, const ecp* s, u32 n)
{
  m.t.erase(lo, hi - lo);
  m.t.insert(lo, s, n);
  m.anchor = m.caret = lo + n;
}

static void edt_model_text(EdtModel& m, EStr* out)
{
  u32 i;
  out->clear();
  for (i = 0; i <= m.t.size(); i++) {
    char u[8];
    if (i == m.caret) out->append("|");
    else if (i == m.anchor) out->append("^");
    if (i == m.t.size()) break;
    if (m.t[i] == '\n') { out->append("\n"); continue; }
    u[es_utf8_encode(m.t[i], u)] = 0;
    out->append(u);
  }
}

static void test_random(TF* tf)
{
  static const ecp alpha[] = { 'a', 'b', ' ', '\t', '.', 0xE9, 0x416, 0x4E2D, 0x1F600, '\n' };
  EdtKind kinds[6];
  u32 k, it, iters = tf->long_run ? 6000 : 1200;
  edt_kinds(kinds);
  for (k = 0; k < 6; k++) {
    EdtModel m;
    Selection s;
    EdCtx e;
    EStr got, want;
    Document* d = edt_make(tf, "|", kinds[k].enc, kinds[k].eol, &s);
    const Codec* c = codec_get(kinds[k].enc);
    u32 fails = 0, undos = 0;
    m.anchor = m.caret = 0;
    edt_ctx(&e, d, s);
    for (it = 0; it < iters && fails < 5; it++) {
      u32 op = tf_rand_below(tf, 100), lo, hi, n = m.t.size();
      int rc;
      u8 tmp[8];
      // random selection on character boundaries (model indices -> document offsets via rendering positions)
      if (tf_rand_below(tf, 4) == 0) {
        u32 a = tf_rand_below(tf, n + 1), b = tf_rand_below(tf, 4) == 0 ? tf_rand_below(tf, n + 1) : a;
        epos pa = d->text_origin(), pb = d->text_origin(), p = d->text_origin();
        u32 i;
        for (i = 0; i <= n; i++) {
          if (i == a) pa = p;
          if (i == b) pb = p;
          if (i < n) {
            ecp ch;
            u32 used = d->decode(p, &ch);
            if (ch == '\r') { ecp nx; u32 u2 = d->decode(p + used, &nx); if (u2 && nx == '\n') used += u2; }
            p += used;
          }
        }
        m.anchor = a; m.caret = b;
        e.sel.anchor = pa; e.sel.caret = pb;
      }
      lo = emin(m.anchor, m.caret); hi = emax(m.anchor, m.caret);
      e.overwrite = 0;
      e.now_ms += 50;
      if (op < 45) {                                         // type
        ecp ch = alpha[tf_rand_below(tf, 9)];
        int ow = tf_rand_below(tf, 5) == 0;
        int repr = c->encode(ch, tmp) != 0;
        e.overwrite = (u8)ow;
        rc = ed_type(&e, ch);
        if (!repr) { TF_EQ_I(rc, E_RANGE); }
        else {
          if (lo == hi && ow && lo < n && m.t[lo] != '\n') hi = lo + 1;
          edt_model_replace(m, lo, hi, &ch, 1);
          TF_EQ_I(rc, E_OK);
        }
      } else if (op < 55) {                                  // Enter
        Vec<ecp> ins;
        u32 ls = edt_line_start(m, lo), p = ls;
        ecp nl = '\n';
        ins.push(nl);
        while (p < lo && (m.t[p] == ' ' || m.t[p] == '\t')) { ins.push(m.t[p]); p++; }
        rc = ed_newline(&e);
        TF_EQ_I(rc, E_OK);
        edt_model_replace(m, lo, hi, ins.data(), ins.size());
      } else if (op < 72) {                                  // Backspace
        rc = ed_backspace(&e);
        TF_EQ_I(rc, E_OK);
        if (lo != hi) edt_model_replace(m, lo, hi, 0, 0);
        else if (lo > 0) edt_model_replace(m, lo - 1, lo, 0, 0);
      } else if (op < 88) {                                  // Del
        rc = ed_delete(&e);
        TF_EQ_I(rc, E_OK);
        if (lo != hi) edt_model_replace(m, lo, hi, 0, 0);
        else if (lo < n) { edt_model_replace(m, lo, lo + 1, 0, 0); m.anchor = m.caret = lo; }
      } else {                                               // paste
        ecp p[6];
        u32 pn = 1 + tf_rand_below(tf, 6), i, rep = 0, bad = 0;
        Vec<ecp> norm;
        for (i = 0; i < pn; i++) {
          p[i] = alpha[tf_rand_below(tf, 10)];
          if (p[i] == '\n' && tf_rand_below(tf, 2)) p[i] = '\r';
        }
        for (i = 0; i < pn; i++) {
          ecp ch = p[i];
          if (ch == '\r') { if (i + 1 < pn && p[i + 1] == '\n') i++; ch = '\n'; }
          else if (ch != '\n' && !c->encode(ch, tmp)) { ch = '?'; bad++; }
          norm.push(ch);
        }
        rc = ed_insert_text(&e, p, pn, &rep);
        TF_EQ_I(rc, E_OK);
        TF_EQ_U(rep, bad);
        edt_model_replace(m, lo, hi, norm.data(), norm.size());
      }
      edt_render(d, e.sel, &got);
      edt_model_text(m, &want);
      if (strcmp(got.c_str(), want.c_str()) != 0) {
        fails++;
        tf_fail(tf, __FILE__, __LINE__, "%s iter %u op %u: got '%s' want '%s'", kinds[k].name, it, op, got.c_str(),
                want.c_str());
        m.t.clear();   // resync the model from the document
        {
          epos p = d->text_origin();
          while (p < d->length()) {
            ecp ch; u32 used = d->decode(p, &ch);
            if (ch == '\r') { ecp nx; u32 u2 = d->decode(p + used, &nx); if (u2 && nx == '\n') { used += u2; ch = '\n'; } }
            m.t.push(ch);
            p += used;
          }
          m.anchor = m.caret = m.t.size();
          e.sel.anchor = e.sel.caret = d->length();
        }
      } else TF_CHECK(1);
    }
    while (d->can_undo()) { Selection us; if (d->undo(&us) != E_OK) break; undos++; }
    TF_EQ_U(d->length(), d->text_origin());
    TF_CHECK(!d->modified());
    while (d->can_redo()) { Selection us; if (d->redo(&us) != E_OK) break; }
    edt_render(d, e.sel, &got);
    m.anchor = m.caret = 0xFFFFFFFFu;
    e.sel.anchor = e.sel.caret = EPOS_NONE;
    edt_render(d, e.sel, &got);
    edt_model_text(m, &want);
    TF_EQ_S(got.c_str(), want.c_str());
    tf_log(tf, "edtype random %s: %u ops, %u undo groups", kinds[k].name, iters, undos);
    delete d;
  }
}

void suite_edtype(TF* tf)
{
  enc_set_system(1252, 437);
  codec_init();
  test_golden(tf);
  test_tab_spaces(tf);
  test_errors(tf);
  test_special(tf);
  test_undo_groups(tf);
  test_lang_from(tf);
  test_random(tf);
}
