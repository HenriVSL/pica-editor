// t_lexer.cpp - suite "lexer" (T04): golden styles per language, registry and .lng parser, Highlighter
// convergence under random edits, the "/*" viewport test, LEX_LINE_MAX, malformed files, OOM, throughput.
// Owner: T04. The sample/golden tables were produced by the lexers and reviewed line by line.
#include "tf.h"
#include "lexer.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

// style codes: one char per character
static const char g_codes[] = ".cshn1234polvSktaeHmx+-@=!";

// ---- samples (generated, reviewed) ----
static const char* const smp_cpp[] = {
  "#include <windows.h>",
  "#define MAX(a, b) ((a) > (b) ? \\",
  "    (a) : (b))",
  "/* block comment",
  "   spans lines */ int x = 0x1F;",
  "// line comment \\",
  "continued",
  "static const char* s = \"esc \\\"q\\\" \\",
  "cont\";",
  "char c = '\\'';",
  "unsigned __int64 big = 123i64 + 1.5e-3f + .5;",
  "LPCSTR w = L\"wide\"; HWND hwnd = NULL;",
  "class Foo : public Bar { virtual void f() override; };",
  "for (i = 0; i < n; i++) { if (x) break; } // caf\xC3\xA9",
  "  # pragma once /* c */ \"s\"",
  "int caf\xC3\xA9 = 1;",
  0
};
static const char* const smp_rc[] = {
  "#include \"resource.h\"",
  "IDD_ABOUT DIALOGEX 0, 0, 186, 95",
  "STYLE DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION",
  "CAPTION \"About\"",
  "FONT 8, \"MS Sans Serif\"",
  "BEGIN",
  "    DEFPUSHBUTTON   \"OK\",IDOK,129,7,50,14",
  "    LTEXT           \"Pica 1.0\",IDC_STATIC,7,7,100,8",
  "END",
  "// comment",
  "IDR_MENU MENU discardable",
  "begin",
  "  POPUP \"&File\"",
  "  MENUITEM \"E&xit\", 40001",
  "end",
  0
};
static const char* const smp_make[] = {
  "# NMAKE makefile",
  "CFLAGS = /nologo /W3 /O2",
  "OBJS=obj\\a.obj obj\\b.obj",
  "!IF \"$(DEBUG)\" == \"1\"",
  "CFLAGS = $(CFLAGS) /Zi",
  "!ENDIF",
  "all: pica.exe",
  "pica.exe: $(OBJS)",
  "\tlink /out:$@ $** user32.lib # tail",
  "{src}.cpp{obj}.obj::",
  "\tcl $(CFLAGS) /c $<",
  "c:\\out\\x.obj: x.c",
  "clean:",
  "\tdel obj\\*.obj",
  "ifeq ($(OS),Windows_NT)",
  "X := $(patsubst %.c,%.o,$(SRC))",
  "endif",
  0
};
static const char* const smp_bat[] = {
  "@echo off",
  "rem This is a comment",
  "REM upper comment",
  "  :: double colon comment",
  ":start",
  "set PATH=%PATH%;C:\\TOOLS",
  "if exist %1 goto found",
  "for %%i in (*.txt) do type %%i",
  "echo %~dp0 and 100%% done",
  "call :sub \"arg\"",
  "copy a.txt b.txt > nul",
  "@rem quiet",
  "remark is not a comment",
  "goto :eof",
  0
};
static const char* const smp_ini[] = {
  "; comment",
  "# hash comment",
  "[General]",
  "vid_renderer = qt_software",
  "cpu_speed=233333333",
  "[HKEY_LOCAL_MACHINE\\Software\\Pica]",
  "\"Name\"=\"value\"",
  "@=\"default\"",
  "path=c:\\a;c:\\b",
  "enabled = true",
  "[Strings]",
  "Disk1=\"%DiskName%\"",
  "REGEDIT4",
  "novalue",
  0
};
static const char* const smp_asm[] = {
  "; MASM sample",
  ".386",
  ".model flat, stdcall",
  ".data",
  "msg     db \"Hello\", 0",
  "count   dd 0FFh, 'x'",
  ".code",
  "start:",
  "    mov eax, [ebx+4]     ; load",
  "    add eax, 1010b",
  "    jmp short @@done",
  "@@done: ret",
  "main PROC",
  "%define X 1",
  "MAIN ENDP",
  "end start",
  0
};
static const char* const smp_python[] = {
  "#!/usr/bin/env python",
  "\"\"\"Module doc",
  "spanning lines\"\"\"",
  "import os, sys",
  "@decorator.name(1)",
  "class Foo(object):",
  "    def bar(self, x=0x10):",
  "        s = r'raw\\n' + b\"bytes\" + u'uni'",
  "        t = '''multi",
  "line''' + 'cont\\",
  "inued'",
  "        print \"py2\", len(s), True, None",
  "        return x ** 2.5e3  # comment",
  "    raise ValueError(\"bad \\\"q\\\"\")",
  0
};
static const char* const smp_js[] = {
  "// comment",
  "/* block",
  " * comment */",
  "var re = /ab+c/gi, x = a / b / c;",
  "function f(n) { return n > 0 ? \"yes\" : 'no'; }",
  "let t = `template ${x}",
  "second line`;",
  "if (typeof x === \"undefined\") alert(document.title);",
  "var o = { a: 1, b: [1, 2.5, 0xFF] };",
  "x = (y) / 2; z = re.test(s) && /[/]/.test(s);",
  "class A extends B { constructor() { super(); this.v = null; } }",
  "return /x/;",
  0
};
static const char* const smp_css[] = {
  "/* stylesheet */",
  "@import url(\"base.css\");",
  "body, h1 > p { color: #ff0000; margin: 0 auto; }",
  ".note:hover { background-color: navy !important; }",
  "#main { font-family: \"Courier New\", monospace; width: 50%; }",
  "@media print {",
  "  a { text-decoration: none; }",
  "}",
  "div::before { content: '\\201C'; }",
  "/* multi",
  "   line */ p { line-height: 1.5em; }",
  0
};
static const char* const smp_diff[] = {
  "diff -u a.c b.c",
  "Index: a.c",
  "===================================================================",
  "--- a.c\t2026-01-01",
  "+++ b.c\t2026-01-02",
  "@@ -1,3 +1,3 @@",
  " context line",
  "-old line",
  "+new line",
  "\\ No newline at end of file",
  "Only in b: new.c",
  "Binary files a.bin and b.bin differ",
  "*** a.c",
  "! changed",
  "< normal del",
  "> normal add",
  0
};
static const char* const smp_html[] = {
  "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\">",
  "<html>",
  "<!-- a comment",
  "     spanning -->",
  "<head><title>Pica &amp; friends &#169; &#x263A; & no</title>",
  "<style type=\"text/css\">",
  "body { color: red; } /* css */",
  "</style>",
  "<SCRIPT language=\"JavaScript\">",
  "var x = \"</b>\"; // js",
  "</script>",
  "</head>",
  "<body bgcolor=white onload='init()' foo=\"bar\">",
  "<p class=\"a",
  "b\">Text &nbsp; <br/><blink>old</blink><custom>x</custom></p>",
  "<![CDATA[ raw ]]> a < b",
  "</body></html>",
  0
};
static const char* const smp_xml[] = {
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>",
  "<!DOCTYPE note SYSTEM \"note.dtd\">",
  "<!-- comment -->",
  "<note xmlns:x=\"urn:x\" id='n1'>",
  "  <to>Tove &lt;3</to>",
  "  <x:from attr=\"a &amp; b\"/>",
  "  <body><![CDATA[ <not a tag>",
  "  ]]></body>",
  "  <Script>case</Script>",
  "  <multi",
  "     attr=\"v\"",
  "  />",
  "</note>",
  0
};
static const char* const smp_md[] = {
  "# Heading 1",
  "Some *emphasis* and **strong** and `code` here.",
  "Setext heading",
  "==============",
  "- list item with [link](http://x.y/z)",
  "  continued",
  "1. numbered",
  "> quote with `code`",
  "```c",
  "int x; /* not lexed */",
  "```",
  "---",
  "",
  "    indented code",
  "<!-- comment",
  "over lines -->",
  "A snake_case_word and _emph_ and <b>tag</b> and <http://auto.link>.",
  "[ref]: http://example.com",
  "| a | b |",
  "\\*not emph\\*",
  0
};
static const char* const smp_results[] = {
  "Search \"Blt\" (17 hits in 6 files of 230 searched)",
  "M:\\dev\\runner\\game.cpp (3 hits)",
  "    120: \tif (dd->Blt(&dst, surf, &src, DDBLT_KEYSRC, NULL) != DD_OK)",
  "    7: x",
  0
};

