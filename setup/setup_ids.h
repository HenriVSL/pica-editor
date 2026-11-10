// setup_ids.h - resource ids of the Pica installer (T19). CRLF, ASCII.
#ifndef PICA_SETUP_IDS_H
#define PICA_SETUP_IDS_H

#define IDI_SETUP           1

// payload (RCDATA, setup.exe only; uninst.exe has none and therefore always uninstalls)
#define IDR_PAY_PICA        101
#define IDR_PAY_README      102
#define IDR_PAY_UNINST      103
#define IDR_PAY_LICENSE     104

// wizard pages
#define IDD_WELCOME         201
#define IDD_FOLDER          202
#define IDD_TYPES           203
#define IDD_PROGRESS        204
#define IDD_FINISH          205

#define IDC_TITLE           1001
#define IDC_W_STATUS        1002
#define IDC_F_DIR           1010
#define IDC_F_BROWSE        1011
#define IDC_F_NOTE          1012
#define IDC_F_START         1013
#define IDC_F_DESK          1014
#define IDC_F_CTX           1015
#define IDC_T_DEFAULTS      1020
#define IDC_T_NONE          1021
#define IDC_P_TEXT          1030
#define IDC_P_BAR           1031
#define IDC_D_TEXT          1040
#define IDC_D_RUN           1041
#define IDC_D_REBOOT        1042
#define IDC_T_GROUP         1050
#define IDC_T_FIRST         1100

#endif
