// fileops.cpp - load, FileSource, index, safe save, A/B files, backups (DESIGN 6.20, 13). STUB from T00.
// Owner after Wave 0: T05.
#include "fileops.h"

const char BACKUP_TRAILER[] = "\r\n@END PICABAK1\r\n";

int file_load(FileSys* fs, const char* path, const LoadOpts& o, ProgressCb* cb, LoadResult* r)
{ (void)fs; (void)path; (void)o; (void)cb; (void)r; return E_NOTIMPL; }

FileSource::FileSource() : fs_(0), f_(0), mode_(0), size_(0), err_(E_OK) { path_[0] = 0; }
FileSource::~FileSource() { close(); }
int FileSource::open(FileSys* fs, const char* path, u32 mode, u32 cache_blocks)
{ (void)fs; (void)path; (void)mode; (void)cache_blocks; return E_NOTIMPL; }
int FileSource::reopen(const char* path) { (void)path; return E_NOTIMPL; }
void FileSource::close() { if (fs_ && f_) fs_->close(f_); f_ = 0; }
epos FileSource::size() const { return size_; }
const u8* FileSource::get(epos off, epos* avail) { (void)off; if (avail) *avail = 0; return 0; }
int FileSource::err() const { return err_; }
const char* FileSource::path() const { return path_; }

int file_index(FileSys* fs, const char* path, const BreakSpec& bs, IndexResult* r, ProgressCb* cb, volatile i32* cancel)
{ (void)fs; (void)path; (void)bs; (void)r; (void)cb; (void)cancel; return E_NOTIMPL; }
int file_write_temp(FileSys* fs, const char* path, ByteSource* data, ProgressCb* cb, volatile i32* cancel, SaveResult* r)
{ (void)fs; (void)path; (void)data; (void)cb; (void)cancel; (void)r; return E_NOTIMPL; }
int file_replace(FileSys* fs, const char* path, const SaveOpts& o, SaveHooks* hooks, SaveResult* r)
{ (void)fs; (void)path; (void)o; (void)hooks; (void)r; return E_NOTIMPL; }
int file_save(FileSys* fs, const char* path, ByteSource* data, const SaveOpts& o, SaveHooks* hooks, ProgressCb* cb, SaveResult* r)
{ (void)fs; (void)path; (void)data; (void)o; (void)hooks; (void)cb; (void)r; return E_NOTIMPL; }
int file_read_all(FileSys* fs, const char* path, EBytes* out, u32 max) { (void)fs; (void)path; (void)out; (void)max; return E_NOTIMPL; }
int file_write_direct(FileSys* fs, const char* path, const u8* p, u32 n) { (void)fs; (void)path; (void)p; (void)n; return E_NOTIMPL; }
int ab_read(FileSys* fs, const char* p1, const char* p2, AbValidator v, EBytes* out, int* which)
{ (void)fs; (void)p1; (void)p2; (void)v; (void)out; if (which) *which = 0; return E_NOTIMPL; }
int ab_write(FileSys* fs, const char* p1, const char* p2, int last_which, const u8* p, u32 n, int* which)
{ (void)fs; (void)p1; (void)p2; (void)last_which; (void)p; (void)n; if (which) *which = 0; return E_NOTIMPL; }
int backup_write(FileSys* fs, const char* dir, const BackupMeta& m, ByteSource* data, ProgressCb* cb, volatile i32* cancel)
{ (void)fs; (void)dir; (void)m; (void)data; (void)cb; (void)cancel; return E_NOTIMPL; }
int backup_list(FileSys* fs, const char* dir, Vec<BackupMeta>* out) { (void)fs; (void)dir; (void)out; return E_NOTIMPL; }
int backup_open(FileSys* fs, const char* dir, const BackupMeta& m, ByteSource** data)
{ (void)fs; (void)dir; (void)m; if (data) *data = 0; return E_NOTIMPL; }
int backup_delete_tab(FileSys* fs, const char* dir, u32 tabid) { (void)fs; (void)dir; (void)tabid; return E_NOTIMPL; }
void backup_file_name(u32 tabid, u32 gen, char out[13]) { (void)tabid; (void)gen; out[0] = 0; }
int backup_header_write(const BackupMeta& m, epos data_len, EStr* out) { (void)m; (void)data_len; (void)out; return E_NOTIMPL; }
int backup_header_parse(const u8* p, u32 n, BackupMeta* m, u32* header_len, epos* data_len)
{ (void)p; (void)n; (void)m; (void)header_len; (void)data_len; return E_NOTIMPL; }