static const char* const gold_cpp[] = {
  "pppppppppppppppppppp",
  "pppppppppppppppppppppppppppppppp",
  "pppppppppppppp",
  "cccccccccccccccc",
  "ccccccccccccccccc.222...o.nnnno",
  "ccccccccccccccccc",
  "ccccccccc",
  "111111.11111.2222o...o.ssssssssssss",
  "ssssso",
  "2222...o.hhhho",
  "22222222.2222222.....o.nnnnnn.o.nnnnnnn.o.nno",
  "333333...o.ssssssso.3333......o.3333o",
  "11111.....o.111111.....o.1111111.2222..oo.11111111o.oo",
  "111.o..o.no...o..o..ooo.o.11.o.o.11111o.o.ccccccc",
  "..ppppppppppppppcccccccpsss",
  "222......o.no",
  0
};
static const char* const gold_rc[] = {
  "pppppppppssssssssssss",
  "..........11111111.no.no.nnno.nn",
  "11111.3333333333.o.3333333333333.o.33333333.o.3333333333",
  "1111111.sssssss",
  "1111.no.sssssssssssssss",
  "11111",
  "....2222222222222...sssso3333onnnononnonn",
  "....22222...........sssssssssso3333333333onononnnon",
  "111",
  "cccccccccc",
  ".........1111.22222222222",
  "11111",
  "..11111.sssssss",
  "..11111111.ssssssso.nnnnn",
  "111",
  0
};
static const char* const gold_make[] = {
  "cccccccccccccccc",
  "vvvvvv.o................",
  "vvvvo...................",
  "ppppppppppppppppppppp",
  "vvvvvv.o.vvvvvvvvv....",
  "pppppp",
  "lllo.........",
  "llllllllo.vvvvvvv",
  ".3333.....ovv.vvv............cccccc",
  "lllllllllllllllllloo",
  ".33.vvvvvvvvv....vv",
  "llllllllllllo....",
  "lllllo",
  ".333..........",
  "1111.ovvvvv...........o",
  "v.oo.vvvvvvvvvvvvvvvvvvvvvvvvvv",
  "11111",
  0
};
static const char* const gold_bat[] = {
  "o1111.111",
  "ccccccccccccccccccccc",
  "ccccccccccccccccc",
  "..ccccccccccccccccccccccc",
  "llllll",
  "111.1111ovvvvvv.........",
  "11.11111.vv.1111......",
  "111.vvv.11.o.....o.11.2222.vvv",
  "1111.vvvvv.....nnn.......",
  "1111...........",
  "2222.............o.111",
  "cccccccccc",
  "..........111..........",
  "1111.....",
  0
};
static const char* const gold_ini[] = {
  "ccccccccc",
  "cccccccccccccc",
  "SSSSSSSSS",
  "kkkkkkkkkkkk.o............",
  "kkkkkkkkkonnnnnnnnn",
  "SSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS",
  "kkkkkkosssssss",
  "kosssssssss",
  "kkkko.o....o..",
  "kkkkkkk.o.1111",
  "SSSSSSSSS",
  "kkkkkossssssssssss",
  "22222222",
  ".......",
  0
};
static const char* const gold_asm[] = {
  "ccccccccccccc",
  "3333",
  "333333.3333o.3333333",
  "33333",
  "lll.....33.ssssssso.n",
  "lllll...33.nnnno.sss",
  "33333",
  "llllll",
  "....111.222o.o222ono.....cccccc",
  "....111.222o.nnnnn",
  "....111.33333.......",
  "lllllll.111",
  "llll.3333",
  "ppppppppppp",
  "llll.3333",
  "333......",
  0
};
static const char* const gold_python[] = {
  "ccccccccccccccccccccc",
  "sssssssssssss",
  "sssssssssssssssss",
  "111111...o....",
  "pppppppppppppppono",
  "11111....o222222oo",
  "....111....o3333o..onnnnoo",
  "..........o.ssssssss.o.ssssssss.o.ssssss",
  "..........o.ssssssss",
  "sssssss.o.ssssss",
  "ssssss",
  "........11111.ssssso.222o.oo.1111o.1111",
  "........111111...oo.nnnnn..ccccccccc",
  "....11111.3333333333ossssssssssso",
  0
};
static const char* const gold_js[] = {
  "cccccccccc",
  "cccccccc",
  "ccccccccccccc",
  "111....o.sssssssso...o...o...o..o",
  "11111111..o.o.o.111111...o.n.o.sssss.o.sssso.o",
  "111...o.ssssssssssssss",
  "sssssssssssso",
  "11.o111111...ooo.ssssssssssso.33333o33333333o.....oo",
  "111...o.o..o.no..o.ono.nnno.nnnno.oo",
  "..o.o.o.o.no...o...o....o.o.oo.ssssso....o.oo",
  "11111...1111111...o.33333333333oo.o.22222ooo.2222o..o.2222o.o.o",
  "111111.ssso",
  0
};
static const char* const gold_css[] = {
  "cccccccccccccccc",
  "3333333.222ossssssssssoo",
  "3333o.33.o.3.o.11111o.o......o.111111o.n.2222o.o",
  "o....o33333.o.1111111111111111o.2222.o222222222o.o",
  "o.....o.11111111111o.ssssssssssssso.222222222o.11111o.nnoo.o",
  "333333.......o",
  "..3.o.111111111111111o.2222o.o",
  "o",
  "333oo333333.o.1111111o.ssssssso.o",
  "cccccccc",
  "cccccccccc.3.o.11111111111o.nnnnno.o",
  0
};
static const char* const gold_diff[] = {
  "===============",
  "==========",
  "===================================================================",
  "==================",
  "==================",
  "@@@@@@@@@@@@@@@",
  ".............",
  "---------",
  "+++++++++",
  "ccccccccccccccccccccccccccc",
  "================",
  "===================================",
  "=======",
  "@@@@@@@@@",
  "------------",
  "++++++++++++",
  0
};
static const char* const gold_html[] = {
  "pppppppppppppppppppppppppppppppppppppppppppppppppp",
  "tttttt",
  "cccccccccccccc",
  "ccccccccccccccccc",
  "ttttttttttttt.....eeeee.........eeeeee.eeeeeeee.....tttttttt",
  "tttttt.aaaaosssssssssst",
  "3333.o.11111o.222o.o.ccccccccc",
  "tttttttt",
  "ttttttt.aaaaaaaaosssssssssssst",
  "111...o.sssssso.ccccc",
  "ttttttttt",
  "ttttttt",
  "ttttt.aaaaaaaosssss.aaaaaaossssssss.444ossssst",
  "tt.aaaaaoss",
  "sst.....eeeeee.tttttttttttt...ttttttttt444444t.tt444444ttttt",
  "xxxxxxxxxxxxxxxxx......",
  "tttttttttttttt",
  0
};
static const char* const gold_xml[] = {
  "pppppppppppppppppppppppppppppppppppppp",
  "ppppppppppppppppppppppppppppppppp",
  "cccccccccccccccc",
  "ttttt.aaaaaaaosssssss.aaosssst",
  "..tttt.....eeee.ttttt",
  "..ttttttt.aaaaossseeeeessstt",
  "..ttttttxxxxxxxxxxxxxxxxxxxxx",
  "xxxxxttttttt",
  "..tttttttt....ttttttttt",
  "..tttttt",
  ".....aaaaosss",
  "..tt",
  "ttttttt",
  0
};
static const char* const gold_md[] = {
  "HHHHHHHHHHH",
  ".....mmmmmmmmmm.....mmmmmmmmmm.....xxxxxx......",
  "..............",
  "HHHHHHHHHHHHHH",
  "o................333333ssssssssssssss",
  "...........",
  "oo.........",
  "o.cccccccccccxxxxxx",
  "xxxx",
  "xxxxxxxxxxxxxxxxxxxxxx",
  "xxx",
  "ooo",
  "",
  "....xxxxxxxxxxxxx",
  "cccccccccccc",
  "cccccccccccccc",
  "......................mmmmmm.....ttt...tttt.....ssssssssssssssssss.",
  "333333sssssssssssssssssss",
  "o...o...o",
  "............",
  0
};
static const char* const gold_results[] = {
  "HHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHH",
  "SSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS",
  "....nnnn.............................................................",
  "....nn..",
  0
};
// ---- end of samples ----

