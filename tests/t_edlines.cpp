// t_edlines.cpp - suite "edlines" (T12): golden before/after tables for the line commands, comments, case, indent
// and brace matching, run in ANSI, UTF-8 and UTF-16 LE (with BOM) documents with CRLF and LF line ends, plus undo
// round trips and out-of-memory rollback.
//
// Case notation (UTF-8 source text): '|' = caret, '^' = anchor (absent: anchor = caret), '\n' = the document EOL.
// Brace cases: '|' = position asked, '@' = expected brace, '#' = expected match.
#include "tf.h"
#include "edops.h"
#include "edlines_int.h"
#include "docbind.h"
#include "codec.h"
#include <string.h>

struct ElVariant { const char* name; int enc; EolStyle eol; };
static const ElVariant g_el_variants[] = {
  { "ansi-crlf", 0, EOL_CRLF }, { "ansi-lf", 0, EOL_LF },
  { "utf8-crlf", 1, EOL_CRLF }, { "utf8-lf", 1, EOL_LF },
  { "utf16le-crlf", 2, EOL_CRLF }, { "utf16le-lf", 2, EOL_LF }
};
enum { EL_NVAR = 6, EL_NMARK = 4 };

static EncSpec el_enc(int k)
{
  if (k == 0) return enc_cp(1252);
  if (k == 1) return enc_utf8(0);
  return enc_utf16le(1);
}

// Encodes spec into out (document bytes after the origin); marks[i] = byte offset (origin included) of marker
// chars "|^@#"[i], EPOS_NONE if absent.
static int el_build(const char* spec, const EncSpec& enc, EolStyle eol, epos origin, EBytes* out, epos* marks)
{
  const char* mk = "|^@#";
  Vec<ecp> cps;
  const Codec* c = codec_get(enc);
  u32 i, k;
  out->clear();
  for (k = 0; k < EL_NMARK; k++) marks[k] = EPOS_NONE;
  if (!c) return E_UNSUPPORTED;
  if (es_utf8_to_ecp(spec, es_len(spec), &cps) != E_OK) return E_NOMEM;
  for (i = 0; i < cps.size(); i++) {
    ecp ch = cps[i];
    u8 b[8];
    u32 n;
    int is_mark = 0;
    for (k = 0; k < EL_NMARK; k++) if (ch == (ecp)mk[k]) { marks[k] = origin + out->size(); is_mark = 1; }
    if (is_mark) continue;
    if (ch == '\n') {
      if (eol == EOL_CRLF) { n = c->encode('\r', b); out->append(b, n); }
      n = c->encode(eol == EOL_CR ? '\r' : '\n', b); out->append(b, n);
      continue;
    }
    n = c->encode(ch, b);
    if (!n) return E_RANGE;
    if (out->append(b, n) != E_OK) return E_NOMEM;
  }
  return E_OK;
}

static Document* el_doc(const EncSpec& enc, EolStyle eol, const EBytes& body)
{
  Document* d = new Document;
  if (d->init_empty(enc, eol) != E_OK) { delete d; return 0; }
  if (body.size() && d->replace(d->text_origin(), 0, body.data(), body.size()) != E_OK) { delete d; return 0; }
  d->clear_undo();
  return d;
}

static int el_doc_equals(Document* d, const EBytes& body)
{
  EBytes all;
  epos o = d->text_origin();
  if (d->length() != o + body.size()) return 0;
  if (all.resize(body.size()) != E_OK) return 0;
  if (body.size() && d->read(o, all.data(), body.size()) != E_OK) return 0;
  return body.size() == 0 || memcmp(all.data(), body.data(), body.size()) == 0;
}

static void el_dump(TF* tf, Document* d)
{
  EBytes all;
  char line[400];
  u32 i, k = 0;
  all.resize(d->length());
  d->read(0, all.data(), all.size());
  for (i = 0; i < all.size() && k + 4 < sizeof(line); i++) k += (u32)es_fmt(line + k, sizeof(line) - k, "%02X ", all[i]);
  line[k] = 0;
  tf_log(tf, "    doc: %s", line);
}

