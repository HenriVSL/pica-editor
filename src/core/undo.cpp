// undo.cpp - undo log (DESIGN 6.6, 7.4, ARCH 6). Owner: T01.
//
// Groups live in one vector in history order; groups [0, cur_) are the undo side, [cur_, size) the redo side. Their
// ops and piece refs are stored contiguously in ops_/refs_ (a group's refs are addressed relative to its first ref).
// State ids: every group gets a unique id when it is created or reopened (merge); the current state id (seq()) is the
// id of the last applied group, or base_id_ with nothing applied. Undoing back to a saved state therefore gives back
// the saved id, and a state that was saved but discarded with the redo side can never come back.
// begin() decides about merging but the group is only created (and the redo side only discarded) by the first
// recorded op, so a command that fails before changing anything leaves the history untouched.
#include "undo.h"

#define SEQ_NONE 0xFFFFFFFFu

UndoLog::UndoLog()
  : cur_(0), seq_(1), save_seq_(1), base_id_(1), next_id_(2), open_(0), started_(0), pend_merge_(0), no_merge_(1),
    pend_kind_(0), pend_time_(0)
{
  pend_before_.anchor = pend_before_.caret = 0;
}

UndoLog::~UndoLog() {}

void UndoLog::clear()
{
  bool was = at_save_point();
  groups_.release(); ops_.release(); refs_.release();
  cur_ = 0;
  base_id_ = next_id_++;
  seq_ = base_id_;
  save_seq_ = was ? seq_ : SEQ_NONE;
  open_ = started_ = pend_merge_ = 0;
  no_merge_ = 1;
}

int UndoLog::begin(u32 kind, const UndoSel& before, u32 now_ms)
{
  int merge = 0;
  if (open_) return 0;                       // nested begin: the Document counts depth; ignore here
  if (!no_merge_ && kind != UK_OTHER && cur_ > 0 && cur_ == groups_.size()) {
    const Group& g = groups_[cur_ - 1];
    if (g.kind == kind && before.anchor == before.caret && before.caret == g.after.caret &&
        (u32)(now_ms - g.time) <= 2000u)
      merge = 1;
  }
  open_ = 1; started_ = 0; pend_merge_ = merge;
  pend_kind_ = kind; pend_time_ = now_ms; pend_before_ = before;
  return merge;
}

int UndoLog::reserve(u32 nops, u32 nrefs)
{
  if (groups_.reserve(groups_.size() + 1) != E_OK) return E_NOMEM;
  if (nops > 0xFFFFFFFFu - ops_.size() || nrefs > 0xFFFFFFFFu - refs_.size()) return E_NOMEM;
  if (ops_.reserve(ops_.size() + nops) != E_OK) return E_NOMEM;
  if (refs_.reserve(refs_.size() + nrefs) != E_OK) return E_NOMEM;
  return E_OK;
}

// Create (or reopen) the open group on the first recorded op.
int UndoLog::start_()
{
  Group g;
  u32 i;
  if (started_) return E_OK;
  if (!open_) return E_STATE;
  if (pend_merge_) {
    Group& last = groups_[cur_ - 1];
    last.id = next_id_++;
    seq_ = last.id;
    started_ = 1;
    no_merge_ = 0;
    return E_OK;
  }
  if (groups_.reserve(cur_ + 1) != E_OK) return E_NOMEM;
  // discard the redo side
  for (i = cur_; i < groups_.size(); i++) if (groups_[i].id == save_seq_) save_seq_ = SEQ_NONE;
  if (cur_ < groups_.size()) {
    ops_.resize(groups_[cur_].op_first);
    refs_.resize(groups_[cur_].ref_first);
    groups_.resize(cur_);
  }
  g.op_first = ops_.size(); g.nops = 0;
  g.ref_first = refs_.size(); g.nrefs = 0;
  g.before = pend_before_; g.after = pend_before_;
  g.kind = pend_kind_; g.time = pend_time_; g.id = next_id_++;
  groups_.push(g);                           // capacity reserved above
  cur_++;
  seq_ = g.id;
  started_ = 1;
  no_merge_ = 0;
  return E_OK;
}