// ---------------------------------------------------------------------------------------------------------------
// helpers

static const char* const g_lng_files[] = {
  "cpp", "rc", "make", "bat", "ini", "asm", "python", "html", "js", "css", "xml", "md", "diff"
};
enum { NLNG = 13 };

class MemLangs : public LangSource {
public:
  MemLangs() : n_(0) {}
  ~MemLangs() { u32 i; for (i = 0; i < n_; i++) e_free(t_[i]); }
  u32 count() { return n_; }
  int get(u32 i, const char** name, const char** text, u32* len)
  {
    if (i >= n_) return E_RANGE;
    *name = nm_[i]; *text = t_[i]; *len = l_[i];
    return E_OK;
  }
  int add(const char* name, const char* text, u32 len)
  {
    char* p;
    if (n_ >= 48) return E_RANGE;
    p = (char*)e_alloc(len + 1);
    if (!p) return E_NOMEM;
    memcpy(p, text, len); p[len] = 0;
    es_copy(nm_[n_], sizeof(nm_[n_]), name);
    t_[n_] = p; l_[n_] = len; n_++;
    return E_OK;
  }
  int add_str(const char* name, const char* text) { return add(name, text, es_len(text)); }
private:
  char* t_[48]; u32 l_[48]; char nm_[48][40]; u32 n_;
};

static int load_builtin(TF* tf, MemLangs* m)
{
  u32 i;
  for (i = 0; i < NLNG; i++) {
    char path[400], name[40];
    FILE* f;
    long n;
    char* buf;
    es_fmt(name, sizeof(name), "%s.lng", g_lng_files[i]);
    es_fmt(path, sizeof(path), "%s/../../src/langs/%s", tf->data_dir, name);
    f = fopen(path, "rb");
    if (!f) { tf_fail(tf, __FILE__, __LINE__, "cannot open %s", path); return 0; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (char*)e_alloc((u32)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); e_free(buf); tf_fail(tf, __FILE__, __LINE__, "read %s", path); return 0; }
    fclose(f);
    m->add(name, buf, (u32)n);
    e_free(buf);
  }
  return 1;
}

static void to_ecp(const char* s, Vec<ecp>* out)
{
  out->clear();
  es_utf8_to_ecp(s, es_len(s), out);
}

static void codes_of(const u8* st, u32 n, char* out)
{
  u32 k;
  for (k = 0; k < n; k++) out[k] = st[k] < ST_COUNT ? g_codes[st[k]] : '?';
  out[n] = 0;
}

static void check_golden(TF* tf, const char* id, const char* const* lines, const char* const* gold)
{
  const LangInfo* li = es_icmp(id, "results") == 0 ? lang_results() : lang_find(id);
  u16 st = 0;
  u32 i, nlines = 0;
  if (!li || !li->lexer) { tf_fail(tf, __FILE__, __LINE__, "language %s missing", id); return; }
  for (i = 0; lines[i]; i++) {
    Vec<ecp> v;
    u8 sty[512];
    char got[513];
    if (!gold[i]) { tf_fail(tf, __FILE__, __LINE__, "%s: golden table shorter than the sample", id); return; }
    to_ecp(lines[i], &v);
    TF_CHECK(v.size() < 512);
    st = li->lexer->lex_line(v.data(), v.size(), st, sty);
    TF_CHECK(st < 0x7FFF);
    codes_of(sty, v.size(), got);
    if (strcmp(got, gold[i]) != 0)
      tf_fail(tf, __FILE__, __LINE__, "%s line %u: %s\n      got  %s\n      want %s", id, i + 1, lines[i], got, gold[i]);
    else tf_check(tf, 1, __FILE__, __LINE__, "golden");
    nlines++;
  }
  TF_CHECK(gold[nlines] == 0);
  TF_CHECK(nlines >= 10 || es_icmp(id, "results") == 0);
}

// ---------------------------------------------------------------------------------------------------------------
// test document: text with '\n' separators

class TDoc : public LineProvider {
public:
  TDoc() : gets(0) {}
  Vec<ecp> t;
  Vec<u32> ls;
  u32 gets;
  void index()
  {
    u32 i;
    ls.clear();
    ls.push(0);
    for (i = 0; i < t.size(); i++) if (t[i] == '\n') ls.push(i + 1);
  }
  u32 lp_count() { return ls.size(); }
  void line(u32 l, const ecp** p, u32* n)
  {
    u32 a = ls[l], b = l + 1 < ls.size() ? ls[l + 1] - 1 : t.size();
    *p = t.data() + a;
    *n = b - a;
  }
  int lp_get(u32 l, ecp* out, u32 cap, u32* n)
  {
    const ecp* p;
    u32 m;
    gets++;
    if (l >= ls.size()) { *n = 0; return E_RANGE; }
    line(l, &p, &m);
    if (m > cap) m = cap;
    memcpy(out, p, m * sizeof(ecp));
    *n = m;
    return E_OK;
  }
  u32 line_of(u32 pos)
  {
    u32 lo = 0, hi = ls.size();
    while (hi - lo > 1) { u32 mid = (lo + hi) / 2; if (ls[mid] <= pos) lo = mid; else hi = mid; }
    return lo;
  }
  void set_lines(const char* const* lines, u32 repeat)
  {
    u32 r, i;
    Vec<ecp> v;
    t.clear();
    for (r = 0; r < repeat; r++) {
      for (i = 0; lines[i]; i++) {
        to_ecp(lines[i], &v);
        t.insert(t.size(), v.data(), v.size());
        t.push('\n');
      }
    }
    if (t.size()) t.pop();
    index();
  }
  // replace [pos, pos+del) by ins; returns the lines_changed arguments
  void edit(u32 pos, u32 del, const ecp* ins, u32 nins, u32* first, u32* removed, u32* added)
  {
    u32 k;
    *first = line_of(pos);
    *removed = 0; *added = 0;
    for (k = pos; k < pos + del; k++) if (t[k] == '\n') (*removed)++;
    for (k = 0; k < nins; k++) if (ins[k] == '\n') (*added)++;
    t.erase(pos, del);
    t.insert(pos, ins, nins);
    index();
  }
};

// reference: states of every line by lexing from the top
static void ref_states(const Lexer* lx, TDoc* d, Vec<u16>* st, u32 upto)   // states of lines [0, upto]
{
  u32 l;
  if (upto >= d->lp_count()) upto = d->lp_count() - 1;
  st->resize(upto + 2);
  (*st)[0] = 0;
  for (l = 0; l <= upto; l++) {
    const ecp* p;
    u32 n;
    d->line(l, &p, &n);
    (*st)[l + 1] = lx->lex_line(p, emin(n, (u32)LEX_LINE_MAX), (*st)[l], 0);
  }
}

