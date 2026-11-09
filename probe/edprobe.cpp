// edprobe.cpp - GDI text rendering + file I/O probe for the editor project.
// Visual C++ 6.0, Win32 + GDI, Windows 98 SE target.
//
//   edprobe -bench [-o FILE] [-reps N]   all tests, report to BENCH.TXT (next to the exe), exits
//   edprobe -render / -io                only the rendering / only the file+memory tests
//   edprobe -show text|hex|fonts|prop [fontIndex]   draw a view and stay (for screenshots)
//
// Timing: QueryPerformanceCounter. Every repaint is forced with InvalidateRect+UpdateWindow
// (or ScrollWindowEx+UpdateWindow) and followed by a GetPixel on the window DC so the display
// driver has finished drawing before the clock stops.

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------------------------
// basics

#define WANT_W 780
#define WANT_H 540
#define MAXCOL 200
#define MAXRUN 80
#define NLINES 3000
#define MAXREPS 400
#define NBG 4

enum { C_DEF, C_KW, C_TYPE, C_NUM, C_STR, C_COM, C_PRE, C_OP, C_GUT, C_SELFG, NFG };
static COLORREF g_fg[NFG] = {
    RGB(0,0,0), RGB(0,0,255), RGB(128,0,255), RGB(255,128,0), RGB(128,128,128),
    RGB(0,128,0), RGB(128,64,0), RGB(0,0,128), RGB(128,128,128), RGB(255,255,255) };
// 0 normal, 1 current line, 2 gutter, 3 selection
static COLORREF g_bgc[NBG] = { RGB(255,255,255), RGB(232,232,255), RGB(228,228,228), RGB(0,0,128) };
#define NCOMBO (NFG * NBG)

struct Run { short start, len; unsigned char col; };
struct Line {
    int len, nruns;
    char text[MAXCOL];
    unsigned char col[MAXCOL];
    WCHAR w[MAXCOL];
    Run runs[MAXRUN];
};

struct FontDef {
    const char *face; int height; int charset; const char *label; int prop;
    HFONT h; int cw, lh, ok; double createMs;
};

static FontDef g_fonts[] = {
    { "Courier New",    -13, ANSI_CHARSET, "Courier New 10pt",    0 },
    { "Courier New",    -12, ANSI_CHARSET, "Courier New 9pt",     0 },
    { "Lucida Console", -13, ANSI_CHARSET, "Lucida Console 10pt", 0 },
    { "Lucida Console", -12, ANSI_CHARSET, "Lucida Console 9pt",  0 },
    { "Fixedsys",         0, ANSI_CHARSET, "Fixedsys",            0 },
    { "Terminal",        12, OEM_CHARSET,  "Terminal (12px)",     0 },
    { "Courier",        -13, ANSI_CHARSET, "Courier raster 10pt", 0 },
    { "Tahoma",         -11, ANSI_CHARSET, "Tahoma 8pt",          1 },
    { "Arial",          -13, ANSI_CHARSET, "Arial 10pt",          1 },
    { "MS Sans Serif",  -11, ANSI_CHARSET, "MS Sans Serif 8pt",   1 },
};
#define NFONTS (sizeof(g_fonts) / sizeof(g_fonts[0]))

static HWND g_hwnd;
static HDC g_hdcWin;
static int g_W = WANT_W, g_H = WANT_H;
static double g_msPerTick;
static int g_reps = 60;

static HDC g_memDC;  static HBITMAP g_memBmp;
static HDC g_dibDC;  static HBITMAP g_dibBmp;  static WORD *g_dibBits;
static HDC g_dibxDC; static HBITMAP g_dibxBmp; static WORD *g_dibxBits;
static int g_pitch;          // DIB pitch in WORDs
static int g_is565 = 1, g_bpp = 16;
static WORD g_pxGreen;
static HBRUSH g_bgBrush[NBG];

static HDC g_glyphDC; static HBITMAP g_glyphBmp, g_glyphOld; static FontDef *g_glyphFont;
static WORD *g_soft; static FontDef *g_softFont; static WORD g_bg16[NBG];
static double g_glyphBuildMs, g_softBuildMs;

static Line *g_norm, *g_dense, *g_lines;
static int g_nlines = NLINES;
static unsigned char *g_hexData; static int g_hexSize = 65536; static int g_hexColour;

enum { V_NONE, V_TEXT, V_HEX, V_PROP, V_BANDS, V_CHARSET };
static void RenderCharsets(HDC dc);
enum { T_DIRECT, T_COMPAT, T_DIB, T_DIBX };
enum { M_NONE, M_A, M_ATRANS, M_W, M_RUNDX, M_LINE, M_LINEDX, M_GLYPHBLT, M_GLYPHSOFT };
static int g_view = V_TEXT, g_target = T_DIRECT, g_method = M_A;
static FontDef *g_font;
static int g_top, g_caretLine = 7, g_rowH = 16;
static int g_dx[512];
static int g_showMode;

static double Now()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * g_msPerTick;
}

// ---------------------------------------------------------------------------------------------
// log

static char g_logPath[MAX_PATH];
static char g_exeDir[MAX_PATH];
static char *g_log; static int g_logLen;

static void Log(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = _vsnprintf(g_log + g_logLen, 60000 - g_logLen - 1, fmt, ap);
    va_end(ap);
    if (n > 0) g_logLen += n;
    if (g_logLen > 50000) {
        // flushed by caller normally; keep safe
    }
}

static void FlushLog()
{
    HANDLE h;
    DWORD wr;
    char *crlf;
    int i, j;
    if (!g_logLen) return;
    crlf = (char *)malloc(g_logLen * 2 + 1);
    for (i = 0, j = 0; i < g_logLen; i++) {
        if (g_log[i] == '\n') crlf[j++] = '\r';
        crlf[j++] = g_log[i];
    }
    h = CreateFile(g_logPath, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, NULL, FILE_END);
        WriteFile(h, crlf, j, &wr, NULL);
        CloseHandle(h);
    }
    free(crlf);
    g_logLen = 0;
}

// ---------------------------------------------------------------------------------------------
// deterministic C-like text + a tiny lexer

static unsigned g_seed = 12345;
static int rn(int n) { g_seed = g_seed * 1103515245u + 12345u; return (int)((g_seed >> 16) & 0x7fff) % n; }
#define NELEM(a) (sizeof(a) / sizeof(a[0]))
#define PICK(a) a[rn(NELEM(a))]

static const char *s_id[] = { "count","buf","len","pos","node","result","index","value","ptr","data",
    "width","height","flags","line","col","text","item","size","next","prev","handle","state","total","offset" };
static const char *s_fn[] = { "strlen","memcpy","GetLine","DrawRun","InsertText","malloc","FindNext",
    "UpdateView","SendMessage","LexLine","AllocBlock" };
static const char *s_ty[] = { "int","char","void","long","unsigned","DWORD","HWND","BOOL" };
static const char *s_cm[] = { "update the view","TODO: handle errors","walk the list","fast path",
    "see below","cache the result","caller frees","must be aligned" };
static const char *s_hd[] = { "windows","stdio","stdlib","string","buffer","lexer","view" };
static const char *s_kw[] = { "if","else","for","while","return","switch","case","break","do","sizeof",
    "static","const","struct","NULL" };

static void GenStmt(char *p)
{
    switch (rn(11)) {
    case 0: sprintf(p, "%s = %s(%s, %d) + %s->%s;", PICK(s_id), PICK(s_fn), PICK(s_id), rn(100), PICK(s_id), PICK(s_id)); break;
    case 1: sprintf(p, "if (%s == NULL) return %d;", PICK(s_id), rn(5) - 1); break;
    case 2: sprintf(p, "for (i = 0; i < %s; i++) {", PICK(s_id)); break;
    case 3: sprintf(p, "printf(\"%s: %%d items\\n\", %s);", PICK(s_cm), PICK(s_id)); break;
    case 4: sprintf(p, "%s %s[%d]; /* %s */", PICK(s_ty), PICK(s_id), 16 << rn(5), PICK(s_cm)); break;
    case 5: sprintf(p, "%s = %s * %d + %s(%s, %s) - (%s << %d) + %s[%d];", PICK(s_id), PICK(s_id), rn(9) + 2,
                PICK(s_fn), PICK(s_id), PICK(s_id), PICK(s_id), rn(8), PICK(s_id), rn(64)); break;
    case 6: sprintf(p, "while (%s != NULL && %s->%s) %s = %s->%s;", PICK(s_id), PICK(s_id), PICK(s_id), PICK(s_id), PICK(s_id), PICK(s_id)); break;
    case 7: sprintf(p, "return %s(%s, %s, %d);", PICK(s_fn), PICK(s_id), PICK(s_id), rn(1000)); break;
    case 8: sprintf(p, "else if (%s > %d && %s) {", PICK(s_id), rn(256), PICK(s_id)); break;
    case 9: strcpy(p, "}"); break;
    default: sprintf(p, "%s += sizeof(%s) * %d;", PICK(s_id), PICK(s_ty), rn(16) + 1); break;
    }
}

static void GenLine(char *out, int dense)
{
    char tmp[600];
    int t = rn(100), ind, i;
    tmp[0] = 0;
    if (!dense && t < 4) { out[0] = 0; return; }
    if (!dense && t < 10) {
        if (rn(2)) sprintf(out, "#include <%s.h>", PICK(s_hd));
        else sprintf(out, "#define %s_MAX %d", PICK(s_id), rn(4096));
        return;
    }
    if (!dense && t < 16) { sprintf(out, rn(2) ? "/* %s, %s */" : "// %s: %s", PICK(s_cm), PICK(s_cm)); return; }
    if (!dense && t < 20) {
        sprintf(out, "static %s %s(%s *%s, int %s)", PICK(s_ty), PICK(s_fn), PICK(s_ty), PICK(s_id), PICK(s_id));
        return;
    }
    ind = 1 + rn(3);
    for (i = 0; i < ind; i++) out[i] = '\t';
    out[ind] = 0;
    GenStmt(tmp);
    strcat(out, tmp);
    if (dense) {
        while ((int)strlen(out) + ind * 3 < 96) { strcat(out, " "); GenStmt(tmp); strcat(out, tmp); }
    } else if (rn(4) == 0) {
        strcat(out, "  // ");
        strcat(out, PICK(s_cm));
    }
}

#define ISAL(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || (c) == '_')
#define ISDG(c) ((c) >= '0' && (c) <= '9')

static int InList(const char *s, int n, const char **list, int cnt)
{
    int i;
    for (i = 0; i < cnt; i++)
        if ((int)strlen(list[i]) == n && memcmp(list[i], s, n) == 0) return 1;
    return 0;
}

static void Lex(Line *ln)
{
    int n = ln->len, i, j, k, c;
    char *t = ln->text;
    unsigned char *cl = ln->col;
    for (j = 0; j < n && t[j] == ' '; j++) cl[j] = C_DEF;
    if (j < n && t[j] == '#') {
        for (i = j; i < n; i++) cl[i] = C_PRE;
        for (i = j; i < n; i++) {
            if (t[i] == '<' || t[i] == '"') {
                char e = t[i] == '<' ? '>' : '"';
                for (k = i; k < n; k++) { cl[k] = C_STR; if (k > i && t[k] == e) break; }
                i = k;
            }
        }
    } else {
        i = j;
        while (i < n) {
            char ch = t[i];
            if (ch == '/' && i + 1 < n && t[i + 1] == '/') {
                while (i < n) cl[i++] = C_COM;
            } else if (ch == '/' && i + 1 < n && t[i + 1] == '*') {
                k = i + 2;
                while (k + 1 < n && !(t[k] == '*' && t[k + 1] == '/')) k++;
                k = (k + 1 < n) ? k + 2 : n;
                while (i < k) cl[i++] = C_COM;
            } else if (ch == '"') {
                k = i + 1;
                while (k < n && t[k] != '"') { if (t[k] == '\\') k++; k++; }
                if (k < n) k++;
                if (k > n) k = n;
                while (i < k) cl[i++] = C_STR;
            } else if (ISDG(ch)) {
                k = i;
                while (k < n && (ISAL(t[k]) || ISDG(t[k]))) k++;
                while (i < k) cl[i++] = C_NUM;
            } else if (ISAL(ch)) {
                k = i;
                while (k < n && (ISAL(t[k]) || ISDG(t[k]))) k++;
                c = InList(t + i, k - i, s_kw, NELEM(s_kw)) ? C_KW :
                    InList(t + i, k - i, s_ty, NELEM(s_ty)) ? C_TYPE : C_DEF;
                while (i < k) cl[i++] = (unsigned char)c;
            } else {
                cl[i++] = C_DEF;
            }
        }
    }
    ln->nruns = 0;
    for (i = 0; i < n; i++) {
        if (ln->nruns == 0 || (cl[i] != ln->runs[ln->nruns - 1].col && ln->nruns < MAXRUN)) {
            ln->runs[ln->nruns].start = (short)i;
            ln->runs[ln->nruns].len = 1;
            ln->runs[ln->nruns].col = cl[i];
            ln->nruns++;
        } else {
            ln->runs[ln->nruns - 1].len++;
        }
        ln->w[i] = (WCHAR)(unsigned char)t[i];
    }
}

static void SetLineText(Line *ln, const char *raw)
{
    int n = 0;
    for (; *raw && n < MAXCOL - 8; raw++) {
        if (*raw == '\t') { do ln->text[n++] = ' '; while (n & 3); }
        else ln->text[n++] = *raw;
    }
    ln->len = n;
    Lex(ln);
}

static Line *GenText(int dense, unsigned seed)
{
    Line *L = (Line *)malloc(sizeof(Line) * NLINES);
    char raw[700];
    int i;
    g_seed = seed;
    for (i = 0; i < NLINES; i++) { GenLine(raw, dense); SetLineText(&L[i], raw); }
    return L;
}

static void TextStats(Line *L, int first, int cnt, double *avgLen, double *avgRuns, int *maxLen)
{
    int i, sl = 0, sr = 0, mx = 0;
    for (i = first; i < first + cnt; i++) {
        sl += L[i].len; sr += L[i].nruns;
        if (L[i].len > mx) mx = L[i].len;
    }
    *avgLen = (double)sl / cnt; *avgRuns = (double)sr / cnt; *maxLen = mx;
}

// ---------------------------------------------------------------------------------------------
// fonts, surfaces, glyph caches

static void InitFonts()
{
    int i;
    char face[64];
    TEXTMETRIC tm;
    SIZE s1, s2;
    double t0;
    for (i = 0; i < (int)NFONTS; i++) {
        FontDef *f = &g_fonts[i];
        t0 = Now();
        f->h = CreateFont(f->height, 0, 0, 0, FW_NORMAL, 0, 0, 0, f->charset, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                          (f->prop ? VARIABLE_PITCH | FF_SWISS : FIXED_PITCH | FF_MODERN), f->face);
        SelectObject(g_hdcWin, f->h);
        GetTextFace(g_hdcWin, sizeof(face), face);
        GetTextMetrics(g_hdcWin, &tm);
        f->createMs = Now() - t0;
        f->cw = tm.tmAveCharWidth;
        f->lh = tm.tmHeight;
        f->ok = lstrcmpi(face, f->face) == 0;
        if (f->ok && !f->prop) {
            GetTextExtentPoint32(g_hdcWin, "iiiiiiiiii", 10, &s1);
            GetTextExtentPoint32(g_hdcWin, "MMMMMMMMMM", 10, &s2);
            if (s1.cx != s2.cx) f->ok = 0;
            else f->cw = s1.cx / 10;
        }
    }
    SelectObject(g_hdcWin, GetStockObject(SYSTEM_FONT));
    for (i = 0; i < 512; i++) g_dx[i] = 8;
}

static FontDef *FindFont(const char *label)
{
    int i;
    for (i = 0; i < (int)NFONTS; i++)
        if (g_fonts[i].ok && lstrcmpi(g_fonts[i].label, label) == 0) return &g_fonts[i];
    return NULL;
}

