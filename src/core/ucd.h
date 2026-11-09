// ucd.h - Unicode character properties (DESIGN 6.11). Owner after Wave 0: T02.
#ifndef PICA_UCD_H
#define PICA_UCD_H

#include "etypes.h"

int uc_width(ecp c);         // 0 combining / zero-width, 2 East Asian Wide/Fullwidth, else 1 (callers handle bad markers and controls)
ecp uc_fold(ecp c);          // simple case folding, BMP + supplementary
ecp uc_upper(ecp c);  ecp uc_lower(ecp c);   // simple 1:1 mappings
int uc_is_word(ecp c);       // letter, digit, connector punctuation (_), combining mark
int uc_is_space(ecp c);
int uc_is_drawable(ecp c);   // Courier New (WGL4) can draw it on Win98: U+0020-052F, U+1E00-1EFF, U+2000-25FF, U+FB01-FB02, U+FFFD

#endif