// ---------------------------------------------------------------------------------------------------------------

static void t_golden(TF* tf)
{
  check_golden(tf, "cpp", smp_cpp, gold_cpp);
  check_golden(tf, "rc", smp_rc, gold_rc);
  check_golden(tf, "make", smp_make, gold_make);
  check_golden(tf, "bat", smp_bat, gold_bat);
  check_golden(tf, "ini", smp_ini, gold_ini);
  check_golden(tf, "asm", smp_asm, gold_asm);
  check_golden(tf, "python", smp_python, gold_python);
  check_golden(tf, "js", smp_js, gold_js);
  check_golden(tf, "css", smp_css, gold_css);
  check_golden(tf, "diff", smp_diff, gold_diff);
  check_golden(tf, "html", smp_html, gold_html);
  check_golden(tf, "xml", smp_xml, gold_xml);
  check_golden(tf, "md", smp_md, gold_md);
  check_golden(tf, "results", smp_results, gold_results);
}

static void eq_tok(TF* tf, const ecp* t, u32 n, const char* want)
{
  u32 k, wn = es_len(want);
  int ok = n == wn;
  for (k = 0; ok && k < n; k++) if (t[k] != (u8)want[k]) ok = 0;
  if (!ok) tf_fail(tf, __FILE__, __LINE__, "comment token differs from '%s'", want);
  else tf_check(tf, 1, __FILE__, __LINE__, "token");
}

static void t_registry(TF* tf)
{
  static const char* const order[] = { "cpp", "rc", "make", "bat", "ini", "asm", "python", "html", "js", "css", "xml", "md", "diff" };
  static const char* const files[][2] = {
    { "foo.c", "cpp" }, { "X.CPP", "cpp" }, { "a.h", "cpp" }, { "w.HPP", "cpp" }, { "inl.inl", "cpp" },
    { "res.rc", "rc" }, { "res.rc2", "rc" }, { "Makefile", "make" }, { "MAKEFILE", "make" }, { "makefile.vc", "make" },
    { "proj.dsp", "make" }, { "x.mak", "make" }, { "RUN.BAT", "bat" }, { "go.cmd", "bat" }, { "86box.cfg", "ini" },
    { "system.ini", "ini" }, { "a.reg", "ini" }, { "setup.inf", "ini" }, { "ws.dsw", "ini" }, { "CONFIG.SYS", "ini" },
    { "boot.asm", "asm" }, { "x.inc", "asm" }, { "art.py", "python" }, { "SConstruct", "python" },
    { "index.htm", "html" }, { "a.HTML", "html" }, { "a.js", "js" }, { "s.css", "css" }, { "a.xml", "xml" },
    { "app.manifest", "xml" }, { "pic.svg", "xml" }, { "README.md", "md" }, { "fix.patch", "diff" },
    { "a.diff", "diff" }, { "C:\\dev\\editor\\src\\file.cpp", "cpp" }, { "M:/x/y.py", "python" },
    { "notes.txt", 0 }, { "noext", 0 }, { "", 0 }, { "dot.", 0 }, { "a.cpp.bak", 0 }
  };
  MemLangs m;
  EStr err;
  const LangInfo* li;
  u32 i;
  if (!load_builtin(tf, &m)) return;
  TF_EQ_I(lang_init(&m, 0, &err), E_OK);
  TF_EQ_S(err.c_str(), "");
  TF_EQ_U(lang_count(), 13);
  for (i = 0; i < 13; i++) {
    li = lang_at(i);
    TF_CHECK(li != 0);
    if (!li) continue;
    TF_EQ_S(li->id, order[i]);
    TF_CHECK(li->lexer != 0);
    TF_CHECK(li->name[0] != 0);
    TF_CHECK(lang_find(order[i]) == li);
  }
  TF_CHECK(lang_at(13) == 0);
  TF_CHECK(lang_find("CPP") == lang_at(0));
  TF_CHECK(lang_find("nope") == 0);
  TF_CHECK(lang_find(0) == 0);
  for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    li = lang_for_file(files[i][0]);
    if (files[i][1] == 0) {
      if (li) tf_fail(tf, __FILE__, __LINE__, "%s should be plain text, got %s", files[i][0], li->id);
      else tf_check(tf, 1, __FILE__, __LINE__, "plain");
    } else if (!li || es_icmp(li->id, files[i][1]) != 0) {
      tf_fail(tf, __FILE__, __LINE__, "%s: want %s, got %s", files[i][0], files[i][1], li ? li->id : "(plain)");
    } else tf_check(tf, 1, __FILE__, __LINE__, "lang_for_file");
  }
  TF_CHECK(lang_for_file(0) == 0);
  // LangInfo fields
  li = lang_find("cpp");
  if (li) {
    eq_tok(tf, li->line_comment, li->line_comment_len, "//");
    eq_tok(tf, li->block_open, li->block_open_len, "/*");
    eq_tok(tf, li->block_close, li->block_close_len, "*/");
    TF_EQ_S(li->braces, "()[]{}");
    TF_EQ_U(li->case_sensitive, 1);
    TF_EQ_U(li->use_tabs, 2);
    TF_EQ_S(li->name, "C/C++");
  }
  li = lang_find("bat");
  if (li) { eq_tok(tf, li->line_comment, li->line_comment_len, "REM "); TF_EQ_U(li->case_sensitive, 0); }
  li = lang_find("make");
  if (li) { TF_EQ_U(li->use_tabs, 1); eq_tok(tf, li->line_comment, li->line_comment_len, "#"); }
  li = lang_find("python");
  if (li) { TF_EQ_U(li->use_tabs, 0); TF_EQ_U(li->tab_width, 4); eq_tok(tf, li->line_comment, li->line_comment_len, "#"); }
  li = lang_find("css");
  if (li) { TF_EQ_U(li->line_comment_len, 0); eq_tok(tf, li->block_open, li->block_open_len, "/*"); }
  li = lang_find("html");
  if (li) {
    TF_EQ_U(li->line_comment_len, 0);
    eq_tok(tf, li->block_open, li->block_open_len, "<!--");
    eq_tok(tf, li->block_close, li->block_close_len, "-->");
  }
  li = lang_find("ini");
  if (li) eq_tok(tf, li->line_comment, li->line_comment_len, ";");
  li = lang_find("asm");
  if (li) eq_tok(tf, li->line_comment, li->line_comment_len, ";");
  li = lang_find("diff");
  if (li) { TF_EQ_S(li->braces, ""); TF_EQ_U(li->line_comment_len, 0); }
  li = lang_results();
  TF_CHECK(li && li->lexer);
  if (li) TF_EQ_S(li->id, "results");
  for (i = 0; i < lang_count(); i++) TF_CHECK(lang_at(i) != lang_results());
  lang_shutdown();
  TF_EQ_U(lang_count(), 0);
  TF_CHECK(lang_find("cpp") == 0);
}

