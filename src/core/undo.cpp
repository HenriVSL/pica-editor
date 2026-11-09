// undo.cpp - undo log (DESIGN 6.6). STUB from T00. Owner after Wave 0: T01.
#include "undo.h"

UndoLog::UndoLog() : seq_(0), save_seq_(0) {}
UndoLog::~UndoLog() {}
void UndoLog::clear() {}
int UndoLog::begin(u32 kind, const UndoSel& before, u32 now_ms) { (void)kind; (void)before; (void)now_ms; return 0; }
int UndoLog::rec_insert(epos pos, epos len, const PieceVec& added) { (void)pos; (void)len; (void)added; return E_NOTIMPL; }
int UndoLog::rec_delete(epos pos, epos len, const PieceVec& removed) { (void)pos; (void)len; (void)removed; return E_NOTIMPL; }
int UndoLog::rec_state(u32 old_enc, u32 new_enc, u32 old_eol, u32 new_eol)
{ (void)old_enc; (void)new_enc; (void)old_eol; (void)new_eol; return E_NOTIMPL; }
void UndoLog::end(const UndoSel& after) { (void)after; }
void UndoLog::break_merge() {}
bool UndoLog::can_undo() const { return false; }
bool UndoLog::can_redo() const { return false; }
int UndoLog::pop_undo(UndoStep* out) { (void)out; return E_NOTIMPL; }
int UndoLog::pop_redo(UndoStep* out) { (void)out; return E_NOTIMPL; }
void UndoLog::set_save_point() { save_seq_ = seq_; }
bool UndoLog::at_save_point() const { return save_seq_ == seq_; }
void UndoLog::invalidate_save_point() { save_seq_ = 0xFFFFFFFFu; }
u32 UndoLog::seq() const { return seq_; }
void UndoLog::set_save_point_seq(u32 s) { save_seq_ = s; }
u32 UndoLog::memory_bytes() const { return 0; }
