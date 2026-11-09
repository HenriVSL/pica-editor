# Pica handoff notes

Collected from every finished task, for the tasks that come later. `docs/DESIGN.md` is the spec and `docs/TASKS.md`
the plan; this file records what was actually built and learned while building.

**How to add your notes:** when your task is done, append one section at the end of this file, in a single edit.
Never change other sections (several agents append in the same wave). Use the template below. Keep it factual and
short: later agents read every section.

```
## Txx <title> (wave N, agent 86box-d1-Txx, date)
### Built
- files and what they do now (one line each)
### Interface notes
- how to use what you built; non-obvious behaviour; performance numbers
### Interface changes / deviations from DESIGN
- anything that differs from DESIGN 6 / 15.1 and why (or "none")
### Requests for files I don't own
- file, exact change, why (the overseer applies these between waves)
### Gotchas
- VC6, Win98, clang or test-harness surprises worth knowing
### Tests
- commands run and results (Mac suites, VC6 suites/objects, selftests, screenshots taken)
### Open issues
- known bugs, unfinished parts, ideas for later waves
```

---

## Probe (phase 1): probe/RESULTS.md
Direct ExtTextOutW per colour run with ETO_OPAQUE: 4-5 ms per full 34-row repaint, 0.4 ms per keystroke; compatible
bitmap +3 ms; DIB sections 20+ ms (never). ScrollWindowEx for line scrolls. Win98 stubs: GetCharABCWidthsW,
CharLowerW/UpperW, GetCharWidth32*, GetTextMetricsW, GetTextExtentExPointW, DrawTextW, CompareStringW, IsTextUnicode,
CreateFileW. Never call GetTextExtentExPoint (50 ms per 1000 chars). ExtTextOutW + Courier New draws Cyrillic and
Polish with any charset. M: share: 1.1 MB/s read, 0.4 MB/s write, no change notifications for Mac-side edits (poll).
MoveFile/DeleteFile fail on files anyone holds open; FILE_SHARE_DELETE is an invalid parameter on 9x. A UTF-8 BOM
breaks cl, rc, nmake and batch files. In-memory threshold 16 MB (50 MB paged heavily); malloc caps at 128 MB, use
VirtualAlloc for big blocks.

