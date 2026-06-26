// UFunPlayer.cpp
// Standalone Unity Web Player.
// Hosts the Unity Web Player ActiveX.
// https://github.com/mtdcmz/UFunPlayer

#define _WIN32_WINNT  0x0600
#define WINVER        0x0600
#define _WIN32_IE     0x0700
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <oleidl.h>
#include <oaidl.h>
#include <winreg.h>
#include <wininet.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#include "resource.h"

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------
#define APP_NAME       "UFunPlayer"
#define APP_VERSION    "1.0"
#define GITHUB_URL     "https://github.com/mtdcmz/UFunPlayer"
#define RUNTIME_DL_URL "https://github.com/mtdcmz/UFunPlayer/releases/latest/download/Runtime.zip"

// Unity Web Player ActiveX CLSID  {444785F1-DE89-4295-863A-D46C3A781394}
static const CLSID CLSID_UnityWebPlayer = {
    0x444785F1, 0xDE89, 0x4295,
    { 0x86, 0x3A, 0xD4, 0x6C, 0x3A, 0x78, 0x13, 0x94 }
};

// Custom window messages
#define WM_POSTINIT  (WM_APP + 1)   // deferred init after window is visible
#define WM_LOADFILE  (WM_APP + 2)   // lParam = heap-allocated char* path (caller frees)

// ---------------------------------------------------------------------------
//  Global state
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst     = nullptr;
static HWND      g_hwndMain  = nullptr;
static HACCEL    g_hAccel    = nullptr;

// Unity ActiveX control handles
static IOleObject*        g_pOleObj = nullptr;
static IDispatch*         g_pDisp   = nullptr;
static IOleInPlaceObject* g_pIPO    = nullptr;

// Container site object (forward declaration)
class UnityClientSite;
static UnityClientSite* g_pSite = nullptr;

// App state
static bool g_unityReady  = false;   // Unity control is alive
static bool g_gameLoaded  = false;   // a game is currently running
static bool g_fullscreen  = false;
static RECT g_savedRect   = {};

static char g_statusText[160] = "Drag a .unity3d file here, or use File > Open.";
static char g_exeDir[MAX_PATH] = {};

// Pending file from command line (used in WM_POSTINIT)
static char g_pendingFile[MAX_PATH * 2] = {};

// ---------------------------------------------------------------------------
//  Forward declarations
// ---------------------------------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK OpenDlgProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK AboutDlgProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK DownloadDlgProc(HWND, UINT, WPARAM, LPARAM);

static void  UnityDestroy();
static bool  UnityCreate(HWND hwnd, const char* src);
static void  UnityResize(int w, int h);
static void  UnitySetPropW(const wchar_t* name, const wchar_t* value);

static void  LoadFileOrUrl(const char* path);
static void  CloseGame();
static void  SetStatus(const char* text);
static void  ToggleFullscreen();

// ---------------------------------------------------------------------------
//  OLE container site
//
//  We need IOleClientSite + IOleInPlaceSite + IOleInPlaceFrame.
//  IOleInPlaceFrame is put into an inner class (UnityFrameSite) to avoid
//  the diamond-inheritance problem from IOleWindow that appears in both
//  IOleInPlaceSite and IOleInPlaceFrame.
// ---------------------------------------------------------------------------

class UnityClientSite;   // forward

// Inner class handles IOleInPlaceFrame independently.
class UnityFrameSite : public IOleInPlaceFrame
{
public:
    UnityClientSite* outer;
    explicit UnityFrameSite(UnityClientSite* o) : outer(o) {}

    // IUnknown – delegated to outer
    STDMETHODIMP         QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;

    // IOleWindow
    STDMETHODIMP GetWindow(HWND* ph) override;
    STDMETHODIMP ContextSensitiveHelp(BOOL) override { return S_OK; }

    // IOleInPlaceUIWindow
    STDMETHODIMP GetBorder(LPRECT)                      override { return INPLACE_E_NOTOOLSPACE; }
    STDMETHODIMP RequestBorderSpace(LPCBORDERWIDTHS)    override { return INPLACE_E_NOTOOLSPACE; }
    STDMETHODIMP SetBorderSpace(LPCBORDERWIDTHS)        override { return S_OK; }
    STDMETHODIMP SetActiveObject(IOleInPlaceActiveObject*, LPCOLESTR) override { return S_OK; }

