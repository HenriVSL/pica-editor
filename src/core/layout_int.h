// layout_int.h - private test hooks of layout.cpp (T06). Not for production code.
#ifndef PICA_LAYOUT_INT_H
#define PICA_LAYOUT_INT_H

#include "etypes.h"

typedef int (*LayPropFn)(ecp c);
// Replace the character property functions used by LineLayout/LongLineIndex (default uc_width and
// uc_is_drawable). NULL restores the default. Tests only: not thread-safe.
void lay_set_props(LayPropFn width, LayPropFn drawable);

#endif
