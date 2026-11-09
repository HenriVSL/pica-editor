// resource.h - resource ids (DESIGN 15.4). Owner after Wave 0: overseer. CRLF, ASCII.
// Command ids 40000-40999 (IDM_*); dialog control ids live in ids_file.h (2000-2099), ids_find.h (2100-2299),
// ids_opts.h (2300-2599), ids_misc.h (2600-2899).
#ifndef PICA_RESOURCE_H
#define PICA_RESOURCE_H

#define IDR_MENU                   100
#define IDA_MAIN                   101   // main accelerator table (15.5)
#define IDA_FIND                   102   // Find dialog accelerators: F3, Shift+F3, Ctrl+F3, F4, Shift+F4
#define IDR_TABMENU                103   // tab bar context menu (submenu 0)
#define IDB_TOOLBAR                110   // 15 buttons, 16x16 each, magenta transparent
#define IDI_APP                    120

// RCDATA: built-in language files (src/langs) and help. Order = Document > Syntax menu order (IDM_LANG_FIRST + i).
#define IDR_LANG_FIRST             200
#define IDR_LANG_CPP               200   // cpp.lng
#define IDR_LANG_RC                201   // rc.lng
#define IDR_LANG_MAKE              202   // make.lng
#define IDR_LANG_BAT               203   // bat.lng
#define IDR_LANG_INI               204   // ini.lng
#define IDR_LANG_ASM               205   // asm.lng
#define IDR_LANG_PYTHON            206   // python.lng
#define IDR_LANG_HTML              207   // html.lng
#define IDR_LANG_JS                208   // js.lng
#define IDR_LANG_CSS               209   // css.lng
#define IDR_LANG_XML               210   // xml.lng
#define IDR_LANG_MD                211   // md.lng
#define IDR_LANG_DIFF              212   // diff.lng
#define IDR_LANG_COUNT             13
#define IDR_HELP                   220   // help.md

// String table: toolbar tooltips use the command id as the string id (IDM_* below).
#define IDS_APP_TITLE              300

// Toolbar: number of buttons in IDB_TOOLBAR (order: New Open Save SaveAll | Cut Copy Paste | Undo Redo |
// Find Replace FindInFiles | Hex | Wrap ShowWS)
#define TOOLBAR_BUTTONS            15

// Reload-as code page submenu (IDM_ENC_RELOAD_CP_FIRST + i): 1250 1251 1253 1254 1257 437 850 852 866 20866
#define ENC_RELOAD_CP_COUNT        10