    // IOleInPlaceFrame
    STDMETHODIMP InsertMenus(HMENU, LPOLEMENUGROUPWIDTHS) override { return S_OK; }
    STDMETHODIMP SetMenu(HMENU, HOLEMENU, HWND)           override { return S_OK; }
    STDMETHODIMP RemoveMenus(HMENU)                        override { return S_OK; }
    STDMETHODIMP SetStatusText(LPCOLESTR)                  override { return S_OK; }
    STDMETHODIMP EnableModeless(BOOL)                      override { return S_OK; }
    STDMETHODIMP TranslateAccelerator(LPMSG, WORD)         override { return E_NOTIMPL; }
};

// Main site: IOleClientSite + IOleInPlaceSite.
class UnityClientSite : public IOleClientSite, public IOleInPlaceSite
{
public:
    LONG           m_refs;
    HWND           m_hwnd;
    UnityFrameSite m_frame;

    explicit UnityClientSite(HWND hwnd)
        : m_refs(1), m_hwnd(hwnd), m_frame(this) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IOleClientSite)
            *ppv = static_cast<IOleClientSite*>(this);
        else if (riid == IID_IOleInPlaceSite || riid == IID_IOleWindow)
            *ppv = static_cast<IOleInPlaceSite*>(this);
        else if (riid == IID_IOleInPlaceFrame || riid == IID_IOleInPlaceUIWindow)
            *ppv = &m_frame;
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_refs);
    }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_refs);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    // IOleClientSite
    STDMETHODIMP SaveObject()                          override { return E_NOTIMPL; }
    STDMETHODIMP GetMoniker(DWORD, DWORD, IMoniker**) override { return E_NOTIMPL; }
    STDMETHODIMP GetContainer(IOleContainer** pp)      override { *pp = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP ShowObject()                          override { return S_OK; }
    STDMETHODIMP OnShowWindow(BOOL)                    override { return S_OK; }
    STDMETHODIMP RequestNewObjectLayout()              override { return E_NOTIMPL; }

    // IOleWindow (via IOleInPlaceSite)
    STDMETHODIMP GetWindow(HWND* ph) override { *ph = m_hwnd; return S_OK; }
    STDMETHODIMP ContextSensitiveHelp(BOOL) override { return S_OK; }

    // IOleInPlaceSite
    STDMETHODIMP CanInPlaceActivate()  override { return S_OK; }
    STDMETHODIMP OnInPlaceActivate()   override { return S_OK; }
    STDMETHODIMP OnUIActivate()        override { return S_OK; }
    STDMETHODIMP OnUIDeactivate(BOOL)  override { return S_OK; }
    STDMETHODIMP OnInPlaceDeactivate() override { return S_OK; }
    STDMETHODIMP DiscardUndoState()    override { return E_NOTIMPL; }
    STDMETHODIMP DeactivateAndUndo()   override { return E_NOTIMPL; }
    STDMETHODIMP Scroll(SIZE)          override { return S_OK; }

    STDMETHODIMP GetWindowContext(
        IOleInPlaceFrame**    ppFrame,
        IOleInPlaceUIWindow** ppDoc,
        LPRECT                rcPos,
        LPRECT                rcClip,
        LPOLEINPLACEFRAMEINFO pFI) override
    {
        *ppFrame = &m_frame; m_frame.AddRef();
        *ppDoc   = nullptr;
        GetClientRect(m_hwnd, rcPos);
        *rcClip  = *rcPos;
        pFI->cb            = sizeof(OLEINPLACEFRAMEINFO);
        pFI->fMDIApp       = FALSE;
        pFI->hwndFrame     = m_hwnd;
        pFI->haccel        = nullptr;
        pFI->cAccelEntries = 0;
        return S_OK;
    }

    STDMETHODIMP OnPosRectChange(LPCRECT rcPos) override
    {
        if (g_pIPO) g_pIPO->SetObjectRects(rcPos, rcPos);
        return S_OK;
    }
};

// UnityFrameSite method bodies
STDMETHODIMP UnityFrameSite::QueryInterface(REFIID riid, void** ppv) {
    return outer->QueryInterface(riid, ppv);
}
STDMETHODIMP_(ULONG) UnityFrameSite::AddRef()  { return outer->AddRef();  }
STDMETHODIMP_(ULONG) UnityFrameSite::Release() { return outer->Release(); }
STDMETHODIMP UnityFrameSite::GetWindow(HWND* ph) {
    *ph = outer->m_hwnd; return S_OK;
}

