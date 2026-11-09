// session.h - session model (DESIGN 6.19, 13.6). Owner after Wave 0: T06.
#ifndef PICA_SESSION_H
#define PICA_SESSION_H

#include "etypes.h"
#include "emem.h"
#include "codec.h"
#include "ini.h"

struct SessionTab {
  u32 id; char path[260]; u8 untitled; u8 view; u8 hex_pane; u8 hex_insert; u32 untitled_no;
  EncSpec enc; u8 enc_forced; u8 eol; u8 lang_forced; u8 wrap; char lang[16];
  u8 tab_width; u8 use_spaces; u8 read_only; u8 modified; u8 has_backup; u8 disk_missing; u8 pad[2];
  epos caret, anchor; u32 top_line, top_subrow; epos hex_top;
  u32 disk_size, disk_mtime_lo, disk_mtime_hi;
  Vec<u32> bookmarks;
};
struct Session { u32 seq; u32 active; u32 next_tabid; u32 next_untitled; u8 clean_exit; u8 pad[3];
                 Vec<SessionTab*> tabs; };
void session_init(Session* s);  void session_free(Session* s);
int  session_to_ini(const Session& s, Ini* ini);
int  session_from_ini(const Ini& ini, Session* s);       // E_FORMAT if "[end] ok=1" is missing
int  session_validate(const u8* p, u32 n, u32* seq);      // for ab_read (13.6)
struct BackupMeta { u32 tabid; u32 gen; u32 seq; char path[260]; u8 untitled; u8 eol; u8 pad[2]; u32 untitled_no;
                    EncSpec enc; char lang[16]; epos caret, anchor;
                    u32 disk_size, disk_mtime_lo, disk_mtime_hi; u32 written_lo, written_hi; };
// backup_header_write/parse live in fileops.h (6.20)

#endif