static void el_lang_c(EdLang* l)
{
  memset(l, 0, sizeof(*l));
  l->lc[0] = '/'; l->lc[1] = '/'; l->lc_len = 2;
  l->bo[0] = '/'; l->bo[1] = '*'; l->bo_len = 2;
  l->bc[0] = '*'; l->bc[1] = '/'; l->bc_len = 2;
  l->tab_width = 4; l->use_spaces = 0;
  memcpy(l->braces, "()[]{}", 7);
}

typedef int (*ElOp)(EdCtx* e, int arg);
static int el_op_dup(EdCtx* e, int arg) { (void)arg; return ed_duplicate(e); }
static int el_op_del(EdCtx* e, int arg) { (void)arg; return ed_delete_lines(e); }
static int el_op_move(EdCtx* e, int arg) { return ed_move_lines(e, arg); }
static int el_op_cmt(EdCtx* e, int arg) { (void)arg; return ed_toggle_comment(e); }
static int el_op_case(EdCtx* e, int arg) { return ed_change_case(e, arg); }
static int el_op_ind(EdCtx* e, int arg) { return ed_indent(e, arg); }

// Runs one golden case in the variants selected by mask (bit i = g_el_variants[i]).
static void el_case_mask(TF* tf, const char* name, const char* before, const char* after, ElOp op, int arg,
                         int want_rc, const EdLang* lang, u32 mask)
{
  u32 v;
  for (v = 0; v < EL_NVAR; v++) {
    const ElVariant& V = g_el_variants[v];
    EncSpec enc = el_enc(V.enc);
    EBytes b0, b1;
    epos m0[EL_NMARK], m1[EL_NMARK], origin;
    Document* d;
    EdCtx e;
    Selection want, und;
    int rc;
    if (!(mask & (1u << v))) continue;
    origin = enc.bom ? 2 : 0;
    if (el_build(before, enc, V.eol, origin, &b0, m0) != E_OK || el_build(after, enc, V.eol, origin, &b1, m1) != E_OK) {
      tf_fail(tf, __FILE__, __LINE__, "%s [%s]: case text not encodable", name, V.name);
      continue;
    }
    d = el_doc(enc, V.eol, b0);
    if (!d) { tf_fail(tf, __FILE__, __LINE__, "%s [%s]: doc", name, V.name); continue; }
    memset(&e, 0, sizeof(e));
    e.doc = d;
    e.sel.caret = m0[0] == EPOS_NONE ? origin : m0[0];
    e.sel.anchor = m0[1] == EPOS_NONE ? e.sel.caret : m0[1];
    e.lang = *lang;
    e.now_ms = 1000;
    rc = op(&e, arg);
    want.caret = m1[0] == EPOS_NONE ? origin : m1[0];
    want.anchor = m1[1] == EPOS_NONE ? want.caret : m1[1];
    if (rc != want_rc) tf_fail(tf, __FILE__, __LINE__, "%s [%s]: rc %s, want %s", name, V.name, e_errname(rc), e_errname(want_rc));
    if (!el_doc_equals(d, b1)) {
      tf_fail(tf, __FILE__, __LINE__, "%s [%s]: bytes differ", name, V.name);
      el_dump(tf, d);
    } else tf->checks++;
    if (e.sel.anchor != want.anchor || e.sel.caret != want.caret)
      tf_fail(tf, __FILE__, __LINE__, "%s [%s]: sel %u,%u want %u,%u", name, V.name, e.sel.anchor, e.sel.caret,
              want.anchor, want.caret);
    else tf->checks++;
    // undo restores the bytes and the selection before the command
    if (d->can_undo()) {
      TF_CHECK(d->undo(&und) == E_OK);
      if (!el_doc_equals(d, b0)) tf_fail(tf, __FILE__, __LINE__, "%s [%s]: undo bytes differ", name, V.name);
      else tf->checks++;
      TF_CHECK(und.caret == (m0[0] == EPOS_NONE ? origin : m0[0]));
      TF_CHECK(d->redo(&und) == E_OK);
      TF_CHECK(el_doc_equals(d, b1));
    } else {
      TF_CHECK(el_doc_equals(d, b0));        // no group: the command changed nothing
    }
    delete d;
  }
}