// ---------------------------------------------------------------------------
//  Status display (drawn on main window when Unity is not active)
// ---------------------------------------------------------------------------
static void SetStatus(const char* text)
{
    strncpy(g_statusText, text, sizeof(g_statusText) - 1);
    g_statusText[sizeof(g_statusText) - 1] = '\0';
    if (g_hwndMain) InvalidateRect(g_hwndMain, nullptr, TRUE);
}

static void PaintStatus(HDC hdc, const RECT& rc)
{
    // White background
    HBRUSH hbr = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);

    if (!g_statusText[0]) return;

    LOGFONTA lf = {};
    lf.lfHeight  = -16;
    lf.lfWeight  = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    strncpy(lf.lfFaceName, "Segoe UI", LF_FACESIZE - 1);
    HFONT hFont = CreateFontIndirectA(&lf);
    HFONT hOld  = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(140, 140, 140));
    RECT r = rc;
    DrawTextA(hdc, g_statusText, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, hOld);
    DeleteObject(hFont);
}

// ---------------------------------------------------------------------------
//  Bundle header parsing
//
//  Unity bundle header layout (decimal offsets):
//     0 –  7  : "UnityWeb" magic
//     8 – 12  : 4 zero bytes + 1 format-type byte
//    13 – 18  : format string  e.g. "5.x.x\0"
//    19 +     : version string e.g. "5.0.0p2\0"
// ---------------------------------------------------------------------------
struct BundleInfo { bool valid; char version[32]; int major, minor; };

static BundleInfo ParseHeader(const unsigned char* buf, size_t n)
{
    BundleInfo info = {false, "", 0, 0};
    if (n < 27 || memcmp(buf, "UnityWeb", 8) != 0) return info;
    char tmp[32] = {};
    strncpy(tmp, reinterpret_cast<const char*>(buf + 19), 31);
    int ma = 0, mi = 0;
    if (sscanf(tmp, "%d.%d", &ma, &mi) == 2) {
        info.valid = true; info.major = ma; info.minor = mi;
        strncpy(info.version, tmp, 31);
    }
    return info;
}

static BundleInfo ReadBundleFromFile(const char* path)
{
    unsigned char buf[64] = {};
    FILE* f = fopen(path, "rb");
    if (!f) return {false, "", 0, 0};
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return ParseHeader(buf, n);
}

