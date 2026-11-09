// ini.cpp - tolerant INI reader/writer (DESIGN 6.19). STUB from T00: parse/set fail with E_NOTIMPL, getters return
// the defaults. Owner after Wave 0: T06.
#include "ini.h"

Ini::Ini() {}
Ini::~Ini() {}
int Ini::parse(const char* text, u32 n) { (void)text; (void)n; return E_NOTIMPL; }
const char* Ini::get(const char* sec, const char* key) const { (void)sec; (void)key; return 0; }
const char* Ini::get_s(const char* sec, const char* key, const char* def) const { (void)sec; (void)key; return def; }
i32 Ini::get_i(const char* sec, const char* key, i32 def, i32 lo, i32 hi) const { (void)sec; (void)key; (void)lo; (void)hi; return def; }
u32 Ini::get_x(const char* sec, const char* key, u32 def) const { (void)sec; (void)key; return def; }
int Ini::get_b(const char* sec, const char* key, int def) const { (void)sec; (void)key; return def; }
int Ini::set(const char* sec, const char* key, const char* val) { (void)sec; (void)key; (void)val; return E_NOTIMPL; }
int Ini::set_i(const char* sec, const char* key, i32 v) { (void)sec; (void)key; (void)v; return E_NOTIMPL; }
int Ini::set_x(const char* sec, const char* key, u32 v) { (void)sec; (void)key; (void)v; return E_NOTIMPL; }
int Ini::remove(const char* sec, const char* key) { (void)sec; (void)key; return E_NOTIMPL; }
int Ini::remove_section(const char* sec) { (void)sec; return E_NOTIMPL; }
u32 Ini::section_count() const { return 0; }
const char* Ini::section(u32 i) const { (void)i; return 0; }
u32 Ini::key_count(const char* sec) const { (void)sec; return 0; }
const char* Ini::key_at(const char* sec, u32 i, const char** val) const { (void)sec; (void)i; if (val) *val = 0; return 0; }
int Ini::write(EStr* out) const { (void)out; return E_NOTIMPL; }
