// ptree.h - piece tree over document bytes (DESIGN 6.5). Owner after Wave 0: T01.
#ifndef PICA_PTREE_H
#define PICA_PTREE_H

#include "etypes.h"
#include "emem.h"
#include "estr.h"
#include "esource.h"

enum { PT_PIECE_MAX = 65536, PT_ADD_CHUNK = 262144, PT_ADD_MAX_CHUNKS = 16384 };
struct BreakSpec { u8 unit; u8 b0; u8 b1; u8 pad; };  // unit 1: byte b0 (0x0A or 0x0D); unit 2: bytes b0,b1 at even offsets
struct PieceRef  { u16 src; u16 flags; u32 start; u32 len; u32 nbreaks; };  // src 0 = original, n >= 1 = add chunk n-1
struct PSpan     { const u8* p; epos len; u16 src; };   // src != 0 means bytes added since load (hex "modified")
typedef Vec<PieceRef> PieceVec;

class AddBuffer {                 // append-only; chunk table allocated once; chunks never move or shrink
public:
  AddBuffer();  ~AddBuffer();
  int  append(const u8* p, epos n, PieceVec* refs);   // splits at chunk ends and PT_PIECE_MAX; nbreaks left 0
  const u8* chunk(u16 src) const;                     // src >= 1
  u32  bytes_used() const;
private:                          // private state: T01 may change freely
  u8** chunks_; u32 nchunks_; u32 used_last_; u32 total_;
  AddBuffer(const AddBuffer&); AddBuffer& operator=(const AddBuffer&);
};

class PieceTree {                 // treap with split/merge; nodes in a pool; subtree sums of len and nbreaks
public:
  PieceTree();  ~PieceTree();
  int  init(ByteSource* original, const BreakSpec& bs, ProgressCb* cb);  // original not owned; counts breaks
  int  init_uncounted(ByteSource* original, const BreakSpec& bs);        // pieces with nbreaks 0 (indexing pending)
  int  set_counts(const u32* block_breaks, u32 nblocks);                  // per 64 KB block of the original
  int  set_break_spec(const BreakSpec& bs, ProgressCb* cb);               // recount every piece
  void clear();
  epos length() const;
  u32  break_count() const;
  const BreakSpec& break_spec() const;
  int  insert(epos pos, const u8* p, epos n, PieceVec* added);   // added: refs created (undo); may be NULL
  int  insert_refs(epos pos, const PieceRef* refs, u32 n);       // re-insert removed pieces (undo/redo)
  int  remove(epos pos, epos n, PieceVec* removed);              // removed: refs taken out; may be NULL
  int  read(epos pos, u8* out, epos n);                          // E_RANGE, E_IO
  int  span(epos pos, PSpan* s);                                 // contiguous bytes from pos to its piece end
  epos break_pos(u32 b);                                         // offset of break unit b; EPOS_NONE if none
  u32  breaks_before(epos pos);                                  // break units ending at or before pos
  int  snapshot(PieceVec* out) const;                            // piece list in order (for SnapshotSource)
  ByteSource* original() const;
  void set_original(ByteSource* s);                              // same bytes, new object (after on-demand save)
  AddBuffer* add_buffer();
  u32  piece_count() const;
  int  check(char* why, u32 cap);                                // invariants (order, heap, sums, caps); 0 = ok
  void dump(EStr* out);
private:                          // private state: T01 may change freely
  ByteSource* orig_; BreakSpec bs_; AddBuffer add_;
  PieceTree(const PieceTree&); PieceTree& operator=(const PieceTree&);
};

class SnapshotSource : public ByteSource {   // reads a frozen piece list; usable from the I/O thread (4.3)
public:
  SnapshotSource();  ~SnapshotSource();
  int  init(const PieceVec& pieces, const AddBuffer* add, ByteSource* original_private);  // copies the list
  epos size() const;  const u8* get(epos off, epos* avail);  int err() const;
private:                          // private state: T01 may change freely
  PieceVec pieces_; const AddBuffer* add_; ByteSource* orig_; epos size_; int err_;
  SnapshotSource(const SnapshotSource&); SnapshotSource& operator=(const SnapshotSource&);
};

#endif