static void el_case(TF* tf, const char* name, const char* before, const char* after, ElOp op, int arg,
                    const EdLang* lang)
{
  el_case_mask(tf, name, before, after, op, arg, E_OK, lang, 0x3F);
}

// ---------------------------------------------------------------------------------------------------------------

static void el_test_duplicate(TF* tf, const EdLang* L)
{
  el_case(tf, "dup line", "ab|c\ndef", "ab|c\nabc\ndef", el_op_dup, 0, L);
  el_case(tf, "dup last line", "abc\nde|f", "abc\nde|f\ndef", el_op_dup, 0, L);
  el_case(tf, "dup caret at eol", "abc|\ndef", "abc|\nabc\ndef", el_op_dup, 0, L);
  el_case(tf, "dup selection", "^ab|c\n", "^ab|abc\n", el_op_dup, 0, L);
  el_case(tf, "dup selection reversed", "|ab^c\n", "|ab^abc\n", el_op_dup, 0, L);
  el_case(tf, "dup multi-line selection", "a^b\nc|d", "a^b\nc|b\ncd", el_op_dup, 0, L);
  el_case(tf, "dup empty doc", "|", "|\n", el_op_dup, 0, L);
  el_case(tf, "dup empty last line", "abc\n|", "abc\n|\n", el_op_dup, 0, L);
  el_case(tf, "dup non-ascii", "\xC3\xA9t\xC3\xA9|", "\xC3\xA9t\xC3\xA9|\n\xC3\xA9t\xC3\xA9", el_op_dup, 0, L);
}

static void el_test_delete(TF* tf, const EdLang* L)
{
  el_case(tf, "del middle", "abc\nd|ef\nghi", "abc\n|ghi", el_op_del, 0, L);
  el_case(tf, "del first", "a|bc\ndef", "|def", el_op_del, 0, L);
  el_case(tf, "del last keeps eol", "abc\ndef\ngh|i", "abc\ndef\n|", el_op_del, 0, L);
  el_case(tf, "del two lines", "a^bc\nde|f\nghi", "|ghi", el_op_del, 0, L);
  el_case(tf, "del sel ends at col 0", "abc\n^def\n|ghi", "abc\n|ghi", el_op_del, 0, L);
  el_case(tf, "del empty last line", "abc\n|", "abc\n|", el_op_del, 0, L);
  el_case(tf, "del only line", "a|bc", "|", el_op_del, 0, L);
}

static void el_test_move(TF* tf, const EdLang* L)
{
  el_case(tf, "up", "abc\nd|ef\nghi", "|def\n^abc\nghi", el_op_move, 0, L);
  el_case(tf, "down", "a|bc\ndef\nghi", "def\n|abc\n^ghi", el_op_move, 1, L);
  el_case(tf, "down onto last line", "abc\nd|ef\nghi", "abc\nghi\n|def^", el_op_move, 1, L);
  el_case(tf, "up from last line", "abc\ndef\ng|hi", "abc\n|ghi\n^def", el_op_move, 0, L);
  el_case(tf, "up at top", "a|bc\ndef", "a|bc\ndef", el_op_move, 0, L);
  el_case(tf, "down at bottom", "abc\nd|ef", "abc\nd|ef", el_op_move, 1, L);
  el_case(tf, "up block", "abc\nd^ef\ng|hi\njkl", "|def\nghi\n^abc\njkl", el_op_move, 0, L);
  el_case(tf, "down sel ends col 0", "abc\n^def\n|ghi\njkl", "abc\nghi\n|def\n^jkl", el_op_move, 1, L);
  el_case(tf, "up empty last line", "abc\ndef\n|", "abc\n|\n^def", el_op_move, 0, L);
  el_case(tf, "down before empty last", "a|bc\n", "\n|abc^", el_op_move, 1, L);
  el_case(tf, "down only line", "a|bc", "a|bc", el_op_move, 1, L);
  el_case(tf, "up whole doc block", "^abc\ndef|", "^abc\ndef|", el_op_move, 0, L);
}