static BundleInfo ReadBundleFromURL(const char* url)
{
    unsigned char buf[64] = {};
    HINTERNET hi = InternetOpenA(APP_NAME "/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hi) return {false, "", 0, 0};
    HINTERNET hu = InternetOpenUrlA(hi, url, nullptr, 0,
                                     INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    DWORD got = 0;
    if (hu) { InternetReadFile(hu, buf, sizeof(buf), &got); InternetCloseHandle(hu); }
    InternetCloseHandle(hi);
    return ParseHeader(buf, got);
}

// ---------------------------------------------------------------------------
//  Runtime management
//
//  Runtime definitions: registry channel value + folder name on disk.
//  Folder names must match RuntimeMgr.vbs exactly:
//    channel-string + format-version-string, concatenated without separator.
//    e.g. "Beta-5.0" + "5.x.x" = "Beta-5.05.x.x"
// ---------------------------------------------------------------------------
struct RuntimeDef {
    const char* channel;    // written to HKCU registry
    const char* folder;     // subfolder inside Runtime/mono/ and Runtime/player/
};

static RuntimeDef GetRuntimeDef(int major, int minor)
{
    if (major == 2 && minor <= 5) return {"Alpha-2.5",   "Alpha-2.52.x.x"};
    if (major == 2)               return {"Beta-2.6",    "Beta-2.62.x.x"};
    if (major == 3)               return {"Alpha-3.5",   "Alpha-3.53.x.x"};
    if (major == 4 && minor == 1) return {"Beta-4.1",    "Beta-4.13.x.x"};
    if (major == 4 && minor <= 3) return {"Beta-4.3",    "Beta-4.33.x.x"};
    if (major == 4 && minor <= 6) return {"Beta-4.6",    "Beta-4.63.x.x"};
    if (major == 4)               return {"Beta-4.7",    "Beta-4.73.x.x"};
    if (major == 5 && minor == 0) return {"Beta-5.0",    "Beta-5.05.x.x"};
    if (major == 5 && minor == 1) return {"Beta-5.1",    "Beta-5.15.x.x"};
    if (major == 5 && minor == 2) return {"Beta-5.2",    "Beta-5.25.x.x"};
    if (major == 5 && minor >= 3) return {"Stable5.3.8", "Stable5.x.x"};
    return {"Stable5.3.8", "Stable5.x.x"};
}

// Convenience: return just the channel string for display
static const char* ChannelForVersion(int major, int minor)
{
    return GetRuntimeDef(major, minor).channel;
}

// Check whether Unity Web Player ActiveX CLSID is registered
static bool IsWebPlayerInstalled()
{
    HKEY hk = nullptr;
    LSTATUS r = RegOpenKeyExA(HKEY_CLASSES_ROOT,
        "CLSID\\{444785F1-DE89-4295-863A-D46C3A781394}",
        0, KEY_READ, &hk);
    if (r == ERROR_SUCCESS) { RegCloseKey(hk); return true; }
    return false;
}

// Check that Runtime\mono\ and Runtime\player\ exist next to the exe
static bool IsRuntimePackagePresent()
{
    char p[MAX_PATH];
    sprintf(p, "%s\\Runtime\\mono",   g_exeDir); if (!PathFileExistsA(p)) return false;
    sprintf(p, "%s\\Runtime\\player", g_exeDir); if (!PathFileExistsA(p)) return false;
    return true;
}

// Recursively delete contents of a directory (the directory itself stays)
static void DeleteFolderContents(const char* path)
{
    char wild[MAX_PATH]; sprintf(wild, "%s\\*", path);
    WIN32_FIND_DATAA fd = {};
    HANDLE hf = FindFirstFileA(wild, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char fp[MAX_PATH]; sprintf(fp, "%s\\%s", path, fd.cFileName);
        SetFileAttributesA(fp, FILE_ATTRIBUTE_NORMAL);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DeleteFolderContents(fp); RemoveDirectoryA(fp);
        } else { DeleteFileA(fp); }
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
}

// Recursively copy src directory contents into dest
static void CopyFolderContents(const char* src, const char* dest)
{
    SHCreateDirectoryExA(nullptr, dest, nullptr);
    char wild[MAX_PATH]; sprintf(wild, "%s\\*", src);
    WIN32_FIND_DATAA fd = {};
    HANDLE hf = FindFirstFileA(wild, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char sp[MAX_PATH], dp[MAX_PATH];
        sprintf(sp, "%s\\%s", src, fd.cFileName);
        sprintf(dp, "%s\\%s", dest, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CopyFolderContents(sp, dp);
        } else {
            SetFileAttributesA(dp, FILE_ATTRIBUTE_NORMAL);
            CopyFileA(sp, dp, FALSE);
        }
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
}

// Switch runtime to the channel matching major.minor.
// Mirrors RuntimeMgr.vbs exactly:
//   1. Delete destination folder (3.x.x) entirely
//   2. Copy from Runtime\mono\<folder> to …\WebPlayer\mono\3.x.x
//   3. Same for player\
//   4. Write registry channel + directory keys
static bool SwitchRuntime(int major, int minor)
{
    RuntimeDef def = GetRuntimeDef(major, minor);

    // Locate source folders
    char monoSrc[MAX_PATH], playerSrc[MAX_PATH];
    sprintf(monoSrc,   "%s\\Runtime\\mono\\%s",   g_exeDir, def.folder);
    sprintf(playerSrc, "%s\\Runtime\\player\\%s", g_exeDir, def.folder);

    if (!PathFileExistsA(monoSrc) || !PathFileExistsA(playerSrc)) {
        // Tell the user exactly which folder is missing so they know what to fix
        char msg[512];
        sprintf(msg,
            "Runtime folder not found for channel [%s]:\n\n"
            "  %s\n\n"
            "Please check your Runtime package.\n"
            "Folder names must follow RuntimeMgr.vbs convention,\n"
            "e.g. Runtime\\mono\\Beta-5.05.x.x\\",
            def.channel, monoSrc);
        MessageBoxA(g_hwndMain, msg, "Runtime Switch Failed", MB_ICONWARNING);
        return false;
    }

    // Build destination paths
    char userProfile[MAX_PATH];
    ExpandEnvironmentStringsA("%USERPROFILE%", userProfile, MAX_PATH);
    char wpBase[MAX_PATH];
    sprintf(wpBase, "%s\\AppData\\LocalLow\\Unity\\WebPlayer", userProfile);

    char monoDst[MAX_PATH], playerDst[MAX_PATH];
    sprintf(monoDst,   "%s\\mono\\3.x.x",   wpBase);
    sprintf(playerDst, "%s\\player\\3.x.x", wpBase);

    // Step 1: delete destination folders entirely (matches VBS)
    DeleteFolderContents(monoDst);   RemoveDirectoryA(monoDst);
    DeleteFolderContents(playerDst); RemoveDirectoryA(playerDst);

    // Step 2: copy new runtime
    CopyFolderContents(monoSrc,   monoDst);
    CopyFolderContents(playerSrc, playerDst);

    // Step 3: update registry (mirrors RuntimeMgr.vbs ws.RegWrite)
    HKEY hk = nullptr;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Unity\\WebPlayer",
            0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        RegSetValueExA(hk, "UnityWebPlayerReleaseChannel", 0, REG_SZ,
                       (const BYTE*)def.channel, (DWORD)strlen(def.channel) + 1);
        RegSetValueExA(hk, "Directory", 0, REG_SZ,
                       (const BYTE*)wpBase,       (DWORD)strlen(wpBase)       + 1);
        RegCloseKey(hk);
    }
    return true;
}

// Silently run Runtime\UnityWebPlayer.exe /S and wait for it
static bool SilentInstallWebPlayer()
{
    char installer[MAX_PATH];
    sprintf(installer, "%s\\Runtime\\UnityWebPlayer.exe", g_exeDir);
    if (!PathFileExistsA(installer)) return false;

    SHELLEXECUTEINFOA sei = {};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb       = "open";
    sei.lpFile       = installer;
    sei.lpParameters = "/S";
    sei.nShow        = SW_HIDE;
    if (!ShellExecuteExA(&sei) || !sei.hProcess) return false;
    WaitForSingleObject(sei.hProcess, 120000);  // up to 2 min
    CloseHandle(sei.hProcess);
    return IsWebPlayerInstalled();
}

// ---------------------------------------------------------------------------
//  Unity ActiveX control lifecycle
// ---------------------------------------------------------------------------

// Set a BSTR property on the Unity player via IDispatch
static void UnitySetPropW(const wchar_t* name, const wchar_t* value)
{
    if (!g_pDisp) return;
    DISPID dispid;
    BSTR bname = SysAllocString(name);
    HRESULT hr = g_pDisp->GetIDsOfNames(IID_NULL, &bname, 1, LOCALE_USER_DEFAULT, &dispid);
    SysFreeString(bname);
    if (FAILED(hr)) return;

    VARIANT var; VariantInit(&var);
    var.vt     = VT_BSTR;
    var.bstrVal = SysAllocString(value);

    DISPID namedArg = DISPID_PROPERTYPUT;
    DISPPARAMS params = { &var, &namedArg, 1, 1 };
    g_pDisp->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT,
                    DISPATCH_PROPERTYPUT, &params, nullptr, nullptr, nullptr);
    VariantClear(&var);
}

// Convenience: set property from ANSI strings
static void UnitySetPropA(const char* name, const char* value)
{
    int nl = MultiByteToWideChar(CP_ACP, 0, name,  -1, nullptr, 0);
    int vl = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
    wchar_t* wn = new wchar_t[nl];
    wchar_t* wv = new wchar_t[vl];
    MultiByteToWideChar(CP_ACP, 0, name,  -1, wn, nl);
    MultiByteToWideChar(CP_ACP, 0, value, -1, wv, vl);
    UnitySetPropW(wn, wv);
    delete[] wn;
    delete[] wv;
}

// Resize the in-place object to fill the given client dimensions
static void UnityResize(int w, int h)
{
    if (!g_pIPO || w <= 0 || h <= 0) return;
    RECT rc = { 0, 0, w, h };
    g_pIPO->SetObjectRects(&rc, &rc);
}

// Destroy Unity control and free DLL
static void UnityDestroy()
{
    g_unityReady = false;
    g_gameLoaded = false;

    if (g_pIPO) {
        g_pIPO->UIDeactivate();
        g_pIPO->InPlaceDeactivate();
        g_pIPO->Release();
        g_pIPO = nullptr;
    }
    if (g_pDisp) { g_pDisp->Release(); g_pDisp = nullptr; }
    if (g_pOleObj) {
        g_pOleObj->Close(OLECLOSE_NOSAVE);
        g_pOleObj->Release();
        g_pOleObj = nullptr;
    }
    if (g_pSite) { g_pSite->Release(); g_pSite = nullptr; }

    // Force unload of Unity DLL so the next CoCreateInstance picks up
    // the freshly-switched runtime files from disk
    CoFreeUnusedLibrariesEx(0, 0);

    if (g_hwndMain) InvalidateRect(g_hwndMain, nullptr, TRUE);
}

// Create Unity control, set properties (src etc.), then activate in-place.
// 'src' may be a local Windows path or an http:// URL.
static bool UnityCreate(HWND hwnd, const char* src)
{
    g_pSite = new UnityClientSite(hwnd);

    HRESULT hr = CoCreateInstance(CLSID_UnityWebPlayer, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_IOleObject, (void**)&g_pOleObj);
    if (FAILED(hr)) {
        g_pSite->Release(); g_pSite = nullptr;
        return false;
    }

    g_pOleObj->SetClientSite(g_pSite);
    OleSetContainedObject(g_pOleObj, TRUE);

    // IDispatch for property access
    g_pOleObj->QueryInterface(IID_IDispatch, (void**)&g_pDisp);

    // Set properties BEFORE activation (same order as UniPlayer)
    UnitySetPropA("src",                src);
    UnitySetPropA("backgroundcolor",    "000000");
    UnitySetPropA("bordercolor",        "000000");
    UnitySetPropA("disableContextMenu", "false");
    UnitySetPropA("disableFullscreen",  "false");

    RECT rc = {};
    GetClientRect(hwnd, &rc);

    hr = g_pOleObj->DoVerb(OLEIVERB_INPLACEACTIVATE,
                            nullptr, g_pSite, 0, hwnd, &rc);
    if (FAILED(hr)) {
        UnityDestroy();
        return false;
    }

    g_pOleObj->QueryInterface(IID_IOleInPlaceObject, (void**)&g_pIPO);
    if (g_pIPO) g_pIPO->SetObjectRects(&rc, &rc);

    g_unityReady = true;
    return true;
}

// ---------------------------------------------------------------------------
//  Game loading – the central function
// ---------------------------------------------------------------------------
static void LoadFileOrUrl(const char* path)
{
    bool isUrl = (PathIsURLA(path) == TRUE);

    // 1. Detect bundle version
    SetStatus("Loading Player...");
    UpdateWindow(g_hwndMain);

    BundleInfo info = isUrl ? ReadBundleFromURL(path) : ReadBundleFromFile(path);

    // 2. Destroy existing control (unloads old DLL)
    UnityDestroy();

    // 3. Switch runtime if version known
    if (info.valid) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Loading Player...  [Unity %s -> %s]",
                 info.version, ChannelForVersion(info.major, info.minor));
        SetStatus(msg);
        UpdateWindow(g_hwndMain);

        SwitchRuntime(info.major, info.minor);

        // Give the OS a moment to finish releasing the old DLL
        CoFreeUnusedLibrariesEx(0, 0);
        Sleep(150);
    }

    // 4. Create Unity control with the new src
    if (!UnityCreate(g_hwndMain, path)) {
        SetStatus("Error: failed to create Unity player. "
                  "Check that Unity Web Player is installed.");
        return;
    }

    g_gameLoaded = true;
    // Unity control now covers client area; WM_PAINT is suppressed by WS_CLIPCHILDREN
}