static void t_overrides(TF* tf)
{
  MemLangs b, o;
  EStr err;
  const LangInfo* li;
  if (!load_builtin(tf, &b)) return;
  o.add_str("CPP.LNG", "name=My C\r\nextensions=c;cxx\r\nkeywords1=foo\r\n");
  o.add_str("rc.lng", "name=Broken\r\nbogus=1\r\n");
  o.add_str("lua.lng", "[language]\r\nname=Lua\r\nextensions=lua\r\nline_comment=--\r\nblock_comment=--[[ ]]\r\n"
                       "string=\" \" \\\r\nkeywords1=local function end if then else\r\n");
  o.add_str("bad2.lng", "extensions=zzz\r\n");
  o.add_str("dup.lng", "id=cpp\r\nname=Dup\r\nextensions=dup\r\n");
  o.add("bin.lng", "\x01\x02\xFF\xFEname=x\n", 12);
  o.add_str("myc.lng", "name=Claims C\r\nextensions=h\r\n");
  TF_EQ_I(lang_init(&b, &o, &err), E_OK);
  TF_CHECK(strstr(err.c_str(), "rc.lng") != 0);
  TF_CHECK(strstr(err.c_str(), "bogus") != 0);
  TF_CHECK(strstr(err.c_str(), "bad2.lng") != 0);
  TF_CHECK(strstr(err.c_str(), "dup.lng") != 0);
  TF_CHECK(strstr(err.c_str(), "bin.lng") != 0);
  TF_CHECK(strstr(err.c_str(), "lua.lng") == 0);
  TF_EQ_U(lang_count(), 15);
  li = lang_at(0);
  if (li) { TF_EQ_S(li->name, "My C"); TF_EQ_S(li->id, "cpp"); }
  li = lang_find("rc");
  if (li) TF_EQ_S(li->name, "Resource Script");
  li = lang_for_file("x.lua");
  TF_CHECK(li != 0 && es_icmp(li->id, "lua") == 0);
  if (li) {
    Vec<ecp> v;
    u8 st[64];
    char got[65];
    u16 s;
    to_ecp("local x = \"a\" --[[ c", &v);
    s = li->lexer->lex_line(v.data(), v.size(), 0, st);
    codes_of(st, v.size(), got);
    TF_EQ_S(got, "11111.....sss.cccccc");
    TF_CHECK(s != 0);
    eq_tok(tf, li->line_comment, li->line_comment_len, "--");
  }
  li = lang_for_file("a.h");                // later (user) languages win extension ties
  TF_CHECK(li != 0 && es_icmp(li->id, "myc") == 0);
  li = lang_for_file("a.cpp");              // the override replaced the extension list
  TF_CHECK(li == 0);
  li = lang_for_file("a.cxx");
  TF_CHECK(li != 0 && es_icmp(li->id, "cpp") == 0);
  // html still finds its js/css sub-languages
  li = lang_find("html");
  if (li) {
    Vec<ecp> v;
    u8 st[64];
    char got[65];
    u16 s;
    to_ecp("<script>var x;", &v);
    s = li->lexer->lex_line(v.data(), v.size(), 0, st);
    codes_of(st, v.size(), got);
    TF_EQ_S(got, "tttttttt111..o");
    TF_CHECK(s != 0);
  }
  lang_shutdown();
  // a broken built-in is reported and skipped; no built-ins at all is fine
  {
    MemLangs bb;
    bb.add_str("cpp.lng", "name=C\r\n");
    bb.add_str("broken.lng", "nothing here\r\n");
    err.clear();
    TF_EQ_I(lang_init(&bb, 0, &err), E_FORMAT);
    TF_EQ_U(lang_count(), 1);
    TF_CHECK(strstr(err.c_str(), "broken.lng") != 0);
    lang_shutdown();
    TF_EQ_I(lang_init(0, 0, 0), E_OK);
    TF_EQ_U(lang_count(), 0);
    lang_shutdown();
  }
}

static void t_parser(TF* tf)
{
  static const char* const bad[] = {
    "extensions=c\n",                                     // missing name
    "name=X\nfoo=1\n",                                    // unknown key
    "name=X\nlexer=pascal\n",
    "name=X\nstring=\" \"\nstring=' '\nstring=a a\nstring=b b\nstring=c c\n",
    "name=X\ncase=maybe\n",
    "name=0123456789012345678901234567890123\n",
    "id=bad id\nname=X\n",
    "id=\nname=X\n",
    "name=X\nbraces=()[\n",
    "name=X\nbraces=()[]{}<>\n",
    "name=X\ntab_width=99\n",
    "name=X\nuse_tabs=3\n",
    "name=X\nline=+ nostyle\n",
    "name=X\nline=123456789 comment\n",
    "name=X\nline=+\n",
    "name=X\xE4\n",
    "name=X\x01\n",
    "[other]\nname=X\n",
    "name=X\nkeywords1=abcdefghijabcdefghijabcdefghijabcdefghijabcdefghij\n",
    "name=X\nkeywords5=a\n",
    "name=X\njust a line\n",
    "name=X\nline_comment=\n",
    "name=X\nline_comment=// sometimes\n",
    "name=X\nblock_comment=/*\n",
    "name=X\nstring=\" \" weird\n",
    "name=X\npreprocessor=##\n",
    "name=X\nnumber=hex\n",
    "name=X\nvariables=perl\n",
    "name=X\nlabel=yes\n",
    "name=X\ncomment=123456789\n",
    "name=X\ncomment_block=<!--- -->\n",
    "name=X\ncomment_block=/*\n",
    "name=X\nident_start=a-z\x7F\n",
    "name=X\ncomment_word=a b c d e\n",
    "name=X\nnested=2\n",
    "",
    0
  };
  LangInfo li;
  EStr err;
  u32 i;
  char longline[1500];
  for (i = 0; bad[i]; i++) {
    int r;
    err.clear();
    r = lang_parse(bad[i], es_len(bad[i]), &li, &err);
    if (r == E_OK) tf_fail(tf, __FILE__, __LINE__, "bad[%u] parsed: %s", i, bad[i]);
    else { TF_EQ_I(r, E_SYNTAX); TF_CHECK(err.len() > 0); TF_CHECK(li.lexer == 0); }
  }
  memset(longline, 'a', sizeof(longline));
  memcpy(longline, "name=", 5);
  err.clear();
  TF_EQ_I(lang_parse(longline, sizeof(longline), &li, &err), E_SYNTAX);
  TF_CHECK(strstr(err.c_str(), "line 1") != 0);
  err.clear();
  TF_EQ_I(lang_parse("name=X\n\nfoo=1\n", 14, &li, &err), E_SYNTAX);
  TF_CHECK(strstr(err.c_str(), "line 3") != 0);
  // valid edge cases: BOM, NUL padding, CRLF, comments, tabs, escapes, case after keywords, default id
  {
    static const char ok[] = "\xEF\xBB\xBF; comment\r\n# also\r\n[Language]\r\nkeywords1=Select\tFrom\r\n"
                             "name=SQL Thing\r\ncase=0\r\nextensions=*.SQL .ddl;x\r\ncomment=--\\s\r\n"
                             "line_comment=--\r\nline=\\\\ comment\r\n\0\0\0garbage";
    Vec<ecp> v;
    u8 st[32];
    char got[33];
    TF_EQ_I(lang_parse(ok, sizeof(ok) - 1, &li, &err), E_OK);
    TF_EQ_S(li.name, "SQL Thing");
    TF_EQ_S(li.id, "sqlthing");
    TF_EQ_S(li.exts, "sql;ddl;x");
    TF_EQ_U(li.case_sensitive, 0);
    eq_tok(tf, li.line_comment, li.line_comment_len, "-- ");
    TF_EQ_S(li.braces, "()[]{}");
    TF_CHECK(li.lexer != 0);
    if (li.lexer) {
      to_ecp("SELECT a from b -- x", &v);
      li.lexer->lex_line(v.data(), v.size(), 0, st);
      codes_of(st, v.size(), got);
      TF_EQ_S(got, "111111...1111...cccc");
      to_ecp("\\ rule", &v);
      li.lexer->lex_line(v.data(), v.size(), 0, st);
      codes_of(st, v.size(), got);
      TF_EQ_S(got, "cccccc");
    }
  }
  lang_shutdown();
}

