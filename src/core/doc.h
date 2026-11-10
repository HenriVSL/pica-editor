// doc.h - the Document (DESIGN 6.7). Owner after Wave 0: T01.
// EolStyle and EolCounts live in codec.h (T00 deviation, see HANDOFF).
#ifndef PICA_DOC_H
#define PICA_DOC_H

#include "etypes.h"
#include "emem.h"
#include "esource.h"
#include "ptree.h"
#include "undo.h"
#include "codec.h"

struct Selection { epos anchor; epos caret; };
inline epos sel_lo(const Selection& s) { return s.anchor < s.caret ? s.anchor : s.caret; }
inline epos sel_hi(const Selection& s) { return s.anchor < s.caret ? s.caret : s.anchor; }
inline bool sel_empty(const Selection& s) { return s.anchor == s.caret; }
struct EditEvent { epos pos; epos removed; epos inserted; u32 line; u32 lines_removed; u32 lines_added; u32 flags; };
enum { EV_UNDO = 1, EV_REDO = 2, EV_RESET = 4, EV_INDEXED = 8 };
class Document;
class DocListener { public: virtual ~DocListener() {} virtual void doc_changed(Document* d, const EditEvent& e) = 0; };
struct ConvertReport { u32 unrepresentable; u32 invalid_src; epos first_bad; };

class Document {
public:
  Document();  ~Document();
  // lifecycle
  int  init_empty(const EncSpec& enc, EolStyle eol);                       // writes the BOM if enc.bom
  int  init_source(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol, ProgressCb* cb); // src owned
  int  init_source_indexing(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol);         // on-demand
  int  finish_indexing(const u32* block_breaks, u32 nblocks, const EolCounts& eol);                   // EV_INDEXED
  int  reset_source(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol, ProgressCb* cb); // reload: clears undo, EV_RESET
  int  reload_as_edit(ByteSource* src);    // undoable reload: replaces all bytes in one group (small docs)
  bool indexing() const;
  ByteSource* source() const;              // original (MemSource / FileSource)
  ByteSource* bytes();                     // live view of the current bytes (UI thread only)
  int  snapshot(SnapshotSource* out, ByteSource* original_private);  // for the I/O thread (in-memory: pass NULL)
  // size and lines (line = 0-based)
  epos length();
  u32  line_count();                       // breaks + 1
  epos line_start(u32 line);
  epos line_end(u32 line);                 // offset of the EOL (the CR of CRLF in LF mode) or length()
  epos line_next(u32 line);                // start of line + 1, or length() for the last line
  u32  line_of(epos pos);
  epos text_origin() const;                // BOM length: text view never shows [0, origin)
  // bytes and characters
  int  read(epos pos, u8* out, epos n);
  int  span(epos pos, PSpan* s);
  u32  decode(epos pos, ecp* cp);          // bytes used (>= 1), 0 at end
  epos char_next(epos pos);
  epos char_prev(epos pos);                // never below text_origin()
  epos snap_char(epos pos);                // start of the character containing pos
  // editing: the only mutators (with undo/redo/reset)
  int  replace(epos pos, epos del, const u8* ins, epos ins_len);           // E_READONLY, E_ALIGN, E_RANGE, E_TOOBIG, E_NOMEM
  int  replace_text(epos pos, epos del, const ecp* s, u32 n, u32* unrepresentable);  // encodes; unrepresentable -> E_RANGE, nothing changed
  void begin_edit(u32 kind, const Selection& before, u32 now_ms);
  void end_edit(const Selection& after);
  int  undo(Selection* sel);  int redo(Selection* sel);
  bool can_undo() const;  bool can_redo() const;
  void break_undo_merge();  void clear_undo();
  bool modified() const;
  void set_saved();                        // current state is on disk
  u32  undo_seq() const;  void set_saved_seq(u32 seq);   // async save: state captured at snapshot time
  void set_always_modified(bool on);       // restored from backup / file deleted on disk
  // encoding and line ends
  const EncSpec& enc() const;  const Codec* codec() const;
  int  reinterpret(const EncSpec& enc, epos bom_len, ProgressCb* cb);      // bytes unchanged, recount; EV_RESET; not undoable
  int  check_convert(const EncSpec& to, ConvertReport* rep, ProgressCb* cb);
  int  convert(const EncSpec& to, ConvertReport* rep, ProgressCb* cb);     // one undo group; E_TOOBIG for on-demand docs
  EolStyle eol_style() const;  void set_eol_style(EolStyle s);             // style for new line breaks
  const EolCounts& eol_counts() const;  bool eol_mixed() const;
  int  convert_eol(EolStyle to, ProgressCb* cb);                            // one undo group; may change the break mode
  const u8* eol_bytes(u32* n) const;       // newline for eol_style() encoded in the document encoding
  // state
  void set_read_only(bool ro);  bool read_only() const;
  void set_busy(bool busy);                // saving: edits refused with E_BUSY
  bool editable() const;                   // !read_only && !indexing && !busy
  int  add_listener(DocListener* l);  void remove_listener(DocListener* l);
  PieceTree* tree();
  u32  change_seq() const;                 // increments on every content change
  // T16 addition: reload into on-demand mode in one call = reset_source (clears undo, EV_RESET with removed = old
  // length, inserted = new length, lines_added = 0) + init_source_indexing (pieces without counts; edits refused
  // until finish_indexing fires EV_INDEXED). src owned, also on failure. E_BUSY while busy, E_STATE inside a group.
  int  reset_source_indexing(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol);
private:                                   // private state: T01 may change freely
  class View : public ByteSource {         // bytes(): live view with a one-span cache
  public:
    View();
    epos size() const;  const u8* get(epos off, epos* avail);
    Document* d_; const u8* p_; epos pos_; epos len_; u32 stamp_; int valid_;
  };
  friend class View;
  PieceTree tree_; UndoLog undo_; ByteSource* src_; EncSpec enc_; const Codec* codec_; epos origin_;
  EolStyle eol_; EolCounts eolc_; u8 eolb_[4]; u32 eolb_n_;
  bool ro_, busy_, indexing_, always_mod_; u32 change_seq_;
  Vec<DocListener*> listeners_;
  bool ondemand_, cr_mode_; u32 depth_; u32 dispatching_; bool lst_dirty_; View view_;
  const Codec* cc_() const;  u32 unit_() const;  void set_enc_(const EncSpec& e, bool cr_mode, EolStyle eol);
  int  reset_(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol, ProgressCb* cb, int indexing, int fire);
  static int count_eol_(ByteSource* s, epos a, epos b, u32 unit, int be, EolCounts* c, ProgressCb* cb);
  void recount_eol_();
  int  check_edit_(epos pos, epos del, epos ins_len);
  int  prim_(epos pos, epos del, PieceVec* removed, const u8* ins, epos ins_len, const PieceRef* refs, u32 nrefs,
             PieceVec* added, u32 flags, int record);
  int  state_(u32 enc_packed, u32 eol_packed, u32 flags);
  int  apply_op_(const UndoOp& op, const PieceRef* refs, int forward, u32 flags);
  int  apply_step_(const UndoStep& st, int forward, u32 flags);
  void emit_(const EditEvent& e);
  int  edit_record_(epos pos, epos del, const u8* ins, epos ins_len);
  int  build_text_(EBytes* out, const Codec* to, int lossy_subst, ConvertReport* rep, ProgressCb* cb);
  Document(const Document&); Document& operator=(const Document&);
};

#endif
