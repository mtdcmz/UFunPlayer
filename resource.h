#pragma once

// Icons
#define IDI_MAINICON         101
#define IDI_SMALLICON        102

// Menu & Accelerators
#define IDR_MAINMENU         200
#define IDR_ACCEL            201

// Menu items - File
#define IDM_FILE_OPEN        1001
#define IDM_FILE_RELOAD      1002
#define IDM_FILE_CLOSE       1003
#define IDM_FILE_EXIT        1004

// Menu items - View
#define IDM_VIEW_FULLSCREEN  2001

// Menu items - Control
#define IDM_CTRL_QUALITY_L   3001
#define IDM_CTRL_QUALITY_M   3002
#define IDM_CTRL_QUALITY_H   3003

// Menu items - Help
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

// Menu items - Tools
#define IDM_TOOLS_ENABLE     5001   // shown before the user agrees to the warning
#define IDM_TOOLS_REFRESH    5002   // shown after agreeing - replaces ENABLE
#define IDM_TOOLS_EMPTY      5003   // grayed placeholder when Tools\ has no .exe

// Tool entries (dynamic, scanned from Tools\ folder) - TOOLS_MAX slots in .cpp
#define IDM_TOOLS_ITEM_0     5100   // 5100 .. 5100+TOOLS_MAX-1

// Menu items - Control > Language  (v1.2p)
#define IDM_LANG_BUILTIN_EN  6001   // permanent "English (Built-in)" entry, no file needed
#define IDM_LANG_ITEM_0      6100   // 6100 .. 6100+LANG_MAX-1, one per langs/*.lang file found

// Dialogs
#define IDD_OPEN             300
#define IDD_ABOUT            301
#define IDD_DOWNLOAD         302
#define IDD_TOOLS_WARNING    303

// Open dialog controls
#define IDC_URLEDIT          1101
#define IDC_BROWSE           1102
#define IDC_REFEDIT          1103
#define IDC_OPEN_URLLABEL    7001   // "Enter the network location..." label
#define IDC_OPEN_EXAMPLE     7002   // "Example: http://..." label
#define IDC_OPEN_SEPARATOR   7003   // "--- or browse your local files ---"
#define IDC_ADV_GROUP        7012   // "Advanced" group box
#define IDC_ADV_REF_LABEL    7013   // "URL Spoofing:" label

// About dialog controls
#define IDC_ABOUT_VERSION    7004   // "UFunPlayer 1.2p" - set from code (APP_NAME/APP_VERSION), not translated
#define IDC_ABOUT_TAGLINE    7005   // "A Standalone Unity Web Player" - translated

// Download dialog controls
#define IDC_DL_PATH          1201
#define IDC_DL_BROWSER       1202
#define IDC_DL_NOTFOUND      7006   // "The Unity Web Player runtime files were not found."
#define IDC_DL_EXPECTED_LBL  7007   // "Expected location:"
#define IDC_DL_BODY          7008   // merged, auto-wrapping explanation paragraph
#define IDC_DL_URLLABEL      7009   // "URL: " + literal link, built in code

// Tools Warning dialog controls
#define IDC_TW_BODY          7010   // merged, auto-wrapping warning paragraph
#define IDC_TW_NOASKAGAIN    7011   // "You will not be asked again after agreeing."