static HBITMAP MakeDib16(int w, int h, int is565, WORD **bits)
{
    struct { BITMAPINFOHEADER h; DWORD m[3]; } bi;
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = w;
    bi.h.biHeight = -h;
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 16;
    bi.h.biCompression = BI_BITFIELDS;
    if (is565) { bi.m[0] = 0xF800; bi.m[1] = 0x07E0; bi.m[2] = 0x001F; }
    else       { bi.m[0] = 0x7C00; bi.m[1] = 0x03E0; bi.m[2] = 0x001F; }
    return CreateDIBSection(g_hdcWin, (BITMAPINFO *)&bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

static void InitSurfaces()
{
    HDC m;
    HBITMAP b, ob;
    WORD px[4];
    int i;
    g_bpp = GetDeviceCaps(g_hdcWin, BITSPIXEL) * GetDeviceCaps(g_hdcWin, PLANES);
    m = CreateCompatibleDC(g_hdcWin);
    b = CreateCompatibleBitmap(g_hdcWin, 4, 1);
    ob = (HBITMAP)SelectObject(m, b);
    SetPixelV(m, 0, 0, RGB(0, 255, 0));
    memset(px, 0, sizeof(px));
    GetBitmapBits(b, 8, px);
    g_pxGreen = px[0];
    g_is565 = (px[0] == 0x07E0);
    SelectObject(m, ob);
    DeleteObject(b);
    DeleteDC(m);

    g_memDC = CreateCompatibleDC(g_hdcWin);
    g_memBmp = CreateCompatibleBitmap(g_hdcWin, g_W, g_H);
    SelectObject(g_memDC, g_memBmp);
    g_pitch = (g_W + 1) & ~1;
    g_dibDC = CreateCompatibleDC(g_hdcWin);
    g_dibBmp = MakeDib16(g_W, g_H, g_is565, &g_dibBits);
    SelectObject(g_dibDC, g_dibBmp);
    g_dibxDC = CreateCompatibleDC(g_hdcWin);
    g_dibxBmp = MakeDib16(g_W, g_H, !g_is565, &g_dibxBits);
    SelectObject(g_dibxDC, g_dibxBmp);
    for (i = 0; i < NBG; i++) g_bgBrush[i] = CreateSolidBrush(g_bgc[i]);
    g_glyphDC = CreateCompatibleDC(g_hdcWin);
}

static void DrawGlyphSheet(HDC dc, FontDef *f)
{
    int c, g;
    char ch;
    RECT rc;
    SelectObject(dc, f->h);
    SetBkMode(dc, OPAQUE);
    for (c = 0; c < NCOMBO; c++) {
        SetTextColor(dc, g_fg[c / NBG]);
        SetBkColor(dc, g_bgc[c % NBG]);
        for (g = 0; g < 96; g++) {
            ch = (char)(32 + g);
            rc.left = g * f->cw; rc.right = rc.left + f->cw; rc.top = c * f->lh; rc.bottom = rc.top + f->lh;
            ExtTextOutA(dc, rc.left, rc.top, ETO_OPAQUE, &rc, &ch, 1, NULL);
        }
    }
}

static void BuildGlyphCaches(FontDef *f)
{
    double t0;
    int w = 96 * f->cw, h = NCOMBO * f->lh, c, g, r, x, pitch;
    HDC dc;
    HBITMAP bmp;
    WORD *bits, *dst;
    if (g_glyphFont == f) return;
    t0 = Now();
    if (g_glyphBmp) { SelectObject(g_glyphDC, g_glyphOld); DeleteObject(g_glyphBmp); }
    g_glyphBmp = CreateCompatibleBitmap(g_hdcWin, w, h);
    g_glyphOld = (HBITMAP)SelectObject(g_glyphDC, g_glyphBmp);
    DrawGlyphSheet(g_glyphDC, f);
    GetPixel(g_glyphDC, 0, 0);
    g_glyphBuildMs = Now() - t0;
    g_glyphFont = f;

    t0 = Now();
    dc = CreateCompatibleDC(g_hdcWin);
    bmp = MakeDib16(w, h, g_is565, &bits);
    SelectObject(dc, bmp);
    DrawGlyphSheet(dc, f);
    GdiFlush();
    pitch = (w + 1) & ~1;
    if (g_soft) free(g_soft);
    g_soft = (WORD *)malloc(NCOMBO * 96 * f->cw * f->lh * sizeof(WORD));
    dst = g_soft;
    for (c = 0; c < NCOMBO; c++)
        for (g = 0; g < 96; g++)
            for (r = 0; r < f->lh; r++)
                for (x = 0; x < f->cw; x++)
                    *dst++ = bits[(c * f->lh + r) * pitch + g * f->cw + x];
    for (c = 0; c < NBG; c++) g_bg16[c] = g_soft[(C_DEF * NBG + c) * 96 * f->lh * f->cw];
    DeleteDC(dc);
    DeleteObject(bmp);
    g_softFont = f;
    g_softBuildMs = Now() - t0;
}

// ---------------------------------------------------------------------------------------------
// text view rendering

static void FillRow16(WORD *bits, int x, int y, int rows, WORD v)
{
    int r, i;
    WORD *d;
    for (r = 0; r < rows; r++) {
        d = bits + (y + r) * g_pitch;
        for (i = x; i < g_W; i++) d[i] = v;
    }
}

static void SoftGlyph(WORD *bits, FontDef *f, int x, int y, int rows, int ch, int combo)
{
    const WORD *src;
    WORD *dst;
    int r, c, cw = f->cw;
    if (ch < 32 || ch > 127) ch = '?';
    src = g_soft + ((combo * 96 + (ch - 32)) * f->lh) * cw;
    dst = bits + y * g_pitch + x;
    if (cw == 8) {
        for (r = 0; r < rows; r++) {
            DWORD *d = (DWORD *)dst; const DWORD *s = (const DWORD *)src;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
            dst += g_pitch; src += 8;
        }
    } else {
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cw; c++) dst[c] = src[c];
            dst += g_pitch; src += cw;
        }
    }
}

static void SoftTextRow(WORD *bits, FontDef *f, int L, int y)
{
    Line *ln = (L >= 0 && L < g_nlines) ? &g_lines[L] : NULL;
    int bgi = (L == g_caretLine) ? 1 : 0, cw = f->cw, G = 6 * cw, x, i, rows;
    char num[16];
    rows = g_H - y; if (rows > f->lh) rows = f->lh;
    if (ln) wsprintf(num, "%5d ", L + 1); else lstrcpy(num, "      ");
    for (i = 0; i < 6; i++) SoftGlyph(bits, f, i * cw, y, rows, num[i], C_GUT * NBG + 2);
    x = G;
    if (ln) {
        for (i = 0; i < ln->len && x + cw <= g_W; i++, x += cw)
            SoftGlyph(bits, f, x, y, rows, (unsigned char)ln->text[i], ln->col[i] * NBG + bgi);
    }
    FillRow16(bits, x, y, rows, g_bg16[bgi]);
}

static void DrawTextRow(HDC dc, FontDef *f, int L, int y, int method)
{
    Line *ln = (L >= 0 && L < g_nlines) ? &g_lines[L] : NULL;
    int bgi = (L == g_caretLine) ? 1 : 0, cw = f->cw, lh = f->lh, G = 6 * cw, W = g_W;
    int i, x, n, cur = -1, opaque = (method != M_ATRANS) ? ETO_OPAQUE : 0;
    int *dx = (method == M_RUNDX || method == M_LINEDX) ? g_dx : NULL;
    char num[16];
    WCHAR wnum[8];
    RECT rc;
    if (ln) wsprintf(num, "%5d ", L + 1); else lstrcpy(num, "      ");

    if (method == M_GLYPHBLT) {
        for (i = 0; i < 6; i++)
            BitBlt(dc, i * cw, y, cw, lh, g_glyphDC, (num[i] - 32) * cw, (C_GUT * NBG + 2) * lh, SRCCOPY);
        x = G;
        if (ln) {
            for (i = 0; i < ln->len && x < W; i++, x += cw)
                BitBlt(dc, x, y, cw, lh, g_glyphDC, (ln->text[i] - 32) * cw, (ln->col[i] * NBG + bgi) * lh, SRCCOPY);
        }
        rc.left = x; rc.right = W; rc.top = y; rc.bottom = y + lh;
        if (rc.left < rc.right) FillRect(dc, &rc, g_bgBrush[bgi]);
        return;
    }

    // gutter
    SetTextColor(dc, g_fg[C_GUT]);
    SetBkColor(dc, g_bgc[2]);
    rc.left = 0; rc.right = G; rc.top = y; rc.bottom = y + lh;
    if (method == M_W) {
        for (i = 0; i < 6; i++) wnum[i] = (WCHAR)num[i];
        ExtTextOutW(dc, 0, y, opaque, &rc, wnum, 6, NULL);
    } else {
        ExtTextOutA(dc, 0, y, opaque, &rc, num, 6, dx);
    }
    SetBkColor(dc, g_bgc[bgi]);
    if (method == M_ATRANS && bgi) {
        rc.left = G; rc.right = W;
        FillRect(dc, &rc, g_bgBrush[bgi]);
    }
    if (method == M_LINE || method == M_LINEDX) {
        SetTextColor(dc, g_fg[C_DEF]);
        rc.left = G; rc.right = W;
        n = ln ? ln->len : 0;
        if (n > (W - G) / cw + 1) n = (W - G) / cw + 1;
        ExtTextOutA(dc, G, y, ETO_OPAQUE, &rc, ln ? ln->text : "", n, dx);
        return;
    }
    x = G;
    if (ln) {
        for (i = 0; i < ln->nruns; i++) {
            Run *r = &ln->runs[i];
            x = G + r->start * cw;
            if (x >= W) break;
            n = r->len;
            if (r->col != cur) { cur = r->col; SetTextColor(dc, g_fg[cur]); }
            rc.left = x; rc.right = x + n * cw;
            if (method == M_W) ExtTextOutW(dc, x, y, opaque, &rc, ln->w + r->start, n, NULL);
            else ExtTextOutA(dc, x, y, opaque, &rc, ln->text + r->start, n, dx);
            x = rc.right;
        }
    }
    if (opaque && x < W) {
        rc.left = x; rc.right = W;
        ExtTextOutA(dc, x, y, ETO_OPAQUE, &rc, "", 0, NULL);
    }
}

static void RenderText(HDC dc, WORD *bits, const RECT *rp)
{
    FontDef *f = g_font;
    int r, r0, r1, y;
    RECT rc;
    if (g_method == M_NONE) return;
    if (g_method == M_GLYPHBLT || g_method == M_GLYPHSOFT) BuildGlyphCaches(f);
    SelectObject(dc, f->h);
    r0 = rp->top / f->lh;
    r1 = (rp->bottom + f->lh - 1) / f->lh;
    if (g_method == M_ATRANS) {
        rc = *rp; rc.left = 0; rc.right = 6 * f->cw;
        FillRect(dc, &rc, g_bgBrush[2]);
        rc.left = rc.right; rc.right = g_W;
        FillRect(dc, &rc, g_bgBrush[0]);
        SetBkMode(dc, TRANSPARENT);
    } else {
        SetBkMode(dc, OPAQUE);
    }
    if (g_method == M_GLYPHSOFT) GdiFlush();
    for (r = r0; r < r1; r++) {
        y = r * f->lh;
        if (y >= g_H) break;
        if (g_method == M_GLYPHSOFT) SoftTextRow(bits, f, g_top + r, y);
        else DrawTextRow(dc, f, g_top + r, y, g_method);
    }
}

// proportional reference: GetTextExtentPoint32 per run to advance x
static void RenderProp(HDC dc, const RECT *rp)
{
    FontDef *f = g_font;
    int r, r0, r1, y, i, x, L, bgi, cur, G;
    Line *ln;
    SIZE sz;
    RECT rc;
    char num[16];
    SelectObject(dc, f->h);
    SetBkMode(dc, OPAQUE);
    GetTextExtentPoint32(dc, "00000 ", 6, &sz);
    G = sz.cx;
    r0 = rp->top / f->lh;
    r1 = (rp->bottom + f->lh - 1) / f->lh;
    for (r = r0; r < r1; r++) {
        y = r * f->lh;
        if (y >= g_H) break;
        L = g_top + r;
        ln = (L < g_nlines) ? &g_lines[L] : NULL;
        bgi = (L == g_caretLine) ? 1 : 0;
        rc.top = y; rc.bottom = y + f->lh; rc.left = 0; rc.right = G;
        SetTextColor(dc, g_fg[C_GUT]); SetBkColor(dc, g_bgc[2]);
        wsprintf(num, "%5d ", L + 1);
        ExtTextOutA(dc, 0, y, ETO_OPAQUE, &rc, num, 6, NULL);
        SetBkColor(dc, g_bgc[bgi]);
        x = G; cur = -1;
        if (ln) {
            for (i = 0; i < ln->nruns && x < g_W; i++) {
                Run *ru = &ln->runs[i];
                GetTextExtentPoint32(dc, ln->text + ru->start, ru->len, &sz);
                if (ru->col != cur) { cur = ru->col; SetTextColor(dc, g_fg[cur]); }
                rc.left = x; rc.right = x + sz.cx;
                ExtTextOutA(dc, x, y, ETO_OPAQUE, &rc, ln->text + ru->start, ru->len, NULL);
                x += sz.cx;
            }
        }
        if (x < g_W) { rc.left = x; rc.right = g_W; ExtTextOutA(dc, x, y, ETO_OPAQUE, &rc, "", 0, NULL); }
    }
}

// ---------------------------------------------------------------------------------------------
// hex view

static void DrawAttrRow(HDC dc, FontDef *f, int y, const char *ch, const unsigned char *fg,
                        const unsigned char *bg, int n)
{
    int i = 0, j, cf = -1, cb = -1;
    RECT rc;
    rc.top = y; rc.bottom = y + f->lh;
    while (i < n) {
        j = i + 1;
        while (j < n && fg[j] == fg[i] && bg[j] == bg[i]) j++;
        if (fg[i] != cf) { cf = fg[i]; SetTextColor(dc, g_fg[cf]); }
        if (bg[i] != cb) { cb = bg[i]; SetBkColor(dc, g_bgc[cb]); }
        rc.left = i * f->cw; rc.right = j * f->cw;
        ExtTextOutA(dc, rc.left, y, ETO_OPAQUE, &rc, ch + i, j - i, NULL);
        i = j;
    }
    if (n * f->cw < g_W) {
        SetBkColor(dc, g_bgc[0]);
        rc.left = n * f->cw; rc.right = g_W;
        ExtTextOutA(dc, rc.left, y, ETO_OPAQUE, &rc, "", 0, NULL);
    }
}

static int g_hexRuns, g_hexRowsDrawn;

static void RenderHex(HDC dc, const RECT *rp)
{
    static const char hx[] = "0123456789ABCDEF";
    FontDef *f = g_font;
    char ch[100];
    unsigned char fg[100], bg[100];
    int r, r0, r1, y, n, k, b, off, sa, sb, sel, fgc, bgc, v;
    SelectObject(dc, f->h);
    SetBkMode(dc, OPAQUE);
    r0 = rp->top / f->lh;
    r1 = (rp->bottom + f->lh - 1) / f->lh;
    sa = (g_caretLine + 3) * 16 + 5;
    sb = (g_caretLine + 9) * 16 + 11;
    for (r = r0; r < r1; r++) {
        y = r * f->lh;
        if (y >= g_H) break;
        off = (g_top + r) * 16;
        n = 0;
        if (off + 16 <= g_hexSize) {
            for (k = 0; k < 8; k++) { ch[n] = hx[(off >> (28 - 4 * k)) & 15]; fg[n] = C_GUT; bg[n] = 2; n++; }
            ch[n] = ' '; fg[n] = C_DEF; bg[n] = 0; n++;
            ch[n] = ' '; fg[n] = C_DEF; bg[n] = 0; n++;
            for (b = 0; b < 16; b++) {
                v = g_hexData[off + b];
                sel = (off + b >= sa && off + b < sb);
                fgc = sel ? C_SELFG : (!g_hexColour ? C_DEF : (v == 0 ? C_STR : (v >= 32 && v < 127 ? C_DEF : C_KW)));
                bgc = sel ? 3 : 0;
                ch[n] = hx[v >> 4]; fg[n] = (unsigned char)fgc; bg[n] = (unsigned char)bgc; n++;
                ch[n] = hx[v & 15]; fg[n] = (unsigned char)fgc; bg[n] = (unsigned char)bgc; n++;
                ch[n] = ' '; fg[n] = (unsigned char)fgc;
                bg[n] = (unsigned char)((sel && off + b + 1 < sb && b != 15) ? 3 : 0); n++;
                if (b == 7) { ch[n] = ' '; fg[n] = (unsigned char)fgc; bg[n] = 0; n++; }
            }
            ch[n] = ' '; fg[n] = C_DEF; bg[n] = 0; n++;
            for (b = 0; b < 16; b++) {
                v = g_hexData[off + b];
                sel = (off + b >= sa && off + b < sb);
                ch[n] = (char)((v >= 32 && v < 127) ? v : '.');
                fg[n] = (unsigned char)(sel ? C_SELFG : (!g_hexColour ? C_DEF : (v == 0 ? C_STR : (v >= 32 && v < 127 ? C_DEF : C_KW))));
                bg[n] = (unsigned char)(sel ? 3 : 0);
                n++;
            }
        }
        DrawAttrRow(dc, f, y, ch, fg, bg, n);
        g_hexRowsDrawn++;
    }
}

