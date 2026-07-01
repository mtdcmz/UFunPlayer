#pragma once

// Icons
#define IDI_MAINICON         101
#define IDI_SMALLICON        102

// Menu & Accelerators
#define IDR_MAINMENU         200
#define IDR_ACCEL            201

// Menu items – File
#define IDM_FILE_OPEN        1001
#define IDM_FILE_RELOAD      1002
#define IDM_FILE_CLOSE       1003
#define IDM_FILE_EXIT        1004

// Menu items – View
#define IDM_VIEW_FULLSCREEN  2001

// Menu items – Control
#define IDM_CTRL_QUALITY_L   3001
#define IDM_CTRL_QUALITY_M   3002
#define IDM_CTRL_QUALITY_H   3003

// Menu items – Help
#define IDM_HELP_REPO        4001
#define IDM_HELP_ABOUT       4002
#define IDM_HELP_CLEARDATA   4003

// Recent-file menu items  (10 slots)
#define IDM_RECENT_0         1100
#define IDM_RECENT_1         1101
#define IDM_RECENT_2         1102
#define IDM_RECENT_3         1103
#define IDM_RECENT_4         1104
#define IDM_RECENT_5         1105
#define IDM_RECENT_6         1106
#define IDM_RECENT_7         1107
#define IDM_RECENT_8         1108
#define IDM_RECENT_9         1109
#define IDM_RECENT_EMPTY     1110   // grayed placeholder when list is empty

// Menu items – Tools
#define IDM_TOOLS_ENABLE     5001   // shown before the user agrees to the warning
#define IDM_TOOLS_REFRESH    5002   // shown after agreeing — replaces ENABLE
#define IDM_TOOLS_EMPTY      5003   // grayed placeholder when Tools\ has no .exe

// Tool entries (dynamic, scanned from Tools\ folder) — TOOLS_MAX slots in .cpp
#define IDM_TOOLS_ITEM_0     5100   // 5100 .. 5100+TOOLS_MAX-1

// Dialogs
#define IDD_OPEN             300
#define IDD_ABOUT            301
#define IDD_DOWNLOAD         302
#define IDD_TOOLS_WARNING    303

// Open dialog controls
#define IDC_URLEDIT          1101
#define IDC_BROWSE           1102

// Download dialog controls
#define IDC_DL_PATH          1201
#define IDC_DL_BROWSER       1202
