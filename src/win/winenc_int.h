// winenc_int.h - PRIVATE to winenc (winenc.cpp, tests/t_winenc.cpp). Owner: T02.
#ifndef PICA_WINENC_INT_H
#define PICA_WINENC_INT_H

#include "etypes.h"
#include "codec.h"

// Builds a provider for code page cp from MultiByteToWideChar (lead bytes from IsDBCSLeadByteEx). NULL if the code
// page is not installed (IsValidCodePage) or memory runs out. Free with winenc_provider_free (only if unregistered).
DbcsProvider* winenc_provider_build(u16 cp);
void          winenc_provider_free(DbcsProvider* p);

#endif
