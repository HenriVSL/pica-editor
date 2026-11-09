// ini.h - tolerant INI reader/writer (DESIGN 6.19). Owner after Wave 0: T06.
#ifndef PICA_INI_H
#define PICA_INI_H

#include "etypes.h"
#include "emem.h"
#include "estr.h"

class Ini {                            // own reader/writer; tolerant; keeps order, comments and unknown lines
public:
  Ini();  ~Ini();
  int  parse(const char* text, u32 n);                 // never fails on bad lines (kept verbatim); E_NOMEM only
  const char* get(const char* sec, const char* key) const;                    // NULL if absent
  const char* get_s(const char* sec, const char* key, const char* def) const;
  i32  get_i(const char* sec, const char* key, i32 def, i32 lo, i32 hi) const; // clamped; bad -> def
  u32  get_x(const char* sec, const char* key, u32 def) const;                 // hex or decimal
  int  get_b(const char* sec, const char* key, int def) const;                 // 0/1, yes/no, true/false
  int  set(const char* sec, const char* key, const char* val);
  int  set_i(const char* sec, const char* key, i32 v);  int set_x(const char* sec, const char* key, u32 v);
  int  remove(const char* sec, const char* key);  int remove_section(const char* sec);
  u32  section_count() const;  const char* section(u32 i) const;
  u32  key_count(const char* sec) const;  const char* key_at(const char* sec, u32 i, const char** val) const;
  int  write(EStr* out) const;                          // CRLF
private:                               // private state: T06 may change freely
  Vec<char*> lines_;
  Ini(const Ini&); Ini& operator=(const Ini&);
};

#endif