// font bands (screenshot only)
static void RenderBands(HDC dc)
{
    int i, k, y = 0, oldTop = g_top;
    FontDef *ui = FindFont("Tahoma 8pt");
    FontDef *save = g_font;
    RECT rc;
    char label[100];
    for (i = 0; i < (int)NFONTS && y < g_H; i++) {
        FontDef *f = &g_fonts[i];
        if (!f->ok) continue;
        if (ui) SelectObject(dc, ui->h);
        SetTextColor(dc, RGB(255, 255, 255)); SetBkColor(dc, RGB(64, 64, 64));
        rc.left = 0; rc.right = g_W; rc.top = y; rc.bottom = y + 14;
        sprintf(label, " %s  (cell %dx%d)", f->label, f->cw, f->lh);
        ExtTextOutA(dc, 2, y, ETO_OPAQUE, &rc, label, strlen(label), NULL);
        y += 14;
        g_font = f;
        for (k = 0; k < (f->prop ? 2 : 3); k++) {
            g_top = 20 + k;
            if (f->prop) {
                RECT rp; int saveH = g_H;
                // draw a single prop row at y using RenderProp by offsetting the viewport
                SetViewportOrgEx(dc, 0, y, NULL);
                rp.left = 0; rp.right = g_W; rp.top = 0; rp.bottom = 1;
                g_H = f->lh;
                RenderProp(dc, &rp);
                g_H = saveH;
                SetViewportOrgEx(dc, 0, 0, NULL);
            } else {
                SetBkMode(dc, OPAQUE);
                SelectObject(dc, f->h);
                DrawTextRow(dc, f, g_top, y, M_A);
            }
            y += f->lh;
        }
    }
    if (y < g_H) { rc.left = 0; rc.right = g_W; rc.top = y; rc.bottom = g_H; FillRect(dc, &rc, g_bgBrush[0]); }
    g_font = save;
    g_top = oldTop;
}

// ---------------------------------------------------------------------------------------------
// window

static void RenderView(HDC dc, WORD *bits, const RECT *rp)
{
    switch (g_view) {
    case V_TEXT: RenderText(dc, bits, rp); break;
    case V_HEX: RenderHex(dc, rp); break;
    case V_PROP: RenderProp(dc, rp); break;
    case V_BANDS: RenderBands(dc); break;
    case V_CHARSET: RenderCharsets(dc); break;
    }
}

