// fileops.h - load, on-demand source, index, safe save, A/B files, backups (DESIGN 6.20). Owner after Wave 0: T05.
#ifndef PICA_FILEOPS_H
#define PICA_FILEOPS_H

#include "etypes.h"
#include "emem.h"
#include "estr.h"
#include "esource.h"
#include "ptree.h"
#include "codec.h"
#include "detect.h"
#include "fsys.h"
#include "session.h"

enum LoadMode { LM_MEMORY = 0, LM_ONDEMAND = 1 };
struct LoadOpts   { epos ondemand_threshold; DetectOpts det; u8 force_enc; u8 pad[3]; EncSpec enc; u32 cache_blocks; };
struct LoadResult { ByteSource* src; u8 mode; u8 ro_attr; u8 shared_ro; u8 pad; DetectResult det; FsStat st; };
int  file_load(FileSys* fs, const char* path, const LoadOpts& o, ProgressCb* cb, LoadResult* r);
class FileSource : public ByteSource {   // on-demand: read handle + LRU cache of 64 KB blocks (UI thread only)
public:
  FileSource();  ~FileSource();
  int  open(FileSys* fs, const char* path, u32 mode, u32 cache_blocks);
  int  reopen(const char* path);         // same bytes under another name (after save)
  void close();                          // before the rename sequence
  epos size() const;  const u8* get(epos off, epos* avail);  int err() const;  const char* path() const;
private:                                 // private state: T05 may change freely
  struct Slot { u8* p; u32 blk; u32 len; u32 stamp; };   // one cached 64 KB block; blk 0xFFFFFFFF = empty
  FileSys* fs_; FsFile f_; char path_[260]; u32 mode_; epos size_; int err_;
  Slot* slots_; u32 nslots_; u32 stamp_; u32 last_;
  void free_cache_();
  FileSource(const FileSource&); FileSource& operator=(const FileSource&);
};
struct IndexResult { Vec<u32> block_breaks; EolCounts eol; epos size; };
int  file_index(FileSys* fs, const char* path, const BreakSpec& bs, IndexResult* r, ProgressCb* cb, volatile i32* cancel);
struct SaveOpts   { u8 keep_bak; u8 pad[3]; };
// T00: after_replace's parameter name is commented out (unused parameter is an error under clang -Wextra -Werror).
class SaveHooks   { public: virtual ~SaveHooks() {} virtual void before_replace() {} virtual void after_replace(const char* /*old_copy*/) {} };
struct SaveResult { FsStat st; u8 in_place; u8 failed_step; u8 pad[2]; char temp[260]; char old_copy[260];
                    char stray[260]; };   // T05: stray = a ~P*.TMP file we may have left behind (could not delete it or tell)
int  file_write_temp(FileSys* fs, const char* path, ByteSource* data, ProgressCb* cb, volatile i32* cancel, SaveResult* r);  // steps 1-3 (13.3), worker thread
int  file_replace(FileSys* fs, const char* path, const SaveOpts& o, SaveHooks* hooks, SaveResult* r);   // steps 4-9, UI thread
int  file_save(FileSys* fs, const char* path, ByteSource* data, const SaveOpts& o, SaveHooks* hooks, ProgressCb* cb, SaveResult* r); // both (tests, settings)
int  file_read_all(FileSys* fs, const char* path, EBytes* out, u32 max);
int  file_write_direct(FileSys* fs, const char* path, const u8* p, u32 n);   // create/truncate + flush (A/B files)
typedef int (*AbValidator)(const u8* p, u32 n, u32* seq);
int  ab_read(FileSys* fs, const char* p1, const char* p2, AbValidator v, EBytes* out, int* which);  // newest valid
int  ab_write(FileSys* fs, const char* p1, const char* p2, int last_which, const u8* p, u32 n, int* which);  // writes the other one
int  backup_write(FileSys* fs, const char* dir, const BackupMeta& m, ByteSource* data, ProgressCb* cb, volatile i32* cancel);
int  backup_list(FileSys* fs, const char* dir, Vec<BackupMeta>* out);   // newest valid generation per tab id
int  backup_open(FileSys* fs, const char* dir, const BackupMeta& m, ByteSource** data);   // MemSource of the data
int  backup_delete_tab(FileSys* fs, const char* dir, u32 tabid);
void backup_file_name(u32 tabid, u32 gen, char out[13]);              // "T0002A07.BAK": T + 5 hex tab id + 2 hex gen (8.3)
int  backup_header_write(const BackupMeta& m, epos data_len, EStr* out);   // ends with "@DATA <n>\r\n"
int  backup_header_parse(const u8* p, u32 n, BackupMeta* m, u32* header_len, epos* data_len);
extern const char BACKUP_TRAILER[];                      // "\r\n@END PICABAK1\r\n"

#endif
