// etypes.cpp - error names and assertion counting (DESIGN 6.1). Owner after Wave 0: overseer.
#include "etypes.h"
#include <stdio.h>

static u32 g_assert_count = 0;

const char* e_errname(int e)
{
  switch (e) {
  case E_OK: return "E_OK";
  case E_NOMEM: return "E_NOMEM";
  case E_RANGE: return "E_RANGE";
  case E_IO: return "E_IO";
  case E_ALIGN: return "E_ALIGN";
  case E_READONLY: return "E_READONLY";
  case E_SYNTAX: return "E_SYNTAX";
  case E_CANCEL: return "E_CANCEL";
  case E_TOOBIG: return "E_TOOBIG";
  case E_BUSY: return "E_BUSY";
  case E_NOTFOUND: return "E_NOTFOUND";
  case E_FORMAT: return "E_FORMAT";
  case E_ACCESS: return "E_ACCESS";
  case E_SHARING: return "E_SHARING";
  case E_DISKFULL: return "E_DISKFULL";
  case E_EXISTS: return "E_EXISTS";
  case E_BUDGET: return "E_BUDGET";
  case E_NOTIMPL: return "E_NOTIMPL";
  case E_STATE: return "E_STATE";
  case E_UNSUPPORTED: return "E_UNSUPPORTED";
  default: break;
  }
  return e > 0 ? "E_OK(+)" : "E_UNKNOWN";
}

void e_assert_fail(const char* file, int line, const char* expr)
{
  g_assert_count++;
  fprintf(stderr, "ASSERT %s:%d: %s\n", file ? file : "?", line, expr ? expr : "?");
}

u32 e_assert_count()
{
  return g_assert_count;
}
