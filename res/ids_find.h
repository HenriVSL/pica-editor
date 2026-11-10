// ids_find.h - dialog and control ids, range 2100-2299. Owner: T11. CRLF, ASCII.
#ifndef PICA_IDS_FIND_H
#define PICA_IDS_FIND_H

#define IDD_FIND                   2100
// Find / Replace / Find in Files dialog
#define IDC_FIND_TAB               2101
#define IDC_FIND_WHAT_LBL          2102
#define IDC_FIND_WHAT              2103
#define IDC_FIND_WHAT_MENU         2104
#define IDC_FIND_REPL_LBL          2105
#define IDC_FIND_REPL              2106
#define IDC_FIND_REPL_MENU         2107
#define IDC_FIND_MASKS_LBL         2108
#define IDC_FIND_MASKS             2109
#define IDC_FIND_DIR_LBL           2110
#define IDC_FIND_DIR               2111
#define IDC_FIND_BROWSE            2112
#define IDC_FIND_SUBDIRS           2113
#define IDC_FIND_CASE              2114
#define IDC_FIND_WORD              2115
#define IDC_FIND_WRAP              2116
#define IDC_FIND_INSEL             2117
#define IDC_FIND_DIRGRP            2118
#define IDC_FIND_UP                2119
#define IDC_FIND_DOWN              2120
#define IDC_FIND_MODEGRP           2121
#define IDC_FIND_NORMAL            2122
#define IDC_FIND_EXTENDED          2123
#define IDC_FIND_REGEX             2124
#define IDC_FIND_HEX               2125
#define IDC_FIND_DOTALL            2126
#define IDC_FIND_STATUS            2127
#define IDC_FIND_NEXT              2130
#define IDC_FIND_COUNT             2131
#define IDC_FIND_ALL               2132
#define IDC_FIND_MARK              2133
#define IDC_FIND_REPLACE           2134
#define IDC_FIND_REPLALL           2135
#define IDC_FIND_REPLTABS          2136
#define IDC_FIND_FIFGO             2137
#define IDC_FIND_STOP              2138
#define IDC_FIND_CLOSE             2139
// incremental search bar (children of g_app.isearch)
#define IDC_IS_LABEL               2150
#define IDC_IS_EDIT                2151
#define IDC_IS_COUNT               2152
#define IDC_IS_CASE                2153
#define IDC_IS_HIGHLIGHT           2154
#define IDC_IS_CLOSE               2155
// results panel (children of g_app.results)
#define IDC_RES_VIEW               2160
#define IDC_RES_LABEL              2161
#define IDC_RES_STOP               2162
#define IDC_RES_CLOSE              2163
// regex token popup menus (TrackPopupMenu with TPM_RETURNCMD; never sent as WM_COMMAND)
#define IDM_TOK_FIRST              2200
#define IDM_TOK_HELP               2290

#endif
