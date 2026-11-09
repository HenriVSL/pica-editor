// codec.h - encodings and codecs (DESIGN 6.9). Owner after Wave 0: T02.
// T00 deviation: EolStyle and EolCounts (DESIGN 6.7) are defined here so detect.h and fileops.h need not include
// doc.h; doc.h includes this header, so every user still sees them.
#ifndef PICA_CODEC_H
#define PICA_CODEC_H

#include "etypes.h"
#include "emem.h"
#include "esource.h"

struct BreakSpec;                                 // ptree.h

enum EolStyle { EOL_CRLF = 0, EOL_LF = 1, EOL_CR = 2 };
struct EolCounts { u32 crlf; u32 lf; u32 cr; };   // LF mode: CRLF pairs, bare LFs, lone CRs. CR mode: cr = breaks

enum EncKind { EK_SBCS = 0, EK_DBCS = 1, EK_UTF8 = 2, EK_UTF16LE = 3, EK_UTF16BE = 4 };
struct EncSpec { u8 kind; u8 bom; u16 cp; };   // cp: 1252, 437, 1251, 932, 65001 (UTF-8), 1200 (LE), 1201 (BE)
bool    enc_equal(const EncSpec& a, const EncSpec& b);
void    enc_set_system(u16 acp, u16 oemcp);    // once at startup (tests: 1252, 437)
EncSpec enc_ansi();  EncSpec enc_oem();
EncSpec enc_utf8(int bom);  EncSpec enc_utf16le(int bom);  EncSpec enc_utf16be(int bom);
EncSpec enc_cp(u16 cp);                        // SBCS or DBCS by code page
const char* enc_label(const EncSpec& e, char* buf, u32 cap);  // status bar: "ANSI", "OEM", "UTF-8", "UTF-8 BOM", "UTF-16 LE", "Windows-1251"
int     enc_parse(const char* token, EncSpec* out);           // ini tokens: ansi, oem, utf8, utf8bom, utf16le, utf16be, cp1251
void    enc_token(const EncSpec& e, char* buf, u32 cap);
u32     enc_bom(const EncSpec& e, u8* out);    // writes the BOM bytes (0, 2 or 3), returns length

class Codec {
public:
  virtual ~Codec() {}
  u8 kind; u8 unit; u8 max_len; u8 ascii_compat; u8 self_sync; u8 pad; u16 cp;
  // Decode one character at p. avail = bytes available at p; eof = nothing follows p + avail.
  // Returns bytes consumed (>= 1) and sets *cp (a bad marker for invalid input). Returns 0 only when the
  // sequence is incomplete, avail is short and !eof (caller supplies more bytes).
  virtual u32 decode(const u8* p, u32 avail, int eof, ecp* cp) const = 0;
  // Encode into out[0..3]. Returns length, or 0 if the character is not representable.
  // Bad markers always encode back to their original byte (BADBYTE) or unit (BADUNIT, 2 bytes).
  virtual u32 encode(ecp cp, u8* out) const = 0;
};
int          codec_init();                       // build reverse tables; UI thread, before any worker
const Codec* codec_get(const EncSpec& e);        // NULL if unsupported (e.g. DBCS without a provider)
int          codec_sbcs_supported(u16 cp);
void         codec_break_spec(const EncSpec& e, int cr_mode, BreakSpec* bs);
class DbcsProvider {                             // implemented by winenc.cpp (MultiByteToWideChar tables)
public:
  virtual ~DbcsProvider() {}
  virtual int is_lead(u8 b) const = 0;
  virtual ecp decode_pair(u8 lead, u8 trail) const = 0;   // bad marker if invalid
  virtual ecp decode_single(u8 b) const = 0;
  virtual u32 encode(ecp c, u8* out) const = 0;            // 0 if not representable
};
int codec_register_dbcs(u16 cp, DbcsProvider* p);         // provider not owned; must outlive all codecs
// helpers
u32  codec_decode_buf(const Codec* c, const u8* p, u32 n, int eof, ecp* out, u32 cap, u32* bytes_used);
int  codec_encode_str(const Codec* c, const ecp* s, u32 n, EBytes* out, u32* unrepresentable, ecp subst);  // subst 0 = stop
epos codec_prev(ByteSource* s, const Codec* c, epos pos, epos floor);   // char start before pos; floor = known boundary
class CharReader {                               // forward decoding across ByteSource chunks
public:
  void init(ByteSource* s, const Codec* c, epos pos, epos end);
  int  next(ecp* cp, epos* at);                  // 1 char, 0 end, < 0 error
  epos pos() const;
private:                                         // private state: T02 may change freely
  ByteSource* s_; const Codec* c_; epos pos_; epos end_;
};

#endif