static void el_test_comment(TF* tf, const EdLang* L)
{
  EdLang bat, css, none;
  el_case(tf, "cmt add", "a|bc", "// a|bc", el_op_cmt, 0, L);
  el_case(tf, "cmt indent", "  x|y", "  // x|y", el_op_cmt, 0, L);
  el_case(tf, "cmt tab indent", "\tx|y", "\t// x|y", el_op_cmt, 0, L);
  el_case(tf, "cmt remove", "// a|bc", "a|bc", el_op_cmt, 0, L);
  el_case(tf, "cmt remove nospace", "//a|bc", "a|bc", el_op_cmt, 0, L);
  el_case(tf, "cmt remove indented", "    // a|bc", "    a|bc", el_op_cmt, 0, L);
  el_case(tf, "cmt caret in token", "/|/ abc", "|abc", el_op_cmt, 0, L);
  el_case(tf, "cmt mixed", "^a\n// b\n\n  c|", "// ^a\nb\n\n  // c|", el_op_cmt, 0, L);
  el_case(tf, "cmt sel ends col 0", "^a\nb\n|c", "// ^a\n// b\n|c", el_op_cmt, 0, L);
  el_case(tf, "cmt caret first", "|ab^c", "// |ab^c", el_op_cmt, 0, L);
  el_case(tf, "cmt blank line", "  |", "  |", el_op_cmt, 0, L);
  el_case(tf, "cmt empty doc", "|", "|", el_op_cmt, 0, L);
  el_case(tf, "cmt uncomment block", "^// a\n// b|", "^a\nb|", el_op_cmt, 0, L);

  bat = *L; bat.lc[0] = 'R'; bat.lc[1] = 'E'; bat.lc[2] = 'M'; bat.lc[3] = ' '; bat.lc_len = 4;
  el_case(tf, "bat add", "echo|", "REM echo|", el_op_cmt, 0, &bat);
  el_case(tf, "bat remove lower", "rem echo|", "echo|", el_op_cmt, 0, &bat);
  el_case(tf, "bat remark", "REMARK|", "REM REMARK|", el_op_cmt, 0, &bat);
  el_case(tf, "bat rem alone", "REM|", "|", el_op_cmt, 0, &bat);

  css = *L; css.lc_len = 0;
  el_case(tf, "css add", "a|b", "/* a|b */", el_op_cmt, 0, &css);
  el_case(tf, "css remove", "/* a|b */", "a|b", el_op_cmt, 0, &css);
  el_case(tf, "css remove nospace", "/*a|b*/", "a|b", el_op_cmt, 0, &css);
  el_case(tf, "css empty comment", "/**/|", "|", el_op_cmt, 0, &css);
  el_case(tf, "css space comment", "/* */|", "|", el_op_cmt, 0, &css);
  el_case(tf, "css two lines", "^ a\nb|", "^ /* a */\n/* b| */", el_op_cmt, 0, &css);

  none = *L; none.lc_len = 0; none.bo_len = 0; none.bc_len = 0;
  el_case_mask(tf, "no comment syntax", "a|b", "a|b", el_op_cmt, 0, E_UNSUPPORTED, &none, 0x3F);
}

