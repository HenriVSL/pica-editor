// esource.cpp - ByteSource helpers, MemSource, ViewSource (T00), SynthSource (T01).
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

// ---- SynthSource (T01) ----
// Pure function of (offset, seed, kind), so any byte of a multi-GB source is O(1) and tests can compute line counts
// by formula:
//   SYN_TEXT_CRLF / SYN_TEXT_LF: 64-byte records, each holding exactly two lines: L = 8 + h(record) % 40 text bytes,
//     EOL, then 64 - L - 2 * eollen text bytes, EOL (eollen 2 for CRLF, 1 for LF). Text bytes are a-z and spaces.
//     So every full 64 KB block holds exactly 2048 breaks.
//   SYN_BINARY: uniformly mixed bytes 00-FF.
//   SYN_LONGLINE: text bytes with an LF at every offset % 1048576 == 1048575 (lines of 1 MB - 1 bytes).
//   SYN_UTF8: 64-byte records of valid UTF-8: 31 two-byte cells (two ASCII letters, a Latin-1 letter C3 80-BF, or a
//     Cyrillic letter D0 90-BF), then a space and LF. One break per record.

static u32 syn_mix(u32 x)
{
  x ^= x >> 16; x *= 0x7FEB352Du;
  x ^= x >> 15; x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

static u8 syn_text(epos off, u32 seed)
{
  u32 h = syn_mix(off * 0x9E3779B1u ^ seed);
  if (h % 7 == 0) return ' ';
  return (u8)('a' + (h >> 8) % 26);
}

u8 SynthSource::byte_at(epos off, u32 seed, int kind)
{
  switch (kind) {
  case SYN_TEXT_CRLF:
  case SYN_TEXT_LF: {
    u32 eol = kind == SYN_TEXT_CRLF ? 2 : 1;
    u32 rec = off / 64, i = off % 64;
    u32 L = 8 + syn_mix(rec ^ (seed * 0x85EBCA6Bu)) % 40;
    if (i >= L && i < L + eol) return (u8)(eol == 2 && i == L ? 0x0D : 0x0A);
    if (i >= 64 - eol) return (u8)(eol == 2 && i == 64 - eol ? 0x0D : 0x0A);
    return syn_text(off, seed);
  }
  case SYN_BINARY:
    return (u8)(syn_mix(off ^ (seed * 0xC2B2AE35u)) >> 7);
  case SYN_LONGLINE:
    if (off % 1048576u == 1048575u) return 0x0A;
    return syn_text(off, seed);
  case SYN_UTF8: {
    u32 rec = off / 64, i = off % 64, cell, h;
    if (i == 63) return 0x0A;
    if (i == 62) return ' ';
    cell = i / 2;
    h = syn_mix((rec * 31 + cell) ^ (seed * 0x27D4EB2Fu));
    switch (h % 4) {
    case 0: return (u8)((i & 1) ? 0x80 + (h >> 8) % 64 : 0xC3);
    case 1: return (u8)((i & 1) ? 0x90 + (h >> 8) % 48 : 0xD0);
    default: return syn_text(off, seed);
    }
  }
  default:
    return 0;
  }
}

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
  epos boff, blen, i;
  if (off >= size_) { if (avail) *avail = 0; return 0; }
  boff = off & ~(epos)0xFFFF;
  if (!block_ || block_off_ != boff || block_len_ == 0) {
    if (!block_) {
      block_ = (u8*)e_alloc(65536);
      if (!block_) { if (avail) *avail = 0; return 0; }
    }
    blen = size_ - boff;
    if (blen > 65536) blen = 65536;
    for (i = 0; i < blen; i++) block_[i] = byte_at(boff + i, seed_, kind_);
    block_off_ = boff;
    block_len_ = blen;
  }
  if (avail) *avail = block_len_ - (off - block_off_);
  return block_ + (off - block_off_);
}
