// esource.h - byte sources (DESIGN 6.4). Owner after Wave 0: T01 (SynthSource), MemSource/ViewSource done in T00.
#ifndef PICA_ESOURCE_H
#define PICA_ESOURCE_H

#include "etypes.h"

class ByteSource {
public:
  virtual ~ByteSource() {}
  virtual epos size() const = 0;
  // Pointer to bytes [off, off + *avail), *avail >= 1. Valid until the next get() on this object or until the
  // underlying data changes. Returns NULL with *avail = 0 if off >= size() or on I/O error (err() != E_OK).
  virtual const u8* get(epos off, epos* avail) = 0;
  virtual int err() const { return E_OK; }
  int read(epos off, u8* out, epos n);          // helper over get(); E_RANGE, E_IO
};
class MemSource : public ByteSource {           // owns its block; immutable after adopt/copy (thread-shareable)
public:
  MemSource();  ~MemSource();
  int  adopt(u8* data, epos n, int is_valloc);  // takes ownership (e_free or e_vfree)
  int  copy_from(const u8* data, epos n);
  epos size() const;  const u8* get(epos off, epos* avail);  const u8* data() const;
private:
  u8* p_; epos n_; int valloc_;
  void free_();
  MemSource(const MemSource&); MemSource& operator=(const MemSource&);
};
class ViewSource : public ByteSource {          // non-owning view of memory
public:
  ViewSource(const u8* p, epos n);
  epos size() const;  const u8* get(epos off, epos* avail);
private:
  const u8* p_; epos n_;
};
enum SynthKind { SYN_TEXT_CRLF = 0, SYN_TEXT_LF = 1, SYN_BINARY = 2, SYN_LONGLINE = 3, SYN_UTF8 = 4 };
class SynthSource : public ByteSource {         // deterministic pseudo content of any size (tests, bench)
public:
  SynthSource(epos size, u32 seed, int kind);
  ~SynthSource();
  epos size() const;  const u8* get(epos off, epos* avail);   // generated into an internal 64 KB block
  static u8 byte_at(epos off, u32 seed, int kind);            // reference for tests
private:                                                      // private state: T01 may change freely
  epos size_; u32 seed_; int kind_; u8* block_; epos block_off_; epos block_len_;
  SynthSource(const SynthSource&); SynthSource& operator=(const SynthSource&);
};

#endif