static void el_test_case(TF* tf, const EdLang* L)
{
  el_case(tf, "upper sel", "^h\xC3\xA9llo w\xC3\xB6rld|", "^H\xC3\x89LLO W\xC3\x96RLD|", el_op_case, 1, L);
  el_case(tf, "lower reversed", "|aB\xC3\x89^c", "|ab\xC3\xA9^c", el_op_case, 0, L);
  el_case(tf, "upper caret char", "|abc", "A|bc", el_op_case, 1, L);
  el_case(tf, "upper caret already", "|ABC", "A|BC", el_op_case, 1, L);
  el_case(tf, "upper at eol", "abc|\ndef", "abc|\ndef", el_op_case, 1, L);
  el_case(tf, "upper across lines", "a^b\nc|d", "a^B\nC|d", el_op_case, 1, L);
  // U+00B5 MICRO SIGN -> U+039C: not in cp1252 (kept), stored in UTF-8/16
  el_case_mask(tf, "upper unstorable", "^\xC2\xB5x|", "^\xC2\xB5X|", el_op_case, 1, E_OK, L, 0x03);
  el_case_mask(tf, "upper micro unicode", "^\xC2\xB5x|", "^\xCE\x9CX|", el_op_case, 1, E_OK, L, 0x3C);
  // U+0131 dotless i (2 bytes in UTF-8) -> 'I' (1 byte): the end of the selection moves
  el_case_mask(tf, "upper shrinks", "^a\xC4\xB1|b", "^AI|b", el_op_case, 1, E_OK, L, 0x3C);
  el_case_mask(tf, "upper shrinks rev", "|a\xC4\xB1^b", "|AI^b", el_op_case, 1, E_OK, L, 0x3C);
}

static void el_test_indent(TF* tf, const EdLang* L)
{
  EdLang sp = *L;
  sp.use_spaces = 1;
  el_case(tf, "indent multi", "^ab\ncd|", "^\tab\n\tcd|", el_op_ind, 0, L);
  el_case(tf, "outdent multi", "^\tab\n    cd|", "^ab\ncd|", el_op_ind, 1, L);
  el_case(tf, "indent skips empty", "^a\n\nb|", "^\ta\n\n\tb|", el_op_ind, 0, L);
  el_case(tf, "indent spaces", "^ab\n  cd|", "^    ab\n      cd|", el_op_ind, 0, &sp);
  el_case(tf, "indent rewrite", "^  ab\ncd|", "^\t  ab\n\tcd|", el_op_ind, 0, L);
  el_case(tf, "indent sel ends col 0", "^ab\n|cd", "^\tab\n|cd", el_op_ind, 0, L);
  el_case(tf, "indent caret above", "a|b\nc^d\nef", "|\tab\n\tcd\n^ef", el_op_ind, 0, L);
  el_case(tf, "outdent partial", "^ \ta\n  b|", "^a\nb|", el_op_ind, 1, L);
  el_case(tf, "outdent 6 spaces", "^      a\nb|", "^  a\nb|", el_op_ind, 1, &sp);
  el_case(tf, "outdent single", "\tab|", "ab|", el_op_ind, 1, L);
  el_case(tf, "indent single", "a|b", "\ta|b", el_op_ind, 0, L);
  el_case(tf, "outdent single in ws", "\t|\tab", "\t|ab", el_op_ind, 1, L);
  el_case(tf, "outdent nothing", "a|b", "a|b", el_op_ind, 1, L);
}

// ---------------------------------------------------------------------------------------------------------------
// brace matching with a real highlighter

static const char g_el_lng[] =
  "name=Ctest\r\nextensions=ctest\r\nline_comment=//\r\nblock_comment=/* */\r\n"
  "string=\" \" \\\r\nstring=' ' \\ char\r\n";

static void el_brace_case(TF* tf, const char* name, const char* spec, const LangInfo* li)
{
  u32 v;
  for (v = 0; v < EL_NVAR; v++) {
    const ElVariant& V = g_el_variants[v];
    EncSpec enc = el_enc(V.enc);
    EBytes body;
    epos m[EL_NMARK], brace, match, origin = enc.bom ? 2 : 0;
    Document* d;
    Highlighter* hl;
    DocLines lines;
    int rc, want;
    if (el_build(spec, enc, V.eol, origin, &body, m) != E_OK) { tf_fail(tf, __FILE__, __LINE__, "%s: build", name); continue; }
    d = el_doc(enc, V.eol, body);
    hl = new Highlighter;
    lines.bind(d, hl);
    hl->attach(&lines, li);
    rc = ed_match_brace(d, m[0], hl, "()[]{}", &brace, &match);
    want = m[2] == EPOS_NONE ? E_NOTFOUND : (m[3] == EPOS_NONE ? 0 : 1);
    if (rc != want || brace != m[2] || match != m[3])
      tf_fail(tf, __FILE__, __LINE__, "%s [%s]: rc %d brace %u match %u, want %d %u %u", name, V.name, rc, brace, match,
              want, m[2], m[3]);
    else tf->checks++;
    hl->detach();
    lines.unbind();
    delete hl;
    delete d;
  }
}