static void CloseGame()
{
    UnityDestroy();
    SetStatus("Drag a .unity3d file here, or use File > Open.");
}

// ---------------------------------------------------------------------------
//  Fullscreen toggle
// ---------------------------------------------------------------------------
static void ToggleFullscreen()
{
    if (!g_fullscreen) {
        GetWindowRect(g_hwndMain, &g_savedRect);
        DWORD style = GetWindowLongA(g_hwndMain, GWL_STYLE);
        SetWindowLongA(g_hwndMain, GWL_STYLE,
            style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                      WS_MAXIMIZEBOX | WS_MINIMIZEBOX));
        HMONITOR hm = MonitorFromWindow(g_hwndMain, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
        GetMonitorInfoA(hm, &mi);
        SetWindowPos(g_hwndMain, HWND_TOPMOST,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right  - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        g_fullscreen = true;
    } else {
        DWORD style = GetWindowLongA(g_hwndMain, GWL_STYLE);
        SetWindowLongA(g_hwndMain, GWL_STYLE,
            style | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                    WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
        SetWindowPos(g_hwndMain, HWND_NOTOPMOST,
                     g_savedRect.left, g_savedRect.top,
                     g_savedRect.right  - g_savedRect.left,
                     g_savedRect.bottom - g_savedRect.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        g_fullscreen = false;
    }
}

// ---------------------------------------------------------------------------
//  Dialogs
// ---------------------------------------------------------------------------
static char g_openResult[MAX_PATH * 2] = {};

INT_PTR CALLBACK OpenDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG:
        if (g_openResult[0]) SetDlgItemTextA(hDlg, IDC_URLEDIT, g_openResult);
        EnableWindow(GetDlgItem(hDlg, IDOK), g_openResult[0] != '\0');
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {

        case IDC_URLEDIT:
            if (HIWORD(wp) == EN_CHANGE) {
                char tmp[4]; GetDlgItemTextA(hDlg, IDC_URLEDIT, tmp, sizeof(tmp));
                EnableWindow(GetDlgItem(hDlg, IDOK), tmp[0] != '\0');
            }
            break;

        case IDC_BROWSE: {
            OPENFILENAMEA ofn = {};
            char file[MAX_PATH] = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hDlg;
            ofn.lpstrFilter = "Unity Bundle (*.unity3d)\0*.unity3d\0All Files\0*.*\0";
            ofn.lpstrFile   = file;
            ofn.nMaxFile    = MAX_PATH;
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            ofn.lpstrTitle  = "Open Unity Bundle";
            if (GetOpenFileNameA(&ofn)) {
                SetDlgItemTextA(hDlg, IDC_URLEDIT, file);
                EnableWindow(GetDlgItem(hDlg, IDOK), TRUE);
            }
            break;
        }

        case IDOK: {
            char buf[MAX_PATH * 2];
            GetDlgItemTextA(hDlg, IDC_URLEDIT, buf, sizeof(buf));
            if (buf[0]) { strncpy(g_openResult, buf, sizeof(g_openResult)-1); EndDialog(hDlg, IDOK); }
            break;
        }
        case IDCANCEL: EndDialog(hDlg, IDCANCEL); break;
        }
        return TRUE;
    }
    return FALSE;
}

static bool ShowOpenDialog()
{
    g_openResult[0] = '\0';
    return DialogBoxA(g_hInst, MAKEINTRESOURCEA(IDD_OPEN),
                      g_hwndMain, OpenDlgProc) == IDOK && g_openResult[0];
}

INT_PTR CALLBACK AboutDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM)
{
    if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL))
        EndDialog(hDlg, 0);
    return msg == WM_INITDIALOG ? TRUE : FALSE;
}

