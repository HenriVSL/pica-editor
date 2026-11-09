// hexfmt.cpp - hex formatting, go-to parsers, data inspector (DESIGN 6.17). STUB from T00 (offset/byte text work).
// Owner after Wave 0: T06.
#include "hexfmt.h"
#include "estr.h"

static const char k_hex[] = "0123456789ABCDEF";

void hex_offset_text(epos off, char out[9])
{
  int i;
  for (i = 7; i >= 0; i--) { out[i] = k_hex[off & 15]; off >>= 4; }
  out[8] = 0;
}
void hex_byte_text(u8 b, char out[2]) { out[0] = k_hex[b >> 4]; out[1] = k_hex[b & 15]; }
u16 hex_char_glyph(u8 b, const Codec* sbcs) { (void)sbcs; return (u16)((b >= 0x20 && b < 0x7F) ? b : '.'); }
int hex_parse_bytes(const char* text, u32 n, EBytes* out) { (void)text; (void)n; (void)out; return E_NOTIMPL; }
int hex_parse_pattern(const ecp* text, u32 n, EBytes* bytes, EBytes* mask) { (void)text; (void)n; (void)bytes; (void)mask; return E_NOTIMPL; }
int goto_parse_offset(const char* expr, epos cur, epos size, epos* out, char* err, u32 errcap)
{ (void)expr; (void)cur; (void)size; (void)out; es_copy(err, errcap, "Not implemented yet"); return E_NOTIMPL; }
int goto_parse_line(const char* expr, u32 cur_line, u32 line_count, u32* line, u32* col, char* err, u32 errcap)
{ (void)expr; (void)cur_line; (void)line_count; (void)line; (void)col; es_copy(err, errcap, "Not implemented yet"); return E_NOTIMPL; }
int inspect_bytes(const u8* p, u32 avail, int big_endian, InspectOut* out) { (void)p; (void)avail; (void)big_endian; (void)out; return E_NOTIMPL; }
