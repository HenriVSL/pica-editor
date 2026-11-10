// t_winenc.cpp - suite "winenc" (VC6 only; links src/win/winenc.cpp). Checks on VM A: the ACP is 1252, ANSI strings
// round-trip, 932 is not installed (so no DBCS codec), the built-in SBCS tables agree with MultiByteToWideChar for
// every installed code page, a provider built from Windows tables decodes like the built-in table, input locale.
// Owner: T02.
#include <windows.h>
#include "tf.h"
#include "codec.h"
#include "codec_tables.h"
#include "winenc.h"
#include "winenc_int.h"
#include <string.h>

static void test_system(TF* tf)
{
  u16 acp = (u16)GetACP(), oem = (u16)GetOEMCP(), icp;
  tf_log(tf, "winenc: ACP %u, OEMCP %u", (u32)acp, (u32)oem);
  TF_EQ_U(acp, 1252);                          // VM A (Western Win98 SE)
  TF_EQ_I(winenc_init(), E_OK);
  TF_EQ_I(winenc_init(), E_OK);                // idempotent
  TF_CHECK(codec_get(enc_ansi()) != 0);
  TF_CHECK(codec_get(enc_oem()) != 0);
  TF_CHECK(winenc_cp_valid(1252));
  TF_CHECK(winenc_cp_valid(437));
  TF_CHECK(!winenc_cp_valid(932));             // not installed on VM A
  TF_CHECK(codec_get(enc_cp(932)) == 0);
  // consistency: an installed DBCS code page has a codec, a missing one has none
  {
    static const u16 k_dbcs[4] = { 932, 936, 949, 950 };
    u32 i;
    for (i = 0; i < 4; i++) {
      int valid = winenc_cp_valid(k_dbcs[i]);
      const Codec* c = codec_get(enc_cp(k_dbcs[i]));
      tf_log(tf, "winenc: cp%u %s", (u32)k_dbcs[i], valid ? "installed" : "not installed");
      TF_CHECK(valid ? (c != 0 && c->kind == EK_DBCS) : c == 0);
    }
  }
  icp = winenc_input_cp();
  tf_log(tf, "winenc: input code page %u", (u32)icp);
  TF_CHECK(icp != 0 && winenc_cp_valid(icp));
  winenc_input_changed();
  TF_EQ_U(winenc_input_cp(), icp);
}

static void test_ansi(TF* tf)
{
  static const char k_s[] = "Hello \xE4\xF6\xFC \x80 \x81 \x9F \xFF";
  Vec<ecp> v;
  EBytes b;
  u32 lost = 99, n = (u32)strlen(k_s);
  ecp zhe[3];
  TF_EQ_I(winenc_ansi_to_ecp(k_s, n, &v), E_OK);
  TF_EQ_U(v.size(), n);
  if (v.size() == n) {
    TF_EQ_U(v[6], 0xE4);
    TF_EQ_U(v[10], 0x20AC);
    TF_EQ_U(v[12], ECP_BADBYTE(0x81));         // undefined in 1252: lossless marker
    TF_EQ_U(v[14], 0x178);
    TF_EQ_U(v[16], 0xFF);
  }
  TF_EQ_I(winenc_ecp_to_ansi(v.data(), v.size(), &b, &lost), E_OK);
  TF_EQ_U(lost, 0);
  TF_EQ_U(b.size(), n);
  if (b.size() == n) TF_EQ_MEM(b.data(), k_s, n);
  // unrepresentable characters become '?'
  zhe[0] = 'a'; zhe[1] = 0x416; zhe[2] = ECP_BADUNIT(0xD800);
  b.clear();
  TF_EQ_I(winenc_ecp_to_ansi(zhe, 3, &b, &lost), E_OK);
  TF_EQ_U(lost, 2);
  TF_EQ_U(b.size(), 3);
  TF_EQ_MEM(b.data(), "a??", 3);
  // explicit code pages
  v.clear();
  TF_EQ_I(winenc_mb_to_ecp(1251, "\xC6\xE0", 2, &v), E_OK);
  TF_CHECK(v.size() == 2 && v[0] == 0x416 && v[1] == 0x430);
  b.clear();
  TF_EQ_I(winenc_ecp_to_mb(1251, zhe + 1, 1, &b, &lost), E_OK);
  TF_CHECK(lost == 0 && b.size() == 1 && b[0] == 0xC6);
  v.clear();
  TF_EQ_I(winenc_mb_to_ecp(65001, "\xE2\x82\xAC\xC0", 4, &v), E_OK);
  TF_CHECK(v.size() == 2 && v[0] == 0x20AC && v[1] == ECP_BADBYTE(0xC0));
  v.clear();
  TF_EQ_I(winenc_mb_to_ecp(1252, "", 0, &v), E_OK);
  TF_EQ_U(v.size(), 0);
  // a code page without a built-in table goes through MultiByteToWideChar (Mac Roman, if installed)
  if (winenc_cp_valid(10000)) {
    v.clear();
    TF_EQ_I(winenc_mb_to_ecp(10000, "a\x8A", 2, &v), E_OK);
    TF_CHECK(v.size() == 2 && v[0] == 'a' && v[1] == 0xE4);
    b.clear();
    TF_EQ_I(winenc_ecp_to_mb(10000, zhe, 2, &b, &lost), E_OK);
    TF_CHECK(lost == 1 && b.size() == 2 && b[0] == 'a' && b[1] == '?');
    b.clear();
    TF_EQ_I(winenc_ecp_to_mb(10000, v.data(), v.size(), &b, &lost), E_OK);
    TF_CHECK(lost == 0 && b.size() == 2 && b[1] == 0x8A);
  } else tf_log(tf, "winenc: cp10000 not installed, MultiByteToWideChar path skipped");
  TF_EQ_I(winenc_mb_to_ecp(59999, "a", 1, &v), E_UNSUPPORTED);   // no such code page
  v.release();
  b.release();
}