static void el_test_brace(TF* tf)
{
  LangInfo li;
  EStr err;
  const LangInfo* lp;
  int rc = lang_parse(g_el_lng, es_len(g_el_lng), &li, &err);
  TF_EQ_I(rc, E_OK);
  if (rc != E_OK) { tf_log(tf, "lang_parse: %s", err.c_str()); return; }
  lp = &li;
  el_brace_case(tf, "after open", "a|@(b(c)d#)e", lp);
  el_brace_case(tf, "before close", "a#(b(c)d@)|e", lp);
  el_brace_case(tf, "before priority", "x#(@)|(y)", lp);
  el_brace_case(tf, "string skipped", "#( \")\" |@)", lp);
  el_brace_case(tf, "char skipped", "#{ '}' |@}", lp);
  el_brace_case(tf, "comment skipped", "#{ /* } */ |@}", lp);
  el_brace_case(tf, "line comment skipped", "@|{ // }\n#}", lp);
  el_brace_case(tf, "brace in comment", "/* |( */", lp);
  el_brace_case(tf, "brace in string", "\"a|(\"", lp);
  el_brace_case(tf, "multi-line", "@|{\n  {\n  }\n#}", lp);
  el_brace_case(tf, "block comment lines", "@|{\n/* }\n*/\n#}", lp);
  el_brace_case(tf, "backwards lines", "#[\n[\n]\n[\n]\n|@]", lp);
  el_brace_case(tf, "unmatched", "@|( abc", lp);
  el_brace_case(tf, "unmatched close", "abc |@)", lp);
  el_brace_case(tf, "no brace", "a|b", lp);
  el_brace_case(tf, "empty", "|", lp);
  el_brace_case(tf, "mixed kinds", "@|( [ #) ] )", lp);
  el_brace_case(tf, "end of doc", "#(x@)|", lp);
  el_brace_case(tf, "no highlighter style", "@|( '#)' )", 0);   // plain text: quotes mean nothing
  lang_shutdown();
}

// ---------------------------------------------------------------------------------------------------------------

static void el_test_columns(TF* tf)
{
  EBytes b;
  epos m[EL_NMARK];
  Document* d;
  el_build("a\tb\xC3\xA9x\nline2", enc_utf8(0), EOL_LF, 0, &b, m);
  d = el_doc(enc_utf8(0), EOL_LF, b);
  TF_EQ_U(ed_col_of(d, 0, 4), 0);
  TF_EQ_U(ed_col_of(d, 1, 4), 1);
  TF_EQ_U(ed_col_of(d, 2, 4), 4);
  TF_EQ_U(ed_col_of(d, 3, 4), 5);
  TF_EQ_U(ed_col_of(d, 5, 4), 6);
  TF_EQ_U(ed_col_of(d, 2, 8), 8);
  TF_EQ_U(ed_pos_of_col(d, 0, 0, 4), 0);
  TF_EQ_U(ed_pos_of_col(d, 0, 4, 4), 2);
  TF_EQ_U(ed_pos_of_col(d, 0, 2, 4), 1);     // inside the tab: its start
  TF_EQ_U(ed_pos_of_col(d, 0, 5, 4), 3);
  TF_EQ_U(ed_pos_of_col(d, 0, 6, 4), 5);
  TF_EQ_U(ed_pos_of_col(d, 0, 99, 4), 6);    // past the end: line end
  TF_EQ_U(ed_pos_of_col(d, 1, 3, 4), 10);
  TF_EQ_U(ed_pos_of_col(d, 9, 3, 4), 10);    // line clamped to the last
  delete d;
  // UTF-16 with BOM: line 0 starts after the BOM
  el_build("ab\ncd", enc_utf16le(1), EOL_CRLF, 2, &b, m);
  d = el_doc(enc_utf16le(1), EOL_CRLF, b);
  TF_EQ_U(ed_pos_of_col(d, 0, 0, 4), 2);
  TF_EQ_U(ed_pos_of_col(d, 0, 1, 4), 4);
  TF_EQ_U(ed_col_of(d, 6, 4), 2);
  TF_EQ_U(ed_pos_of_col(d, 1, 1, 4), 12);
  delete d;
}

