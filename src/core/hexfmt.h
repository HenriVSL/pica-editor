// hexfmt.h - hex view formatting, parsers, data inspector (DESIGN 6.17). Owner after Wave 0: T06.
#ifndef PICA_HEXFMT_H
#define PICA_HEXFMT_H

#include "etypes.h"
#include "emem.h"
#include "codec.h"

enum { HEX_BPR = 16 };
void hex_offset_text(epos off, char out[9]);            // "00001F40"
void hex_byte_text(u8 b, char out[2]);                  // "4D"
u16  hex_char_glyph(u8 b, const Codec* sbcs);           // char column glyph; '.' for controls/non-printables; sbcs may be NULL (ASCII only)
int  hex_parse_bytes(const char* text, u32 n, EBytes* out);                  // "4D 5A", "4d5a", "0x4D,0x5A", newlines ok
int  hex_parse_pattern(const ecp* text, u32 n, EBytes* bytes, EBytes* mask); // "DE AD ?? EF"
int  goto_parse_offset(const char* expr, epos cur, epos size, epos* out, char* err, u32 errcap);
int  goto_parse_line(const char* expr, u32 cur_line, u32 line_count, u32* line, u32* col, char* err, u32 errcap);
struct InspectOut { char i8[8]; char u8_[8]; char i16[12]; char u16_[12]; char i32_[16]; char u32_[16];
                    char i64[24]; char u64_[24]; char f32[40]; char f64[48]; char bin[12]; u8 avail; };
int  inspect_bytes(const u8* p, u32 avail, int big_endian, InspectOut* out);  // fields "-" when too few bytes

#endif