// ---- commands (40000-40999)
#define IDM_FILE_NEW               40001
#define IDM_FILE_OPEN              40002
#define IDM_FILE_REVERT            40003
#define IDM_FILE_SAVE              40004
#define IDM_FILE_SAVEAS            40005
#define IDM_FILE_SAVEALL           40006
#define IDM_FILE_CLOSE             40007
#define IDM_FILE_CLOSEALL          40008
#define IDM_FILE_CLOSEOTHERS       40009
#define IDM_FILE_EXIT              40010
#define IDM_MRU_CLEAR              40011
#define IDM_MRU_FIRST              40020
#define IDM_MRU_LAST               40029
#define IDM_EDIT_UNDO              40100
#define IDM_EDIT_REDO              40101
#define IDM_EDIT_CUT               40102
#define IDM_EDIT_COPY              40103
#define IDM_EDIT_PASTE             40104
#define IDM_EDIT_DELETE            40105
#define IDM_EDIT_SELECTALL         40106
#define IDM_LINE_DUP               40110
#define IDM_LINE_DEL               40111
#define IDM_LINE_UP                40112
#define IDM_LINE_DOWN              40113
#define IDM_EDIT_COMMENT           40114
#define IDM_EDIT_UPPER             40115
#define IDM_EDIT_LOWER             40116
#define IDM_EDIT_INDENT            40117
#define IDM_EDIT_UNINDENT          40118
#define IDM_EDIT_OVERWRITE         40119
#define IDM_SEARCH_FIND            40200
#define IDM_SEARCH_NEXT            40201
#define IDM_SEARCH_PREV            40202
#define IDM_SEARCH_SELNEXT         40203
#define IDM_SEARCH_SELPREV         40204
#define IDM_SEARCH_ISEARCH         40205
#define IDM_SEARCH_REPLACE         40206
#define IDM_SEARCH_FIF             40207
#define IDM_SEARCH_RESNEXT         40208
#define IDM_SEARCH_RESPREV         40209
#define IDM_SEARCH_CLEARMARKS      40210
#define IDM_SEARCH_GOTO            40211
#define IDM_SEARCH_BRACE           40212
#define IDM_BM_TOGGLE              40220
#define IDM_BM_NEXT                40221
#define IDM_BM_PREV                40222
#define IDM_BM_CLEAR               40223
#define IDM_VIEW_HEX               40300
#define IDM_VIEW_INSPECTOR         40301
#define IDM_VIEW_WRAP              40302
#define IDM_VIEW_SHOWWS            40303
#define IDM_VIEW_LINENUM           40304
#define IDM_VIEW_TOOLBAR           40305
#define IDM_VIEW_STATUSBAR         40306
#define IDM_VIEW_RESULTS           40307
#define IDM_LANG_AUTO              40400
#define IDM_LANG_PLAIN             40401
#define IDM_LANG_FIRST             40410
#define IDM_LANG_LAST              40449
#define IDM_ENC_RELOAD_ANSI        40450
#define IDM_ENC_RELOAD_OEM         40451
#define IDM_ENC_RELOAD_UTF8        40452
#define IDM_ENC_RELOAD_UTF16LE     40453
#define IDM_ENC_RELOAD_UTF16BE     40454
#define IDM_ENC_RELOAD_CP_FIRST    40460
#define IDM_ENC_RELOAD_CP_LAST     40479
#define IDM_ENC_CONV_ANSI          40480
#define IDM_ENC_CONV_UTF8          40481
#define IDM_ENC_CONV_UTF8BOM       40482
#define IDM_ENC_CONV_UTF16LE       40483
#define IDM_ENC_CONV_UTF16BE       40484
#define IDM_EOL_CRLF               40490
#define IDM_EOL_LF                 40491
#define IDM_EOL_CR                 40492
#define IDM_TAB_2                  40500
#define IDM_TAB_4                  40501
#define IDM_TAB_8                  40502
#define IDM_TAB_SPACES             40503
#define IDM_DOC_READONLY           40510
#define IDM_DOC_PROPERTIES         40511
#define IDM_TOOLS_OPTIONS          40600
#define IDM_WIN_NEXT               40700
#define IDM_WIN_PREV               40701
#define IDM_WIN_FIRST              40710
#define IDM_WIN_LAST               40739
#define IDM_HELP_KEYS              40800
#define IDM_HELP_REGEX             40801
#define IDM_HELP_ABOUT             40802
#define IDM_TAB_CLOSE              40900
#define IDM_TAB_CLOSEOTHERS        40901
#define IDM_TAB_COPYPATH           40902
#define IDM_TAB_OPENFOLDER         40903

// Named items of the static Document menu (T09 may rebuild these submenus at run time)
#define IDM_LANG_CPP               40410
#define IDM_LANG_RC                40411
#define IDM_LANG_MAKE              40412
#define IDM_LANG_BAT               40413
#define IDM_LANG_INI               40414
#define IDM_LANG_ASM               40415
#define IDM_LANG_PYTHON            40416
#define IDM_LANG_HTML              40417
#define IDM_LANG_JS                40418
#define IDM_LANG_CSS               40419
#define IDM_LANG_XML               40420
#define IDM_LANG_MD                40421
#define IDM_LANG_DIFF              40422
#define IDM_ENC_RELOAD_CP_1250     40460
#define IDM_ENC_RELOAD_CP_1251     40461
#define IDM_ENC_RELOAD_CP_1253     40462
#define IDM_ENC_RELOAD_CP_1254     40463
#define IDM_ENC_RELOAD_CP_1257     40464
#define IDM_ENC_RELOAD_CP_437      40465
#define IDM_ENC_RELOAD_CP_850      40466
#define IDM_ENC_RELOAD_CP_852      40467
#define IDM_ENC_RELOAD_CP_866      40468
#define IDM_ENC_RELOAD_CP_KOI8R    40469

#endif