static void el_test_range(TF* tf)
{
  EBytes b;
  epos m[EL_NMARK], a, z;
  Document* d;
  Selection s;
  u32 f, l;
  el_build("abc\ndef\nghi", enc_utf16le(1), EOL_CRLF, 2, &b, m);
  d = el_doc(enc_utf16le(1), EOL_CRLF, b);
  s.anchor = s.caret = 2;
  ed_lines_range(d, s, &a, &z);
  TF_EQ_U(a, 2); TF_EQ_U(z, 12);
  s.anchor = 4; s.caret = 12;                // ends at the start of line 1: line 0 only
  ed_lines_touched(d, s, &f, &l);
  TF_EQ_U(f, 0); TF_EQ_U(l, 0);
  s.caret = 14;
  ed_lines_touched(d, s, &f, &l);
  TF_EQ_U(l, 1);
  s.anchor = s.caret = d->length();
  ed_lines_range(d, s, &a, &z);
  TF_EQ_U(a, 22); TF_EQ_U(z, d->length());
  delete d;
}

// Read-only / busy documents refuse every command and change nothing.
static void el_test_refuse(TF* tf, const EdLang* L)
{
  EBytes b;
  epos m[EL_NMARK];
  Document* d;
  EdCtx e;
  el_build("ab|c\ndef", enc_utf8(0), EOL_LF, 0, &b, m);
  d = el_doc(enc_utf8(0), EOL_LF, b);
  memset(&e, 0, sizeof(e));
  e.doc = d; e.sel.anchor = e.sel.caret = 2; e.lang = *L;
  d->set_read_only(true);
  TF_EQ_I(ed_duplicate(&e), E_READONLY);
  TF_EQ_I(ed_delete_lines(&e), E_READONLY);
  TF_EQ_I(ed_move_lines(&e, 1), E_READONLY);
  TF_EQ_I(ed_toggle_comment(&e), E_READONLY);
  TF_EQ_I(ed_change_case(&e, 1), E_READONLY);
  TF_EQ_I(ed_indent(&e, 0), E_READONLY);
  d->set_read_only(false);
  d->set_busy(true);
  TF_EQ_I(ed_duplicate(&e), E_BUSY);
  TF_EQ_I(ed_move_lines(&e, 0), E_BUSY);
  d->set_busy(false);
  TF_CHECK(el_doc_equals(d, b));
  TF_CHECK(!d->can_undo());
  delete d;
}

// Allocation failures at every step: the document is either unchanged or fully changed, never half.
static void el_oom_one(TF* tf, const char* name, const char* before, const char* after, ElOp op, int arg,
                       const EdLang* L)
{
  i32 n;
  int done = 0;
  for (n = 1; n < 200 && !done; n++) {
    EBytes b0, b1;
    epos m0[EL_NMARK], m1[EL_NMARK];
    Document* d;
    EdCtx e;
    int rc;
    el_build(before, enc_utf8(0), EOL_CRLF, 0, &b0, m0);
    el_build(after, enc_utf8(0), EOL_CRLF, 0, &b1, m1);
    d = el_doc(enc_utf8(0), EOL_CRLF, b0);
    memset(&e, 0, sizeof(e));
    e.doc = d; e.sel.caret = m0[0]; e.sel.anchor = m0[1] == EPOS_NONE ? m0[0] : m0[1]; e.lang = *L;
    e_test_fail_after(n);
    rc = op(&e, arg);
    e_test_fail_after(-1);
    if (rc == E_OK) {
      done = 1;
      if (!el_doc_equals(d, b1)) tf_fail(tf, __FILE__, __LINE__, "%s: oom %d: ok but wrong bytes", name, (int)n);
      else tf->checks++;
    } else {
      if (!el_doc_equals(d, b0)) tf_fail(tf, __FILE__, __LINE__, "%s: oom %d: %s left a partial edit", name, (int)n,
                                         e_errname(rc));
      else tf->checks++;
    }
    delete d;
  }
  TF_CHECK(done);
}

