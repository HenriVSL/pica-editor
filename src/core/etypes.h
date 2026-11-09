// etypes.h - basic types, error codes, assertions (DESIGN 6.1). Owner after Wave 0: overseer.
#ifndef PICA_ETYPES_H
#define PICA_ETYPES_H

typedef unsigned char  u8;   typedef unsigned short u16;
typedef unsigned int   u32;  typedef signed int     i32;
#if defined(_MSC_VER)
typedef __int64 i64;  typedef unsigned __int64 u64;
#else
__extension__ typedef long long i64;  __extension__ typedef unsigned long long u64;
#endif
typedef u32 epos;   // byte offset in a document
typedef u32 ecp;    // decoded character or bad marker
#define EPOS_NONE 0xFFFFFFFFu
#define EPOS_MAX  0xFFFFFFFEu
#define ECP_BADBYTE(b) (0x110000u + (u32)(b))
#define ECP_BADUNIT(u) (0x120000u + (u32)(u))
#define ECP_IS_BAD(c)  ((c) >= 0x110000u)
#define ECP_REPLACEMENT 0xFFFDu
// T00: winerror.h (windows.h) defines E_NOTIMPL as an HRESULT macro. Win sources include <windows.h> BEFORE any core
// header (tools/lint.py checks it), so this #undef wins and E_NOTIMPL is always the enum value below.
#ifdef E_NOTIMPL
#undef E_NOTIMPL
#endif
enum EErr {
  E_OK = 0, E_NOMEM = -1, E_RANGE = -2, E_IO = -3, E_ALIGN = -4, E_READONLY = -5, E_SYNTAX = -6,
  E_CANCEL = -7, E_TOOBIG = -8, E_BUSY = -9, E_NOTFOUND = -10, E_FORMAT = -11, E_ACCESS = -12,
  E_SHARING = -13, E_DISKFULL = -14, E_EXISTS = -15, E_BUDGET = -16, E_NOTIMPL = -17, E_STATE = -18,
  E_UNSUPPORTED = -19
};
const char* e_errname(int e);                 // "E_NOMEM" etc.
template <class T> inline T emin(T a, T b) { return a < b ? a : b; }
template <class T> inline T emax(T a, T b) { return a > b ? a : b; }
void e_assert_fail(const char* file, int line, const char* expr);   // E_DEBUG builds: print + count
#ifdef E_DEBUG
#define EASSERT(x) do { if (!(x)) e_assert_fail(__FILE__, __LINE__, #x); } while (0)
#else
#define EASSERT(x) do { } while (0)
#endif
u32 e_assert_count();
struct MarkRange { epos pos; epos len; };    // search hits / Mark All ranges (used by search.h and marks.h)
class ProgressCb {                            // long operations report here; return nonzero to cancel
public:
  virtual ~ProgressCb() {}
  virtual int progress(u32 done, u32 total) = 0;   // units chosen by the caller (bytes)
};

#endif
