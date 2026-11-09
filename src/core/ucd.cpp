// ucd.cpp - MINIMAL Unicode properties from T00: ASCII/Latin-1 case mapping and word class, width 1 for everything.
// T02 replaces it with table-driven versions (ucdtables.cpp).
// Owner after Wave 0: T02.
#include "ucd.h"

int uc_width(ecp c)
{
  (void)c;
  return 1;
}

static int latin1_upper_letter(ecp c)
{
  return c >= 0xC0 && c <= 0xDE && c != 0xD7;
}

static int latin1_lower_letter(ecp c)
{
  return c >= 0xDF && c <= 0xFF && c != 0xF7;
}

ecp uc_fold(ecp c)
{
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (latin1_upper_letter(c)) return c + 32;
  return c;
}

ecp uc_upper(ecp c)
{
  if (c >= 'a' && c <= 'z') return c - 32;
  if (c == 0xFF) return 0x178;
  if (c != 0xDF && latin1_lower_letter(c)) return c - 32;
  return c;
}

ecp uc_lower(ecp c)
{
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (latin1_upper_letter(c)) return c + 32;
  if (c == 0x178) return 0xFF;
  return c;
}

int uc_is_word(ecp c)
{
  if (ECP_IS_BAD(c)) return 0;
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') return 1;
  if (c == 0xAA || c == 0xB5 || c == 0xBA) return 1;
  if (latin1_upper_letter(c) || latin1_lower_letter(c)) return 1;
  if (c >= 0x100) return uc_is_space(c) ? 0 : 1;   // minimal: treat everything above Latin-1 as letters
  return 0;
}

int uc_is_space(ecp c)
{
  if (c == ' ' || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0) return 1;
  if (c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
      c == 0x3000) return 1;
  return 0;
}

int uc_is_drawable(ecp c)
{
  return (c >= 0x20 && c <= 0x52F) || (c >= 0x1E00 && c <= 0x1EFF) || (c >= 0x2000 && c <= 0x25FF) ||
         c == 0xFB01 || c == 0xFB02 || c == 0xFFFD;
}