// random mutations of real language files: never crash, results always usable
static void t_fuzz_parse(TF* tf)
{
  MemLangs m;
  u32 it, i;
  if (!load_builtin(tf, &m)) return;
  for (it = 0; it < 1500; it++) {
    const char* name;
    const char* text;
    u32 len, k, nm;
    char* buf;
    LangInfo li;
    EStr err;
    int r;
    m.get(it % NLNG, &name, &text, &len);
    buf = (char*)e_alloc(len + 1);
    if (!buf) continue;
    memcpy(buf, text, len);
    nm = 1 + tf_rand_below(tf, 8);
    for (k = 0; k < nm; k++) {
      u32 p = tf_rand_below(tf, len);
      switch (tf_rand_below(tf, 4)) {
      case 0: buf[p] = (char)tf_rand_below(tf, 256); break;
      case 1: buf[p] = "=;# \\\n[]\"'"[tf_rand_below(tf, 11)]; break;
      case 2: len = p; break;
      default: buf[p] = '\n'; break;
      }
    }
    r = lang_parse(buf, len, &li, &err);
    TF_CHECK(r == E_OK || r == E_SYNTAX);
    if (r == E_OK) {
      Vec<ecp> v;
      u8 st[128];
      u16 s = 0;
      TF_CHECK(li.lexer != 0 && li.name[0] != 0);
      for (i = 0; i < 20 && li.lexer; i++) {
        u32 n = tf_rand_below(tf, 100), j;
        v.clear();
        static const char fz[] = "ab /*\"'#;<>!-[]{}()$%@\\\t`*x01";
        for (j = 0; j < n; j++) v.push((ecp)(u8)fz[tf_rand_below(tf, sizeof(fz) - 1)]);
        s = li.lexer->lex_line(v.data(), v.size(), s, st);
        if (s >= 0x7FFF) { tf_fail(tf, __FILE__, __LINE__, "state out of range"); break; }
      }
    } else {
      TF_CHECK(err.len() > 0);
    }
    e_free(buf);
    if (it % 50 == 49) lang_shutdown();
  }
  lang_shutdown();
}

// every lexer: random text, random (also garbage) start states, LEX_LINE_MAX
static void t_robust(TF* tf)
{
  MemLangs m;
  EStr err;
  u32 li_i, it, k;
  Vec<ecp> v;
  Vec<u8> st, st2;
  const char* alpha = "ab Z09_.\t\"'`/*#;:=%$()<>{}[]\\@!-+&?|~^,x\xE4";
  u32 alen = es_len(alpha);
  if (!load_builtin(tf, &m)) return;
  TF_EQ_I(lang_init(&m, 0, &err), E_OK);
  for (li_i = 0; li_i <= lang_count(); li_i++) {
    const LangInfo* li = li_i < lang_count() ? lang_at(li_i) : lang_results();
    for (it = 0; it < 300; it++) {
      u32 n = tf_rand_below(tf, it < 290 ? 200 : 40000), bad = 0;
      u16 s0 = (u16)tf_rand_below(tf, 0x7FFF), s1, s2;
      v.resize(n); st.resize(n + 1); st2.resize(n + 1);
      for (k = 0; k < n; k++) {
        u32 r = tf_rand_below(tf, alen + 6);
        v[k] = r < alen ? (ecp)(u8)alpha[r] : r < alen + 2 ? ECP_BADBYTE(0x80 + r - alen) : r < alen + 4 ? (ecp)0x4E2D : (ecp)0x1F600;
      }
      st[n] = 0xAA;
      s1 = li->lexer->lex_line(v.data(), n, s0, st.data());
      s2 = li->lexer->lex_line(v.data(), n, s0, 0);
      TF_CHECK(s1 == s2);                     // styles == NULL gives the same state
      TF_CHECK(s1 < 0x7FFF);
      TF_EQ_U(st[n], 0xAA);
      for (k = 0; k < n; k++) if (st[k] >= ST_COUNT) bad++;
      for (k = LEX_LINE_MAX; k < n; k++) if (st[k] != ST_DEFAULT) bad++;
      TF_EQ_U(bad, 0);
      if (n > LEX_LINE_MAX) {                 // the state only depends on the first LEX_LINE_MAX chars
        u16 s3 = li->lexer->lex_line(v.data(), LEX_LINE_MAX, s0, st2.data());
        TF_CHECK(s3 == s1);
        TF_CHECK(memcmp(st.data(), st2.data(), LEX_LINE_MAX) == 0);
      }
    }
  }
  lang_shutdown();
}

// ---------------------------------------------------------------------------------------------------------------
// Highlighter

struct View { u32 top; u32 rows; Vec<u8> painted[30]; u32 pline[30]; };

static void paint_row(Highlighter* h, TDoc* d, View* vw, u32 r)
{
  u32 l = vw->top + r, n;
  const ecp* p;
  vw->pline[r] = l;
  if (l >= d->lp_count()) { vw->painted[r].clear(); return; }
  d->line(l, &p, &n);
  vw->painted[r].resize(n);
  h->style_line(l, p, n, vw->painted[r].data());
}

static void paint_lines(Highlighter* h, TDoc* d, View* vw, u32 a, u32 b)   // inclusive line range
{
  u32 r;
  for (r = 0; r < vw->rows; r++) if (vw->top + r >= a && vw->top + r <= b) paint_row(h, d, vw, r);
}

static int verify_view(TF* tf, const LangInfo* li, Highlighter* h, TDoc* d, View* vw, const char* tag, u32 edit)
{
  Vec<u16> ref;
  Vec<u8> st;
  u32 r, l, bad = 0;
  ref_states(li->lexer, d, &ref, vw->top + vw->rows);
  for (l = 0; l < h->valid_upto() && l < d->lp_count() && l <= vw->top + vw->rows; l++) {
    u16 s;
    if (!h->debug_state(l, &s) || s != ref[l]) {
      tf_fail(tf, __FILE__, __LINE__, "%s edit %u: state of line %u is %04X, want %04X", tag, edit, l, (u32)s, (u32)ref[l]);
      return 0;
    }
  }
  for (r = 0; r < vw->rows; r++) {
    u32 n;
    const ecp* p;
    l = vw->top + r;
    if (l >= d->lp_count()) break;
    if (!h->state_known(l)) { tf_fail(tf, __FILE__, __LINE__, "%s edit %u: line %u unknown after work", tag, edit, l); return 0; }
    d->line(l, &p, &n);
    st.resize(n);
    li->lexer->lex_line(p, n, ref[l], st.data());
    if (vw->pline[r] != l || vw->painted[r].size() != n || (n && memcmp(st.data(), vw->painted[r].data(), n) != 0)) bad++;
  }
  if (bad) { tf_fail(tf, __FILE__, __LINE__, "%s edit %u: %u stale rows on screen", tag, edit, bad); return 0; }
  tf_check(tf, 1, __FILE__, __LINE__, "view");
  return 1;
}

static const char* const g_tokens[] = {
  "/*", "*/", "\"", "'", "`", "\"\"\"", "'''", "<!--", "-->", "<script>", "</script>", "<style>", "</style>",
  "```", "<![CDATA[", "]]>", "\\", "\n", "\n\n", "#", "//", "rem ", ":", "[", "]", "<", ">", "=", "%", "$(", ")",
  "!IF ", "    ", "\t", "- ", "> ", "@", "x", "if ", "abc", "  ", "&amp;", "*"
};

