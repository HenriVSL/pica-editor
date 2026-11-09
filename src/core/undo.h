// undo.h - undo log (DESIGN 6.6). Owner after Wave 0: T01.
#ifndef PICA_UNDO_H
#define PICA_UNDO_H

#include "etypes.h"
#include "emem.h"
#include "ptree.h"

enum UndoKind { UK_OTHER = 0, UK_TYPING = 1, UK_DELETE = 2, UK_BACKSPACE = 3, UK_HEXTYPE = 4 };
struct UndoSel  { epos anchor; epos caret; };
struct UndoOp   { u8 kind; u8 pad[3]; epos pos; epos len; u32 ref_first; u32 ref_count; };
// kind 1 insert, 2 delete, 3 state change (encoding/break mode, 7.5): pos = old EncSpec packed (kind | bom<<8 | cp<<16),
// len = new EncSpec packed, ref_first = old EolStyle, ref_count = new EolStyle
struct UndoStep { const UndoOp* ops; u32 nops; const PieceRef* refs; UndoSel before; UndoSel after; };
class UndoLog {
public:
  UndoLog();  ~UndoLog();
  void clear();
  int  begin(u32 kind, const UndoSel& before, u32 now_ms);   // returns 1 if it reopened (merged into) the last group
  int  rec_insert(epos pos, epos len, const PieceVec& added);
  int  rec_delete(epos pos, epos len, const PieceVec& removed);
  int  rec_state(u32 old_enc, u32 new_enc, u32 old_eol, u32 new_eol);   // kind 3 op
  void end(const UndoSel& after);
  void break_merge();
  bool can_undo() const;  bool can_redo() const;
  int  pop_undo(UndoStep* out);   // the group moves to the redo side; Document applies ops in reverse
  int  pop_redo(UndoStep* out);
  void set_save_point();  bool at_save_point() const;  void invalidate_save_point();
  u32  seq() const;               // id of the current state (changes on every group, undo, redo)
  void set_save_point_seq(u32 s); // mark a past state as saved (async save completes later)
  u32  memory_bytes() const;
private:                          // private state: T01 may change freely
  u32 seq_; u32 save_seq_;
  UndoLog(const UndoLog&); UndoLog& operator=(const UndoLog&);
};

#endif