static void Paint(HDC hdc, const RECT *rp)
{
    int w = rp->right - rp->left, h = rp->bottom - rp->top;
    if (w <= 0 || h <= 0) return;
    switch (g_target) {
    case T_DIRECT:
        RenderView(hdc, NULL, rp);
        break;
    case T_COMPAT:
        RenderView(g_memDC, NULL, rp);
        BitBlt(hdc, rp->left, rp->top, w, h, g_memDC, rp->left, rp->top, SRCCOPY);
        break;
    case T_DIB:
        RenderView(g_dibDC, g_dibBits, rp);
        BitBlt(hdc, rp->left, rp->top, w, h, g_dibDC, rp->left, rp->top, SRCCOPY);
        break;
    case T_DIBX:
        RenderView(g_dibxDC, g_dibxBits, rp);
        BitBlt(hdc, rp->left, rp->top, w, h, g_dibxDC, rp->left, rp->top, SRCCOPY);
        break;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PAINTSTRUCT ps;
    switch (msg) {
    case WM_PAINT:
        BeginPaint(hwnd, &ps);
        Paint(ps.hdc, &ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void Pump()
{
    MSG m;
    while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); }
}

static void Sync() { GetPixel(g_hdcWin, 1, 1); }

// ---------------------------------------------------------------------------------------------
// measurement

struct Stat { double med, worst, mean, best; };

static int CmpD(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static Stat MakeStat(double *t, int n)
{
    Stat s;
    int i;
    double sum = 0;
    qsort(t, n, sizeof(double), CmpD);
    for (i = 0; i < n; i++) sum += t[i];
    s.med = t[n / 2]; s.worst = t[n - 1]; s.best = t[0]; s.mean = sum / n;
    return s;
}

enum { RP_FULL, RP_SCROLL, RP_PAGE, RP_TYPE };
static int g_typeCol = 12;

static void TypeOne(int i)
{
    Line *ln = &g_lines[g_caretLine];
    int c = g_typeCol;
    if (c > ln->len) c = ln->len;
    if (!(i & 1) && ln->len < MAXCOL - 10) {
        memmove(ln->text + c + 1, ln->text + c, ln->len - c);
        ln->text[c] = 'x';
        ln->len++;
    } else if (ln->len > c) {
        memmove(ln->text + c, ln->text + c + 1, ln->len - c - 1);
        ln->len--;
    }
    Lex(ln);
}

static Stat Measure(int kind, int reps)
{
    double t[MAXREPS], t0;
    int i, base = g_top, rows = g_H / g_rowH, r;
    RECT rr;
    if (reps > MAXREPS) reps = MAXREPS;
    for (i = -3; i < reps; i++) {
        Pump();
        t0 = Now();
        switch (kind) {
        case RP_FULL:
            g_top = base + ((i + 3) % 40);
            InvalidateRect(g_hwnd, NULL, FALSE);
            UpdateWindow(g_hwnd);
            break;
        case RP_SCROLL:
            g_top++;
            ScrollWindowEx(g_hwnd, 0, -g_rowH, NULL, NULL, NULL, NULL, SW_INVALIDATE);
            UpdateWindow(g_hwnd);
            break;
        case RP_PAGE:
            g_top += rows - 1;
            InvalidateRect(g_hwnd, NULL, FALSE);
            UpdateWindow(g_hwnd);
            break;
        case RP_TYPE:
            TypeOne(i);
            r = g_caretLine - g_top;
            rr.left = 0; rr.right = g_W; rr.top = r * g_rowH; rr.bottom = rr.top + g_rowH;
            InvalidateRect(g_hwnd, &rr, FALSE);
            UpdateWindow(g_hwnd);
            HideCaret(g_hwnd);
            SetCaretPos(6 * g_font->cw + (g_typeCol + 1 - (i & 1)) * g_font->cw, rr.top);
            ShowCaret(g_hwnd);
            break;
        }
        Sync();
        if (i >= 0) t[i] = Now() - t0;
    }
    if (kind == RP_FULL) g_top = base;
    return MakeStat(t, reps);
}

static void LogStat(const char *name, Stat s)
{
    Log("  %-44s med %7.2f  worst %7.2f  mean %7.2f  best %7.2f ms  (%5.0f fps at med)\n",
        name, s.med, s.worst, s.mean, s.best, s.med > 0 ? 1000.0 / s.med : 0.0);
}

static void Setup(int view, int target, int method, FontDef *f)
{
    g_view = view; g_target = target; g_method = method; g_font = f;
    g_rowH = f->lh;
    if (method == M_GLYPHBLT || method == M_GLYPHSOFT) BuildGlyphCaches(f);
    // prime: one full paint
    InvalidateRect(g_hwnd, NULL, FALSE);
    UpdateWindow(g_hwnd);
    Sync();
}

struct TestDef { const char *name; int target, method; };
static TestDef g_tt[] = {
    { "direct: ETO_A opaque per run",           T_DIRECT, M_A },
    { "direct: FillRect + transparent runs",     T_DIRECT, M_ATRANS },
    { "direct: ETO_W opaque per run",           T_DIRECT, M_W },
    { "direct: ETO_A per run + lpDx",           T_DIRECT, M_RUNDX },
    { "direct: 1 ETO_A per line (mono)",        T_DIRECT, M_LINE },
    { "direct: 1 ETO_A per line + lpDx (mono)", T_DIRECT, M_LINEDX },
    { "compat bmp: ETO_A runs + BitBlt",        T_COMPAT, M_A },
    { "compat bmp: ETO_W runs + BitBlt",        T_COMPAT, M_W },
    { "DIB16: ETO_A runs + BitBlt",             T_DIB,    M_A },
    { "DIB16 other 555/565: ETO_A + BitBlt",    T_DIBX,   M_A },
    { "glyph cache: BitBlt/char -> compat+blit", T_COMPAT, M_GLYPHBLT },
    { "glyph cache: soft copy -> DIB16+blit",   T_DIB,    M_GLYPHSOFT },
    { "present only: compat BitBlt full",       T_COMPAT, M_NONE },
    { "present only: DIB16 BitBlt full",        T_DIB,    M_NONE },
    { "empty WM_PAINT (overhead)",              T_DIRECT, M_NONE },
};
#define NTT (sizeof(g_tt) / sizeof(g_tt[0]))

static double g_matrix[NTT][NFONTS];

static void InfoSection()
{
    OSVERSIONINFO vi;
    BOOL smooth = 0;
    int i, ok = 0;
    TEXTMETRIC tm;
    vi.dwOSVersionInfoSize = sizeof(vi);
    GetVersionEx(&vi);
    Log("== system ==\n");
    Log("Windows %d.%d build %d %s\n", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber & 0xffff, vi.szCSDVersion);
    Log("screen %dx%d, %d bpp, 16-bit format %s (green pixel = 0x%04X)\n",
        GetDeviceCaps(g_hdcWin, HORZRES), GetDeviceCaps(g_hdcWin, VERTRES), g_bpp, g_is565 ? "RGB565" : "RGB555", g_pxGreen);
    Log("logical dpi %d, client area %dx%d\n", GetDeviceCaps(g_hdcWin, LOGPIXELSY), g_W, g_H);
    SystemParametersInfo(SPI_GETFONTSMOOTHING, 0, &smooth, 0);
    Log("SPI_GETFONTSMOOTHING = %d (Win98 'smooth edges of screen fonts'; no ClearType on 9x)\n", smooth);
    Log("caret blink %d ms, QPC %.0f Hz\n", GetCaretBlinkTime(), 1000.0 / g_msPerTick);
    Log("\n== editor fonts (CreateFont + select + metrics) ==\n");
    for (i = 0; i < (int)NFONTS; i++) {
        FontDef *f = &g_fonts[i];
        if (f->ok) {
            SelectObject(g_hdcWin, f->h);
            GetTextMetrics(g_hdcWin, &tm);
            Log("  %-22s ok   cell %2dx%-2d  ascent %d  %s  create %.2f ms\n", f->label, f->cw, f->lh,
                tm.tmAscent, (tm.tmPitchAndFamily & TMPF_TRUETYPE) ? "TrueType" : "raster", f->createMs);
            ok++;
        } else {
            Log("  %-22s MISSING (substituted)\n", f->label);
        }
    }
    SelectObject(g_hdcWin, GetStockObject(SYSTEM_FONT));
}

struct EnumCtx { char names[200][LF_FACESIZE]; int types[200]; int cs[200]; int n, all; };

static int CALLBACK EnumProc(const LOGFONT *lf, const TEXTMETRIC *tm, DWORD type, LPARAM lp)
{
    EnumCtx *c = (EnumCtx *)lp;
    int i;
    (void)tm;
    c->all++;
    if ((lf->lfPitchAndFamily & 3) != FIXED_PITCH) return 1;
    for (i = 0; i < c->n; i++) if (lstrcmpi(c->names[i], lf->lfFaceName) == 0) return 1;
    if (c->n < 200) {
        lstrcpyn(c->names[c->n], lf->lfFaceName, LF_FACESIZE);
        c->types[c->n] = (int)type;
        c->cs[c->n] = lf->lfCharSet;
        c->n++;
    }
    return 1;
}

static void FontListSection()
{
    static EnumCtx ctx;
    LOGFONT lf;
    int i;
    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    ctx.n = 0; ctx.all = 0;
    EnumFontFamiliesEx(g_hdcWin, &lf, (FONTENUMPROC)EnumProc, (LPARAM)&ctx, 0);
    Log("\n== installed fixed-pitch faces (EnumFontFamiliesEx, %d face/charset entries total) ==\n", ctx.all);
    for (i = 0; i < ctx.n; i++)
        Log("  %-28s %s  charset %d\n", ctx.names[i],
            (ctx.types[i] & TRUETYPE_FONTTYPE) ? "TrueType" : (ctx.types[i] & RASTER_FONTTYPE) ? "raster  " : "device  ",
            ctx.cs[i]);
}

// count distinct colours of black-on-white text rendered into a 24-bit DIB (antialiasing check)
static int CountTextColours(HFONT f, const char *s)
{
    BITMAPINFO bi;
    BYTE *bits;
    HDC dc = CreateCompatibleDC(g_hdcWin);
    HBITMAP b;
    RECT rc = { 0, 0, 400, 40 };
    DWORD seen[64];
    int ns = 0, i, j, k;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 400; bi.bmiHeader.biHeight = 40; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
    b = CreateDIBSection(g_hdcWin, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    SelectObject(dc, b);
    SelectObject(dc, f);
    SetTextColor(dc, RGB(0, 0, 0)); SetBkColor(dc, RGB(255, 255, 255));
    ExtTextOutA(dc, 2, 2, ETO_OPAQUE, &rc, s, strlen(s), NULL);
    GdiFlush();
    for (i = 0; i < 40 * 1200; i += 3) {
        DWORD v = bits[i] | (bits[i + 1] << 8) | (bits[i + 2] << 16);
        k = 0;
        for (j = 0; j < ns; j++) if (seen[j] == v) { k = 1; break; }
        if (!k && ns < 64) seen[ns++] = v;
    }
    DeleteDC(dc);
    DeleteObject(b);
    return ns;
}

static void AASection()
{
    HFONT big = CreateFont(-24, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, DEFAULT_QUALITY, 0, "Arial");
    FontDef *f;
    Log("\n== antialiasing check (distinct colours in black-on-white text, 2 = no AA) ==\n");
    if ((f = FindFont("Courier New 10pt")) != NULL) Log("  Courier New 10pt: %d\n", CountTextColours(f->h, "The quick brown fox { 0123 }"));
    if ((f = FindFont("Lucida Console 10pt")) != NULL) Log("  Lucida Console 10pt: %d\n", CountTextColours(f->h, "The quick brown fox { 0123 }"));
    if ((f = FindFont("Tahoma 8pt")) != NULL) Log("  Tahoma 8pt: %d\n", CountTextColours(f->h, "The quick brown fox { 0123 }"));
    Log("  Arial 24px: %d\n", CountTextColours(big, "The quick brown fox"));
    DeleteObject(big);
}

static void TextSection()
{
    int i, j, k, ncols;
    double al, ar;
    int ml, chars;
    Stat s;
    FontDef *f;
    g_lines = g_norm;
    TextStats(g_norm, 0, NLINES, &al, &ar, &ml);
    Log("\n== text view: full repaints (%d reps each, ms) ==\n", g_reps);
    Log("generated C text: %d lines, avg %.1f chars/line (tabs expanded to 4), max %d, avg %.2f colour runs/line\n",
        NLINES, al, ml, ar);
    Log("each row also has a 6-char line-number gutter run; one row is the highlighted current line\n");
    for (j = 0; j < (int)NFONTS; j++) {
        f = &g_fonts[j];
        if (!f->ok || f->prop) continue;
        for (k = 0; k < 512; k++) g_dx[k] = f->cw;
        ncols = (g_W - 6 * f->cw) / f->cw;
        chars = 0;
        for (k = 0; k < g_H / f->lh + 1; k++) chars += min(g_norm[40 + k].len, ncols) + 6;
        Log("\n-- %s: cell %dx%d, %d rows x %d text cols visible, ~%d chars/screen --\n",
            f->label, f->cw, f->lh, (g_H + f->lh - 1) / f->lh, ncols, chars);
        for (i = 0; i < (int)NTT; i++) {
            g_top = 40;
            g_caretLine = 47;
            Setup(V_TEXT, g_tt[i].target, g_tt[i].method, f);
            s = Measure(RP_FULL, g_reps);
            g_matrix[i][j] = s.med;
            LogStat(g_tt[i].name, s);
        }
        Log("  (glyph cache build: compat bitmap %.1f ms, soft DIB copy %.1f ms)\n", g_glyphBuildMs, g_softBuildMs);
        FlushLog();
    }
    // matrix
    Log("\n== matrix: median ms per full repaint ==\n%-44s", "method \\ font");
    for (j = 0; j < (int)NFONTS; j++) if (g_fonts[j].ok && !g_fonts[j].prop) Log(" | %-10.10s", g_fonts[j].label);
    Log("\n");
    for (i = 0; i < (int)NTT; i++) {
        Log("%-44s", g_tt[i].name);
        for (j = 0; j < (int)NFONTS; j++) if (g_fonts[j].ok && !g_fonts[j].prop) Log(" | %10.2f", g_matrix[i][j]);
        Log("\n");
    }
    FlushLog();
}

static void DenseSection()
{
    static const int ids[] = { 0, 6, 8, 10, 11 };
    const char *fl[] = { "Courier New 10pt", "Fixedsys" };
    int a, i, k;
    double al, ar;
    int ml;
    FontDef *f;
    Stat s;
    g_lines = g_dense;
    TextStats(g_dense, 0, NLINES, &al, &ar, &ml);
    Log("\n== dense text (every line ~full width): avg %.1f chars/line, %.2f runs/line ==\n", al, ar);
    for (a = 0; a < 2; a++) {
        if ((f = FindFont(fl[a])) == NULL) continue;
        for (k = 0; k < 512; k++) g_dx[k] = f->cw;
        Log("-- %s --\n", f->label);
        for (i = 0; i < (int)NELEM(ids); i++) {
            g_top = 40; g_caretLine = 47;
            Setup(V_TEXT, g_tt[ids[i]].target, g_tt[ids[i]].method, f);
            s = Measure(RP_FULL, g_reps);
            LogStat(g_tt[ids[i]].name, s);
        }
    }
    g_lines = g_norm;
    FlushLog();
}

static void ScrollSection()
{
    const char *fl[] = { "Courier New 10pt", "Fixedsys" };
    static const int ids[] = { 0, 6, 11 };
    int a, i;
    FontDef *f;
    Stat s;
    char name[100];
    g_lines = g_norm;
    Log("\n== scrolling (text view) ==\n");
    for (a = 0; a < 2; a++) {
        if ((f = FindFont(fl[a])) == NULL) continue;
        Log("-- %s --\n", f->label);
        for (i = 0; i < (int)NELEM(ids); i++) {
            TestDef *t = &g_tt[ids[i]];
            g_top = 100; g_caretLine = 107;
            Setup(V_TEXT, t->target, t->method, f);
            s = Measure(RP_FULL, g_reps);
            sprintf(name, "%s | 1 line, full repaint", t->name);
            LogStat(name, s);
            g_top = 100;
            Setup(V_TEXT, t->target, t->method, f);
            s = Measure(RP_SCROLL, g_reps);
            sprintf(name, "%s | 1 line, ScrollWindowEx+exposed", t->name);
            LogStat(name, s);
            g_top = 100;
            Setup(V_TEXT, t->target, t->method, f);
            s = Measure(RP_PAGE, g_reps);
            sprintf(name, "%s | page down (full)", t->name);
            LogStat(name, s);
        }
    }
    FlushLog();
}

static void PropSection()
{
    const char *fl[] = { "Tahoma 8pt", "Arial 10pt", "MS Sans Serif 8pt" };
    int a;
    FontDef *f;
    Stat s;
    char name[100];
    g_lines = g_norm;
    Log("\n== proportional font reference (GetTextExtentPoint32 + ETO_A opaque per run) ==\n");
    for (a = 0; a < 3; a++) {
        if ((f = FindFont(fl[a])) == NULL) continue;
        g_top = 40; g_caretLine = 47;
        Setup(V_PROP, T_DIRECT, M_A, f);
        s = Measure(RP_FULL, g_reps);
        sprintf(name, "%s (%dpx rows) direct", f->label, f->lh);
        LogStat(name, s);
        g_top = 40;
        Setup(V_PROP, T_COMPAT, M_A, f);
        s = Measure(RP_FULL, g_reps);
        sprintf(name, "%s (%dpx rows) compat+blit", f->label, f->lh);
        LogStat(name, s);
    }
    FlushLog();
}

static void HexSection()
{
    const char *fl[] = { "Courier New 10pt", "Lucida Console 10pt", "Fixedsys" };
    static const int tg[] = { T_DIRECT, T_COMPAT, T_DIB };
    static const char *tn[] = { "direct", "compat+blit", "DIB16+blit" };
    int a, i, c;
    FontDef *f;
    Stat s;
    char name[100];
    Log("\n== hex view: offset + 16 hex + 16 ASCII per row, 6-row selection, ETO_A per attribute run ==\n");
    for (a = 0; a < 3; a++) {
        if ((f = FindFont(fl[a])) == NULL) continue;
        Log("-- %s (%d rows visible) --\n", f->label, (g_H + f->lh - 1) / f->lh);
        for (c = 0; c < 2; c++) {
            g_hexColour = c;
            for (i = 0; i < 3; i++) {
                g_top = 40; g_caretLine = 42;
                Setup(V_HEX, tg[i], M_A, f);
                g_hexRuns = 0;
                s = Measure(RP_FULL, g_reps);
                sprintf(name, "%s %s", c ? "byte-coloured" : "plain", tn[i]);
                LogStat(name, s);
            }
        }
        g_hexColour = 0;
        g_top = 40; g_caretLine = 42;
        Setup(V_HEX, T_COMPAT, M_A, f);
        s = Measure(RP_SCROLL, g_reps);
        LogStat("plain compat: 1 row ScrollWindowEx+exposed", s);
        g_top = 40;
        Setup(V_HEX, T_COMPAT, M_A, f);
        s = Measure(RP_PAGE, g_reps);
        LogStat("plain compat: page down", s);
    }
    FlushLog();
}


static double TimeBlit(int kind, int w, int h, int reps)
{
    double t[64], t0;
    int i;
    struct { BITMAPINFOHEADER h; DWORD m[3]; } bi;
    memset(&bi, 0, sizeof(bi));
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = g_pitch; bi.h.biHeight = -g_H; bi.h.biPlanes = 1; bi.h.biBitCount = 16;
    bi.h.biCompression = BI_BITFIELDS;
    bi.m[0] = g_is565 ? 0xF800 : 0x7C00; bi.m[1] = g_is565 ? 0x07E0 : 0x03E0; bi.m[2] = 0x1F;
    if (reps > 64) reps = 64;
    for (i = -2; i < reps; i++) {
        Pump();
        t0 = Now();
        switch (kind) {
        case 0: BitBlt(g_hdcWin, 0, 0, w, h, g_memDC, 0, 0, SRCCOPY); break;
        case 1: BitBlt(g_hdcWin, 0, 0, w, h, g_dibDC, 0, 0, SRCCOPY); break;
        case 2: SetDIBitsToDevice(g_hdcWin, 0, 0, w, h, 0, 0, 0, g_H, g_dibBits, (BITMAPINFO *)&bi, DIB_RGB_COLORS); break;
        case 3: BitBlt(g_memDC, 0, 0, w, h, g_dibDC, 0, 0, SRCCOPY); GetPixel(g_memDC, 0, 0); break;
        case 4: BitBlt(g_hdcWin, 0, 0, w, h, g_hdcWin, 0, 16, SRCCOPY); break;
        case 5: { RECT r; r.left = 0; r.top = 0; r.right = w; r.bottom = h; FillRect(g_hdcWin, &r, g_bgBrush[1]); } break;
        case 6: { RECT r; r.left = 0; r.top = 0; r.right = w; r.bottom = h; FillRect(g_memDC, &r, g_bgBrush[1]); GetPixel(g_memDC, 0, 0); } break;
        case 7: { RECT r; r.left = 0; r.top = 0; r.right = w; r.bottom = h; GdiFlush(); FillRect(g_dibDC, &r, g_bgBrush[1]); GdiFlush(); } break;
        }
        Sync();
        if (i >= 0) t[i] = Now() - t0;
    }
    return MakeStat(t, reps).med;
}

static void PresentSection()
{
    static const int hs[] = { 16, 68, 135, 270, 540 };
    static const char *kn[] = { "BitBlt compat DDB -> window", "BitBlt DIB16 section -> window",
        "SetDIBitsToDevice DIB16 -> window", "BitBlt DIB16 -> compat DDB", "BitBlt window -> window (scroll)",
        "FillRect window", "FillRect compat DDB", "FillRect DIB16 (software)" };
    int k, i, h;
    g_view = V_NONE;
    Log("\n== present / blit path characterisation (median ms, %d px wide, by height) ==\n", g_W);
    Log("%-36s", "operation");
    for (i = 0; i < (int)NELEM(hs); i++) Log(" | h=%-5d", hs[i] > g_H ? g_H : hs[i]);
    Log("\n");
    for (k = 0; k < 8; k++) {
        Log("%-36s", kn[k]);
        for (i = 0; i < (int)NELEM(hs); i++) {
            h = hs[i] > g_H ? g_H : hs[i];
            if (k == 4 && h + 16 > g_H) h = g_H - 16;
            Log(" | %7.2f", TimeBlit(k, g_W, h, 20));
        }
        Log("\n");
    }
    Log("half width (390 x 540): DIB16 BitBlt %.2f ms, compat BitBlt %.2f ms\n", TimeBlit(1, g_W / 2, g_H, 20), TimeBlit(0, g_W / 2, g_H, 20));
    InvalidateRect(g_hwnd, NULL, FALSE);
    FlushLog();
}

static void TypingSection()
{
    static const int ids[] = { 0, 6, 11 };
    const char *fl[] = { "Courier New 10pt", "Fixedsys" };
    int a, i, k;
    FontDef *f;
    Stat s;
    double t0, t;
    char name[120];
    g_lines = g_norm;
    Log("\n== typing: insert/delete 1 char + relex line + repaint that line + caret move ==\n");
    for (a = 0; a < 2; a++) {
        if ((f = FindFont(fl[a])) == NULL) continue;
        Log("-- %s --\n", f->label);
        for (i = 0; i < (int)NELEM(ids); i++) {
            g_top = 200; g_caretLine = 207;
            Setup(V_TEXT, g_tt[ids[i]].target, g_tt[ids[i]].method, f);
            SetFocus(g_hwnd);
            CreateCaret(g_hwnd, NULL, 2, f->lh);
            SetCaretPos(6 * f->cw + g_typeCol * f->cw, 7 * f->lh);
            ShowCaret(g_hwnd);
            s = Measure(RP_TYPE, g_reps);
            sprintf(name, "keystroke, %s", g_tt[ids[i]].name);
            LogStat(name, s);
            DestroyCaret();
        }
    }
    // caret move only
    if ((f = FindFont("Courier New 10pt")) != NULL) {
        CreateCaret(g_hwnd, NULL, 2, f->lh);
        ShowCaret(g_hwnd);
        t0 = Now();
        for (k = 0; k < 1000; k++) {
            HideCaret(g_hwnd);
            SetCaretPos(100 + (k & 15) * f->cw, 7 * f->lh);
            ShowCaret(g_hwnd);
        }
        Sync();
        t = Now() - t0;
        Log("  caret move (HideCaret+SetCaretPos+ShowCaret): %.1f us each\n", t);
        DestroyCaret();
    }
    // relex cost alone
    t0 = Now();
    for (k = 0; k < 1000; k++) Lex(&g_norm[300 + (k % 100)]);
    t = Now() - t0;
    Log("  relex one line (toy C lexer, ~%d chars): %.1f us\n", 50, t);
    FlushLog();
}

static void MeasureSection()
{
    const char *fl[] = { "Courier New 10pt", "Fixedsys", "Tahoma 8pt", "Arial 10pt" };
    char text[1100];
    WCHAR wtext[1100];
    INT widths[256], fit[1100];
    int a, k, nfit;
    INT w1;
    FontDef *f;
    SIZE sz;
    double t0;
    for (k = 0; k < 1000; k++) { text[k] = (char)(32 + (k * 7) % 95); wtext[k] = (WCHAR)(unsigned char)text[k]; }
    Log("\n== text measuring (microseconds per 1000 characters unless noted) ==\n");
    for (a = 0; a < 4; a++) {
        if ((f = FindFont(fl[a])) == NULL) continue;
        SelectObject(g_hdcWin, f->h);
        Log("-- %s --\n", f->label);
        widths['M'] = -1;
        k = GetCharWidth32(g_hdcWin, 32, 255, widths + 32);
        Log("  GetCharWidth32(32..255) returns %d (err %lu), width('M') = %d\n", k, k ? 0 : GetLastError(), widths['M']);
        widths['M'] = -1;
        k = GetCharWidth(g_hdcWin, 32, 255, widths + 32);
        Log("  GetCharWidth(32..255)   returns %d, width('M') = %d\n", k, widths['M']);
        {
            ABC abc[256];
            k = GetCharABCWidths(g_hdcWin, 32, 255, abc + 32);
            Log("  GetCharABCWidths(32..255) returns %d (TrueType only)\n", k);
            t0 = Now();
            for (k = 0; k < 100; k++) GetCharABCWidths(g_hdcWin, 32, 255, abc + 32);
            Log("  GetCharABCWidths 224-char table, per call      %8.1f us\n", (Now() - t0) * 10.0);
        }
        t0 = Now();
        for (k = 0; k < 1000; k++) GetCharWidth32(g_hdcWin, 32, 255, widths + 32);
        Log("  GetCharWidth32 224-char table, per call        %8.1f us\n", (Now() - t0));
        t0 = Now();
        for (k = 0; k < 1000; k++) GetCharWidth(g_hdcWin, 32, 255, widths + 32);
        Log("  GetCharWidth 224-char table, per call          %8.1f us\n", (Now() - t0));
        t0 = Now();
        for (k = 0; k < 1000; k++) GetCharWidth32(g_hdcWin, (UINT)(unsigned char)text[k], (UINT)(unsigned char)text[k], &w1);
        Log("  GetCharWidth32 1 char x1000                    %8.1f us\n", (Now() - t0) * 1000.0);
        t0 = Now();
        for (k = 0; k < 1000; k++) GetTextExtentPoint32(g_hdcWin, text + k, 1, &sz);
        Log("  GetTextExtentPoint32 1 char x1000              %8.1f us\n", (Now() - t0) * 1000.0);
        t0 = Now();
        for (k = 0; k < 100; k++) GetTextExtentPoint32(g_hdcWin, text + k * 10, 10, &sz);
        Log("  GetTextExtentPoint32 10-char runs x100         %8.1f us\n", (Now() - t0) * 1000.0);
        t0 = Now();
        for (k = 0; k < 100; k++) GetTextExtentPoint32(g_hdcWin, text, 1000, &sz);
        Log("  GetTextExtentPoint32 one 1000-char call        %8.1f us\n", (Now() - t0) * 10.0);
        t0 = Now();
        for (k = 0; k < 100; k++) GetTextExtentPoint32W(g_hdcWin, wtext + k * 10, 10, &sz);
        Log("  GetTextExtentPoint32W 10-char runs x100        %8.1f us\n", (Now() - t0) * 1000.0);
        t0 = Now();
        for (k = 0; k < 100; k++) GetTextExtentExPoint(g_hdcWin, text, 1000, 100000, &nfit, fit, &sz);
        Log("  GetTextExtentExPoint 1000 chars + dx array     %8.1f us\n", (Now() - t0) * 10.0);
    }
    SelectObject(g_hdcWin, GetStockObject(SYSTEM_FONT));
    FlushLog();
}

// ---------------------------------------------------------------------------------------------
// file I/O and memory

#define CHUNK 65536
static char *g_ioBuf;

static int CountLF(const char *p, int n)
{
    int c = 0;
    const char *e = p + n;
    while ((p = (const char *)memchr(p, '\n', e - p)) != NULL) { c++; p++; }
    return c;
}

static int CountLFLoop(const char *p, int n)
{
    int c = 0, i;
    for (i = 0; i < n; i++) c += (p[i] == '\n');
    return c;
}

static double WriteTest(const char *path, DWORD size)
{
    HANDLE h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD done = 0, wr;
    double t0;
    if (h == INVALID_HANDLE_VALUE) return -1;
    t0 = Now();
    while (done < size) {
        if (!WriteFile(h, g_ioBuf, CHUNK, &wr, NULL) || wr != CHUNK) { CloseHandle(h); return -2; }
        done += CHUNK;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    return Now() - t0;
}

static double ReadTest(const char *path, int scan, DWORD flags, int *lines, DWORD *bytes)
{
    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | flags, NULL);
    DWORD got, total = 0;
    int nl = 0;
    double t0;
    char *buf = g_ioBuf + CHUNK;   // page aligned (VirtualAlloc base + 64 KB)
    if (h == INVALID_HANDLE_VALUE) return -1;
    t0 = Now();
    for (;;) {
        got = 0;
        if (!ReadFile(h, buf, CHUNK, &got, NULL) || got == 0) break;
        total += got;
        if (scan) nl += CountLF(buf, got);
    }
    t0 = Now() - t0;
    CloseHandle(h);
    if (lines) *lines = nl;
    if (bytes) *bytes = total;
    return t0;
}

static double MBs(double bytes, double ms) { return ms > 0 ? bytes / 1048576.0 / (ms / 1000.0) : 0; }

typedef BOOL (WINAPI *PGDFSE)(LPCSTR, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER);

static void MemStatus(const char *when)
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    Log("  GlobalMemoryStatus %s: load %d%%, phys %lu/%lu MB free, pagefile %lu/%lu MB free, virtual %lu/%lu MB free\n",
        when, ms.dwMemoryLoad, ms.dwAvailPhys >> 20, ms.dwTotalPhys >> 20, ms.dwAvailPageFile >> 20,
        ms.dwTotalPageFile >> 20, ms.dwAvailVirtual >> 20, ms.dwTotalVirtual >> 20);
}

static int TryAlloc(int kind, DWORD bytes)
{
    void *p = NULL;
    switch (kind) {
    case 0: p = VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_NOACCESS); if (p) VirtualFree(p, 0, MEM_RELEASE); break;
    case 1: p = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); if (p) VirtualFree(p, 0, MEM_RELEASE); break;
    case 2: p = HeapAlloc(GetProcessHeap(), 0, bytes); if (p) HeapFree(GetProcessHeap(), 0, p); break;
    case 3: p = malloc(bytes); if (p) free(p); break;
    }
    return p != NULL;
}

static DWORD MaxAllocMB(int kind)
{
    DWORD lo = 0, hi = 2047, mid;
    while (lo < hi) {
        mid = (lo + hi + 1) / 2;
        if (TryAlloc(kind, mid << 20)) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static void IoSection()
{
    char cdir[] = "C:\\EDPROBE";
    char mdir[MAX_PATH], path[MAX_PATH], name[160];
    const char *dirs[2];
    static const DWORD sizes[2] = { 10u << 20, 50u << 20 };
    static const char *fn[2] = { "T10.TXT", "T50.TXT" };
    int d, s, i, nl, pos, k;
    DWORD bytes, mb;
    double t, t0;
    char *big;
    HANDLE hf, hm;
    char *view;
    PGDFSE gdfse;
    ULARGE_INTEGER fa, tot, fr;

    g_ioBuf = (char *)VirtualAlloc(NULL, 2 * CHUNK, MEM_COMMIT, PAGE_READWRITE);
    // fill pattern chunk with generated CRLF text
    pos = 0; k = 0;
    while (pos < CHUNK) {
        Line *ln = &g_norm[k++ % NLINES];
        for (i = 0; i < ln->len && pos < CHUNK; i++) g_ioBuf[pos++] = ln->text[i];
        if (pos < CHUNK) g_ioBuf[pos++] = '\r';
        if (pos < CHUNK) g_ioBuf[pos++] = '\n';
    }
    sprintf(mdir, "%s\\BIG", g_exeDir);
    dirs[0] = cdir; dirs[1] = mdir;
    CreateDirectory(cdir, NULL);
    CreateDirectory(mdir, NULL);

    Log("\n== file I/O (64 KB ReadFile/WriteFile chunks; text = generated C, ~%d lines per 64 KB) ==\n", CountLF(g_ioBuf, CHUNK));
    gdfse = (PGDFSE)GetProcAddress(GetModuleHandle("KERNEL32.DLL"), "GetDiskFreeSpaceExA");
    if (gdfse) {
        if (gdfse("C:\\", &fa, &tot, &fr)) Log("  C: free %lu MB\n", (DWORD)(fa.QuadPart >> 20));
    }
    MemStatus("at start");
    FlushLog();

    for (d = 0; d < 2; d++)
        for (s = 0; s < 2; s++) {
            sprintf(path, "%s\\%s", dirs[d], fn[s]);
            t = WriteTest(path, sizes[s]);
            Log("  write %-30s %3lu MB: %8.0f ms  %6.2f MB/s\n", path, sizes[s] >> 20, t, MBs(sizes[s], t));
            Pump();
        }
    FlushLog();

    for (d = 0; d < 2; d++)
        for (s = 0; s < 2; s++) {
            sprintf(path, "%s\\%s", dirs[d], fn[s]);
            t = ReadTest(path, 0, 0, NULL, &bytes);
            Log("  read  %-30s 1st (after write)   %8.0f ms  %6.2f MB/s\n", path, t, MBs(bytes, t));
            t = ReadTest(path, 0, 0, NULL, &bytes);
            Log("  read  %-30s 2nd                 %8.0f ms  %6.2f MB/s\n", path, t, MBs(bytes, t));
            t = ReadTest(path, 1, 0, &nl, &bytes);
            Log("  read  %-30s + count lines       %8.0f ms  %6.2f MB/s  (%d lines)\n", path, t, MBs(bytes, t), nl);
            t = ReadTest(path, 0, FILE_FLAG_NO_BUFFERING, NULL, &bytes);
            if (t < 0) Log("  read  %-30s NO_BUFFERING        open failed (err %lu)\n", path, GetLastError());
            else Log("  read  %-30s NO_BUFFERING        %8.0f ms  %6.2f MB/s (%lu bytes)\n", path, t, MBs(bytes, t), bytes);
            FlushLog();
            Pump();
        }

    // whole-file loads into memory + in-memory scans
    Log("\n== load whole file into one buffer + scan in memory ==\n");
    for (s = 0; s < 2; s++) {
        sprintf(path, "%s\\%s", cdir, fn[s]);
        big = (char *)VirtualAlloc(NULL, sizes[s], MEM_COMMIT, PAGE_READWRITE);
        if (!big) { Log("  VirtualAlloc %lu MB failed\n", sizes[s] >> 20); continue; }
        hf = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        t0 = Now();
        for (bytes = 0; bytes < sizes[s];) { DWORD got = 0; if (!ReadFile(hf, big + bytes, CHUNK, &got, NULL) || !got) break; bytes += got; }
        t = Now() - t0;
        Log("  C: %2lu MB load (64 KB reads into fresh buffer, first touch): %7.0f ms  %6.2f MB/s\n", sizes[s] >> 20, t, MBs(bytes, t));
        SetFilePointer(hf, 0, NULL, FILE_BEGIN);
        t0 = Now();
        ReadFile(hf, big, sizes[s], &bytes, NULL);
        t = Now() - t0;
        Log("  C: %2lu MB single ReadFile call into same buffer:          %7.0f ms  %6.2f MB/s\n", sizes[s] >> 20, t, MBs(bytes, t));
        CloseHandle(hf);
        t0 = Now(); nl = CountLF(big, sizes[s]); t = Now() - t0;
        Log("  count lines memchr:      %7.1f ms  %7.1f MB/s  (%d lines)\n", t, MBs(sizes[s], t), nl);
        t0 = Now(); nl = CountLFLoop(big, sizes[s]); t = Now() - t0;
        Log("  count lines byte loop:   %7.1f ms  %7.1f MB/s\n", t, MBs(sizes[s], t));
        {
            DWORD *idx = (DWORD *)VirtualAlloc(NULL, (nl + 16) * 4, MEM_COMMIT, PAGE_READWRITE);
            const char *p = big, *e = big + sizes[s];
            int n = 0;
            t0 = Now();
            while ((p = (const char *)memchr(p, '\n', e - p)) != NULL) { idx[n++] = (DWORD)(p - big); p++; }
            t = Now() - t0;
            Log("  build line-start index (DWORD/line, %d KB):  %7.1f ms\n", (nl * 4) >> 10, t);
            VirtualFree(idx, 0, MEM_RELEASE);
        }
        if (s == 1) MemStatus("with 50 MB buffer");
        VirtualFree(big, 0, MEM_RELEASE);
        FlushLog();
        Pump();
    }

    // memory mapping
    Log("\n== memory-mapped files ==\n");
    for (d = 0; d < 2; d++) {
        sprintf(path, "%s\\%s", dirs[d], fn[1]);
        t0 = Now();
        hf = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        hm = hf != INVALID_HANDLE_VALUE ? CreateFileMapping(hf, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
        view = hm ? (char *)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0) : NULL;
        t = Now() - t0;
        if (!view) {
            Log("  %s: map whole 50 MB FAILED (err %lu)\n", path, GetLastError());
        } else {
            Log("  %s: open+CreateFileMapping+MapViewOfFile(50 MB): %.2f ms\n", path, t);
            t0 = Now(); nl = CountLF(view, sizes[1]); t = Now() - t0;
            Log("    scan whole view (page faults read the file): %7.0f ms  %6.2f MB/s (%d lines)\n", t, MBs(sizes[1], t), nl);
            t0 = Now(); nl = CountLF(view, sizes[1]); t = Now() - t0;
            Log("    scan again:                                  %7.0f ms  %6.2f MB/s\n", t, MBs(sizes[1], t));
            UnmapViewOfFile(view);
        }
        if (hm) {
            // sliding 1 MB views
            t0 = Now(); nl = 0;
            for (bytes = 0; bytes < sizes[1]; bytes += 1u << 20) {
                view = (char *)MapViewOfFile(hm, FILE_MAP_READ, 0, bytes, 1u << 20);
                if (!view) break;
                nl += CountLF(view, 1u << 20);
                UnmapViewOfFile(view);
            }
            t = Now() - t0;
            Log("    50 sliding 1 MB views (map, scan, unmap): %7.0f ms  %6.2f MB/s (%d lines)\n", t, MBs(bytes, t), nl);
            t0 = Now();
            for (k = 0; k < 200; k++) {
                view = (char *)MapViewOfFile(hm, FILE_MAP_READ, 0, (k % 50) << 20, 1u << 20);
                if (view) UnmapViewOfFile(view);
            }
            t = Now() - t0;
            Log("    MapViewOfFile+UnmapViewOfFile 1 MB (no touch): %.1f us each\n", t * 1000.0 / 200);
            CloseHandle(hm);
        }
        if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
        FlushLog();
        Pump();
    }
    {
        static const DWORD mbs[] = { 64, 128, 256, 512, 768, 1024, 1536 };
        Log("  pagefile-backed mappings (CreateFileMapping(INVALID_HANDLE_VALUE) + full MapViewOfFile):\n");
        for (i = 0; i < (int)NELEM(mbs); i++) {
            hm = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, mbs[i] << 20, NULL);
            if (!hm) { Log("    %4lu MB: CreateFileMapping failed (err %lu)\n", mbs[i], GetLastError()); continue; }
            view = (char *)MapViewOfFile(hm, FILE_MAP_WRITE, 0, 0, 0);
            if (view) { Log("    %4lu MB: mapped at 0x%08lX\n", mbs[i], (DWORD)view); UnmapViewOfFile(view); }
            else Log("    %4lu MB: MapViewOfFile failed (err %lu)\n", mbs[i], GetLastError());
            CloseHandle(hm);
        }
        hm = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE, 0, 1024u << 20, NULL);
        view = hm ? (char *)MapViewOfFile(hm, FILE_MAP_WRITE, 0, 0, 0) : NULL;
        Log("    1024 MB SEC_RESERVE: %s (err %lu)\n", view ? "mapped" : "failed", view ? 0 : GetLastError());
        if (view) UnmapViewOfFile(view);
        if (hm) CloseHandle(hm);
    }
    FlushLog();

    // memory
    Log("\n== memory ==\n");
    MemStatus("now");
    mb = MaxAllocMB(0); Log("  largest VirtualAlloc MEM_RESERVE:           %4lu MB\n", mb);
    mb = MaxAllocMB(1); Log("  largest VirtualAlloc MEM_COMMIT:            %4lu MB\n", mb);
    mb = MaxAllocMB(2); Log("  largest HeapAlloc(GetProcessHeap()):        %4lu MB\n", mb);
    mb = MaxAllocMB(3); Log("  largest malloc:                             %4lu MB\n", mb);
    big = (char *)VirtualAlloc(NULL, 32u << 20, MEM_COMMIT, PAGE_READWRITE);
    if (big) {
        t0 = Now(); memset(big, 0x55, 32u << 20); t = Now() - t0;
        Log("  commit + first touch 32 MB (memset): %.0f ms (%.0f MB/s)\n", t, MBs(32u << 20, t));
        t0 = Now(); memset(big, 0x33, 32u << 20); t = Now() - t0;
        Log("  memset 32 MB again: %.0f ms (%.0f MB/s)\n", t, MBs(32u << 20, t));
        t0 = Now(); memmove(big, big + 1, (32u << 20) - 1); t = Now() - t0;
        Log("  memmove 32 MB by 1 byte (gap-buffer worst case): %.0f ms\n", t);
        VirtualFree(big, 0, MEM_RELEASE);
    }
    {
        // many small allocations: e.g. one heap block per line
        void **ptrs = (void **)malloc(200000 * sizeof(void *));
        t0 = Now();
        for (i = 0; i < 200000; i++) ptrs[i] = malloc(40 + (i & 31));
        t = Now() - t0;
        Log("  200k mallocs of 40-71 bytes: %.0f ms (%.2f us each)\n", t, t * 1000.0 / 200000);
        t0 = Now();
        for (i = 0; i < 200000; i++) free(ptrs[i]);
        t = Now() - t0;
        Log("  ... and freeing them: %.0f ms\n", t);
        free(ptrs);
    }
    MemStatus("at end");
    FlushLog();

    // clean up
    for (d = 0; d < 2; d++)
        for (s = 0; s < 2; s++) {
            sprintf(path, "%s\\%s", dirs[d], fn[s]);
            DeleteFile(path);
        }
    RemoveDirectory(mdir);
    RemoveDirectory(cdir);
    sprintf(name, "  test files deleted\n");
    Log("%s", name);
    VirtualFree(g_ioBuf, 0, MEM_RELEASE);
    FlushLog();
}

// ---------------------------------------------------------------------------------------------

static void MakeHexData()
{
    int i, pos = 0, k = 0, j = 0;
    g_hexData = (unsigned char *)malloc(g_hexSize);
    g_seed = 777;
    while (pos < g_hexSize) {
        int kind = rn(4), n = 64 + rn(512);
        for (i = 0; i < n && pos < g_hexSize; i++) {
            if (kind == 0) g_hexData[pos++] = 0;
            else if (kind == 1) g_hexData[pos++] = (unsigned char)rn(256);
            else {
                Line *ln = &g_norm[k % NLINES];
                if (j < ln->len) g_hexData[pos++] = (unsigned char)ln->text[j++];
                else { g_hexData[pos++] = '\n'; j = 0; k++; }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// platform facts (-facts): API availability, charsets, file semantics, chunked I/O, byte loops

static HFONT g_csFont[3];
static int g_chunksOnly;
static const int g_csSet[3] = { ANSI_CHARSET, RUSSIAN_CHARSET, EASTEUROPE_CHARSET };
static const char *g_csName[3] = { "ANSI_CHARSET", "RUSSIAN_CHARSET", "EASTEUROPE_CHARSET" };
static const WCHAR g_wCyr[] = { 0x041F,0x0440,0x0438,0x0432,0x0435,0x0442,',',' ',0x043C,0x0438,0x0440,'!',' ',0x0416,0x0414,0x042F,0 };
static const WCHAR g_wPol[] = { 'Z','a',0x017C,0x00F3,0x0142,0x0107,' ','g',0x0119,0x015B,'l',0x0105,' ','j','a',0x017A,0x0144,' ',0x0141,0x00F3,'d',0x017A,0 };
static const char g_a1251[] = "\xCF\xF0\xE8\xE2\xE5\xF2, \xEC\xE8\xF0! \xC6\xC4\xDF";
static const char g_a1250[] = "Za\xBF\xF3\xB3\xE6 g\xEA\x9Cl\xB9 ja\x9F\xF1 \xA3\xF3\x64\x9F";

static void RenderCharsets(HDC dc)
{
    RECT rc;
    int i, y = 4;
    FontDef *ui = FindFont("Tahoma 8pt");
    char lab[100];
    rc.left = 0; rc.top = 0; rc.right = g_W; rc.bottom = g_H;
    FillRect(dc, &rc, g_bgBrush[0]);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    for (i = 0; i < 3; i++) {
        if (ui) SelectObject(dc, ui->h);
        sprintf(lab, "Courier New 10pt %s, ExtTextOutW:", g_csName[i]);
        TextOutA(dc, 4, y, lab, strlen(lab));
        SelectObject(dc, g_csFont[i]);
        ExtTextOutW(dc, 250, y, 0, NULL, g_wCyr, lstrlenW(g_wCyr), NULL);
        ExtTextOutW(dc, 250 + 18 * 8, y, 0, NULL, g_wPol, lstrlenW(g_wPol), NULL);
        y += 22;
    }
    for (i = 0; i < 3; i++) {
        if (ui) SelectObject(dc, ui->h);
        sprintf(lab, "Courier New 10pt %s, ExtTextOutA cp1251 | cp1250 bytes:", g_csName[i]);
        TextOutA(dc, 4, y, lab, strlen(lab));
        SelectObject(dc, g_csFont[i]);
        ExtTextOutA(dc, 330, y, 0, NULL, g_a1251, strlen(g_a1251), NULL);
        ExtTextOutA(dc, 330 + 18 * 8, y, 0, NULL, g_a1250, strlen(g_a1250), NULL);
        y += 22;
    }
    SetBkMode(dc, OPAQUE);
}

static DWORD GlyphHash(HFONT f, WCHAR c)
{
    static HDC dc;
    static HBITMAP b;
    static BYTE *bits;
    RECT rc = { 0, 0, 32, 24 };
    DWORD h = 2166136261u;
    int i;
    if (!dc) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 32; bi.bmiHeader.biHeight = 24; bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(g_hdcWin);
        b = CreateDIBSection(g_hdcWin, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
        SelectObject(dc, b);
    }
    SelectObject(dc, f);
    SetTextColor(dc, RGB(0, 0, 0)); SetBkColor(dc, RGB(255, 255, 255));
    ExtTextOutW(dc, 4, 2, ETO_OPAQUE, &rc, &c, 1, NULL);
    GdiFlush();
    for (i = 0; i < 32 * 24 * 3; i++) h = (h ^ bits[i]) * 16777619u;
    return h;
}

static void FactCharsets()
{
    static const WCHAR cyr[] = { 0x041F, 0x0416, 0x0414, 0x042F, 0x0436, 0x044F };
    static const WCHAR pol[] = { 0x0141, 0x0105, 0x0119, 0x015A, 0x017C, 0x0144 };
    int i, k, j, real, distinct;
    DWORD hq, hblank, hthai, hc[6];
    char face[64];
    Log("\n== 2. charsets: Courier New 10pt (-13) by lfCharSet, glyph check (6 test chars; 'real' = differs from '?', blank, Thai default) ==\n");
    for (i = 0; i < 3; i++) {
        g_csFont[i] = CreateFont(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, g_csSet[i], 0, 0, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
        SelectObject(g_hdcWin, g_csFont[i]);
        GetTextFace(g_hdcWin, sizeof(face), face);
        hq = GlyphHash(g_csFont[i], '?');
        hblank = GlyphHash(g_csFont[i], ' ');
        hthai = GlyphHash(g_csFont[i], 0x0E01);
        for (k = 0; k < 2; k++) {
            const WCHAR *set = k ? pol : cyr;
            real = 0; distinct = 0;
            for (j = 0; j < 6; j++) {
                int m, dup = 0;
                hc[j] = GlyphHash(g_csFont[i], set[j]);
                if (hc[j] != hq && hc[j] != hblank && hc[j] != hthai) real++;
                for (m = 0; m < j; m++) if (hc[m] == hc[j]) dup = 1;
                if (!dup) distinct++;
            }
            Log("  %-18s face '%s' GetTextCharset=%d: ExtTextOutW %s: %d/6 real glyphs, %d/6 distinct\n",
                g_csName[i], face, GetTextCharset(g_hdcWin), k ? "Polish  " : "Cyrillic", real, distinct);
        }
    }
    SelectObject(g_hdcWin, GetStockObject(SYSTEM_FONT));
    Log("  (window shows the strings; see shot_charsets.png)\n");
}

static void ApiLine(const char *name, FARPROC p, int ok, DWORD err, const char *detail)
{
    if (!p) { Log("  %-28s MISSING\n", name); return; }
    Log("  %-28s present, %s (err %lu) %s\n", name, ok ? "WORKS" : (err == ERROR_CALL_NOT_IMPLEMENTED ? "STUB (ERROR_CALL_NOT_IMPLEMENTED)" : "FAILS"), err, detail);
}

typedef BOOL (WINAPI *P_ABCW)(HDC, UINT, UINT, LPABC);
typedef LPWSTR (WINAPI *P_CLW)(LPWSTR);
typedef BOOL (WINAPI *P_CW32W)(HDC, UINT, UINT, LPINT);
typedef BOOL (WINAPI *P_TMW)(HDC, TEXTMETRICW *);
typedef BOOL (WINAPI *P_TEEPW)(HDC, LPCWSTR, int, int, LPINT, LPINT, LPSIZE);
typedef BOOL (WINAPI *P_GFAE)(LPCSTR, int, LPVOID);
typedef BOOL (WINAPI *P_SHSFP)(HWND, LPSTR, int, BOOL);
typedef HRESULT (WINAPI *P_SHGFP)(HWND, int, HANDLE, DWORD, LPSTR);
typedef BOOL (WINAPI *P_ITU)(const VOID *, int, LPINT);
typedef HANDLE (WINAPI *P_CFW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef int (WINAPI *P_DTW)(HDC, LPCWSTR, int, LPRECT, UINT);
typedef int (WINAPI *P_CSW)(LCID, DWORD, LPCWSTR, int, LPCWSTR, int);

static void FactApi()
{
    HMODULE k32 = GetModuleHandle("KERNEL32.DLL"), u32 = GetModuleHandle("USER32.DLL"), g32 = GetModuleHandle("GDI32.DLL");
    HMODULE sh = LoadLibrary("SHELL32.DLL"), adv = LoadLibrary("ADVAPI32.DLL"), shf, usp, uni;
    FARPROC p;
    int r, fit;
    DWORD e;
    char det[200], path[MAX_PATH];
    FontDef *cn = FindFont("Courier New 10pt");
    Log("\n== 1. API availability (GetProcAddress) and behaviour; Courier New 10pt selected ==\n");
    SelectObject(g_hdcWin, cn ? cn->h : (HFONT)GetStockObject(ANSI_FIXED_FONT));

    p = GetProcAddress(g32, "GetCharABCWidthsW");
    { ABC abc[96]; memset(abc, 0, sizeof(abc)); SetLastError(0); r = p ? ((P_ABCW)p)(g_hdcWin, 0x20, 0x7F, abc) : 0; e = GetLastError();
      sprintf(det, "A/B/C('M') = %d/%u/%d", abc['M' - 32].abcA, abc['M' - 32].abcB, abc['M' - 32].abcC); ApiLine("GetCharABCWidthsW", p, r, e, det); }
    p = GetProcAddress(g32, "GetCharABCWidthsA");
    { ABC abc[96]; memset(abc, 0, sizeof(abc)); SetLastError(0); r = p ? ((P_ABCW)p)(g_hdcWin, 0x20, 0x7F, abc) : 0; e = GetLastError();
      sprintf(det, "B('M') = %u", abc['M' - 32].abcB); ApiLine("GetCharABCWidthsA", p, r, e, det); }
    {
        WCHAR buf[8] = { 'A', 'b', 'C', 0x00C4, 0x0416, 0x0141, 0 };
        p = GetProcAddress(u32, "CharLowerW");
        SetLastError(0); r = p ? ((P_CLW)p)(buf) != NULL : 0; e = GetLastError();
        sprintf(det, "\"AbC\\xC4\\x416\\x141\" -> %04X %04X %04X %04X %04X %04X (want 0061 0062 0063 00E4 0436 0142)", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
        ApiLine("CharLowerW", p, r, e, det);
        p = GetProcAddress(u32, "CharUpperW");
        SetLastError(0); r = p ? ((P_CLW)p)(buf) != NULL : 0; e = GetLastError();
        sprintf(det, "-> %04X %04X %04X %04X %04X %04X (want 0041 0042 0043 00C4 0416 0141)", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
        ApiLine("CharUpperW", p, r, e, det);
    }
    p = GetProcAddress(g32, "GetCharWidth32W");
    { INT w[256]; memset(w, 0, sizeof(w)); SetLastError(0); r = p ? ((P_CW32W)p)(g_hdcWin, 0x20, 0xFF, w) : 0; e = GetLastError();
      sprintf(det, "width('M') = %d", w['M' - 32]); ApiLine("GetCharWidth32W", p, r, e, det); }
    p = GetProcAddress(g32, "GetCharWidthW");
    { INT w[256]; memset(w, 0, sizeof(w)); SetLastError(0); r = p ? ((P_CW32W)p)(g_hdcWin, 0x20, 0xFF, w) : 0; e = GetLastError();
      sprintf(det, "width('M') = %d", w['M' - 32]); ApiLine("GetCharWidthW", p, r, e, det); }
    p = GetProcAddress(g32, "GetTextMetricsW");
    { TEXTMETRICW tm; memset(&tm, 0, sizeof(tm)); SetLastError(0); r = p ? ((P_TMW)p)(g_hdcWin, &tm) : 0; e = GetLastError();
      sprintf(det, "tmHeight %d tmAveCharWidth %d tmLastChar %04X", tm.tmHeight, tm.tmAveCharWidth, tm.tmLastChar); ApiLine("GetTextMetricsW", p, r, e, det); }
    p = GetProcAddress(g32, "GetTextExtentExPointW");
    { INT dx[16]; SIZE sz; WCHAR s[] = { 'H','e','l','l','o',' ','w','o','r','l','d',0 }; sz.cx = sz.cy = 0; fit = -1;
      SetLastError(0); r = p ? ((P_TEEPW)p)(g_hdcWin, s, 11, 50, &fit, dx, &sz) : 0; e = GetLastError();
      sprintf(det, "\"Hello world\" max 50px: fit %d, cx %d, dx[10] %d", fit, sz.cx, dx[10]); ApiLine("GetTextExtentExPointW", p, r, e, det); }
    p = GetProcAddress(k32, "GetDiskFreeSpaceExA");
    { ULARGE_INTEGER a, t, f; SetLastError(0); r = p ? ((PGDFSE)p)("C:\\", &a, &t, &f) : 0; e = GetLastError();
      sprintf(det, "C: %lu MB free of %lu MB", r ? (DWORD)(a.QuadPart >> 20) : 0, r ? (DWORD)(t.QuadPart >> 20) : 0); ApiLine("GetDiskFreeSpaceExA", p, r, e, det); }
    p = GetProcAddress(k32, "GetFileAttributesExA");
    { WIN32_FILE_ATTRIBUTE_DATA d; char exe[MAX_PATH]; memset(&d, 0, sizeof(d)); GetModuleFileName(NULL, exe, MAX_PATH);
      SetLastError(0); r = p ? ((P_GFAE)p)(exe, 0, &d) : 0; e = GetLastError();
      sprintf(det, "edprobe.exe size %lu attr %lX", d.nFileSizeLow, d.dwFileAttributes); ApiLine("GetFileAttributesExA", p, r, e, det); }
    p = sh ? GetProcAddress(sh, "SHGetSpecialFolderPathA") : NULL;
    { path[0] = 0; SetLastError(0); r = p ? ((P_SHSFP)p)(NULL, path, 0x1A, FALSE) : 0; e = GetLastError();
      sprintf(det, "CSIDL_APPDATA = '%s'", path); ApiLine("SHGetSpecialFolderPathA", p, r, e, det);
      path[0] = 0; r = p ? ((P_SHSFP)p)(NULL, path, 0x05, FALSE) : 0;
      Log("  %-28s CSIDL_PERSONAL = '%s' (ret %d)\n", "", path, r); }
    shf = LoadLibrary("SHFOLDER.DLL");
    p = shf ? GetProcAddress(shf, "SHGetFolderPathA") : NULL;
    { HRESULT hr = E_FAIL; path[0] = 0; if (p) hr = ((P_SHGFP)p)(NULL, 0x1A, NULL, 0, path);
      sprintf(det, "shfolder.dll %s, CSIDL_APPDATA = '%s'", shf ? "loads" : "missing", path); ApiLine("SHGetFolderPathA", p, p && hr == S_OK, 0, det); }
    p = adv ? GetProcAddress(adv, "IsTextUnicode") : NULL;
    { WCHAR s[] = { 'H','e','l','l','o',' ','w','o','r','l','d',',',' ','t','h','i','s',' ','i','s',' ','U','T','F','-','1','6',0 };
      INT fl = IS_TEXT_UNICODE_UNICODE_MASK; SetLastError(0); r = p ? ((P_ITU)p)(s, sizeof(s) - 2, &fl) : 0; e = GetLastError();
      sprintf(det, "UTF-16LE English text -> %d, flags out %X (both 0 on a stub)", r, fl); ApiLine("IsTextUnicode", p, r, e, det); }
    usp = LoadLibrary("USP10.DLL");
    Log("  %-28s %s\n", "usp10.dll (Uniscribe)", usp ? "LOADS" : "not present");
    uni = LoadLibrary("UNICOWS.DLL");
    Log("  %-28s %s\n", "unicows.dll (MSLU)", uni ? "LOADS" : "not present");
    p = GetProcAddress(k32, "CreateFileW");
    { WCHAR fn[] = { 'C',':','\\','A','U','T','O','E','X','E','C','.','B','A','T',0 }; HANDLE h;
      SetLastError(0); h = p ? ((P_CFW)p)(fn, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) : INVALID_HANDLE_VALUE; e = GetLastError();
      if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
      ApiLine("CreateFileW", p, h != INVALID_HANDLE_VALUE, e, "(opening C:\\AUTOEXEC.BAT)"); }
    p = GetProcAddress(u32, "DrawTextW");
    { WCHAR s[] = { 'a','b','c',0 }; RECT rc = { 0, 0, 100, 20 }; SetLastError(0);
      r = p ? ((P_DTW)p)(g_hdcWin, s, 3, &rc, DT_CALCRECT) : 0; e = GetLastError();
      sprintf(det, "DT_CALCRECT -> %dx%d", rc.right, rc.bottom); ApiLine("DrawTextW", p, r, e, det); }
    p = GetProcAddress(k32, "CompareStringW");
    { WCHAR a[] = { 'a','b','c',0 }, b[] = { 'A','B','C',0 }; SetLastError(0);
      r = p ? ((P_CSW)p)(LOCALE_USER_DEFAULT, NORM_IGNORECASE, a, 3, b, 3) : 0; e = GetLastError();
      sprintf(det, "abc vs ABC ignorecase -> %d (2 = equal)", r); ApiLine("CompareStringW", p, r != 0, e, det); }
    {
        char u8[] = "\xC3\xA4\xD0\x96\xE2\x82\xAC";   // ä Ж €
        WCHAR w[8]; char back[16];
        int n;
        memset(w, 0, sizeof(w));
        SetLastError(0); n = MultiByteToWideChar(CP_UTF8, 0, u8, 7, w, 8); e = GetLastError();
        sprintf(det, "-> %d chars %04X %04X %04X", n, w[0], w[1], w[2]); ApiLine("MultiByteToWideChar(UTF8)", (FARPROC)1, n == 3, e, det);
        SetLastError(0); n = WideCharToMultiByte(CP_UTF8, 0, w, 3, back, 16, NULL, NULL); e = GetLastError();
        sprintf(det, "-> %d bytes, roundtrip %s", n, (n == 7 && !memcmp(back, u8, 7)) ? "ok" : "BAD"); ApiLine("WideCharToMultiByte(UTF8)", (FARPROC)1, n == 7, e, det);
        u8[0] = (char)0xC0; u8[1] = (char)0xAF;    // overlong '/'
        SetLastError(0); n = MultiByteToWideChar(CP_UTF8, 0, u8, 2, w, 8); e = GetLastError();
        Log("  %-28s overlong C0 AF -> %d chars, w[0]=%04X, err %lu (MB_ERR_INVALID_CHARS is not supported for UTF-8 on 9x)\n", "", n, w[0], e);
        SetLastError(0); n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8, 2, w, 8); e = GetLastError();
        Log("  %-28s with MB_ERR_INVALID_CHARS -> %d, err %lu\n", "", n, e);
    }
    Log("  IsValidCodePage: 1250 %d, 1251 %d, 1252 %d, 437 %d, 850 %d, 932 %d, 65001 %d; ACP %u, OEMCP %u\n",
        IsValidCodePage(1250), IsValidCodePage(1251), IsValidCodePage(1252), IsValidCodePage(437), IsValidCodePage(850),
        IsValidCodePage(932), IsValidCodePage(65001), GetACP(), GetOEMCP());
    SelectObject(g_hdcWin, GetStockObject(SYSTEM_FONT));
    FlushLog();
}

// ---- file semantics
static void WriteSmall(const char *path, int n)
{
    HANDLE h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wr;
    char buf[4096];
    if (n > 4096) n = 4096;
    memset(buf, 'x', n);
    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, buf, n, &wr, NULL); CloseHandle(h); }
}

static const char *ErrName(DWORD e)
{
    switch (e) {
    case 0: return "ok";
    case ERROR_SHARING_VIOLATION: return "ERROR_SHARING_VIOLATION";
    case ERROR_ACCESS_DENIED: return "ERROR_ACCESS_DENIED";
    case ERROR_INVALID_PARAMETER: return "ERROR_INVALID_PARAMETER";
    case ERROR_FILE_NOT_FOUND: return "ERROR_FILE_NOT_FOUND";
    case ERROR_LOCK_VIOLATION: return "ERROR_LOCK_VIOLATION";
    case ERROR_NOT_SUPPORTED: return "ERROR_NOT_SUPPORTED";
    case ERROR_CALL_NOT_IMPLEMENTED: return "ERROR_CALL_NOT_IMPLEMENTED";
    }
    return "?";
}

static const char *WaitName(DWORD w) { return w == WAIT_OBJECT_0 ? "SIGNALLED" : w == WAIT_TIMEOUT ? "no signal (3 s timeout)" : "wait failed"; }

static void FactFiles(const char *dir, const char *label)
{
    static const DWORD shares[3] = { FILE_SHARE_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | 4 };
    static const char *sn[3] = { "READ", "READ|WRITE", "READ|WRITE|DELETE(4)" };
    char a[MAX_PATH], b[MAX_PATH], c[MAX_PATH], cmd[MAX_PATH * 2];
    HANDLE h, h2, hn;
    DWORD e, w, got, attr;
    int i, r;
    char buf[64];
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    sprintf(a, "%s\\FA.TXT", dir); sprintf(b, "%s\\FB.TXT", dir); sprintf(c, "%s\\FC.TXT", dir);
    Log("\n== 3. file semantics on %s (%s) ==\n", label, dir);
    for (i = 0; i < 3; i++) {
        WriteSmall(a, 100); DeleteFile(b);
        SetLastError(0);
        h = CreateFile(a, GENERIC_READ, shares[i], NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { e = GetLastError(); Log("  open GENERIC_READ share %-20s: CreateFile FAILED err %lu %s\n", sn[i], e, ErrName(e)); continue; }
        SetLastError(0); r = MoveFile(a, b); e = GetLastError();
        Log("  MoveFile of file we hold open (share %-20s): %s err %lu %s\n", sn[i], r ? "SUCCEEDS" : "fails", r ? 0 : e, r ? "" : ErrName(e));
        CloseHandle(h);
        if (r) MoveFile(b, a);
    }
    // same-process delete while open deny-write
    WriteSmall(a, 100);
    h = CreateFile(a, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    SetLastError(0); r = DeleteFile(a); e = GetLastError();
    attr = GetFileAttributes(a);
    got = 0; ReadFile(h, buf, 10, &got, NULL);
    Log("  DeleteFile while our own handle holds it (share READ = deny write): %s err %lu %s; file %s afterwards; read on old handle got %lu bytes\n",
        r ? "SUCCEEDS" : "fails", r ? 0 : e, r ? "" : ErrName(e), attr == 0xFFFFFFFF ? "gone" : "still exists", got);
    CloseHandle(h);
    Log("    after CloseHandle: file %s\n", GetFileAttributes(a) == 0xFFFFFFFF ? "gone" : "still exists");
    // another process holds it deny-write
    WriteSmall(a, 100);
    sprintf(cmd, "\"%s\\EDPROBE.EXE\" -hold %s 6", g_exeDir, a);
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    if (CreateProcess(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        Sleep(2500);
        SetLastError(0); h2 = CreateFile(a, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL); e = GetLastError();
        Log("  other process holds it (GENERIC_READ, share READ): open for write %s err %lu %s\n", h2 != INVALID_HANDLE_VALUE ? "SUCCEEDS" : "fails", e, ErrName(e));
        if (h2 != INVALID_HANDLE_VALUE) CloseHandle(h2);
        SetLastError(0); r = MoveFile(a, b); e = GetLastError();
        Log("    MoveFile: %s err %lu %s\n", r ? "SUCCEEDS" : "fails", r ? 0 : e, r ? "" : ErrName(e));
        if (r) MoveFile(b, a);
        SetLastError(0); r = DeleteFile(a); e = GetLastError();
        Log("    DeleteFile: %s err %lu %s; file %s right after\n", r ? "SUCCEEDS" : "fails", r ? 0 : e, r ? "" : ErrName(e),
            GetFileAttributes(a) == 0xFFFFFFFF ? "gone" : "still exists");
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        Log("    after the other process exits: file %s\n", GetFileAttributes(a) == 0xFFFFFFFF ? "gone" : "still exists");
    } else {
        Log("  CreateProcess for -hold failed (err %lu)\n", GetLastError());
    }
    // change notification
    WriteSmall(a, 100);
    hn = FindFirstChangeNotification(dir, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    if (hn == INVALID_HANDLE_VALUE) {
        e = GetLastError();
        Log("  FindFirstChangeNotification(LAST_WRITE|SIZE) FAILED err %lu %s\n", e, ErrName(e));
    } else {
        w = WaitForSingleObject(hn, 300);
        Log("  FindFirstChangeNotification(LAST_WRITE|SIZE): ok; before any change: %s\n", w == WAIT_OBJECT_0 ? "already signalled" : "quiet");
        if (w == WAIT_OBJECT_0) FindNextChangeNotification(hn);
        WriteSmall(a, 200);
        w = WaitForSingleObject(hn, 3000);
        Log("    rewrite file (CREATE_ALWAYS, new size): %s\n", WaitName(w));
        if (w == WAIT_OBJECT_0) FindNextChangeNotification(hn);
        h = CreateFile(a, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        SetFilePointer(h, 0, NULL, FILE_BEGIN);
        WriteFile(h, "yyyy", 4, &got, NULL);
        CloseHandle(h);
        w = WaitForSingleObject(hn, 3000);
        Log("    overwrite 4 bytes in place (same size): %s\n", WaitName(w));
        FindCloseChangeNotification(hn);
    }
    hn = FindFirstChangeNotification(dir, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);
    if (hn != INVALID_HANDLE_VALUE) {
        WriteSmall(c, 10);
        w = WaitForSingleObject(hn, 3000);
        Log("    FILE_NAME filter, create new file: %s\n", WaitName(w));
        FindCloseChangeNotification(hn);
    }
    DeleteFile(a); DeleteFile(b); DeleteFile(c);
    FlushLog();
}

static void FactMacTouch(const char *dir)
{
    char watch[MAX_PATH], flag[MAX_PATH];
    HANDLE hn;
    WIN32_FILE_ATTRIBUTE_DATA d0, d1;
    double t0, tn = -1, tp = -1, now;
    DWORD w;
    sprintf(watch, "%s\\WATCH.TXT", dir);
    sprintf(flag, "%s\\MACWAIT.FLG", g_exeDir);
    WriteSmall(watch, 50);
    Sleep(500);
    GetFileAttributesEx(watch, GetFileExInfoStandard, &d0);
    hn = FindFirstChangeNotification(dir, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_FILE_NAME);
    Log("\n== 3b. M: change made on the Mac side (probe waits up to 90 s) ==\n");
    if (hn == INVALID_HANDLE_VALUE) { Log("  FindFirstChangeNotification failed err %lu\n", GetLastError()); return; }
    if (WaitForSingleObject(hn, 300) == WAIT_OBJECT_0) FindNextChangeNotification(hn);
    WriteSmall(flag, 1);
    t0 = Now();
    for (;;) {
        Pump();
        w = WaitForSingleObject(hn, 250);
        now = (Now() - t0) / 1000.0;
        if (w == WAIT_OBJECT_0 && tn < 0) tn = now;
        if (tp < 0 && GetFileAttributesEx(watch, GetFileExInfoStandard, &d1) &&
            (d1.nFileSizeLow != d0.nFileSizeLow || CompareFileTime(&d1.ftLastWriteTime, &d0.ftLastWriteTime) != 0)) tp = now;
        if (tn >= 0 && tp >= 0) break;
        if (tp >= 0 && now > tp + 15) break;
        if (now > 90) break;
    }
    DeleteFile(flag);
    FindCloseChangeNotification(hn);
    if (tp < 0) Log("  polling GetFileAttributesEx never saw a change (did the Mac edit happen?)\n");
    else Log("  polling GetFileAttributesEx saw the Mac edit at t=%.1f s (size %lu -> %lu)\n", tp, d0.nFileSizeLow, d1.nFileSizeLow);
    if (tn < 0) Log("  change notification: NEVER signalled (waited %.0f s after the edit was visible)\n", tp >= 0 ? 15.0 : 90.0);
    else Log("  change notification: signalled at t=%.1f s\n", tn);
    DeleteFile(watch);
    FlushLog();
}

// ---- chunked reads, random reads, many small files
static void FactChunks(const char *dir, const char *label)
{
    static const DWORD cs[4] = { 4096, 16384, 65536, 262144 };
    char path[MAX_PATH], many[MAX_PATH], fp[MAX_PATH], pat[MAX_PATH];
    char *buf = (char *)VirtualAlloc(NULL, 262144, MEM_COMMIT, PAGE_READWRITE);
    DWORD size = 8u << 20, got, total, off;
    HANDLE h;
    WIN32_FIND_DATA fd;
    double t0, t;
    int i, k, pass, n, hits;
    sprintf(path, "%s\\CHUNK.TXT", dir);
    Log("\n== 4. %s: ReadFile chunk size (8 MB file), random reads, ~200 small files ==\n", label);
    t = WriteTest(path, size);
    Log("  write 8 MB: %.0f ms (%.2f MB/s)\n", t, MBs(size, t));
    for (pass = 0; pass < 2; pass++) {
        Log("  pass %d:", pass + 1);
        for (i = 0; i < 4; i++) {
            h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            t0 = Now(); total = 0;
            while (ReadFile(h, buf, cs[i], &got, NULL) && got) total += got;
            t = Now() - t0;
            CloseHandle(h);
            Log("  %3lu KB: %6.2f MB/s", cs[i] >> 10, MBs(total, t));
        }
        Log("\n");
    }
    // fresh (uncached) 2 MB file per chunk size: write, then a single read pass
    Log("  fresh 2 MB file per chunk size, first read:");
    for (i = 0; i < 4; i++) {
        char fp2[MAX_PATH];
        sprintf(fp2, "%s\\FRESH%d.TXT", dir, i);
        WriteTest(fp2, 2u << 20);
        h = CreateFile(fp2, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        t0 = Now(); total = 0;
        while (ReadFile(h, buf, cs[i], &got, NULL) && got) total += got;
        t = Now() - t0;
        CloseHandle(h);
        DeleteFile(fp2);
        Log("  %3lu KB: %6.2f MB/s", cs[i] >> 10, MBs(total, t));
    }
    Log("\n");
    h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    g_seed = 4242;
    t0 = Now();
    for (k = 0; k < 100; k++) {
        off = (DWORD)rn(size / 4096 - 16) * 4096;
        SetFilePointer(h, off, NULL, FILE_BEGIN);
        ReadFile(h, buf, 65536, &got, NULL);
    }
    t = Now() - t0;
    CloseHandle(h);
    Log("  100 random 64 KB reads (SetFilePointer+ReadFile): %.2f ms each\n", t / 100);
    DeleteFile(path);

    sprintf(many, "%s\\MANY", dir);
    CreateDirectory(many, NULL);
    t0 = Now();
    for (k = 0; k < 200; k++) {
        sprintf(fp, "%s\\F%03d.C", many, k);
        h = CreateFile(fp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        WriteFile(h, g_ioBuf + (k * 97) % 30000, 2000 + (k * 37) % 6000, &got, NULL);
        CloseHandle(h);
    }
    t = Now() - t0;
    Log("  create 200 files of 2-8 KB: %.0f ms (%.2f ms/file)\n", t, t / 200);
    for (pass = 0; pass < 2; pass++) {
        sprintf(pat, "%s\\*.*", many);
        t0 = Now(); n = 0;
        h = FindFirstFile(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) { do n++; while (FindNextFile(h, &fd)); FindClose(h); }
        t = Now() - t0;
        Log("  pass %d: FindFirstFile/FindNextFile enumerate %d entries: %.1f ms;", pass + 1, n, t);
        t0 = Now(); hits = 0;
        h = FindFirstFile(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                HANDLE hf;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                sprintf(fp, "%s\\%s", many, fd.cFileName);
                hf = CreateFile(fp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
                if (hf == INVALID_HANDLE_VALUE) continue;
                if (ReadFile(hf, buf, 65536, &got, NULL)) hits += CountLF(buf, got);
                CloseHandle(hf);
            } while (FindNextFile(h, &fd));
            FindClose(h);
        }
        t = Now() - t0;
        Log(" find-in-files (enum + open + read + scan + close) %.0f ms = %.2f ms/file\n", t, t / 200);
    }
    t0 = Now();
    for (k = 0; k < 200; k++) {
        HANDLE hf;
        sprintf(fp, "%s\\F%03d.C", many, k);
        hf = CreateFile(fp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    }
    t = Now() - t0;
    Log("  CreateFile+CloseHandle only: %.2f ms/file\n", t / 200);
    t0 = Now();
    for (k = 0; k < 200; k++) {
        sprintf(fp, "%s\\F%03d.C", many, k);
        GetFileAttributes(fp);
    }
    t = Now() - t0;
    Log("  GetFileAttributes: %.2f ms/file\n", t / 200);
    t0 = Now();
    for (k = 0; k < 200; k++) { sprintf(fp, "%s\\F%03d.C", many, k); DeleteFile(fp); }
    t = Now() - t0;
    RemoveDirectory(many);
    Log("  delete 200 files: %.2f ms/file\n", t / 200);
    VirtualFree(buf, 0, MEM_RELEASE);
    FlushLog();
}

// ---- byte loops
static int Horspool(const unsigned char *s, int n, const unsigned char *p, int m)
{
    int skip[256], i, k, hits = 0;
    for (i = 0; i < 256; i++) skip[i] = m;
    for (i = 0; i < m - 1; i++) skip[p[i]] = m - 1 - i;
    i = 0;
    while (i <= n - m) {
        unsigned char last = s[i + m - 1];
        if (last == p[m - 1]) {
            for (k = m - 2; k >= 0 && s[i + k] == p[k]; k--) ;
            if (k < 0) hits++;
        }
        i += skip[last];
    }
    return hits;
}

static int HorspoolNoCase(const unsigned char *s, int n, const unsigned char *p, int m)
{
    static unsigned char fold[256];
    int skip[256], i, k, hits = 0;
    for (i = 0; i < 256; i++) fold[i] = (unsigned char)((i >= 'A' && i <= 'Z') ? i + 32 : i);
    for (i = 0; i < 256; i++) skip[i] = m;
    for (i = 0; i < m - 1; i++) { skip[fold[p[i]]] = m - 1 - i; skip[fold[p[i]] >= 'a' && fold[p[i]] <= 'z' ? fold[p[i]] - 32 : fold[p[i]]] = m - 1 - i; }
    i = 0;
    while (i <= n - m) {
        unsigned char last = s[i + m - 1];
        if (fold[last] == fold[p[m - 1]]) {
            for (k = m - 2; k >= 0 && fold[s[i + k]] == fold[p[k]]; k--) ;
            if (k < 0) hits++;
        }
        i += skip[last];
    }
    return hits;
}

static int NaiveSearch(const char *s, int n, const char *p, int m)
{
    const char *e = s + n - m + 1, *q = s;
    int hits = 0;
    while ((q = (const char *)memchr(q, p[0], e - q)) != NULL) {
        if (memcmp(q, p, m) == 0) hits++;
        q++;
    }
    return hits;
}

static int Utf8Valid(const unsigned char *s, int n)
{
    int i = 0, k, need;
    unsigned c;
    while (i < n) {
        c = s[i];
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c >= 0xE0 && c <= 0xEF) need = 2;
        else if (c >= 0xF0 && c <= 0xF4) need = 3;
        else return 0;
        if (i + need >= n) return 0;
        for (k = 1; k <= need; k++) if ((s[i + k] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && s[i + 1] < 0xA0) return 0;
        if (c == 0xED && s[i + 1] > 0x9F) return 0;
        if (c == 0xF0 && s[i + 1] < 0x90) return 0;
        if (c == 0xF4 && s[i + 1] > 0x8F) return 0;
        i += need + 1;
    }
    return 1;
}

static void FactLoops()
{
    int n = 1 << 20, k, pos, r = 0, rep;
    char *a = (char *)malloc(n + 16), *u = (char *)malloc(n + 16);
    WCHAR *w = (WCHAR *)malloc(n * 2 + 32);
    char *back = (char *)malloc(n * 2);
    double t0, t, best;
    const char *pat = "AllocBlockZ";
    const char *pat2 = "UpdateView";
    // ASCII text buffer and a UTF-8 buffer (every ~40 bytes a 2-byte char)
    for (pos = 0; pos < n; pos++) a[pos] = g_ioBuf[pos % CHUNK];
    for (pos = 0, k = 0; pos < n - 4; k++) {
        if (k % 40 == 39) { u[pos++] = (char)0xC3; u[pos++] = (char)0xA4; }
        else if (k % 97 == 96) { u[pos++] = (char)0xD0; u[pos++] = (char)0x96; }
        else u[pos++] = g_ioBuf[k % CHUNK];
    }
    while (pos < n) u[pos++] = ' ';
    Log("\n== 5. byte loops on a 1 MB buffer (best of 5) ==\n");
#define BEST(expr, label) best = 1e9; for (rep = 0; rep < 5; rep++) { t0 = Now(); r = (expr); t = Now() - t0; if (t < best) best = t; } \
    Log("  %-48s %7.2f ms  %7.1f MB/s  (result %d)\n", label, best, MBs(n, best), r);
    BEST(CountLFLoop(a, n), "LF count, byte loop");
    BEST(CountLF(a, n), "LF count, memchr");
    BEST(Horspool((const unsigned char *)a, n, (const unsigned char *)pat, 11), "Horspool, 11-char pattern, absent");
    BEST(Horspool((const unsigned char *)a, n, (const unsigned char *)pat2, 10), "Horspool, 10-char pattern, frequent");
    BEST(HorspoolNoCase((const unsigned char *)a, n, (const unsigned char *)pat2, 10), "Horspool case-insensitive, frequent");
    BEST(Horspool((const unsigned char *)a, n, (const unsigned char *)"if", 2), "Horspool, 2-char pattern \"if\"");
    BEST(NaiveSearch(a, n, pat, 11), "memchr first char + memcmp, absent");
    BEST(Utf8Valid((const unsigned char *)a, n), "UTF-8 validate, pure ASCII");
    BEST(Utf8Valid((const unsigned char *)u, n), "UTF-8 validate, ~3% 2-byte chars");
    BEST(MultiByteToWideChar(CP_UTF8, 0, u, n, w, n + 16), "MultiByteToWideChar(CP_UTF8) -> UTF-16");
    BEST(MultiByteToWideChar(CP_ACP, 0, a, n, w, n + 16), "MultiByteToWideChar(CP_ACP) -> UTF-16");
    MultiByteToWideChar(CP_UTF8, 0, u, n, w, n + 16);
    k = MultiByteToWideChar(CP_UTF8, 0, u, n, w, n + 16);
    BEST(WideCharToMultiByte(CP_UTF8, 0, w, k, back, n * 2, NULL, NULL), "WideCharToMultiByte(CP_UTF8)");
    BEST(WideCharToMultiByte(CP_ACP, 0, w, k, back, n * 2, NULL, NULL), "WideCharToMultiByte(CP_ACP)");
    {
        // ANSI case-fold copy (CharLowerBuffA) for case-insensitive search prep
        memcpy(back, a, n);
        BEST((int)CharLowerBuffA(back, n), "CharLowerBuffA (in place)");
    }
    free(a); free(u); free(w); free(back);
    FlushLog();
}

// ---- ExtTextOut cost by run length, font switch cost (ARCH.md section 12 items 1 and 5)
static void FactEto()
{
    static const int lens[] = { 1, 4, 8, 16, 32, 64, 128 };
    static const char *vn[] = { "W plain", "W ETO_OPAQUE", "W OPAQUE|CLIPPED", "W OPAQUE + lpDx", "A ETO_OPAQUE", "A OPAQUE + lpDx" };
    const char *fl[] = { "Courier New 10pt", "Fixedsys" };
    WCHAR wt[130];
    char at[130];
    int dx[130];
    int a, v, l, k, n;
    FontDef *f, *f2;
    RECT rc;
    double t0, t;
    for (k = 0; k < 128; k++) { at[k] = (char)('a' + k % 26); wt[k] = (WCHAR)at[k]; }
    g_view = V_NONE;
    Log("\n== 6. ExtTextOut cost per call by run length (us per call, window DC, 300 calls, synced) ==\n");
    for (a = 0; a < 2; a++) {
        if ((f = FindFont(fl[a])) == NULL) continue;
        for (k = 0; k < 130; k++) dx[k] = f->cw;
        SelectObject(g_hdcWin, f->h);
        SetBkMode(g_hdcWin, OPAQUE);
        SetTextColor(g_hdcWin, RGB(0, 0, 128)); SetBkColor(g_hdcWin, RGB(255, 255, 255));
        Log("-- %s --\n  %-18s", f->label, "variant \\ chars");
        for (l = 0; l < (int)NELEM(lens); l++) Log(" %7d", lens[l]);
        Log("\n");
        for (v = 0; v < (int)NELEM(vn); v++) {
            Log("  %-18s", vn[v]);
            for (l = 0; l < (int)NELEM(lens); l++) {
                n = lens[l];
                Pump();
                t0 = Now();
                for (k = 0; k < 300; k++) {
                    int y = (k % 30) * f->lh, x = (k % 3) * 8;
                    rc.left = x; rc.top = y; rc.right = x + n * f->cw; rc.bottom = y + f->lh;
                    switch (v) {
                    case 0: ExtTextOutW(g_hdcWin, x, y, 0, NULL, wt, n, NULL); break;
                    case 1: ExtTextOutW(g_hdcWin, x, y, ETO_OPAQUE, &rc, wt, n, NULL); break;
                    case 2: ExtTextOutW(g_hdcWin, x, y, ETO_OPAQUE | ETO_CLIPPED, &rc, wt, n, NULL); break;
                    case 3: ExtTextOutW(g_hdcWin, x, y, ETO_OPAQUE, &rc, wt, n, dx); break;
                    case 4: ExtTextOutA(g_hdcWin, x, y, ETO_OPAQUE, &rc, at, n, NULL); break;
                    case 5: ExtTextOutA(g_hdcWin, x, y, ETO_OPAQUE, &rc, at, n, dx); break;
                    }
                }
                Sync();
                t = Now() - t0;
                Log(" %7.1f", t * 1000.0 / 300);
            }
            Log("\n");
        }
    }
    f = FindFont("Courier New 10pt"); f2 = FindFont("Lucida Console 10pt");
    if (f && f2) {
        t0 = Now();
        for (k = 0; k < 1000; k++) SelectObject(g_hdcWin, (k & 1) ? f->h : f2->h);
        Log("  SelectObject font switch (Courier New <-> Lucida Console): %.2f us\n", (Now() - t0));
        t0 = Now();
        for (k = 0; k < 1000; k++) { SelectObject(g_hdcWin, (k & 1) ? f->h : f2->h); ExtTextOutA(g_hdcWin, 0, 0, ETO_OPAQUE, NULL, "abcdefgh", 8, NULL); }
        Sync();
        Log("  font switch + 8-char ExtTextOutA: %.2f us (vs no switch: ", (Now() - t0));
        t0 = Now();
        for (k = 0; k < 1000; k++) ExtTextOutA(g_hdcWin, 0, 0, ETO_OPAQUE, NULL, "abcdefgh", 8, NULL);
        Sync();
        Log("%.2f us)\n", (Now() - t0));
    }
    t0 = Now();
    for (k = 0; k < 1000; k++) SetTextColor(g_hdcWin, (k & 1) ? RGB(0, 0, 255) : RGB(0, 128, 0));
    Log("  SetTextColor: %.2f us\n", (Now() - t0));
    t0 = Now();
    for (k = 0; k < 1000; k++) SetBkColor(g_hdcWin, (k & 1) ? RGB(255, 255, 255) : RGB(232, 232, 255));
    Log("  SetBkColor: %.2f us\n", (Now() - t0));
    SelectObject(g_hdcWin, GetStockObject(SYSTEM_FONT));
    InvalidateRect(g_hwnd, NULL, FALSE);
    FlushLog();
}

static void FactsMain()
{
    char cdir[] = "C:\\EDPROBE";
    char mdir[MAX_PATH];
    int pos, k, i;
    g_ioBuf = (char *)VirtualAlloc(NULL, 2 * CHUNK, MEM_COMMIT, PAGE_READWRITE);
    pos = 0; k = 0;
    while (pos < CHUNK) {
        Line *ln = &g_norm[k++ % NLINES];
        for (i = 0; i < ln->len && pos < CHUNK; i++) g_ioBuf[pos++] = ln->text[i];
        if (pos < CHUNK) g_ioBuf[pos++] = '\r';
        if (pos < CHUNK) g_ioBuf[pos++] = '\n';
    }
    sprintf(mdir, "%s\\FT", g_exeDir);
    CreateDirectory(cdir, NULL);
    CreateDirectory(mdir, NULL);
    Log("edprobe platform facts\n");
    if (g_chunksOnly) { FactChunks(cdir, "C: local"); FactChunks(mdir, "M: Samba"); RemoveDirectory(mdir); RemoveDirectory(cdir); return; }
    FactApi();
    FactCharsets();
    g_view = V_CHARSET; g_target = T_DIRECT;
    InvalidateRect(g_hwnd, NULL, FALSE);
    UpdateWindow(g_hwnd);
    FactLoops();
    FactEto();
    g_view = V_CHARSET;
    InvalidateRect(g_hwnd, NULL, FALSE);
    UpdateWindow(g_hwnd);
    FactFiles(cdir, "C: local FAT32");
    FactFiles(mdir, "M: Samba share");
    FactMacTouch(mdir);
    FactChunks(cdir, "C: local");
    FactChunks(mdir, "M: Samba");
    RemoveDirectory(mdir);
    RemoveDirectory(cdir);
    VirtualFree(g_ioBuf, 0, MEM_RELEASE);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    WNDCLASS wc;
    RECT rc;
    MSG m;
    LARGE_INTEGER f;
    int i, doRender = 0, doIo = 0, doFacts = 0, showFont = 0;
    const char *showWhat = NULL;
    char *p;
    double tAll;
    (void)hp; (void)cmd; (void)show;

    QueryPerformanceFrequency(&f);
    g_msPerTick = 1000.0 / (double)f.QuadPart;
    g_log = (char *)malloc(64000);
    GetModuleFileName(NULL, g_exeDir, MAX_PATH);
    p = strrchr(g_exeDir, '\\');
    if (p) *p = 0;
    sprintf(g_logPath, "%s\\BENCH.TXT", g_exeDir);

    for (i = 1; i < __argc; i++) {
        if (!lstrcmpi(__argv[i], "-bench")) { doRender = doIo = 1; }
        else if (!lstrcmpi(__argv[i], "-chunks")) { doFacts = 1; g_chunksOnly = 1; sprintf(g_logPath, "%s\\CHUNKS.TXT", g_exeDir); }
        else if (!lstrcmpi(__argv[i], "-facts")) { doFacts = 1; sprintf(g_logPath, "%s\\FACTS.TXT", g_exeDir); }
        else if (!lstrcmpi(__argv[i], "-hold") && i + 2 < __argc) {
            HANDLE hh = CreateFile(__argv[i + 1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            Sleep(atoi(__argv[i + 2]) * 1000);
            if (hh != INVALID_HANDLE_VALUE) CloseHandle(hh);
            return 0;
        }
        else if (!lstrcmpi(__argv[i], "-render")) doRender = 1;
        else if (!lstrcmpi(__argv[i], "-io")) doIo = 1;
        else if (!lstrcmpi(__argv[i], "-o") && i + 1 < __argc) sprintf(g_logPath, "%s\\%s", g_exeDir, __argv[++i]);
        else if (!lstrcmpi(__argv[i], "-reps") && i + 1 < __argc) g_reps = atoi(__argv[++i]);
        else if (!lstrcmpi(__argv[i], "-show") && i + 1 < __argc) {
            showWhat = __argv[++i];
            if (i + 1 < __argc && __argv[i + 1][0] != '-') showFont = atoi(__argv[++i]);
        }
    }
    if (!doRender && !doIo && !doFacts && !showWhat) showWhat = "text";
    if (g_reps < 5) g_reps = 5;

    g_norm = GenText(0, 12345);
    g_dense = GenText(1, 999);
    g_lines = g_norm;
    MakeHexData();

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wc.lpszClassName = "EdProbe";
    RegisterClass(&wc);
    rc.left = 0; rc.top = 0; rc.right = WANT_W; rc.bottom = WANT_H;
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, WS_EX_TOPMOST);
    g_hwnd = CreateWindowEx(WS_EX_TOPMOST, "EdProbe", "edprobe", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            0, 0, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hi, NULL);
    g_hdcWin = GetDC(g_hwnd);
    GetClientRect(g_hwnd, &rc);
    g_W = rc.right; g_H = rc.bottom;
    InitFonts();
    InitSurfaces();
    g_font = &g_fonts[0];
    for (i = 0; i < (int)NFONTS; i++) if (g_fonts[i].ok) { g_font = &g_fonts[i]; break; }
    g_rowH = g_font->lh;

    if (showWhat) {
        char title[120];
        if (showFont >= 0 && showFont < (int)NFONTS && g_fonts[showFont].ok) g_font = &g_fonts[showFont];
        g_rowH = g_font->lh;
        g_target = T_COMPAT; g_method = M_A; g_top = 40; g_caretLine = 47;
        if (!lstrcmpi(showWhat, "hex")) { g_view = V_HEX; g_hexColour = 1; g_caretLine = 42; }
        else if (!lstrcmpi(showWhat, "fonts")) { g_view = V_BANDS; }
        else if (!lstrcmpi(showWhat, "prop")) { g_view = V_PROP; }
        else if (!lstrcmpi(showWhat, "soft")) { g_view = V_TEXT; g_target = T_DIB; g_method = M_GLYPHSOFT; }
        else g_view = V_TEXT;
        sprintf(title, "edprobe - %s view - %s (%dx%d cell)", showWhat, g_font->label, g_font->cw, g_font->lh);
        SetWindowText(g_hwnd, title);
        g_showMode = 1;
        ShowWindow(g_hwnd, SW_SHOWNORMAL);
        ShowWindow(g_hwnd, SW_SHOWNORMAL);
        UpdateWindow(g_hwnd);
        SetForegroundWindow(g_hwnd);
        while (GetMessage(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessage(&m); }
        return 0;
    }

    SetWindowText(g_hwnd, "edprobe - benchmark running");
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    SetForegroundWindow(g_hwnd);
    Pump();
    Sleep(500);
    Pump();

    DeleteFile(g_logPath);
    tAll = Now();
    if (doFacts) {
        SetWindowText(g_hwnd, "edprobe - platform facts");
        FactsMain();
        Log("\n== DONE in %.1f s ==\n", (Now() - tAll) / 1000.0);
        FlushLog();
        DestroyWindow(g_hwnd);
        Pump();
        return 0;
    }
    Log("edprobe benchmark (%s%s), %d repaints per test (+3 warm-up)\n", doRender ? "render " : "", doIo ? "io" : "", g_reps);
    InfoSection();
    FontListSection();
    AASection();
    FlushLog();
    if (doRender) {
        PresentSection();
        TextSection();
        DenseSection();
        ScrollSection();
        PropSection();
        HexSection();
        TypingSection();
        MeasureSection();
    }
    if (doIo) IoSection();
    Log("\n== DONE in %.1f s ==\n", (Now() - tAll) / 1000.0);
    FlushLog();
    DestroyWindow(g_hwnd);
    Pump();
    return 0;
}
