// ptree.cpp - piece tree, AddBuffer, SnapshotSource (DESIGN 6.5). STUB from T00: every operation returns E_NOTIMPL
// or an empty value. Owner after Wave 0: T01.
#include "ptree.h"

// ---- AddBuffer ----
AddBuffer::AddBuffer() : chunks_(0), nchunks_(0), used_last_(0), total_(0) {}
AddBuffer::~AddBuffer() {}
int AddBuffer::append(const u8* p, epos n, PieceVec* refs) { (void)p; (void)n; (void)refs; return E_NOTIMPL; }
const u8* AddBuffer::chunk(u16 src) const { (void)src; return 0; }
u32 AddBuffer::bytes_used() const { return total_; }

// ---- PieceTree ----
PieceTree::PieceTree() : orig_(0) { bs_.unit = 1; bs_.b0 = 0x0A; bs_.b1 = 0; bs_.pad = 0; }
PieceTree::~PieceTree() {}
int PieceTree::init(ByteSource* original, const BreakSpec& bs, ProgressCb* cb)
{ (void)original; (void)bs; (void)cb; return E_NOTIMPL; }
int PieceTree::init_uncounted(ByteSource* original, const BreakSpec& bs) { (void)original; (void)bs; return E_NOTIMPL; }
int PieceTree::set_counts(const u32* block_breaks, u32 nblocks) { (void)block_breaks; (void)nblocks; return E_NOTIMPL; }
int PieceTree::set_break_spec(const BreakSpec& bs, ProgressCb* cb) { (void)bs; (void)cb; return E_NOTIMPL; }
void PieceTree::clear() {}
epos PieceTree::length() const { return 0; }
u32 PieceTree::break_count() const { return 0; }
const BreakSpec& PieceTree::break_spec() const { return bs_; }
int PieceTree::insert(epos pos, const u8* p, epos n, PieceVec* added)
{ (void)pos; (void)p; (void)n; (void)added; return E_NOTIMPL; }
int PieceTree::insert_refs(epos pos, const PieceRef* refs, u32 n) { (void)pos; (void)refs; (void)n; return E_NOTIMPL; }
int PieceTree::remove(epos pos, epos n, PieceVec* removed) { (void)pos; (void)n; (void)removed; return E_NOTIMPL; }
int PieceTree::read(epos pos, u8* out, epos n) { (void)pos; (void)out; (void)n; return E_NOTIMPL; }
int PieceTree::span(epos pos, PSpan* s) { (void)pos; (void)s; return E_NOTIMPL; }
epos PieceTree::break_pos(u32 b) { (void)b; return EPOS_NONE; }
u32 PieceTree::breaks_before(epos pos) { (void)pos; return 0; }
int PieceTree::snapshot(PieceVec* out) const { (void)out; return E_NOTIMPL; }
ByteSource* PieceTree::original() const { return orig_; }
void PieceTree::set_original(ByteSource* s) { orig_ = s; }
AddBuffer* PieceTree::add_buffer() { return &add_; }
u32 PieceTree::piece_count() const { return 0; }
int PieceTree::check(char* why, u32 cap) { if (why && cap) why[0] = 0; return 0; }
void PieceTree::dump(EStr* out) { (void)out; }

// ---- SnapshotSource ----
SnapshotSource::SnapshotSource() : add_(0), orig_(0), size_(0), err_(E_OK) {}
SnapshotSource::~SnapshotSource() {}
int SnapshotSource::init(const PieceVec& pieces, const AddBuffer* add, ByteSource* original_private)
{ (void)pieces; (void)add; (void)original_private; return E_NOTIMPL; }
epos SnapshotSource::size() const { return size_; }
const u8* SnapshotSource::get(epos off, epos* avail) { (void)off; if (avail) *avail = 0; return 0; }
int SnapshotSource::err() const { return err_; }
