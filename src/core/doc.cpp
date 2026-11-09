// doc.cpp - the Document (DESIGN 6.7, 7). STUB from T00: lifecycle calls return E_NOTIMPL, queries return empty
// values (an empty one-line document). Owner after Wave 0: T01.
#include "doc.h"

Document::Document()
  : src_(0), codec_(0), origin_(0), eol_(EOL_CRLF), eolb_n_(0), ro_(false), busy_(false), indexing_(false),
    always_mod_(false), change_seq_(0)
{
  enc_ = enc_ansi();
  eolc_.crlf = eolc_.lf = eolc_.cr = 0;
  eolb_[0] = eolb_[1] = eolb_[2] = eolb_[3] = 0;
}
Document::~Document() {}
int Document::init_empty(const EncSpec& enc, EolStyle eol) { (void)enc; (void)eol; return E_NOTIMPL; }
int Document::init_source(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol, ProgressCb* cb)
{ (void)src; (void)enc; (void)bom_len; (void)eol; (void)cb; return E_NOTIMPL; }
int Document::init_source_indexing(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol)
{ (void)src; (void)enc; (void)bom_len; (void)eol; return E_NOTIMPL; }
int Document::finish_indexing(const u32* block_breaks, u32 nblocks, const EolCounts& eol)
{ (void)block_breaks; (void)nblocks; (void)eol; return E_NOTIMPL; }
int Document::reset_source(ByteSource* src, const EncSpec& enc, epos bom_len, EolStyle eol, ProgressCb* cb)
{ (void)src; (void)enc; (void)bom_len; (void)eol; (void)cb; return E_NOTIMPL; }
int Document::reload_as_edit(ByteSource* src) { (void)src; return E_NOTIMPL; }
bool Document::indexing() const { return indexing_; }
ByteSource* Document::source() const { return src_; }
ByteSource* Document::bytes() { return 0; }
int Document::snapshot(SnapshotSource* out, ByteSource* original_private) { (void)out; (void)original_private; return E_NOTIMPL; }
epos Document::length() { return 0; }
u32 Document::line_count() { return 1; }
epos Document::line_start(u32 line) { (void)line; return 0; }
epos Document::line_end(u32 line) { (void)line; return 0; }
epos Document::line_next(u32 line) { (void)line; return 0; }
u32 Document::line_of(epos pos) { (void)pos; return 0; }
epos Document::text_origin() const { return origin_; }
int Document::read(epos pos, u8* out, epos n) { (void)pos; (void)out; (void)n; return E_NOTIMPL; }
int Document::span(epos pos, PSpan* s) { (void)pos; (void)s; return E_NOTIMPL; }
u32 Document::decode(epos pos, ecp* cp) { (void)pos; (void)cp; return 0; }
epos Document::char_next(epos pos) { return pos; }
epos Document::char_prev(epos pos) { return pos; }
epos Document::snap_char(epos pos) { return pos; }
int Document::replace(epos pos, epos del, const u8* ins, epos ins_len)
{ (void)pos; (void)del; (void)ins; (void)ins_len; return E_NOTIMPL; }
int Document::replace_text(epos pos, epos del, const ecp* s, u32 n, u32* unrepresentable)
{ (void)pos; (void)del; (void)s; (void)n; if (unrepresentable) *unrepresentable = 0; return E_NOTIMPL; }
void Document::begin_edit(u32 kind, const Selection& before, u32 now_ms) { (void)kind; (void)before; (void)now_ms; }
void Document::end_edit(const Selection& after) { (void)after; }
int Document::undo(Selection* sel) { (void)sel; return E_NOTIMPL; }
int Document::redo(Selection* sel) { (void)sel; return E_NOTIMPL; }
bool Document::can_undo() const { return false; }
bool Document::can_redo() const { return false; }
void Document::break_undo_merge() {}
void Document::clear_undo() {}
bool Document::modified() const { return always_mod_; }
void Document::set_saved() {}
u32 Document::undo_seq() const { return 0; }
void Document::set_saved_seq(u32 seq) { (void)seq; }
void Document::set_always_modified(bool on) { always_mod_ = on; }
const EncSpec& Document::enc() const { return enc_; }
const Codec* Document::codec() const { return codec_; }
int Document::reinterpret(const EncSpec& enc, epos bom_len, ProgressCb* cb) { (void)enc; (void)bom_len; (void)cb; return E_NOTIMPL; }
int Document::check_convert(const EncSpec& to, ConvertReport* rep, ProgressCb* cb) { (void)to; (void)rep; (void)cb; return E_NOTIMPL; }
int Document::convert(const EncSpec& to, ConvertReport* rep, ProgressCb* cb) { (void)to; (void)rep; (void)cb; return E_NOTIMPL; }
EolStyle Document::eol_style() const { return eol_; }
void Document::set_eol_style(EolStyle s) { eol_ = s; }
const EolCounts& Document::eol_counts() const { return eolc_; }
bool Document::eol_mixed() const { return false; }
int Document::convert_eol(EolStyle to, ProgressCb* cb) { (void)to; (void)cb; return E_NOTIMPL; }
const u8* Document::eol_bytes(u32* n) const { if (n) *n = eolb_n_; return eolb_; }
void Document::set_read_only(bool ro) { ro_ = ro; }
bool Document::read_only() const { return ro_; }
void Document::set_busy(bool busy) { busy_ = busy; }
bool Document::editable() const { return !ro_ && !indexing_ && !busy_; }
int Document::add_listener(DocListener* l) { return listeners_.push(l); }
void Document::remove_listener(DocListener* l)
{
  u32 i;
  for (i = 0; i < listeners_.size(); i++) if (listeners_[i] == l) { listeners_.erase(i, 1); return; }
}
PieceTree* Document::tree() { return &tree_; }
u32 Document::change_seq() const { return change_seq_; }
