// esource.cpp - ByteSource helpers, MemSource, ViewSource (T00, working), SynthSource (stub until T01).
// Owner after Wave 0: T01.
#include "esource.h"
#include "emem.h"
#include <string.h>

int ByteSource::read(epos off, u8* out, epos n)
{
  epos sz = size();
  if (off > sz || n > sz - off) return E_RANGE;
  while (n > 0) {
    epos avail = 0;
    const u8* p = get(off, &avail);
    if (!p || avail == 0) return E_IO;
    if (avail > n) avail = n;
    memcpy(out, p, avail);
    out += avail; off += avail; n -= avail;
  }
  return E_OK;
}

// ---- MemSource ----

MemSource::MemSource() : p_(0), n_(0), valloc_(0) {}

MemSource::~MemSource()
{
  free_();
}

void MemSource::free_()
{
  if (p_) {
    if (valloc_) e_vfree(p_, n_);
    else e_free(p_);
  }
  p_ = 0; n_ = 0; valloc_ = 0;
}

int MemSource::adopt(u8* data, epos n, int is_valloc)
{
  free_();
  p_ = data; n_ = data ? n : 0; valloc_ = is_valloc ? 1 : 0;
  return E_OK;
}

int MemSource::copy_from(const u8* data, epos n)
{
  u8* p = (u8*)e_alloc(n);
  if (!p) return E_NOMEM;
  if (n) memcpy(p, data, n);
  free_();
  p_ = p; n_ = n; valloc_ = 0;
  return E_OK;
}

epos MemSource::size() const
{
  return n_;
}

const u8* MemSource::get(epos off, epos* avail)
{
  if (off >= n_) { if (avail) *avail = 0; return 0; }
  if (avail) *avail = n_ - off;
  return p_ + off;
}

const u8* MemSource::data() const
{
  return p_;
}

// ---- ViewSource ----

ViewSource::ViewSource(const u8* p, epos n) : p_(p), n_(p ? n : 0) {}

epos ViewSource::size() const
{
  return n_;
}

const u8* ViewSource::get(epos off, epos* avail)
{
  if (off >= n_) { if (avail) *avail = 0; return 0; }
  if (avail) *avail = n_ - off;
  return p_ + off;
}

// ---- SynthSource: stub (T01 implements the generator) ----

SynthSource::SynthSource(epos size, u32 seed, int kind)
  : size_(size), seed_(seed), kind_(kind), block_(0), block_off_(0), block_len_(0) {}

SynthSource::~SynthSource()
{
  if (block_) e_free(block_);
}

epos SynthSource::size() const
{
  return size_;
}

const u8* SynthSource::get(epos off, epos* avail)
{
  (void)off;
  if (avail) *avail = 0;
  return 0;                                  // E_NOTIMPL until T01
}

u8 SynthSource::byte_at(epos off, u32 seed, int kind)
{
  (void)off; (void)seed; (void)kind;
  return 0;
}