INT_PTR CALLBACK DownloadDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG: {
        char p[MAX_PATH]; sprintf(p, "%s\\Runtime", g_exeDir);
        SetDlgItemTextA(hDlg, IDC_DL_PATH, p);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_DL_BROWSER:
            ShellExecuteA(hDlg, "open", RUNTIME_DL_URL, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDOK: case IDCANCEL:
            EndDialog(hDlg, LOWORD(wp)); break;
        }
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
//  Main window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE:
        DragAcceptFiles(hwnd, TRUE);
        PostMessageA(hwnd, WM_POSTINIT, 0, 0);
        return 0;

    case WM_POSTINIT: {
        // STEP 1: Runtime/ folder must exist FIRST, regardless of install state
        if (!IsRuntimePackagePresent()) {
            SetStatus("Runtime not found, please download Runtime.zip.");
            ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);
            DialogBoxA(g_hInst, MAKEINTRESOURCEA(IDD_DOWNLOAD), hwnd, DownloadDlgProc);
            if (!IsRuntimePackagePresent()) {
                SetStatus("Runtime not found. Download Runtime.zip from GitHub and extract next to UFunPlayer.exe.");
                return 0;
            }
        }

        // STEP 2: Install base Unity Web Player if not yet registered
        if (!IsWebPlayerInstalled()) {
            SetStatus("Initializing player...");
            InvalidateRect(hwnd, nullptr, TRUE); UpdateWindow(hwnd);
            if (!SilentInstallWebPlayer()) {
                SetStatus("Initialization failed. Check Runtime\\UnityWebPlayer.exe.");
                return 0;
            }
        }

        // STEP 3: Ready — show default status
        SetStatus("Drag a .unity3d file here, or use File > Open.");

        // STEP 4: Load command-line file if provided
        if (g_pendingFile[0]) {
            char tmp[MAX_PATH * 2];
            strncpy(tmp, g_pendingFile, sizeof(tmp) - 1);
            g_pendingFile[0] = '\0';
            LoadFileOrUrl(tmp);
        }
        return 0;
    }

    case WM_LOADFILE: {
        char* p = reinterpret_cast<char*>(lp);
        if (p) { LoadFileOrUrl(p); delete[] p; }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_FILE_OPEN:
            if (ShowOpenDialog()) LoadFileOrUrl(g_openResult);
            break;
        case IDM_FILE_EXIT:
            DestroyWindow(hwnd); break;
        case IDM_VIEW_FULLSCREEN:
            ToggleFullscreen(); break;
        case IDM_HELP_REPO:
            ShellExecuteA(hwnd, "open", GITHUB_URL, nullptr, nullptr, SW_SHOWNORMAL); break;
        case IDM_HELP_ABOUT:
            DialogBoxA(g_hInst, MAKEINTRESOURCEA(IDD_ABOUT), hwnd, AboutDlgProc); break;
        }
        return 0;

    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        if (DragQueryFileA(hd, 0xFFFFFFFF, nullptr, 0) > 0) {
            char* buf = new char[MAX_PATH];
            DragQueryFileA(hd, 0, buf, MAX_PATH);
            PostMessageA(hwnd, WM_LOADFILE, 0, (LPARAM)buf);
        }
        DragFinish(hd);
        return 0;
    }

    case WM_SIZE:
        if (g_unityReady) UnityResize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!g_gameLoaded) {
            RECT rc; GetClientRect(hwnd, &rc);
            PaintStatus(hdc, rc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        if (g_gameLoaded) return 1;   // Unity draws its own background
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_F11) { ToggleFullscreen(); return 0; }
        break;

    case WM_DESTROY:
        UnityDestroy();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
//  WinMain
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow)
{
    g_hInst = hInst;

    // Exe directory
    GetModuleFileNameA(nullptr, g_exeDir, MAX_PATH);
    PathRemoveFileSpecA(g_exeDir);

    // Command-line argument = file or URL to open
    if (__argc >= 2 && __argv[1][0])
        strncpy(g_pendingFile, __argv[1], sizeof(g_pendingFile) - 1);

    OleInitialize(nullptr);

    // Register window class (WS_CLIPCHILDREN keeps Unity child from being
    // painted over when the main window receives WM_PAINT)
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconA(hInst, MAKEINTRESOURCEA(IDI_MAINICON));
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName  = MAKEINTRESOURCEA(IDR_MAINMENU);
    wc.lpszClassName = "UFunPlayerWnd";
    wc.hIconSm       = LoadIconA(hInst, MAKEINTRESOURCEA(IDI_SMALLICON));
    RegisterClassExA(&wc);

    g_hAccel = LoadAcceleratorsA(hInst, MAKEINTRESOURCEA(IDR_ACCEL));

    g_hwndMain = CreateWindowExA(
        WS_EX_ACCEPTFILES,
        "UFunPlayerWnd",
        APP_NAME " " APP_VERSION,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 860, 660,
        nullptr, nullptr, hInst, nullptr);

    if (!g_hwndMain) { OleUninitialize(); return 1; }

    ShowWindow(g_hwndMain, nShow);
    UpdateWindow(g_hwndMain);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0)) {
        if (!TranslateAcceleratorA(g_hwndMain, g_hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    OleUninitialize();
    return (int)msg.wParam;
}