static void t_converge_lang(TF* tf, const LangInfo* li, const char* const* sample, u32 nedits)
{
  TDoc d;
  View vw;
  Highlighter* h = new Highlighter;
  u32 e, k, f, rm, ad, first, last;
  Vec<ecp> ins;
  if (!h) return;
  d.set_lines(sample, 20);
  vw.top = 0; vw.rows = 30;
  TF_EQ_I(h->attach(&d, li), E_OK);
  h->set_target(vw.top + vw.rows - 1);
  for (k = 0; k < vw.rows; k++) paint_row(h, &d, &vw, k);
  for (e = 0; e < nedits; e++) {
    u32 size = d.t.size(), pos, del, act;
    // random edit, biased to the viewport so the screen sees most changes
    if (tf_rand_below(tf, 3) && vw.top < d.lp_count()) {
      u32 l = vw.top + tf_rand_below(tf, vw.rows);
      if (l >= d.lp_count()) l = d.lp_count() - 1;
      pos = d.ls[l] + tf_rand_below(tf, 30);
    } else pos = tf_rand_below(tf, size + 1);
    if (pos > size) pos = size;
    del = tf_rand_below(tf, 4) == 0 ? tf_rand_below(tf, 60) : tf_rand_below(tf, 3);
    if (del > size - pos) del = size - pos;
    ins.clear();
    if (tf_rand_below(tf, 4)) {
      const char* t = g_tokens[tf_rand_below(tf, sizeof(g_tokens) / sizeof(g_tokens[0]))];
      for (k = 0; t[k]; k++) ins.push((u8)t[k]);
    }
    if (size - del + ins.size() > 30000) ins.clear();   // keep the document small
    d.edit(pos, del, ins.data(), ins.size(), &f, &rm, &ad);
    h->lines_changed(f, rm, ad);
    // the view repaints the edited row, or everything below it when lines moved
    if (rm == 0 && ad == 0) paint_lines(h, &d, &vw, f, f);
    else paint_lines(h, &d, &vw, f, 0xFFFFFFFFu);
    act = tf_rand_below(tf, 10);
    if (act == 0) {                                     // scroll somewhere (maybe far: unknown states painted)
      vw.top = tf_rand_below(tf, d.lp_count());
      for (k = 0; k < vw.rows; k++) paint_row(h, &d, &vw, k);
    }
    h->set_target(vw.top + vw.rows - 1);
    if (act < 6) {                                      // idle work to the target, then repaint dirty rows
      u32 guard = 0;
      while (h->work(1 + tf_rand_below(tf, 40)) && guard < 100000) guard++;
      TF_CHECK(guard < 100000);
      while (h->take_dirty(&first, &last)) paint_lines(h, &d, &vw, first, last);
      if (act < 3 && !verify_view(tf, li, h, &d, &vw, li->id, e)) break;
    } else if (act < 8) {                               // a little work only
      h->work(tf_rand_below(tf, 5));
      if (h->take_dirty(&first, &last)) paint_lines(h, &d, &vw, first, last);
    }
    if (e % 97 == 96) {                                 // whole-document check
      Vec<u16> ref;
      u32 l, bad = 0;
      h->set_target(0xFFFFFFFFu);
      while (h->work(500)) {}
      TF_EQ_U(h->valid_upto(), d.lp_count());
      ref_states(li->lexer, &d, &ref, 0xFFFFFFFFu);
      for (l = 0; l < d.lp_count(); l++) { u16 s; if (!h->debug_state(l, &s) || s != ref[l]) bad++; }
      if (bad) { tf_fail(tf, __FILE__, __LINE__, "%s edit %u: %u wrong states in the document", li->id, e, bad); break; }
      while (h->take_dirty(&first, &last)) paint_lines(h, &d, &vw, first, last);
      h->set_target(vw.top + vw.rows - 1);
    }
  }
  h->detach();
  delete h;
}

static void t_converge(TF* tf)
{
  MemLangs m;
  EStr err;
  u32 n = tf->long_run ? 10000 : 2000;
  if (!load_builtin(tf, &m)) return;
  TF_EQ_I(lang_init(&m, 0, &err), E_OK);
  t_converge_lang(tf, lang_find("cpp"), smp_cpp, n);
  t_converge_lang(tf, lang_find("rc"), smp_rc, n);
  t_converge_lang(tf, lang_find("make"), smp_make, n);
  t_converge_lang(tf, lang_find("bat"), smp_bat, n);
  t_converge_lang(tf, lang_find("ini"), smp_ini, n);
  t_converge_lang(tf, lang_find("asm"), smp_asm, n);
  t_converge_lang(tf, lang_find("python"), smp_python, n);
  t_converge_lang(tf, lang_find("js"), smp_js, n);
  t_converge_lang(tf, lang_find("css"), smp_css, n);
  t_converge_lang(tf, lang_find("diff"), smp_diff, n);
  t_converge_lang(tf, lang_find("html"), smp_html, n);
  t_converge_lang(tf, lang_find("xml"), smp_xml, n);
  t_converge_lang(tf, lang_find("md"), smp_md, n);
  t_converge_lang(tf, lang_results(), smp_results, n);
  lang_shutdown();
}

// typing "/*" at the top of a 10,000-line C file re-lexes only to the viewport; closing it restores the states
static void t_viewport(TF* tf)
{
  static const char* const lines[] = { "int x = 1; // c", "    y++;", "#define A \"s\"", "", 0 };
  MemLangs m;
  EStr err;
  TDoc d;
  Highlighter* h;
  const LangInfo* li;
  u32 f, rm, ad, slices = 0, before, l, bad = 0;
  ecp open[2] = { '/', '*' };
  Vec<u16> ref;
  u16 s;
  if (!load_builtin(tf, &m)) return;
  TF_EQ_I(lang_init(&m, 0, &err), E_OK);
  li = lang_find("cpp");
  h = new Highlighter;
  if (!h || !li) { delete h; lang_shutdown(); return; }
  d.set_lines(lines, 2500);
  TF_EQ_U(d.lp_count(), 10000);
  TF_EQ_I(h->attach(&d, li), E_OK);
  h->set_target(39);
  while (h->work(200)) {}
  TF_CHECK(h->valid_upto() >= 40 && h->valid_upto() < 300);   // idle work stops at the target
  h->set_target(0xFFFFFFFFu);
  while (h->work(1000)) {}
  TF_EQ_U(h->valid_upto(), 10000);
  // type "/*" at the start of line 0
  d.edit(0, 0, open, 2, &f, &rm, &ad);
  h->lines_changed(f, rm, ad);
  TF_EQ_U(h->valid_upto(), 1);
  before = h->debug_lexed();
  h->set_target(39);
  while (h->work(10)) slices++;
  TF_CHECK(slices >= 3);                                    // done in budgeted slices
  TF_CHECK(h->valid_upto() >= 40 && h->valid_upto() <= 60);
  TF_CHECK(h->debug_lexed() - before <= 60);
  TF_CHECK(h->debug_state(39, &s) == 1 && (s & 7) == 1);    // line 39 starts inside the comment
  TF_CHECK(h->debug_state(5000, &s) == 0);                  // far lines are not re-lexed
  // close it again: the states converge right after the viewport
  d.edit(0, 2, 0, 0, &f, &rm, &ad);
  h->lines_changed(f, rm, ad);
  before = h->debug_lexed();
  while (h->work(10)) {}
  TF_EQ_U(h->valid_upto(), 10000);
  TF_CHECK(h->debug_lexed() - before <= 60);
  ref_states(li->lexer, &d, &ref, 0xFFFFFFFFu);
  for (l = 0; l < 10000; l++) if (!h->debug_state(l, &s) || s != ref[l]) bad++;
  TF_EQ_U(bad, 0);
  // an edit that changes no state: a far row painted while unknown is repainted once the states converge
  {
    ecp xch = 'q';
    const ecp* p;
    u32 n;
    u8 st[64];
    while (h->take_dirty(&f, &l)) {}
    d.edit(4, 0, &xch, 1, &f, &rm, &ad);
    h->lines_changed(f, rm, ad);
    d.line(9000, &p, &n);
    TF_EQ_I(h->style_line(9000, p, n, st), 0);
    before = h->debug_lexed();
    h->set_target(9000);
    while (h->work(10)) {}
    TF_EQ_U(h->valid_upto(), 10000);
    TF_CHECK(h->debug_lexed() - before <= 2);
    TF_CHECK(h->take_dirty(&f, &l) == 1 && f <= 9000 && l >= 9000);
    d.edit(4, 1, 0, 0, &f, &rm, &ad);
    h->lines_changed(f, rm, ad);
    while (h->work(10)) {}
    while (h->take_dirty(&f, &l)) {}
  }
  // style_line catches up synchronously for nearby lines, not for far ones
  d.edit(0, 0, open, 2, &f, &rm, &ad);
  h->lines_changed(f, rm, ad);
  {
    const ecp* p;
    u32 n;
    u8 st[64];
    d.line(10, &p, &n);
    TF_EQ_I(h->style_line(10, p, n, st), 1);
    TF_EQ_U(st[0], ST_COMMENT);
    d.line(9000, &p, &n);
    TF_EQ_I(h->style_line(9000, p, n, st), 0);
    TF_EQ_U(st[0], ST_DEFAULT);
    h->set_target(9000);
    while (h->work(5000)) {}
    TF_CHECK(h->take_dirty(&f, &l) == 1 && f <= 9000 && l >= 9000);   // the row painted unknown is repainted
    TF_EQ_I(h->style_line(9000, p, n, st), 1);
    TF_EQ_U(st[0], ST_COMMENT);
  }
  // disabled / plain text
  h->set_enabled(false);
  TF_CHECK(!h->enabled());
  TF_CHECK(h->state_known(5));
  TF_EQ_I(h->work(10), 0);
  TF_EQ_U(h->valid_upto(), 10000);
  h->set_enabled(true);
  TF_EQ_U(h->valid_upto(), 1);
  TF_EQ_I(h->attach(&d, 0), E_OK);
  {
    const ecp* p;
    u32 n;
    u8 st[64];
    d.line(1, &p, &n);
    TF_EQ_I(h->style_line(1, p, n, st), 1);
    TF_EQ_U(st[0], ST_DEFAULT);
    TF_EQ_I(h->work(10), 0);
  }
  // a provider whose line count changed behind our back resets instead of misbehaving
  TF_EQ_I(h->attach(&d, li), E_OK);
  h->set_target(100);
  d.t.push('\n'); d.index();
  TF_EQ_I(h->work(5), 1);
  TF_EQ_U(h->valid_upto(), 6);
  h->lines_changed(5, 0, 1);                                // inconsistent with the provider: reset, no crash
  h->lines_changed(999999, 3, 0);
  TF_EQ_U(h->valid_upto(), 1);
  delete h;
  lang_shutdown();
}