static void el_test_oom(TF* tf, const EdLang* L)
{
  el_oom_one(tf, "oom dup", "ab|c\ndef", "ab|c\nabc\ndef", el_op_dup, 0, L);
  el_oom_one(tf, "oom move", "abc\nd|ef\nghi", "abc\nghi\n|def^", el_op_move, 1, L);
  el_oom_one(tf, "oom comment", "^a\n// b\n\n  c|", "// ^a\nb\n\n  // c|", el_op_cmt, 0, L);
  el_oom_one(tf, "oom case", "^h\xC3\xA9llo|", "^H\xC3\x89LLO|", el_op_case, 1, L);
  el_oom_one(tf, "oom indent", "^ab\ncd\nef|", "^\tab\n\tcd\n\tef|", el_op_ind, 0, L);
  el_oom_one(tf, "oom delete", "a^bc\nde|f\nghi", "|ghi", el_op_del, 0, L);
}

// Random line moves against a model (list of lines): moving down then up returns the original text.
static void el_test_random_moves(TF* tf, const EdLang* L)
{
  u32 iter, n_iter = tf->long_run ? 3000 : 400;
  for (iter = 0; iter < n_iter; iter++) {
    EBytes b, orig;
    epos m[EL_NMARK];
    char spec[400];
    u32 nl = 2 + tf_rand_below(tf, 6), i, k = 0, a, z;
    Document* d;
    EdCtx e;
    int rc;
    for (i = 0; i < nl; i++) {
      u32 len = tf_rand_below(tf, 4), j;
      for (j = 0; j < len; j++) spec[k++] = (char)('a' + tf_rand_below(tf, 26));
      if (i + 1 < nl) spec[k++] = '\n';
    }
    spec[k] = 0;
    el_build(spec, enc_utf8(0), (EolStyle)(iter & 1), 0, &b, m);
    d = el_doc(enc_utf8(0), (EolStyle)(iter & 1), b);
    a = tf_rand_below(tf, nl - 1);             // a block of lines that is not at the bottom
    z = a + tf_rand_below(tf, nl - 1 - a);
    memset(&e, 0, sizeof(e));
    e.doc = d; e.lang = *L;
    e.sel.anchor = d->line_start(a);
    e.sel.caret = d->line_end(z);
    orig.resize(d->length());
    d->read(0, orig.data(), orig.size());
    rc = ed_move_lines(&e, 1);
    TF_EQ_I(rc, E_OK);
    TF_EQ_U(d->line_count(), nl);
    rc = ed_move_lines(&e, 0);
    TF_EQ_I(rc, E_OK);
    if (!el_doc_equals(d, orig)) { tf_fail(tf, __FILE__, __LINE__, "random move %u: '%s' %u-%u", iter, spec, a, z); el_dump(tf, d); }
    else tf->checks++;
    delete d;
  }
}

void suite_edlines(TF* tf)
{
  EdLang L;
  codec_init();
  el_lang_c(&L);
  el_test_duplicate(tf, &L);
  el_test_delete(tf, &L);
  el_test_move(tf, &L);
  el_test_comment(tf, &L);
  el_test_case(tf, &L);
  el_test_indent(tf, &L);
  el_test_brace(tf);
  el_test_columns(tf);
  el_test_range(tf);
  el_test_refuse(tf, &L);
  el_test_oom(tf, &L);
  el_test_random_moves(tf, &L);
}