// The Python-generated tables against the real Win98 tables. Bytes Python leaves undefined are skipped (Windows maps
// some of them to C1 controls or best-fit characters). On VM A 20 of the 38 tables are installed; the only
// differences are the five ISO-8859-7 revision bytes skipped below.
static void test_tables_vs_windows(TF* tf)
{
  u32 t, b, checked = 0, fails = 0, skipped = 0;
  for (t = 0; t < g_sbcs_table_count; t++) {
    u16 cp = g_sbcs_tables[t].cp;
    const Codec* c = codec_get(enc_cp(cp));
    if (!IsValidCodePage(cp)) { skipped++; continue; }
    for (b = 0x80; b < 256; b++) {
      char s[1];
      WCHAR w[4];
      u8 byte = (u8)b;
      ecp v;
      if (!g_sbcs_tables[t].hi[b - 0x80]) continue;
      // ISO-8859-7: Python follows the 2003 revision (A1/A2 quotes, A4 euro, A5 drachma, AA ypogegrammeni), Win98
      // the 1987 one (modifier letters / private use)
      if (cp == 28597 && (b == 0xA1 || b == 0xA2 || b == 0xA4 || b == 0xA5 || b == 0xAA)) continue;
      s[0] = (char)b;
      if (MultiByteToWideChar(cp, 0, s, 1, w, 4) != 1) { fails++; continue; }
      c->decode(&byte, 1, 1, &v);
      if (v != w[0]) {
        fails++;
        if (fails <= 10) tf_log(tf, "winenc: cp%u byte %02X: table U+%04X, Windows U+%04X", (u32)cp, b, v, (u32)w[0]);
      }
      checked++;
    }
  }
  tf_log(tf, "winenc: %u table bytes checked against Windows, %u code pages not installed, %u differ", checked,
         skipped, fails);
  TF_CHECK(checked > 1000);
  TF_EQ_U(fails, 0);
}

static void test_provider(TF* tf)
{
  static const u16 k_cps[2] = { 1252, 1251 };
  u32 i, b, fails = 0;
  for (i = 0; i < 2; i++) {
    DbcsProvider* p = winenc_provider_build(k_cps[i]);
    const Codec* c = codec_get(enc_cp(k_cps[i]));
    TF_CHECK(p != 0);
    if (!p) continue;
    for (b = 0; b < 256; b++) {
      u8 byte = (u8)b, out[4];
      ecp mine, theirs;
      if (p->is_lead(byte)) fails++;
      c->decode(&byte, 1, 1, &mine);
      theirs = p->decode_single(byte);
      if (!ECP_IS_BAD(mine) && mine != theirs) fails++;
      if (!ECP_IS_BAD(theirs) && (p->encode(theirs, out) != 1 || out[0] != byte)) fails++;
    }
    {
      u8 out[4];
      TF_EQ_U(p->encode(0x1F600, out), 0);
    }
    winenc_provider_free(p);
  }
  TF_EQ_U(fails, 0);
  TF_CHECK(winenc_provider_build(932) == 0);   // not installed
  TF_CHECK(winenc_provider_build(59999) == 0);
}

void suite_winenc(TF* tf)
{
  enc_set_system((u16)GetACP(), (u16)GetOEMCP());
  codec_init();
  test_system(tf);
  test_ansi(tf);
  test_tables_vs_windows(tf);
  test_provider(tf);
}
