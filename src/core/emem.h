// emem.h - allocation wrappers and Vec<T> (DESIGN 6.2). Owner after Wave 0: overseer.
#ifndef PICA_EMEM_H
#define PICA_EMEM_H

#include "etypes.h"
#include <string.h>

void* e_alloc(u32 n);                 // NULL on failure; e_alloc(0) returns a unique non-NULL block
void* e_realloc(void* p, u32 n);      // NULL on failure (p unchanged)
void  e_free(void* p);                // NULL ok
void* e_valloc(u32 n);                // big blocks: VirtualAlloc (Win32) / malloc (POSIX); NULL on failure
void  e_vfree(void* p, u32 n);
void  e_test_fail_after(i32 n);       // n-th following e_alloc/e_realloc/e_valloc fails; n < 0 disables
u32   e_alloc_count();                // live blocks (tests check for leaks)

// Vec<T>: POD elements only (memcpy/memmove/memset semantics); copying a Vec is forbidden.
// Growth doubles the capacity (min 8). Storage comes from e_realloc, so e_test_fail_after applies.
// All members are defined in the class body (VC6 handles in-class template members best).
template <class T> class Vec {
public:
  Vec() : d_(0), n_(0), cap_(0) {}
  ~Vec() { release(); }
  u32  size() const { return n_; }
  bool empty() const { return n_ == 0; }
  T*   data() { return d_; }
  const T* data() const { return d_; }
  T&   operator[](u32 i) { return d_[i]; }
  const T& operator[](u32 i) const { return d_[i]; }
  T&   back() { return d_[n_ - 1]; }
  int  reserve(u32 n) {               // E_OK / E_NOMEM
    T* nd;
    if (n <= cap_) return E_OK;
    if (n > 0xFFFFFFFFu / (u32)sizeof(T)) return E_NOMEM;
    nd = (T*)e_realloc(d_, n * (u32)sizeof(T));
    if (!nd) return E_NOMEM;
    d_ = nd; cap_ = n;
    return E_OK;
  }
  int  resize(u32 n) {                // new elements zero-filled
    if (n > cap_ && grow_(n) != E_OK) return E_NOMEM;
    if (n > n_) memset(d_ + n_, 0, (n - n_) * sizeof(T));
    n_ = n;
    return E_OK;
  }
  int  push(const T& v) {
    if (n_ == cap_ && grow_(n_ + 1) != E_OK) return E_NOMEM;
    memcpy(d_ + n_, &v, sizeof(T));
    n_++;
    return E_OK;
  }
  void pop() { if (n_) n_--; }
  int  insert(u32 at, const T* p, u32 n) {   // at <= size(); p must not point into this Vec
    if (at > n_) at = n_;
    if (n == 0) return E_OK;
    if (n > 0xFFFFFFFFu - n_) return E_NOMEM;
    if (n_ + n > cap_ && grow_(n_ + n) != E_OK) return E_NOMEM;
    memmove(d_ + at + n, d_ + at, (n_ - at) * sizeof(T));
    memcpy(d_ + at, p, n * sizeof(T));
    n_ += n;
    return E_OK;
  }
  void erase(u32 at, u32 n) {
    if (at >= n_) return;
    if (n > n_ - at) n = n_ - at;
    memmove(d_ + at, d_ + at + n, (n_ - at - n) * sizeof(T));
    n_ -= n;
  }
  void clear() { n_ = 0; }            // size 0, keeps capacity
  void release() { if (d_) e_free(d_); d_ = 0; n_ = 0; cap_ = 0; }   // frees storage
  void swap(Vec<T>& o) {
    T* td = d_; u32 tn = n_; u32 tc = cap_;
    d_ = o.d_; n_ = o.n_; cap_ = o.cap_;
    o.d_ = td; o.n_ = tn; o.cap_ = tc;
  }
private:
  T* d_; u32 n_; u32 cap_;
  int grow_(u32 need) {
    u32 nc = cap_ ? cap_ : 8;
    while (nc < need) {
      if (nc > 0x7FFFFFFFu) { nc = need; break; }
      nc *= 2;
    }
    return reserve(nc);
  }
  Vec(const Vec<T>&); Vec<T>& operator=(const Vec<T>&);
};
class EBytes : public Vec<u8> { public: int append(const void* p, u32 n); };

#endif