// lines longer than LEX_LINE_MAX: the Highlighter lexes the capped line, styles beyond are default
static void t_longline(TF* tf)
{
  MemLangs m;
  EStr err;
  TDoc d;
  Highlighter* h;
  u32 i, n = 100000, bad = 0;
  Vec<u8> st;
  if (!load_builtin(tf, &m)) return;
  TF_EQ_I(lang_init(&m, 0, &err), E_OK);
  d.t.push('"');
  for (i = 1; i < n; i++) d.t.push(i % 50 == 0 ? '\\' : 'a');
  d.t.push('\n');
  d.t.push('x');
  d.index();
  for (i = 0; i < lang_count(); i++) {
    const LangInfo* li = lang_at(i);
    u32 k;
    h = new Highlighter;
    if (!h) break;
    TF_EQ_I(h->attach(&d, li), E_OK);
    h->set_target(1);
    while (h->work(10)) {}
    TF_CHECK(h->state_known(1));
    st.resize(n + 1);
    st[n] = 0x5A;
    TF_EQ_I(h->style_line(0, d.t.data(), n, st.data()), 1);
    TF_EQ_U(st[n], 0x5A);
    for (k = LEX_LINE_MAX; k < n; k++) if (st[k] != ST_DEFAULT) bad++;
    delete h;
  }
  TF_EQ_U(bad, 0);
  lang_shutdown();
}

static void t_oom(TF* tf)
{
  MemLangs m;
  i32 k;
  int done = 0;
  if (!load_builtin(tf, &m)) return;
  for (k = 1; k < 20000 && !done; k += (k < 300 ? 1 : 37)) {
    u32 before = e_alloc_count();
    {
      EStr err;
      int r;
      e_test_fail_after(k);
      r = lang_init(&m, 0, &err);
      e_test_fail_after(-1);
      TF_CHECK(r == E_OK || r == E_NOMEM);
      if (r == E_OK) { TF_EQ_U(lang_count(), 13); done = 1; }
      lang_shutdown();
    }
    if (e_alloc_count() != before) { tf_fail(tf, __FILE__, __LINE__, "leak after OOM at %d", (int)k); break; }
  }
  TF_CHECK(done);
  // Highlighter: failing allocations leave it inert, never crashing
  {
    EStr err;
    TDoc d;
    Highlighter* h = new Highlighter;
    const LangInfo* li;
    u32 f, rm, ad;
    ecp nl[3] = { 'a', '\n', 'b' };
    TF_EQ_I(lang_init(&m, 0, &err), E_OK);
    li = lang_find("cpp");
    d.set_lines(smp_cpp, 3);
    for (k = 1; k <= 3 && h && li; k++) {
      u8 st[64];
      const ecp* p;
      u32 n;
      e_test_fail_after(k);
      h->attach(&d, li);
      e_test_fail_after(-1);
      d.line(0, &p, &n);
      h->style_line(0, p, n, st);
      h->set_target(100);
      while (h->work(50)) {}
      d.edit(5, 0, nl, 3, &f, &rm, &ad);
      e_test_fail_after(1);
      h->lines_changed(f, rm, ad);
      e_test_fail_after(-1);
      h->work(10);
    }
    if (h) { h->attach(&d, li); TF_CHECK(h->work(10000) == 0 || h->valid_upto() > 0); }
    delete h;
    lang_shutdown();
  }
}

static void t_speed(TF* tf)
{
  static const char* const ids[] = { "cpp", "html", "python", "md", "asm", "js" };
  static const char* const* smp[] = { smp_cpp, smp_html, smp_python, smp_md, smp_asm, smp_js };
  MemLangs m;
  EStr err;
  u32 i;
  if (!load_builtin(tf, &m)) return;
  TF_EQ_I(lang_init(&m, 0, &err), E_OK);
  for (i = 0; i < 6; i++) {
    const LangInfo* li = lang_find(ids[i]);
    TDoc d;
    Highlighter* h;
    clock_t t0, t1;
    u32 reps = 0, lines, chars;
    double ms;
    if (!li) continue;
    d.set_lines(smp[i], 1000);
    lines = d.lp_count();
    chars = d.t.size();
    h = new Highlighter;
    if (!h) break;
    t0 = clock();
    do {
      h->attach(&d, li);
      h->set_target(0xFFFFFFFFu);
      while (h->work(100000)) {}
      reps++;
      t1 = clock();
    } while (t1 - t0 < CLOCKS_PER_SEC / 4 && reps < 50);
    TF_EQ_U(h->valid_upto(), lines);
    TF_EQ_U(h->debug_lexed(), (lines - 1) * reps);      // every line lexed each time (the last needs no lexing)
    ms = (double)(t1 - t0) * 1000.0 / (double)CLOCKS_PER_SEC / (double)reps;
    if (ms < 0.001) ms = 0.001;
    tf_log(tf, "lexer speed %-6s %6u lines %8u chars: %6u.%u ms = %8u lines/s, %5u KB/s", ids[i], lines, chars,
           (u32)ms, (u32)(ms * 10.0) % 10, (u32)((double)lines * 1000.0 / ms), (u32)((double)chars / 1024.0 * 1000.0 / ms));
    delete h;
  }
  lang_shutdown();
}

static void t_golden_all(TF* tf)
{
  MemLangs m;
  EStr err;
  if (!load_builtin(tf, &m)) return;
  TF_EQ_I(lang_init(&m, 0, &err), E_OK);
  TF_EQ_S(err.c_str(), "");
  t_golden(tf);
  lang_shutdown();
}

void suite_lexer(TF* tf)
{
  t_golden_all(tf);
  t_registry(tf);
  t_overrides(tf);
  t_parser(tf);
  t_fuzz_parse(tf);
  t_robust(tf);
  t_viewport(tf);
  t_longline(tf);
  t_converge(tf);
  t_oom(tf);
  t_speed(tf);
}