static int rec_op(Vec<UndoOp>& ops, Vec<PieceRef>& refs, u32 kind, epos pos, epos len, u32 ref_rel,
                  const PieceVec* pv, u32 a, u32 b)
{
  UndoOp op;
  op.kind = (u8)kind; op.pad[0] = op.pad[1] = op.pad[2] = 0;
  op.pos = pos; op.len = len;
  if (pv) {
    if (refs.reserve(refs.size() + pv->size()) != E_OK) return E_NOMEM;
    if (ops.reserve(ops.size() + 1) != E_OK) return E_NOMEM;
    op.ref_first = ref_rel; op.ref_count = pv->size();
    refs.insert(refs.size(), pv->data(), pv->size());
  } else {
    if (ops.reserve(ops.size() + 1) != E_OK) return E_NOMEM;
    op.ref_first = a; op.ref_count = b;
  }
  ops.push(op);
  return E_OK;
}

int UndoLog::rec_insert(epos pos, epos len, const PieceVec& added)
{
  int rc = start_();
  Group* g;
  if (rc != E_OK) return rc;
  g = &groups_[cur_ - 1];
  rc = rec_op(ops_, refs_, 1, pos, len, g->nrefs, &added, 0, 0);
  if (rc != E_OK) return rc;
  g->nops++; g->nrefs += added.size();
  return E_OK;
}

int UndoLog::rec_delete(epos pos, epos len, const PieceVec& removed)
{
  int rc = start_();
  Group* g;
  if (rc != E_OK) return rc;
  g = &groups_[cur_ - 1];
  rc = rec_op(ops_, refs_, 2, pos, len, g->nrefs, &removed, 0, 0);
  if (rc != E_OK) return rc;
  g->nops++; g->nrefs += removed.size();
  return E_OK;
}

int UndoLog::rec_state(u32 old_enc, u32 new_enc, u32 old_eol, u32 new_eol)
{
  int rc = start_();
  if (rc != E_OK) return rc;
  rc = rec_op(ops_, refs_, 3, old_enc, new_enc, 0, 0, old_eol, new_eol);
  if (rc != E_OK) return rc;
  groups_[cur_ - 1].nops++;
  return E_OK;
}

void UndoLog::end(const UndoSel& after)
{
  if (!open_) return;
  open_ = 0;
  if (!started_) return;                     // nothing recorded: history unchanged
  started_ = 0;
  groups_[cur_ - 1].after = after;
  groups_[cur_ - 1].time = pend_time_;
}

void UndoLog::break_merge() { no_merge_ = 1; }
bool UndoLog::can_undo() const { return !open_ && cur_ > 0; }
bool UndoLog::can_redo() const { return !open_ && cur_ < groups_.size(); }
bool UndoLog::in_group() const { return open_ != 0; }

void UndoLog::fill_(const Group& g, UndoStep* out) const
{
  out->ops = g.nops ? &ops_[g.op_first] : 0;
  out->nops = g.nops;
  out->refs = g.nrefs ? &refs_[g.ref_first] : 0;
  out->before = g.before;
  out->after = g.after;
}

int UndoLog::pop_undo(UndoStep* out)
{
  if (open_ || cur_ == 0) return E_STATE;
  cur_--;
  fill_(groups_[cur_], out);
  seq_ = cur_ ? groups_[cur_ - 1].id : base_id_;
  no_merge_ = 1;
  return E_OK;
}

int UndoLog::pop_redo(UndoStep* out)
{
  if (open_ || cur_ >= groups_.size()) return E_STATE;
  fill_(groups_[cur_], out);
  seq_ = groups_[cur_].id;
  cur_++;
  no_merge_ = 1;
  return E_OK;
}

void UndoLog::set_save_point() { save_seq_ = seq_; }
bool UndoLog::at_save_point() const { return save_seq_ == seq_; }
void UndoLog::invalidate_save_point() { save_seq_ = SEQ_NONE; }
u32 UndoLog::seq() const { return seq_; }
void UndoLog::set_save_point_seq(u32 s) { save_seq_ = s; }

u32 UndoLog::memory_bytes() const
{
  return groups_.size() * (u32)sizeof(Group) + ops_.size() * (u32)sizeof(UndoOp) +
         refs_.size() * (u32)sizeof(PieceRef);
}
