// detect.h - encoding and line-end detection (DESIGN 6.10). Owner after Wave 0: T02.
#ifndef PICA_DETECT_H
#define PICA_DETECT_H

#include "etypes.h"
#include "codec.h"

struct DetectOpts   { u8 utf8_nobom; u8 utf16_nobom; u8 pad[2]; EncSpec ansi; EolStyle default_eol; };
struct DetectResult { EncSpec enc; epos bom_len; u8 binary; u8 pure_ascii; u8 utf32; u8 eol_mixed;
                      EolCounts eol; EolStyle eol_style; };
int detect(const u8* p, u32 n, int whole_file, const DetectOpts& o, DetectResult* r);
int detect_eol(const u8* p, u32 n, const EncSpec& e, EolCounts* c);    // counts in code units of e
EolStyle detect_dominant_eol(const EolCounts& c, EolStyle dflt);
int detect_is_binary(const u8* p, u32 n);   // find-in-files skip: NUL in the sample and not UTF-16

#endif
