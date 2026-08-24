// UFunPlayer.cpp
// Standalone Unity Web Player.
// Hosts the Unity Web Player ActiveX.
// https://github.com/mtdcmz/UFunPlayer

#define _WIN32_WINNT  0x0600
#define WINVER        0x0600
#define _WIN32_IE     0x0700
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

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
#include <urlmon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>

#include "resource.h"
#include <MinHook.h>
#include <d3d9.h>
#include <dxgi.h>

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------
#define APP_NAME       L"UFunPlayer"
#define APP_VERSION    L"1.4p"
#define GITHUB_URL     L"https://github.com/mtdcmz/UFunPlayer"
#define RUNTIME_DL_URL L"https://github.com/mtdcmz/UFunPlayer/releases/latest/download/Runtime.zip"

// Registry keys
#define REG_ROOT_KEY     L"Software\\UFunPlayer"
#define REG_MRU_KEY      REG_ROOT_KEY L"\\RecentFiles"
#define REG_SETTINGS_KEY REG_ROOT_KEY L"\\Settings"
#define MRU_MAX        10
#define TOOLS_MAX      64

// Language pack limits
#define LANG_MAX          32
#define MAX_LANG_STRINGS  220
#define LANG_KEY_LEN      40
#define LANG_VAL_LEN      1024

// Unity Web Player ActiveX CLSID  {444785F1-DE89-4295-863A-D46C3A781394}
static const CLSID CLSID_UnityWebPlayer = {
    0x444785F1,0xDE89,0x4295,{0x86,0x3A,0xD4,0x6C,0x3A,0x78,0x13,0x94}
};

// Custom window messages
#define WM_POSTINIT  (WM_APP + 1)
#define WM_LOADFILE  (WM_APP + 2)

// Timer for delayed runtime hook installation (webplayer_win.dll loads
// asynchronously after DoVerb, not during UnityCreate).
#define TIMER_ID_INSTALL_HOOK   9550
#define HOOK_RETRY_INTERVAL_MS  200
#define HOOK_MAX_RETRIES        50   // 50 * 200ms = 10s max wait

// ---------------------------------------------------------------------------
//  Global state
//
//  Strings are wide (wchar_t) everywhere except:
//    - BundleInfo::version  (plain ASCII from binary format)
//    - LangString::key      (internal identifiers)
//    - Raw UTF-8 bytes during .lang file parsing
// ---------------------------------------------------------------------------
static HINSTANCE g_hInst    = nullptr;
static HWND      g_hwndMain = nullptr;
static HACCEL    g_hAccel   = nullptr;

// Unity ActiveX control handles
static IOleObject*        g_pOleObj = nullptr;
static IDispatch*         g_pDisp   = nullptr;
static IOleInPlaceObject* g_pIPO    = nullptr;

class UnityClientSite;
static UnityClientSite* g_pSite = nullptr;

// App state
static bool  g_unityReady = false;
static bool  g_gameLoaded = false;
static bool  g_fullscreen = false;
static RECT  g_savedRect  = {};
static HMENU g_savedMenu  = nullptr;   // menu detached during fullscreen

static wchar_t g_currentPath[MAX_PATH * 2] = {};

static wchar_t g_statusText[160] = L"Drag a .unity3d file here, or use File > Open.";
static wchar_t g_exeDir[MAX_PATH] = {};
static wchar_t g_pendingFile[MAX_PATH * 2] = {};
static wchar_t g_pendingReferer[MAX_PATH * 2] = {};   // referer from cmdline/Open dialog
static wchar_t g_currentReferer[MAX_PATH * 2] = {};   // referer for the loaded game
static HANDLE  g_hSingleInstance = nullptr;            // mutex preventing multiple launches

// OCX IAT hook state: patch the OCX's urlmon!RegisterBindStatusCallback import
// so src downloads carry our Referer. Re-patched per UnityCreate — the OCX may
// be unloaded between games.
static HRESULT (WINAPI *g_origRegisterBindStatusCallback)(IBindCtx*,IBindStatusCallback*,IBindStatusCallback**,DWORD) = nullptr;

// OCX IAT hook: intercept the OCX's LoadLibraryW so runtime hooks install the
// moment webplayer_win.dll loads (timer polling was too slow).
static HMODULE (WINAPI *g_origLoadLibraryW)(LPCWSTR) = nullptr;
static HMODULE (WINAPI *g_origLoadLibraryExW)(LPCWSTR, HANDLE, DWORD) = nullptr;
static HMODULE (WINAPI *g_origLoadLibraryA)(LPCSTR) = nullptr;
static HMODULE (WINAPI *g_origLoadLibraryExA)(LPCSTR, HANDLE, DWORD) = nullptr;

// ---- Runtime inline hooks (webplayer_win.dll) ----
// Spoof Application.absoluteURL / srcValue / webSecurityHostUrl from
// g_currentReferer + filename so URL-based anti-piracy checks pass.
// RVAs are specific to Unity 4.7.2f1; other versions skip hooking.
static char  g_spoofedUrl[1024] = {};          // UTF-8, empty = no spoof
static void* g_rtHookAbsURL  = nullptr;        // trampoline for sub_101AB81D
static void* g_rtHookSrcVal  = nullptr;        // trampoline for sub_101AB80B
static void* g_rtHookTgtAbsURL = nullptr;      // target addr (for MH_RemoveHook)
static void* g_rtHookTgtSrcVal = nullptr;      // target addr (for MH_RemoveHook)
static bool  g_rtHookTried   = false;          // avoid re-attempting per load
static int   g_hookRetryCnt  = 0;              // timer-based retry counter

// file:// URL of the current game data file. Used as the base URL for
// resolving relative WWW downloads (multi-bundle games).
static wchar_t g_gameFileUrl[MAX_PATH * 2] = {};
typedef HRESULT (WINAPI *CoInternetCombineUrlFn)(LPCWSTR, LPCWSTR, DWORD, LPWSTR, DWORD, DWORD*, DWORD);
static CoInternetCombineUrlFn g_pCoInternetCombineUrl = nullptr;

// ---- Experimental: frame-rate override hooks ----
// Throttle chain: npUnity3D32.dll pumps UnityWinWebLoop at the rate reported
// by UnityGetPlayerTargetFPS(); the engine's internal frame wait uses
// targetFrameRate/vSyncCount; the GPU-side Present blocks per the (vsync-
// derived) presentation interval. The hooks below neutralize all three.
static int   g_fpsTarget    = 0;            // user FPS, 0 = off (from registry)
static int   g_rtLoopCalls  = 0;            // completed UnityWinWebLoop pumps
static void* g_rtHookTgtGetFPS  = nullptr;  // UnityGetPlayerTargetFPS (for MH_RemoveHook)
static void* g_rtOrigGetFPS    = nullptr;   // trampoline
static void* g_rtHookTgtLoop   = nullptr;   // UnityWinWebLoop (for MH_RemoveHook)
static void* g_rtOrigLoop      = nullptr;   // trampoline
static void* g_rtHookTgtSetTfr = nullptr;   // Application::set_targetFrameRate
static void* g_rtOrigSetTfr    = nullptr;   // trampoline
static void* g_rtHookTgtSetVsy = nullptr;   // QualitySettings::set_vSyncCount
static void* g_rtOrigSetVsy    = nullptr;   // trampoline

// ---- Experimental: D3D9 interception ----
// Direct3DCreate9 is resolved dynamically; hooking the function body (not an
// IAT slot — GetProcAddress is a delay-load import here and patches get
// wiped on first call) lets us force an IMMEDIATE PresentationInterval in
// CreateDevice/Reset.
typedef IDirect3D9* (WINAPI *D3DCreate9Fn)(UINT);
typedef HRESULT (WINAPI *D3DCreateDeviceFn)(IDirect3D9*, UINT, D3DDEVTYPE,
                                            HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                            IDirect3DDevice9**);
typedef HRESULT (WINAPI *D3DResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (WINAPI *D3DPresentFn)(IDirect3DDevice9*, const RECT*,
                                       const RECT*, HWND, const RGNDATA*);
static void* g_rtHookTgtD3D9 = nullptr;      // Direct3DCreate9 (for MH_RemoveHook)
static D3DCreate9Fn    g_origD3DCreate9    = nullptr;
static D3DCreateDeviceFn g_origD3DCreateDevice = nullptr;
static D3DResetFn      g_origD3DReset      = nullptr;
static D3DPresentFn    g_origD3DPresent    = nullptr;
static void** g_d3d9VtSlotCreate   = nullptr;  // &IDirect3D9::vft[16]
static void** g_devVtSlotReset     = nullptr;  // &IDirect3DDevice9::vft[16]
static void** g_devVtSlotPresent   = nullptr;  // &IDirect3DDevice9::vft[17]

// ---- Experimental: DXGI interception (D3D11 path, Unity 5.x) ----
// Swap chains come from IDXGIFactory2::CreateSwapChainForHwnd (vtable slot
// 15; the legacy CreateSwapChain is never called). Present takes the sync
// interval per call — force 0.
typedef HRESULT (WINAPI *DxgiCreateFactoryFn)(REFIID, void**);
// MinGW's dxgi.h predates DXGI 1.2 — declare the DXGI 1.2 pieces by hand.
interface IDXGISwapChain1;
struct DXGI_SWAP_CHAIN_FULLSCREEN_DESC;
typedef HRESULT (WINAPI *DxgiCreateSwapChainForHwndFn)(void*, IUnknown*,
                                                       HWND, const void*,
                                                       const void*,
                                                       IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT (WINAPI *DxgiPresentFn)(IDXGISwapChain*, UINT, UINT);
static void* g_rtHookTgtDxgiF  = nullptr;    // CreateDXGIFactory (for MH_RemoveHook)
static void* g_rtHookTgtDxgiF1 = nullptr;    // CreateDXGIFactory1 (for MH_RemoveHook)
static DxgiCreateFactoryFn g_origCreateDxgiFactory  = nullptr;
static DxgiCreateFactoryFn g_origCreateDxgiFactory1 = nullptr;
static void** g_dxgiFtSlotCreateSwapChain = nullptr;  // &IDXGIFactory2::vft[15]
static void* g_origDxgiCreateSwapChain = nullptr;    // original CreateSwapChainForHwnd
static void** g_scVtSlotPresent = nullptr;   // &IDXGISwapChain::vft[8]
static DxgiPresentFn g_origSwapChainPresent = nullptr;

// ---- Adaptive loader-rate compensation ----
// The loader schedules pump ticks as period = FLOOR + 1000/report (ms),
// where `report` is what our UnityGetPlayerTargetFPS hook returns and
// FLOOR (≈ one refresh period) is pacing we cannot remove. The two delays
// SERIALIZE, so reporting the user's target verbatim self-throttles. We
// measure the achieved period every 60 queries, track FLOOR with an EMA,
// and solve `report` for the user's target.
static DWORD g_monitorRefreshHz = 60;
static double g_baseTickMs = 16.7;    // EMA of the inherent per-tick floor
static int    g_reportFps  = 1000;   // value returned to the loader
static LARGE_INTEGER g_windowStart = {};  // QPC of current measure window
static volatile LONG g_fpsQueryCount = 0; // loader FPS queries (adaptive step)
static LARGE_INTEGER g_qpcFreq = {};      // QPC frequency
// While the override is active the engine's own targetFrameRate is forced
// high so its internal slot wait never throttles — the loader timer (via
// our adaptive report value) is the single pacer.
static const int ENGINE_TFR_ACTIVE = 1000;

static int ComputeReportFpsFor(double baseMs)
{
    if (g_fpsTarget <= 0) return 0;
    double period = 1000.0 / g_fpsTarget;
    if (period <= baseMs + 2.0) return 1000;
    int r = (int)(1000.0 / (period - baseMs) + 0.5);
    if (r < 1) r = 1;
    if (r > 1000) r = 1000;
    return r;
}

// Adaptive compensation step: called every 60 FPS queries (~1 s). Windows
// outside 0.2–4 s are ignored (engine init / loading stalls would poison
// the estimate).
static void FpsAdaptiveStep()
{
    if (!g_qpcFreq.QuadPart || !g_windowStart.QuadPart) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double windowMs = 1000.0 * (now.QuadPart - g_windowStart.QuadPart)
                    / g_qpcFreq.QuadPart;
    QueryPerformanceCounter(&g_windowStart);
    if (windowMs < 200.0 || windowMs > 4000.0) return;
    double periodAvg = windowMs / 60.0;
    double reportContrib = (g_reportFps >= 1000) ? 1.0 : 1000.0 / g_reportFps;
    double baseMeas = periodAvg - reportContrib;
    if (baseMeas < 0.0) baseMeas = 0.0;
    if (baseMeas > 250.0) baseMeas = 250.0;
    g_baseTickMs = 0.6 * g_baseTickMs + 0.4 * baseMeas;
    g_reportFps = ComputeReportFpsFor(g_baseTickMs);
}

// defined later (after the runtime-hook machinery they belong to)
static void RestoreVtableSlot(void** slot, void* savedOrig);

// Forward declarations: UnityDestroy (above) needs to disable hooks before
// the runtime is unloaded, so it references these symbols defined later.
typedef void* (__cdecl *MonoStrNewFn)(const char*, int);
typedef void* (__cdecl *GetterFn)();
static MonoStrNewFn g_rtMonoStrNew = nullptr;
static GetterFn     g_origAbsURL   = nullptr;
static GetterFn     g_origSrcVal   = nullptr;

// MRU list
static wchar_t g_mruList[MRU_MAX][MAX_PATH * 2];
static wchar_t g_mruReferer[MRU_MAX][MAX_PATH * 2];
static wchar_t g_mruRealSrc[MRU_MAX][MAX_PATH * 2]; // Actual source path passed to Unity
static int     g_mruCount = 0;

// Tools menu state
static bool     g_toolsEnabled = false;
static wchar_t  g_toolsList[TOOLS_MAX][MAX_PATH];
static wchar_t  g_toolsName[TOOLS_MAX][MAX_PATH];
static int      g_toolsCount = 0;

// Language pack state
struct LangString { char key[LANG_KEY_LEN]; wchar_t value[LANG_VAL_LEN]; };
static LangString g_strings[MAX_LANG_STRINGS];
static int         g_stringCount = 0;

struct LangFileInfo { wchar_t path[MAX_PATH]; wchar_t code[64]; wchar_t name[128]; };
static LangFileInfo g_langFiles[LANG_MAX];
static int          g_langFileCount = 0;
static wchar_t      g_currentLangCode[64] = L"";   // L"" = built-in English

// ---------------------------------------------------------------------------
//  Forward declarations
// ---------------------------------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND,UINT,WPARAM,LPARAM);
INT_PTR CALLBACK OpenDlgProc(HWND,UINT,WPARAM,LPARAM);
INT_PTR CALLBACK AboutDlgProc(HWND,UINT,WPARAM,LPARAM);
INT_PTR CALLBACK DownloadDlgProc(HWND,UINT,WPARAM,LPARAM);
INT_PTR CALLBACK ToolsWarningDlgProc(HWND,UINT,WPARAM,LPARAM);
INT_PTR CALLBACK ExperimentalDlgProc(HWND,UINT,WPARAM,LPARAM);
static void UnityDestroy();
static bool UnityCreate(HWND,const wchar_t*);
static void UnityResize(int,int);
static void LoadFileOrUrl(const wchar_t*,const wchar_t*refererArg=nullptr);
static void ParseCmdArg(const wchar_t* arg,wchar_t* outGame,size_t gameCap,wchar_t* outRef,size_t refCap);
static void ReloadGame();
static void CloseGame();
static void InstallOcxRefererHook();
static void InstallOcxLoadLibraryHook();
static void InstallOcxUrlResolveHook();
static bool InstallRuntimeHooks();
static void SetStatus(const wchar_t*);
static void ToggleFullscreen();
static void RebuildFileMenu();
static void RebuildToolsMenu();
static void ScanToolsFolder();
static void EnableTools();
static void LaunchTool(const wchar_t*);
static void ClearUserData();
static const wchar_t* LS(const char* key);
static void InitDefaultStrings();
static void ScanLangsFolder();
static void ApplyLanguage(HWND hwnd, const wchar_t* code);
static void ApplyMenuLanguage();
static void RebuildLanguageMenu();

// ---------------------------------------------------------------------------
//  OLE container site
//
//  We need IOleClientSite + IOleInPlaceSite + IOleInPlaceFrame.
//  IOleInPlaceFrame is put into an inner class (UnityFrameSite) to avoid
//  the diamond-inheritance problem from IOleWindow.
// ---------------------------------------------------------------------------

class UnityClientSite;

class UnityFrameSite : public IOleInPlaceFrame {
public:
    UnityClientSite* outer;
    explicit UnityFrameSite(UnityClientSite* o) : outer(o) {}
    STDMETHODIMP         QueryInterface(REFIID,void**) override;
    STDMETHODIMP_(ULONG) AddRef()  override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP GetWindow(HWND* ph) override;
    STDMETHODIMP ContextSensitiveHelp(BOOL) override { return S_OK; }
    STDMETHODIMP GetBorder(LPRECT)                      override { return INPLACE_E_NOTOOLSPACE; }
    STDMETHODIMP RequestBorderSpace(LPCBORDERWIDTHS)    override { return INPLACE_E_NOTOOLSPACE; }
    STDMETHODIMP SetBorderSpace(LPCBORDERWIDTHS)        override { return S_OK; }
    STDMETHODIMP SetActiveObject(IOleInPlaceActiveObject*,LPCOLESTR) override { return S_OK; }
    STDMETHODIMP InsertMenus(HMENU,LPOLEMENUGROUPWIDTHS) override { return S_OK; }
    STDMETHODIMP SetMenu(HMENU,HOLEMENU,HWND)            override { return S_OK; }
    STDMETHODIMP RemoveMenus(HMENU)                      override { return S_OK; }
    STDMETHODIMP SetStatusText(LPCOLESTR)                override { return S_OK; }
    STDMETHODIMP EnableModeless(BOOL)                    override { return S_OK; }
    STDMETHODIMP TranslateAccelerator(LPMSG,WORD)        override { return E_NOTIMPL; }
};

// ---------------------------------------------------------------------------
//  Referer injection via URL Moniker binding
//
//  UnityBindCallback wraps an existing IBindStatusCallback and adds
//  IHttpNegotiate::BeginningTransaction(), which injects a "Referer:" header.
//  It is used in two places:
//    1. Our own URL probes/downloads (URLOpenBlockingStreamW) in ReadBundleFromURL.
//    2. The OCX IAT hook (InstallOcxRefererHook): we patch the loader's import
//       of urlmon!RegisterBindStatusCallback so the control's own src bind goes
//       through this wrapper, carrying the referer the user passed in.
//
//  g_currentReferer is read at bind time; set it before UnityCreate().
//  Empty referer -> no injection, control behaves as before.
// ---------------------------------------------------------------------------
class UnityBindCallback : public IBindStatusCallback, public IHttpNegotiate {
public:
    LONG m_refs;
    wchar_t m_referer[MAX_PATH * 2];
    IBindStatusCallback* m_inner;   // original callback in the bind ctx (may be null)

    UnityBindCallback(const wchar_t* referer, IBindStatusCallback* inner)
        : m_refs(1), m_inner(inner) {
        m_referer[0] = L'\0';
        if (referer) { wcsncpy(m_referer, referer, (sizeof(m_referer)/sizeof(wchar_t))-1);
                       m_referer[(sizeof(m_referer)/sizeof(wchar_t))-1] = L'\0'; }
        if (m_inner) m_inner->AddRef();
    }
    ~UnityBindCallback(){ if (m_inner) m_inner->Release(); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid==IID_IUnknown || riid==IID_IBindStatusCallback) *ppv=static_cast<IBindStatusCallback*>(this);
        else if (riid==IID_IHttpNegotiate) *ppv=static_cast<IHttpNegotiate*>(this);
        else { *ppv=nullptr; return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef()  override { return InterlockedIncrement(&m_refs); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r=InterlockedDecrement(&m_refs); if(r==0) delete this; return (ULONG)r;
    }

    // IBindStatusCallback - forward everything to inner, we only exist for IHttpNegotiate
    STDMETHODIMP OnStartBinding(DWORD r, IBinding* b) override { return m_inner?m_inner->OnStartBinding(r,b):S_OK; }
    STDMETHODIMP GetPriority(LONG* p) override { return m_inner?m_inner->GetPriority(p):E_NOTIMPL; }
    STDMETHODIMP OnLowResource(DWORD r) override { return m_inner?m_inner->OnLowResource(r):S_OK; }
    STDMETHODIMP OnProgress(ULONG a, ULONG b, ULONG c, LPCWSTR d) override { return m_inner?m_inner->OnProgress(a,b,c,d):S_OK; }
    STDMETHODIMP OnStopBinding(HRESULT h, LPCWSTR s) override { return m_inner?m_inner->OnStopBinding(h,s):S_OK; }
    STDMETHODIMP GetBindInfo(DWORD* f, BINDINFO* bi) override { return m_inner?m_inner->GetBindInfo(f,bi):E_NOTIMPL; }
    STDMETHODIMP OnDataAvailable(DWORD a, DWORD b, FORMATETC* c, STGMEDIUM* d) override { return m_inner?m_inner->OnDataAvailable(a,b,c,d):S_OK; }
    STDMETHODIMP OnObjectAvailable(REFIID r, IUnknown* p) override { return m_inner?m_inner->OnObjectAvailable(r,p):S_OK; }

    // IHttpNegotiate - the actual referer injection
    STDMETHODIMP BeginningTransaction(LPCWSTR szURL, LPCWSTR szHeaders, DWORD, LPWSTR* pszAdditionalHeaders) override {
        if (m_referer[0] && pszAdditionalHeaders) {
            wchar_t buf[1100];
            _snwprintf(buf, (sizeof(buf)/sizeof(wchar_t))-1, L"Referer: %s\r\n", m_referer);
            buf[(sizeof(buf)/sizeof(wchar_t))-1]=L'\0';
            size_t len=wcslen(buf)+1;
            LPWSTR heap=(LPWSTR)CoTaskMemAlloc(len*sizeof(wchar_t));
            if (heap) { wcscpy(heap,buf); *pszAdditionalHeaders=heap; return S_OK; }
        }
        *pszAdditionalHeaders=nullptr; return S_OK;
    }
    STDMETHODIMP OnResponse(DWORD, LPCWSTR, LPCWSTR, LPWSTR* pszAdditionalRequestHeaders) override {
        if (pszAdditionalRequestHeaders) *pszAdditionalRequestHeaders=nullptr; return S_OK;
    }
};

// (UnityBindCallback is also used by ReadBundleFromURL's own URL probe.)

class UnityClientSite : public IOleClientSite, public IOleInPlaceSite
{
public:
    LONG           m_refs;
    HWND           m_hwnd;
    UnityFrameSite m_frame;
    wchar_t        m_url[MAX_PATH * 2];   // file:// URL for GetMoniker base context

    explicit UnityClientSite(HWND hwnd, const wchar_t* url = nullptr)
        : m_refs(1), m_hwnd(hwnd), m_frame(this) {
        m_url[0] = L'\0';
        if (url) {
            wcsncpy(m_url, url, (sizeof(m_url)/sizeof(wchar_t))-1);
            m_url[(sizeof(m_url)/sizeof(wchar_t))-1] = L'\0';
        }
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid==IID_IUnknown || riid==IID_IOleClientSite)
            *ppv = static_cast<IOleClientSite*>(this);
        else if (riid==IID_IOleInPlaceSite || riid==IID_IOleWindow)
            *ppv = static_cast<IOleInPlaceSite*>(this);
        else if (riid==IID_IOleInPlaceFrame || riid==IID_IOleInPlaceUIWindow)
            *ppv = &m_frame;
        else { *ppv=nullptr; return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef()  override { return InterlockedIncrement(&m_refs); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&m_refs);
        if (r==0) delete this;
        return (ULONG)r;
    }

    // IOleClientSite
    STDMETHODIMP SaveObject()                     override { return E_NOTIMPL; }
    STDMETHODIMP GetMoniker(DWORD, DWORD, IMoniker** ppmk) override {
        if (!m_url[0]) return E_NOTIMPL;
        return CreateURLMoniker(nullptr, m_url, ppmk);
    }
    STDMETHODIMP GetContainer(IOleContainer** pp)  override { *pp=nullptr; return E_NOINTERFACE; }
    STDMETHODIMP ShowObject()                      override { return S_OK; }
    STDMETHODIMP OnShowWindow(BOOL)                override { return S_OK; }
    STDMETHODIMP RequestNewObjectLayout()          override { return E_NOTIMPL; }

    // IOleWindow + IOleInPlaceSite
    STDMETHODIMP GetWindow(HWND* ph) override { *ph=m_hwnd; return S_OK; }
    STDMETHODIMP ContextSensitiveHelp(BOOL) override { return S_OK; }
    STDMETHODIMP CanInPlaceActivate()  override { return S_OK; }
    STDMETHODIMP OnInPlaceActivate()   override { return S_OK; }
    STDMETHODIMP OnUIActivate()        override { return S_OK; }
    STDMETHODIMP OnUIDeactivate(BOOL)  override { return S_OK; }
    STDMETHODIMP OnInPlaceDeactivate() override { return S_OK; }
    STDMETHODIMP DiscardUndoState()    override { return E_NOTIMPL; }
    STDMETHODIMP DeactivateAndUndo()   override { return E_NOTIMPL; }
    STDMETHODIMP Scroll(SIZE)          override { return S_OK; }
    STDMETHODIMP GetWindowContext(IOleInPlaceFrame** ppFrame, IOleInPlaceUIWindow** ppDoc,
                                  LPRECT rcPos, LPRECT rcClip, LPOLEINPLACEFRAMEINFO pFI) override {
        *ppFrame=&m_frame; m_frame.AddRef(); *ppDoc=nullptr;
        GetClientRect(m_hwnd,rcPos); *rcClip=*rcPos;
        pFI->cb=sizeof(OLEINPLACEFRAMEINFO); pFI->fMDIApp=FALSE;
        pFI->hwndFrame=m_hwnd; pFI->haccel=nullptr; pFI->cAccelEntries=0;
        return S_OK;
    }
    STDMETHODIMP OnPosRectChange(LPCRECT rc) override {
        if (g_pIPO) g_pIPO->SetObjectRects(rc,rc); return S_OK;
    }
};

STDMETHODIMP UnityFrameSite::QueryInterface(REFIID riid,void** ppv){return outer->QueryInterface(riid,ppv);}
STDMETHODIMP_(ULONG) UnityFrameSite::AddRef() {return outer->AddRef();}
STDMETHODIMP_(ULONG) UnityFrameSite::Release(){return outer->Release();}
STDMETHODIMP UnityFrameSite::GetWindow(HWND* ph){*ph=outer->m_hwnd;return S_OK;}

// ---------------------------------------------------------------------------
//  Status display (drawn on main window when Unity is not active)
// ---------------------------------------------------------------------------
static void SetStatus(const wchar_t* text){
    wcsncpy(g_statusText,text,(sizeof(g_statusText)/sizeof(wchar_t))-1);
    g_statusText[(sizeof(g_statusText)/sizeof(wchar_t))-1]=L'\0';
    if(g_hwndMain)InvalidateRect(g_hwndMain,nullptr,TRUE);
}
static void PaintStatus(HDC hdc,const RECT& rc){
    HBRUSH hbr=CreateSolidBrush(RGB(255,255,255));
    FillRect(hdc,&rc,hbr);DeleteObject(hbr);
    if(!g_statusText[0])return;
    LOGFONT lf={};lf.lfHeight=-16;lf.lfWeight=FW_NORMAL;lf.lfCharSet=DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName,L"Segoe UI",LF_FACESIZE-1);
    HFONT hFont=CreateFontIndirect(&lf),hOld=(HFONT)SelectObject(hdc,hFont);
    SetBkMode(hdc,TRANSPARENT);SetTextColor(hdc,RGB(140,140,140));
    RECT r=rc;
    DrawText(hdc,g_statusText,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    SelectObject(hdc,hOld);DeleteObject(hFont);
}

// ---------------------------------------------------------------------------
//  PlayerPrefs save path checker
//
//  Unity encodes the source path into the PlayerPrefs file name.
//  Long or non-ASCII paths can exceed MAX_PATH, silently preventing saves.
//  Warn the user before loading such a file.
// ---------------------------------------------------------------------------
static void UnityEncodeFilename(const wchar_t* widePath, wchar_t* out, int outLen)
{
    char utf8[MAX_PATH*4]={};
    WideCharToMultiByte(CP_UTF8,0,widePath,-1,utf8,sizeof(utf8),nullptr,nullptr);

    wchar_t* dst=out;
    const wchar_t* limit=out+outLen-16;

    if(dst<limit){wcscpy(dst,L"pref");dst+=4;}

    for(const unsigned char*p=(const unsigned char*)utf8;*p&&dst<limit;p++){
        unsigned char b=*p;
        if(b=='\\'||b=='/')      *dst++=L'-';
        else if(b==' ')          *dst++=L' ';
        else if(b>='a'&&b<='z')  *dst++=(wchar_t)b;
        else if(b>='A'&&b<='Z')  *dst++=(wchar_t)(b+32);
        else if(b>='0'&&b<='9')  *dst++=(wchar_t)b;
        else {
            unsigned int uval=(unsigned int)(int)(signed char)b;
            int written=_snwprintf(dst,limit-dst,L"_%x",uval);
            if(written>0)dst+=written;
        }
    }
    if(dst+4<out+outLen){wcscpy(dst,L".upp");}
}

static bool CheckAndWarnSavePath(const wchar_t* gamePath)
{
    if(PathIsURL(gamePath))return true;

    wchar_t appData[MAX_PATH]={};
    if(FAILED(SHGetFolderPath(nullptr,CSIDL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,appData)))
        appData[0]=L'\0';
    if(!appData[0]){
        wchar_t* env=_wgetenv(L"APPDATA");
        if(env)wcsncpy(appData,env,MAX_PATH-1);
    }

    wchar_t encoded[MAX_PATH*40]={};
    UnityEncodeFilename(gamePath,encoded,sizeof(encoded)/sizeof(wchar_t));

    wchar_t fullPath[MAX_PATH*42]={};
    _snwprintf(fullPath,(sizeof(fullPath)/sizeof(wchar_t))-1,
               L"%s\\Unity\\WebPlayerPrefs\\localhost\\%s",appData,encoded);
    fullPath[(sizeof(fullPath)/sizeof(wchar_t))-1]=L'\0';

    int totalLen=(int)wcslen(fullPath);
    if(totalLen<=MAX_PATH)return true;

    wchar_t dispPath[512]={};
    if(totalLen<=480){
        wcsncpy(dispPath,fullPath,480);
    } else {
        _snwprintf(dispPath,(sizeof(dispPath)/sizeof(wchar_t))-1,L"%.200s  [...]  %.200s",
                   fullPath, fullPath+totalLen-200);
    }
    dispPath[(sizeof(dispPath)/sizeof(wchar_t))-1]=L'\0';

    wchar_t msg[1200]={};
    _snwprintf(msg,(sizeof(msg)/sizeof(wchar_t))-1,LS("MSG_SAVEPATH_BODY"),
               totalLen,MAX_PATH,totalLen,dispPath);
    msg[(sizeof(msg)/sizeof(wchar_t))-1]=L'\0';

    int res=MessageBox(g_hwndMain,msg,LS("MSG_SAVEPATH_TITLE"),
                       MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2);
    return (res==IDYES);
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
struct BundleInfo{bool valid;char version[32];int major,minor;};
static BundleInfo ParseHeader(const unsigned char*buf,size_t n){
    BundleInfo i={false,"",0,0};
    if(n<27||memcmp(buf,"UnityWeb",8))return i;
    char tmp[32]={};memcpy(tmp,buf+19,31);
    int ma=0,mi=0;
    if(sscanf(tmp,"%d.%d",&ma,&mi)==2){i.valid=true;i.major=ma;i.minor=mi;strncpy(i.version,tmp,31);}
    return i;
}
static BundleInfo ReadBundleFromFile(const wchar_t*path){
    unsigned char buf[64]={};
    HANDLE h=CreateFile(path,GENERIC_READ,FILE_SHARE_READ,nullptr,
                        OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return{false,"",0,0};
    DWORD got=0;
    ReadFile(h,buf,sizeof(buf),&got,nullptr);
    CloseHandle(h);
    return ParseHeader(buf,got);
}
static BundleInfo ReadBundleFromURL(const wchar_t*url){
    // Probe version via URL Moniker so the same referer injection applies.
    unsigned char buf[64]={};
    DWORD got=0;
    IStream* pStr=nullptr;
    UnityBindCallback* cb=new UnityBindCallback(g_currentReferer,nullptr);
    HRESULT hr=URLOpenBlockingStreamW(nullptr,url,&pStr,0,cb);
    cb->Release();
    if(SUCCEEDED(hr)&&pStr){
        pStr->Read(buf,sizeof(buf),&got);
        pStr->Release();
    }
    return ParseHeader(buf,got);
}

// ---------------------------------------------------------------------------
//  Runtime management
//
//  Runtime definitions: registry channel value + folder name on disk.
//    channel-string + format-version-string, concatenated without separator.
//    e.g. "Beta-5.0" + "5.x.x" = "Beta-5.05.x.x"
// ---------------------------------------------------------------------------
struct RuntimeDef{const wchar_t*channel;const wchar_t*folder;};
static RuntimeDef GetRuntimeDef(int ma,int mi){
    if(ma==2&&mi<=5)return{L"Alpha-2.5",L"Alpha-2.52.x.x"};
    if(ma==2)       return{L"Beta-2.6", L"Beta-2.62.x.x"};
    if(ma==3)       return{L"Alpha-3.5",L"Alpha-3.53.x.x"};
    if(ma==4&&mi==1)return{L"Beta-4.1", L"Beta-4.13.x.x"};
    if(ma==4&&mi<=3)return{L"Beta-4.3", L"Beta-4.33.x.x"};
    if(ma==4&&mi<=6)return{L"Beta-4.6", L"Beta-4.63.x.x"};
    if(ma==4)       return{L"Beta-4.7", L"Beta-4.73.x.x"};
    if(ma==5&&mi==0)return{L"Beta-5.0", L"Beta-5.05.x.x"};
    if(ma==5&&mi==1)return{L"Beta-5.1", L"Beta-5.15.x.x"};
    if(ma==5&&mi==2)return{L"Beta-5.2", L"Beta-5.25.x.x"};
    if(ma==5&&mi>=3)return{L"Stable5.3.8",L"Stable5.x.x"};
    return{L"Stable5.3.8",L"Stable5.x.x"};
}
static const wchar_t*ChannelForVersion(int ma,int mi){return GetRuntimeDef(ma,mi).channel;}

static bool IsWebPlayerInstalled(){
    HKEY hk=nullptr;
    LSTATUS r=RegOpenKeyEx(HKEY_CLASSES_ROOT,
        L"CLSID\\{444785F1-DE89-4295-863A-D46C3A781394}",0,KEY_READ,&hk);
    if(r==ERROR_SUCCESS){RegCloseKey(hk);return true;}return false;
}
static bool IsRuntimePackagePresent(){
    wchar_t p[MAX_PATH];
    _snwprintf(p,MAX_PATH-1,L"%s\\Runtime\\mono",  g_exeDir);p[MAX_PATH-1]=0;if(!PathFileExists(p))return false;
    _snwprintf(p,MAX_PATH-1,L"%s\\Runtime\\player",g_exeDir);p[MAX_PATH-1]=0;if(!PathFileExists(p))return false;
    return true;
}
static void DeleteFolderContents(const wchar_t*path){
    wchar_t wild[MAX_PATH];_snwprintf(wild,MAX_PATH-1,L"%s\\*",path);wild[MAX_PATH-1]=0;
    WIN32_FIND_DATA fd={};HANDLE hf=FindFirstFile(wild,&fd);
    if(hf==INVALID_HANDLE_VALUE)return;
    do{
        if(!wcscmp(fd.cFileName,L".")||!wcscmp(fd.cFileName,L".."))continue;
        wchar_t fp[MAX_PATH];_snwprintf(fp,MAX_PATH-1,L"%s\\%s",path,fd.cFileName);fp[MAX_PATH-1]=0;
        SetFileAttributes(fp,FILE_ATTRIBUTE_NORMAL);
        if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){DeleteFolderContents(fp);RemoveDirectory(fp);}
        else DeleteFile(fp);
    }while(FindNextFile(hf,&fd));FindClose(hf);
}
static void CopyFolderContents(const wchar_t*src,const wchar_t*dest){
    SHCreateDirectoryEx(nullptr,dest,nullptr);
    wchar_t wild[MAX_PATH];_snwprintf(wild,MAX_PATH-1,L"%s\\*",src);wild[MAX_PATH-1]=0;
    WIN32_FIND_DATA fd={};HANDLE hf=FindFirstFile(wild,&fd);
    if(hf==INVALID_HANDLE_VALUE)return;
    do{
        if(!wcscmp(fd.cFileName,L".")||!wcscmp(fd.cFileName,L".."))continue;
        wchar_t sp[MAX_PATH],dp[MAX_PATH];
        _snwprintf(sp,MAX_PATH-1,L"%s\\%s",src,fd.cFileName);sp[MAX_PATH-1]=0;
        _snwprintf(dp,MAX_PATH-1,L"%s\\%s",dest,fd.cFileName);dp[MAX_PATH-1]=0;
        if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)CopyFolderContents(sp,dp);
        else{SetFileAttributes(dp,FILE_ATTRIBUTE_NORMAL);CopyFile(sp,dp,FALSE);}
    }while(FindNextFile(hf,&fd));FindClose(hf);
}
static bool SwitchRuntime(int major,int minor){
    RuntimeDef def=GetRuntimeDef(major,minor);
    wchar_t monoSrc[MAX_PATH],playerSrc[MAX_PATH];
    _snwprintf(monoSrc,  MAX_PATH-1,L"%s\\Runtime\\mono\\%s",  g_exeDir,def.folder);monoSrc[MAX_PATH-1]=0;
    _snwprintf(playerSrc,MAX_PATH-1,L"%s\\Runtime\\player\\%s",g_exeDir,def.folder);playerSrc[MAX_PATH-1]=0;
    if(!PathFileExists(monoSrc)||!PathFileExists(playerSrc)){
        wchar_t msg[600];
        _snwprintf(msg,599,LS("MSG_RUNTIME_SWITCH_FAILED_BODY"),def.channel,monoSrc);msg[599]=0;
        MessageBox(g_hwndMain,msg,LS("MSG_RUNTIME_SWITCH_FAILED_TITLE"),MB_ICONWARNING);return false;
    }
    wchar_t userProfile[MAX_PATH];
    ExpandEnvironmentStrings(L"%USERPROFILE%",userProfile,MAX_PATH);
    wchar_t wpBase[MAX_PATH];_snwprintf(wpBase,MAX_PATH-1,L"%s\\AppData\\LocalLow\\Unity\\WebPlayer",userProfile);wpBase[MAX_PATH-1]=0;
    wchar_t monoDst[MAX_PATH],playerDst[MAX_PATH];
    _snwprintf(monoDst,  MAX_PATH-1,L"%s\\mono\\3.x.x",  wpBase);monoDst[MAX_PATH-1]=0;
    _snwprintf(playerDst,MAX_PATH-1,L"%s\\player\\3.x.x",wpBase);playerDst[MAX_PATH-1]=0;
    DeleteFolderContents(monoDst);  RemoveDirectory(monoDst);
    DeleteFolderContents(playerDst);RemoveDirectory(playerDst);
    CopyFolderContents(monoSrc,monoDst);CopyFolderContents(playerSrc,playerDst);
    HKEY hk=nullptr;
    if(RegCreateKeyEx(HKEY_CURRENT_USER,L"Software\\Unity\\WebPlayer",
            0,nullptr,0,KEY_WRITE,nullptr,&hk,nullptr)==ERROR_SUCCESS){
        RegSetValueEx(hk,L"UnityWebPlayerReleaseChannel",0,REG_SZ,
                     (const BYTE*)def.channel,(DWORD)(wcslen(def.channel)+1)*sizeof(wchar_t));
        RegSetValueEx(hk,L"Directory",0,REG_SZ,
                     (const BYTE*)wpBase,(DWORD)(wcslen(wpBase)+1)*sizeof(wchar_t));
        RegCloseKey(hk);
    }
    return true;
}
static bool SilentInstallWebPlayer(){
    wchar_t installer[MAX_PATH];_snwprintf(installer,MAX_PATH-1,L"%s\\Runtime\\UnityWebPlayer.exe",g_exeDir);installer[MAX_PATH-1]=0;
    if(!PathFileExists(installer))return false;
    SHELLEXECUTEINFO sei={};sei.cbSize=sizeof(sei);sei.fMask=SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb=L"open";sei.lpFile=installer;sei.lpParameters=L"/S";sei.nShow=SW_HIDE;
    if(!ShellExecuteEx(&sei)||!sei.hProcess)return false;
    WaitForSingleObject(sei.hProcess,120000);CloseHandle(sei.hProcess);
    return IsWebPlayerInstalled();
}

// ---------------------------------------------------------------------------
//  MRU – Most Recently Used file list
// ---------------------------------------------------------------------------
static void MruLoad(){
    g_mruCount=0;HKEY hk=nullptr;
    if(RegOpenKeyEx(HKEY_CURRENT_USER,REG_MRU_KEY,0,KEY_READ,&hk)!=ERROR_SUCCESS)return;
    for(int i=0;i<MRU_MAX;i++){
        wchar_t name[4];_snwprintf(name,3,L"%d",i);name[3]=0;
        DWORD sz=(DWORD)sizeof(g_mruList[0]),type;
        if(RegQueryValueEx(hk,name,nullptr,&type,(BYTE*)g_mruList[g_mruCount],&sz)
                ==ERROR_SUCCESS&&type==REG_SZ&&g_mruList[g_mruCount][0]){
            g_mruReferer[g_mruCount][0]=L'\0';
            g_mruRealSrc[g_mruCount][0]=L'\0';
            wchar_t rname[8];_snwprintf(rname,7,L"%d_ref",i);rname[7]=0;
            DWORD rsz=(DWORD)sizeof(g_mruReferer[0]),rtype;
            RegQueryValueEx(hk,rname,nullptr,&rtype,(BYTE*)g_mruReferer[g_mruCount],&rsz);
            g_mruReferer[g_mruCount][(MAX_PATH*2)-1]=L'\0';
            wchar_t sname[8];_snwprintf(sname,7,L"%d_real",i);sname[7]=0;
            DWORD ssz=(DWORD)sizeof(g_mruRealSrc[0]),stype;
            RegQueryValueEx(hk,sname,nullptr,&stype,(BYTE*)g_mruRealSrc[g_mruCount],&ssz);
            g_mruRealSrc[g_mruCount][(MAX_PATH*2)-1]=L'\0';
            ++g_mruCount;
        } else break;
    }
    RegCloseKey(hk);
}
static void MruSave(){
    HKEY hk=nullptr;DWORD disp;
    if(RegCreateKeyEx(HKEY_CURRENT_USER,REG_MRU_KEY,
            0,nullptr,0,KEY_WRITE,nullptr,&hk,&disp)!=ERROR_SUCCESS)return;
    for(int i=0;i<g_mruCount;i++){
        wchar_t name[4];_snwprintf(name,3,L"%d",i);name[3]=0;
        RegSetValueEx(hk,name,0,REG_SZ,(const BYTE*)g_mruList[i],(DWORD)(wcslen(g_mruList[i])+1)*sizeof(wchar_t));
        wchar_t rname[8];_snwprintf(rname,7,L"%d_ref",i);rname[7]=0;
        RegSetValueEx(hk,rname,0,REG_SZ,(const BYTE*)g_mruReferer[i],(DWORD)(wcslen(g_mruReferer[i])+1)*sizeof(wchar_t));
        wchar_t sname[8];_snwprintf(sname,7,L"%d_real",i);sname[7]=0;
        RegSetValueEx(hk,sname,0,REG_SZ,(const BYTE*)g_mruRealSrc[i],(DWORD)(wcslen(g_mruRealSrc[i])+1)*sizeof(wchar_t));
    }
    for(int i=g_mruCount;i<MRU_MAX;i++){
        wchar_t name[4];_snwprintf(name,3,L"%d",i);name[3]=0;RegDeleteValue(hk,name);
        wchar_t rname[8];_snwprintf(rname,7,L"%d_ref",i);rname[7]=0;RegDeleteValue(hk,rname);
        wchar_t sname[8];_snwprintf(sname,7,L"%d_real",i);sname[7]=0;RegDeleteValue(hk,sname);
    }
    RegCloseKey(hk);
}
static void MruAdd(const wchar_t*pathArg,const wchar_t*refererArg=nullptr,
                   const wchar_t*realSrcArg=nullptr){
    // Defensive copy – prevents aliasing if pathArg points into g_mruList itself
    wchar_t path[MAX_PATH*2];
    wcsncpy(path,pathArg,(MAX_PATH*2)-1);path[(MAX_PATH*2)-1]=L'\0';
    wchar_t ref[MAX_PATH*2]={};
    if(refererArg){wcsncpy(ref,refererArg,(MAX_PATH*2)-1);ref[(MAX_PATH*2)-1]=L'\0';}
    // realSrc = path actually passed to Unity (cache file when referer is set).
    // UPPEditor reads this to locate save data, which is keyed off this path.
    wchar_t real[MAX_PATH*2]={};
    if(realSrcArg){wcsncpy(real,realSrcArg,(MAX_PATH*2)-1);real[(MAX_PATH*2)-1]=L'\0';}

    for(int i=0;i<g_mruCount;i++){
        if(_wcsicmp(g_mruList[i],path)==0){
            for(int j=i;j<g_mruCount-1;j++){
                wcscpy(g_mruList[j],g_mruList[j+1]);
                wcscpy(g_mruReferer[j],g_mruReferer[j+1]);
                wcscpy(g_mruRealSrc[j],g_mruRealSrc[j+1]);
            }
            --g_mruCount;break;
        }
    }
    if(g_mruCount>=MRU_MAX)g_mruCount=MRU_MAX-1;
    for(int i=g_mruCount;i>0;i--){
        wcscpy(g_mruList[i],g_mruList[i-1]);
        wcscpy(g_mruReferer[i],g_mruReferer[i-1]);
        wcscpy(g_mruRealSrc[i],g_mruRealSrc[i-1]);
    }
    wcsncpy(g_mruList[0],path,(MAX_PATH*2)-1);g_mruList[0][(MAX_PATH*2)-1]=L'\0';
    wcsncpy(g_mruReferer[0],ref,(MAX_PATH*2)-1);g_mruReferer[0][(MAX_PATH*2)-1]=L'\0';
    wcsncpy(g_mruRealSrc[0],real,(MAX_PATH*2)-1);g_mruRealSrc[0][(MAX_PATH*2)-1]=L'\0';
    ++g_mruCount;MruSave();
}
static void RebuildFileMenu(){
    HMENU hBar=GetMenu(g_hwndMain);if(!hBar)return;
    HMENU hFile=GetSubMenu(hBar,0);if(!hFile)return;
    while(GetMenuItemCount(hFile)>4)DeleteMenu(hFile,4,MF_BYPOSITION);
    if(g_mruCount==0){
        AppendMenu(hFile,MF_STRING|MF_GRAYED,IDM_RECENT_EMPTY,LS("FILE_NO_RECENT"));
    }else{
        for(int i=0;i<g_mruCount;i++){
            wchar_t esc[MAX_PATH*2+4]={};const wchar_t*s=g_mruList[i];wchar_t*d=esc;
            while(*s&&(d-esc)<(int)(sizeof(esc)/sizeof(wchar_t))-2){if(*s==L'&')*d++=L'&';*d++=*s++;}
            wchar_t label[MAX_PATH*2+8];
            if(i<9)_snwprintf(label,(sizeof(label)/sizeof(wchar_t))-1,L"&%d %s",i+1,esc);
            else    _snwprintf(label,(sizeof(label)/sizeof(wchar_t))-1,L"1&0 %s",esc);
            label[(sizeof(label)/sizeof(wchar_t))-1]=0;
            AppendMenu(hFile,MF_STRING,IDM_RECENT_0+i,label);
        }
    }
    AppendMenu(hFile,MF_SEPARATOR,0,nullptr);
    AppendMenu(hFile,MF_STRING,IDM_FILE_EXIT,LS("MENU_FILE_EXIT"));
    DrawMenuBar(g_hwndMain);
}

// ---------------------------------------------------------------------------
//  Tools menu – launch external programs from Tools\ folder
// ---------------------------------------------------------------------------
static void SettingsLoad()
{
    g_toolsEnabled = false;
    g_fpsTarget    = 0;
    HKEY hk = nullptr;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD val = 0, sz = sizeof(val), type = 0;
        if (RegQueryValueEx(hk, L"ToolsEnabled", nullptr, &type, (BYTE*)&val, &sz) == ERROR_SUCCESS
                && type == REG_DWORD)
            g_toolsEnabled = (val != 0);
        val = 0; sz = sizeof(val); type = 0;
        if (RegQueryValueEx(hk, L"ExperimentalFPS", nullptr, &type, (BYTE*)&val, &sz) == ERROR_SUCCESS
                && type == REG_DWORD && (int)val > 0 && (int)val <= 1000)
            g_fpsTarget = (int)val;
        RegCloseKey(hk);
    }
}
static void SettingsSaveFpsTarget()
{
    HKEY hk = nullptr; DWORD disp;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_SETTINGS_KEY,
            0, nullptr, 0, KEY_WRITE, nullptr, &hk, &disp) == ERROR_SUCCESS) {
        DWORD val = (DWORD)g_fpsTarget;
        RegSetValueEx(hk, L"ExperimentalFPS", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
        RegCloseKey(hk);
    }
}
static void SettingsSaveToolsEnabled()
{
    HKEY hk = nullptr; DWORD disp;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_SETTINGS_KEY,
            0, nullptr, 0, KEY_WRITE, nullptr, &hk, &disp) == ERROR_SUCCESS) {
        DWORD val = g_toolsEnabled ? 1 : 0;
        RegSetValueEx(hk, L"ToolsEnabled", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
        RegCloseKey(hk);
    }
}
static void ScanToolsFolder()
{
    g_toolsCount = 0;
    wchar_t dir[MAX_PATH]; _snwprintf(dir,MAX_PATH-1,L"%s\\Tools", g_exeDir);dir[MAX_PATH-1]=0;
    wchar_t wild[MAX_PATH]; _snwprintf(wild,MAX_PATH-1,L"%s\\*.exe", dir);wild[MAX_PATH-1]=0;

    WIN32_FIND_DATA fd = {};
    HANDLE hf = FindFirstFile(wild, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_toolsCount >= TOOLS_MAX) break;

        _snwprintf(g_toolsList[g_toolsCount],MAX_PATH-1, L"%s\\%s", dir, fd.cFileName);
        g_toolsList[g_toolsCount][MAX_PATH-1]=0;

        wcsncpy(g_toolsName[g_toolsCount], fd.cFileName, MAX_PATH - 1);
        g_toolsName[g_toolsCount][MAX_PATH - 1] = L'\0';
        wchar_t* dot = wcsrchr(g_toolsName[g_toolsCount], L'.');
        if (dot && _wcsicmp(dot, L".exe") == 0) *dot = L'\0';

        g_toolsCount++;
    } while (FindNextFile(hf, &fd));
    FindClose(hf);
}
static void RebuildToolsMenu()
{
    HMENU hBar = GetMenu(g_hwndMain); if (!hBar) return;
    HMENU hTools = GetSubMenu(hBar, 3); if (!hTools) return;

    while (GetMenuItemCount(hTools) > 0)
        DeleteMenu(hTools, 0, MF_BYPOSITION);

    if (!g_toolsEnabled) {
        AppendMenu(hTools, MF_STRING, IDM_TOOLS_ENABLE, LS("TOOLS_ENABLE"));
        DrawMenuBar(g_hwndMain);
        return;
    }

    AppendMenu(hTools, MF_STRING, IDM_TOOLS_REFRESH, LS("TOOLS_REFRESH"));
    AppendMenu(hTools, MF_SEPARATOR, 0, nullptr);

    if (g_toolsCount == 0) {
        AppendMenu(hTools, MF_STRING | MF_GRAYED, IDM_TOOLS_EMPTY, LS("TOOLS_EMPTY"));
    } else {
        for (int i = 0; i < g_toolsCount; i++) {
            wchar_t esc[MAX_PATH + 4] = {}; const wchar_t* s = g_toolsName[i]; wchar_t* d = esc;
            while (*s && (d - esc) < (int)(sizeof(esc)/sizeof(wchar_t)) - 2) { if (*s == L'&') *d++ = L'&'; *d++ = *s++; }
            AppendMenu(hTools, MF_STRING, IDM_TOOLS_ITEM_0 + i, esc);
        }
    }
    DrawMenuBar(g_hwndMain);
}
static void EnableTools()
{
    g_toolsEnabled = true;
    SettingsSaveToolsEnabled();

    wchar_t toolsDir[MAX_PATH];
    _snwprintf(toolsDir,MAX_PATH-1,L"%s\\Tools", g_exeDir);toolsDir[MAX_PATH-1]=0;
    SHCreateDirectoryEx(nullptr, toolsDir, nullptr);

    ScanToolsFolder();
    RebuildToolsMenu();
}
static void LaunchTool(const wchar_t* exePath)
{
    wchar_t dir[MAX_PATH];
    wcsncpy(dir, exePath, MAX_PATH - 1);
    dir[MAX_PATH - 1] = L'\0';
    PathRemoveFileSpec(dir);

    SHELLEXECUTEINFO sei = {};
    sei.cbSize    = sizeof(sei);
    sei.fMask     = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb    = L"open";
    sei.lpFile    = exePath;
    sei.lpDirectory = dir;
    sei.nShow     = SW_SHOWNORMAL;

    if (!ShellExecuteEx(&sei)) {
        wchar_t msg[MAX_PATH + 64];
        _snwprintf(msg,(sizeof(msg)/sizeof(wchar_t))-1, LS("MSG_LAUNCH_TOOL_BODY"), exePath);
        msg[(sizeof(msg)/sizeof(wchar_t))-1]=0;
        MessageBox(g_hwndMain, msg, LS("MSG_LAUNCH_TOOL_TITLE"), MB_ICONERROR);
        return;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
}
static void ClearUserData()
{
    SHDeleteKey(HKEY_CURRENT_USER, REG_ROOT_KEY);

    g_mruCount     = 0;
    g_toolsEnabled = false;
    g_toolsCount   = 0;
    g_fpsTarget    = 0;

    g_currentLangCode[0] = L'\0';
    InitDefaultStrings();
    ApplyMenuLanguage();
    if (!g_gameLoaded) SetStatus(LS("STATUS_IDLE"));

    RebuildFileMenu();
    RebuildToolsMenu();
}

// ---------------------------------------------------------------------------
//  Language Packs – load .lang files from langs\ folder
//
//  .lang file format: UTF-8 text, optional BOM.
//  Lines: KEY=VALUE  (supports \n, \t, \\ escape sequences)
//  Special keys: LANG_NAME, LANG_CODE
// ---------------------------------------------------------------------------
static void SetString(const char* key, const wchar_t* value)
{
    for (int i = 0; i < g_stringCount; i++) {
        if (!strcmp(g_strings[i].key, key)) {
            wcsncpy(g_strings[i].value, value, LANG_VAL_LEN - 1);
            g_strings[i].value[LANG_VAL_LEN - 1] = L'\0';
            return;
        }
    }
    if (g_stringCount < MAX_LANG_STRINGS) {
        strncpy(g_strings[g_stringCount].key, key, LANG_KEY_LEN - 1);
        g_strings[g_stringCount].key[LANG_KEY_LEN - 1] = '\0';
        wcsncpy(g_strings[g_stringCount].value, value, LANG_VAL_LEN - 1);
        g_strings[g_stringCount].value[LANG_VAL_LEN - 1] = L'\0';
        g_stringCount++;
    }
}
static const wchar_t* LS(const char* key)
{
    for (int i = 0; i < g_stringCount; i++)
        if (!strcmp(g_strings[i].key, key)) return g_strings[i].value;
    // Dev fallback: widen the key if lookup fails (should not happen)
    static wchar_t fallback[LANG_KEY_LEN];
    int i=0; for(;key[i]&&i<LANG_KEY_LEN-1;i++) fallback[i]=(wchar_t)(unsigned char)key[i];
    fallback[i]=L'\0';
    return fallback;
}
static void InitDefaultStrings()
{
    g_stringCount = 0;

    SetString("MENU_FILE",    L"&File");
    SetString("MENU_VIEW",    L"&View");
    SetString("MENU_CONTROL", L"&Control");
    SetString("MENU_TOOLS",   L"&Tools");
    SetString("MENU_HELP",    L"&Help");

    SetString("MENU_FILE_OPEN",   L"&Open...");
    SetString("MENU_FILE_RELOAD", L"&Reload");
    SetString("MENU_FILE_CLOSE",  L"&Close");
    SetString("MENU_FILE_EXIT",   L"E&xit");
    SetString("FILE_NO_RECENT",   L"(No recent files)");

    SetString("MENU_VIEW_FULLSCREEN", L"&Fullscreen");

    SetString("MENU_CTRL_QUALITY",      L"&Quality");
    SetString("MENU_CTRL_QUALITY_LOW",  L"&Low");
    SetString("MENU_CTRL_QUALITY_MED",  L"&Medium");
    SetString("MENU_CTRL_QUALITY_HIGH", L"&High");
    SetString("MENU_CTRL_LANGUAGE",     L"&Language");
    SetString("LANG_BUILTIN_EN",        L"English (Built-in)");

    SetString("TOOLS_ENABLE",  L"&Enable Tools");
    SetString("TOOLS_REFRESH", L"&Refresh");
    SetString("TOOLS_EMPTY",   L"(No tools found in Tools\\)");

    SetString("MENU_HELP_REPO",       L"&GitHub Repository");
    SetString("MENU_HELP_CLEARDATA",  L"Clear User &Data...");
    SetString("MENU_HELP_ABOUT",      L"&About UFunPlayer...");

    SetString("STATUS_IDLE",            L"Drag a .unity3d file here, or use File > Open.");
    SetString("STATUS_LOADING",         L"Loading Player...");
    SetString("STATUS_LOADING_VERSION", L"Loading Player...  [Unity %s -> %s]");
    SetString("STATUS_INITIALIZING",    L"Initializing player...");
    SetString("STATUS_RUNTIME_MISSING_PROMPT", L"Runtime not found - please download Runtime.zip.");
    SetString("STATUS_RUNTIME_MISSING",         L"Runtime not found. Download Runtime.zip from GitHub.");
    SetString("STATUS_INIT_FAILED",     L"Initialization failed. Check Runtime\\UnityWebPlayer.exe.");
    SetString("STATUS_CREATE_FAILED",   L"Error: failed to create Unity player. Is Unity Web Player installed?");

    SetString("OPEN_TITLE",              L"Open (UFunPlayer)");
    SetString("OPEN_URL_LABEL",          L"Enter the network location (URL) of a .unity3d file:");
    SetString("OPEN_URL_EXAMPLE",        L"Example:  http://example.com/game.unity3d");
    SetString("OPEN_BROWSE_SEPARATOR",   L"--- or browse your local files ---");
    SetString("OPEN_BROWSE_BTN",         L"&Browse...");
    SetString("OPEN_OK_BTN",             L"&OK");
    SetString("OPEN_CANCEL_BTN",         L"Cancel");
    SetString("OPEN_ADV_GROUP",          L"&Advanced");
    SetString("OPEN_ADV_REF_LABEL",      L"URL Spoofing:");
    SetString("OPEN_FILEDLG_TITLE",      L"Open Unity Bundle");
    SetString("OPEN_FILEDLG_FILTER_NAME",L"Unity Bundle (*.unity3d)");
    SetString("OPEN_FILEDLG_FILTER_ALL", L"All Files");

    SetString("ABOUT_TITLE",   L"About UFunPlayer");
    SetString("ABOUT_TAGLINE", L"A Standalone Unity Web Player");
    SetString("ABOUT_OK_BTN",  L"OK");

    SetString("DOWNLOAD_TITLE",          L"Runtime Package Not Found");
    SetString("DOWNLOAD_NOTFOUND",       L"The Unity Web Player runtime files were not found.");
    SetString("DOWNLOAD_EXPECTED_LABEL", L"Expected location:");
    SetString("DOWNLOAD_BODY",
        L"Download Runtime.zip from GitHub and extract it so that Runtime\\mono\\ "
        L"and Runtime\\player\\ sit next to UFunPlayer.exe.");
    SetString("DOWNLOAD_URL_LABEL",    L"URL:");
    SetString("DOWNLOAD_OPENPAGE_BTN", L"Open &Download Page");
    SetString("DOWNLOAD_LATER_BTN",    L"I'll do it &later");
    SetString("DOWNLOAD_CANCEL_BTN",   L"Cancel");

    SetString("TOOLSWARN_TITLE", L"Enable Tools - Warning");
    SetString("TOOLSWARN_BODY",
        L"Tools are third-party programs that you add yourself. UFunPlayer does not "
        L"vet, sandbox, or verify them.\n\nOnly download tools from sources you trust, "
        L"and place them in the Tools\\ folder next to UFunPlayer.exe (the same folder "
        L"that contains Runtime\\).\n\nRunning an untrusted program can harm your computer.");
    SetString("TOOLSWARN_NOASKAGAIN", L"You will not be asked again after agreeing.");
    SetString("TOOLSWARN_ENABLE_BTN", L"I Understand, &Enable Tools");
    SetString("TOOLSWARN_CANCEL_BTN", L"Cancel");

    SetString("MSG_RUNTIME_SWITCH_FAILED_TITLE", L"Runtime Switch Failed");
    SetString("MSG_RUNTIME_SWITCH_FAILED_BODY",
        L"Runtime folder not found for [%s]:\n\n  %s\n\nPlease check your Runtime "
        L"package.\ne.g. Runtime\\mono\\Beta-5.05.x.x\\");

    SetString("MSG_SAVEPATH_TITLE", L"Save Path Too Long - Warning");
    SetString("MSG_SAVEPATH_BODY",
        L"Warning: Save data (PlayerPrefs) will NOT work!\n\nUnity Web Player encodes "
        L"the game file path into the save file name. The resulting path is %d characters "
        L"long, exceeding Windows' %d-character limit. Windows will silently refuse to "
        L"create the file.\n\nExpected save path (%d chars):\n%s\n\nHow to fix:\n  - Move "
        L"the game file to a short, all-ASCII folder\n    e.g.  C:\\games\\game.unity3d\n"
        L"  - Rename the file to a shorter English-only name\n\nLoad anyway (saves will "
        L"NOT be written)?");

    SetString("MSG_LAUNCH_TOOL_TITLE", L"Launch Tool");
    SetString("MSG_LAUNCH_TOOL_BODY",  L"Failed to launch:\n%s");

    SetString("MSG_CLEARDATA_TITLE",   L"Clear User Data");
    SetString("MSG_CLEARDATA_CONFIRM",
        L"This clears your recent-file history and resets the Tools warning prompt "
        "(Tools will need to be re-enabled).\n\nContinue?");
    SetString("MSG_CLEARDATA_DONE",    L"User data has been cleared.");

    SetString("MENU_CTRL_EXPERIMENTAL", L"Experimental &Features...");
    SetString("EXP_TITLE",              L"Experimental Features");
    SetString("EXP_FPS_GROUP",          L"Frame Rate Override");
    SetString("EXP_FPS_LABEL",          L"Target FPS:");
    SetString("EXP_FPS_ZERO",           L"(0 = disabled)");
    SetString("EXP_FPS_STATUS_ON",      L"Current setting: %d FPS");
    SetString("EXP_FPS_STATUS_OFF",     L"Current setting: disabled (game default)");
    SetString("EXP_FPS_HINT",
        L"Forces the player to run at the given frame rate, ignoring the game's own "
        "frame limiter and V-Sync setting. V-Sync stays disabled while this is "
        "active, so mild screen tearing may occur. Targets above your monitor's "
        "refresh rate are capped by it. Set 0 to disable. Changes usually apply "
        "immediately; if a game keeps its old frame rate, reload the game.");
    SetString("EXP_FPS_APPLY_BTN",      L"&Apply");
    SetString("EXP_CLOSE_BTN",          L"Close");
    SetString("EXP_FPS_BAD_VALUE",      L"Enter a value between 0 and 1000.");
    SetString("EXP_FPS_SAVED",
        L"Saved. Changes usually apply immediately; if not, reload the game "
        L"(File > Reload).");
}

// Parse a single .lang file, optionally applying its keys to the string table.
static bool ParseLanguageFile(const wchar_t* path, bool applyToTable,
                               wchar_t* outName, int outNameLen,
                               wchar_t* outCode, int outCodeLen)
{
    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, nullptr);
    if (sz == INVALID_FILE_SIZE || sz == 0) { CloseHandle(h); return false; }

    char* buf = new char[sz + 1];
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sz, &got, nullptr);
    CloseHandle(h);
    if (!ok) { delete[] buf; return false; }
    buf[got] = '\0';

    char* p = buf;
    if (got >= 3 && (unsigned char)p[0]==0xEF && (unsigned char)p[1]==0xBB && (unsigned char)p[2]==0xBF)
        p += 3;

    char* lineStart = p;
    while (*lineStart) {
        char* lineEnd  = strchr(lineStart, '\n');
        char* nextLine = lineEnd ? lineEnd + 1 : lineStart + strlen(lineStart);
        if (lineEnd) *lineEnd = '\0';

        size_t llen = strlen(lineStart);
        if (llen > 0 && lineStart[llen - 1] == '\r') lineStart[llen - 1] = '\0';

        char* line = lineStart;
        while (*line == ' ' || *line == '\t') line++;

        if (*line && *line != '#') {
            char* eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char* rawKey = line;
                char* rawVal = eq + 1;

                char* keyEnd = rawKey + strlen(rawKey);
                while (keyEnd > rawKey && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) *(--keyEnd) = '\0';

                char unescaped[LANG_VAL_LEN * 4] = {};
                char* d = unescaped;
                const char* s = rawVal;
                while (*s && (d - unescaped) < (int)sizeof(unescaped) - 1) {
                    if      (s[0]=='\\' && s[1]=='n')  { *d++ = '\n'; s += 2; }
                    else if (s[0]=='\\' && s[1]=='t')  { *d++ = '\t'; s += 2; }
                    else if (s[0]=='\\' && s[1]=='\\') { *d++ = '\\'; s += 2; }
                    else                                 { *d++ = *s++; }
                }
                *d = '\0';

                wchar_t wbuf[LANG_VAL_LEN] = {};
                MultiByteToWideChar(CP_UTF8, 0, unescaped, -1, wbuf, LANG_VAL_LEN);

                if (!strcmp(rawKey, "LANG_NAME")) {
                    if (outName) { wcsncpy(outName, wbuf, outNameLen - 1); outName[outNameLen - 1] = L'\0'; }
                } else if (!strcmp(rawKey, "LANG_CODE")) {
                    if (outCode) { wcsncpy(outCode, wbuf, outCodeLen - 1); outCode[outCodeLen - 1] = L'\0'; }
                } else if (applyToTable) {
                    SetString(rawKey, wbuf);
                }
            }
        }
        lineStart = nextLine;
    }

    delete[] buf;
    return true;
}

static void ScanLangsFolder()
{
    g_langFileCount = 0;

    wchar_t dir[MAX_PATH];  _snwprintf(dir,MAX_PATH-1, L"%s\\langs", g_exeDir);dir[MAX_PATH-1]=0;
    wchar_t wild[MAX_PATH]; _snwprintf(wild,MAX_PATH-1, L"%s\\*.lang", dir);wild[MAX_PATH-1]=0;

    WIN32_FIND_DATA fd = {};
    HANDLE hf = FindFirstFile(wild, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_langFileCount >= LANG_MAX) break;

        LangFileInfo info = {};
        _snwprintf(info.path,MAX_PATH-1, L"%s\\%s", dir, fd.cFileName);info.path[MAX_PATH-1]=0;

        wchar_t name[128] = {}, code[64] = {};
        ParseLanguageFile(info.path, false, name, 128, code, 64);

        wchar_t base[MAX_PATH];
        wcsncpy(base, fd.cFileName, MAX_PATH - 1); base[MAX_PATH - 1] = L'\0';
        wchar_t* dot = wcsrchr(base, L'.');
        if (dot) *dot = L'\0';

        wcsncpy(info.name, name[0] ? name : base, (sizeof(info.name)/sizeof(wchar_t)) - 1);
        wcsncpy(info.code, code[0] ? code : base, (sizeof(info.code)/sizeof(wchar_t)) - 1);

        g_langFiles[g_langFileCount++] = info;
    } while (FindNextFile(hf, &fd));
    FindClose(hf);
}

static void SettingsSaveLanguage(const wchar_t* code)
{
    HKEY hk = nullptr; DWORD disp;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_SETTINGS_KEY,
            0, nullptr, 0, KEY_WRITE, nullptr, &hk, &disp) == ERROR_SUCCESS) {
        if (code && code[0])
            RegSetValueEx(hk, L"Language", 0, REG_SZ, (const BYTE*)code, (DWORD)(wcslen(code) + 1)*sizeof(wchar_t));
        else
            RegDeleteValue(hk, L"Language");
        RegCloseKey(hk);
    }
}

static void ApplyLanguage(HWND hwnd, const wchar_t* code)
{
    InitDefaultStrings();

    bool found = false;
    if (code && code[0]) {
        for (int i = 0; i < g_langFileCount && !found; i++) {
            if (!_wcsicmp(g_langFiles[i].code, code)) {
                ParseLanguageFile(g_langFiles[i].path, true, nullptr, 0, nullptr, 0);
                wcsncpy(g_currentLangCode, code, (sizeof(g_currentLangCode)/sizeof(wchar_t)) - 1);
                g_currentLangCode[(sizeof(g_currentLangCode)/sizeof(wchar_t)) - 1] = L'\0';
                found = true;
            }
        }
    }
    if (!found) g_currentLangCode[0] = L'\0';

    SettingsSaveLanguage(g_currentLangCode);

    if (hwnd) {
        ApplyMenuLanguage();
        if (!g_gameLoaded) SetStatus(LS("STATUS_IDLE"));
        InvalidateRect(hwnd, nullptr, TRUE);
    }
}

static void RebuildLanguageMenu()
{
    HMENU hBar = GetMenu(g_hwndMain); if (!hBar) return;
    HMENU hCtrl = GetSubMenu(hBar, 2); if (!hCtrl) return;
    HMENU hLang = GetSubMenu(hCtrl, 1); if (!hLang) return;

    while (GetMenuItemCount(hLang) > 0) DeleteMenu(hLang, 0, MF_BYPOSITION);

    UINT flagsBuiltin = MF_STRING | (g_currentLangCode[0] == L'\0' ? MF_CHECKED : MF_UNCHECKED);
    AppendMenu(hLang, flagsBuiltin, IDM_LANG_BUILTIN_EN, LS("LANG_BUILTIN_EN"));

    if (g_langFileCount > 0) AppendMenu(hLang, MF_SEPARATOR, 0, nullptr);

    for (int i = 0; i < g_langFileCount; i++) {
        UINT flags = MF_STRING | (!_wcsicmp(g_currentLangCode, g_langFiles[i].code) ? MF_CHECKED : MF_UNCHECKED);
        AppendMenu(hLang, flags, IDM_LANG_ITEM_0 + i, g_langFiles[i].name);
    }
    DrawMenuBar(g_hwndMain);
}

static void ApplyMenuLanguage()
{
    HMENU hBar = GetMenu(g_hwndMain); if (!hBar) return;

    HMENU hFile  = GetSubMenu(hBar, 0);
    HMENU hView  = GetSubMenu(hBar, 1);
    HMENU hCtrl  = GetSubMenu(hBar, 2);
    HMENU hTools = GetSubMenu(hBar, 3);
    HMENU hHelp  = GetSubMenu(hBar, 4);

    if (hFile)  ModifyMenu(hBar, 0, MF_BYPOSITION|MF_STRING|MF_POPUP, (UINT_PTR)hFile,  LS("MENU_FILE"));
    if (hView)  ModifyMenu(hBar, 1, MF_BYPOSITION|MF_STRING|MF_POPUP, (UINT_PTR)hView,  LS("MENU_VIEW"));
    if (hCtrl)  ModifyMenu(hBar, 2, MF_BYPOSITION|MF_STRING|MF_POPUP, (UINT_PTR)hCtrl,  LS("MENU_CONTROL"));
    if (hTools) ModifyMenu(hBar, 3, MF_BYPOSITION|MF_STRING|MF_POPUP, (UINT_PTR)hTools, LS("MENU_TOOLS"));
    if (hHelp)  ModifyMenu(hBar, 4, MF_BYPOSITION|MF_STRING|MF_POPUP, (UINT_PTR)hHelp,  LS("MENU_HELP"));

    wchar_t buf[300];

    if (hFile) {
        _snwprintf(buf,299, L"%s\tCtrl+O", LS("MENU_FILE_OPEN"));buf[299]=0;
        ModifyMenu(hFile, IDM_FILE_OPEN, MF_BYCOMMAND|MF_STRING, IDM_FILE_OPEN, buf);

        UINT ena = g_gameLoaded ? MF_ENABLED : MF_GRAYED;
        _snwprintf(buf,299, L"%s\tCtrl+R", LS("MENU_FILE_RELOAD"));buf[299]=0;
        ModifyMenu(hFile, IDM_FILE_RELOAD, MF_BYCOMMAND|MF_STRING|ena, IDM_FILE_RELOAD, buf);
        ModifyMenu(hFile, IDM_FILE_CLOSE,  MF_BYCOMMAND|MF_STRING|ena, IDM_FILE_CLOSE,  LS("MENU_FILE_CLOSE"));
    }

    if (hView) {
        _snwprintf(buf,299, L"%s\tF11", LS("MENU_VIEW_FULLSCREEN"));buf[299]=0;
        ModifyMenu(hView, IDM_VIEW_FULLSCREEN, MF_BYCOMMAND|MF_STRING, IDM_VIEW_FULLSCREEN, buf);
    }

    if (hCtrl) {
        HMENU hQuality = GetSubMenu(hCtrl, 0);
        if (hQuality) {
            ModifyMenu(hCtrl, 0, MF_BYPOSITION|MF_STRING|MF_POPUP, (UINT_PTR)hQuality, LS("MENU_CTRL_QUALITY"));
            ModifyMenu(hQuality, IDM_CTRL_QUALITY_L, MF_BYCOMMAND|MF_STRING|MF_GRAYED, IDM_CTRL_QUALITY_L, LS("MENU_CTRL_QUALITY_LOW"));
            ModifyMenu(hQuality, IDM_CTRL_QUALITY_M, MF_BYCOMMAND|MF_STRING|MF_GRAYED, IDM_CTRL_QUALITY_M, LS("MENU_CTRL_QUALITY_MED"));
            ModifyMenu(hQuality, IDM_CTRL_QUALITY_H, MF_BYCOMMAND|MF_STRING|MF_GRAYED, IDM_CTRL_QUALITY_H, LS("MENU_CTRL_QUALITY_HIGH"));
        }
        HMENU hLang = GetSubMenu(hCtrl, 1);
        if (hLang) ModifyMenu(hCtrl, 1, MF_BYPOSITION|MF_STRING|MF_POPUP, (UINT_PTR)hLang, LS("MENU_CTRL_LANGUAGE"));
        ModifyMenu(hCtrl, IDM_CTRL_EXPERIMENTAL, MF_BYCOMMAND|MF_STRING, IDM_CTRL_EXPERIMENTAL, LS("MENU_CTRL_EXPERIMENTAL"));
    }

    if (hHelp) {
        ModifyMenu(hHelp, IDM_HELP_REPO,      MF_BYCOMMAND|MF_STRING, IDM_HELP_REPO,      LS("MENU_HELP_REPO"));
        ModifyMenu(hHelp, IDM_HELP_CLEARDATA, MF_BYCOMMAND|MF_STRING, IDM_HELP_CLEARDATA, LS("MENU_HELP_CLEARDATA"));
        ModifyMenu(hHelp, IDM_HELP_ABOUT,     MF_BYCOMMAND|MF_STRING, IDM_HELP_ABOUT,     LS("MENU_HELP_ABOUT"));
    }

    RebuildFileMenu();
    RebuildToolsMenu();
    RebuildLanguageMenu();

    DrawMenuBar(g_hwndMain);
}

// ---------------------------------------------------------------------------
//  Unity ActiveX control lifecycle
// ---------------------------------------------------------------------------
static void UnitySetPropW(const wchar_t*name,const wchar_t*value){
    if(!g_pDisp)return;
    DISPID dispid;BSTR bname=SysAllocString(name);
    HRESULT hr=g_pDisp->GetIDsOfNames(IID_NULL,&bname,1,LOCALE_USER_DEFAULT,&dispid);
    SysFreeString(bname);if(FAILED(hr))return;
    VARIANT var;VariantInit(&var);var.vt=VT_BSTR;var.bstrVal=SysAllocString(value);
    DISPID na=DISPID_PROPERTYPUT;DISPPARAMS p={&var,&na,1,1};
    g_pDisp->Invoke(dispid,IID_NULL,LOCALE_USER_DEFAULT,DISPATCH_PROPERTYPUT,&p,nullptr,nullptr,nullptr);
    VariantClear(&var);
}
static void UnityResize(int w,int h){
    if(!g_pIPO||w<=0||h<=0)return;RECT rc={0,0,w,h};g_pIPO->SetObjectRects(&rc,&rc);
}
// Remove runtime inline hooks by concrete target address
// (MH_RemoveHook(MH_ALL_HOOKS) is unreliable). Does not touch g_rtMonoStrNew:
// InstallRuntimeHooks re-assigns it after calling this.
static void RemoveExistingRtHooks(){
    if(g_rtHookTgtAbsURL){
        MH_DisableHook(g_rtHookTgtAbsURL);
        MH_RemoveHook(g_rtHookTgtAbsURL);
        g_rtHookTgtAbsURL=nullptr;
    }
    if(g_rtHookTgtSrcVal){
        MH_DisableHook(g_rtHookTgtSrcVal);
        MH_RemoveHook(g_rtHookTgtSrcVal);
        g_rtHookTgtSrcVal=nullptr;
    }
    if(g_rtHookTgtGetFPS){
        MH_DisableHook(g_rtHookTgtGetFPS);
        MH_RemoveHook(g_rtHookTgtGetFPS);
        g_rtHookTgtGetFPS=nullptr;
    }
    if(g_rtHookTgtLoop){
        MH_DisableHook(g_rtHookTgtLoop);
        MH_RemoveHook(g_rtHookTgtLoop);
        g_rtHookTgtLoop=nullptr;
    }
    if(g_rtHookTgtSetTfr){
        MH_DisableHook(g_rtHookTgtSetTfr);
        MH_RemoveHook(g_rtHookTgtSetTfr);
        g_rtHookTgtSetTfr=nullptr;
    }
    if(g_rtHookTgtSetVsy){
        MH_DisableHook(g_rtHookTgtSetVsy);
        MH_RemoveHook(g_rtHookTgtSetVsy);
        g_rtHookTgtSetVsy=nullptr;
    }
    // Undo the D3D9/DXGI interception so a dying runtime (or anything still
    // holding the device) never calls into freed trampoline code.
    if(g_rtHookTgtD3D9){
        MH_DisableHook(g_rtHookTgtD3D9);
        MH_RemoveHook(g_rtHookTgtD3D9);
        g_rtHookTgtD3D9=nullptr;
    }
    if(g_rtHookTgtDxgiF){
        MH_DisableHook(g_rtHookTgtDxgiF);
        MH_RemoveHook(g_rtHookTgtDxgiF);
        g_rtHookTgtDxgiF=nullptr;
    }
    if(g_rtHookTgtDxgiF1){
        MH_DisableHook(g_rtHookTgtDxgiF1);
        MH_RemoveHook(g_rtHookTgtDxgiF1);
        g_rtHookTgtDxgiF1=nullptr;
    }
    RestoreVtableSlot(g_scVtSlotPresent, reinterpret_cast<void*>(g_origSwapChainPresent));
    RestoreVtableSlot(g_dxgiFtSlotCreateSwapChain, reinterpret_cast<void*>(g_origDxgiCreateSwapChain));
    g_scVtSlotPresent=nullptr; g_dxgiFtSlotCreateSwapChain=nullptr;
    g_origSwapChainPresent=nullptr; g_origDxgiCreateSwapChain=nullptr;
    g_origCreateDxgiFactory=nullptr; g_origCreateDxgiFactory1=nullptr;
    RestoreVtableSlot(g_devVtSlotPresent, reinterpret_cast<void*>(g_origD3DPresent));
    RestoreVtableSlot(g_devVtSlotReset,   reinterpret_cast<void*>(g_origD3DReset));
    RestoreVtableSlot(g_d3d9VtSlotCreate, reinterpret_cast<void*>(g_origD3DCreateDevice));
    g_devVtSlotPresent=nullptr; g_devVtSlotReset=nullptr; g_d3d9VtSlotCreate=nullptr;
    g_origD3DPresent=nullptr; g_origD3DReset=nullptr; g_origD3DCreateDevice=nullptr;
    g_origD3DCreate9=nullptr;
    g_rtHookAbsURL=nullptr;
    g_rtHookSrcVal=nullptr;
    g_origAbsURL=nullptr;
    g_origSrcVal=nullptr;
    g_rtOrigGetFPS=nullptr;
    g_rtOrigLoop=nullptr;
    g_rtOrigSetTfr=nullptr;
    g_rtOrigSetVsy=nullptr;
}
static void UnityDestroy(){
    // Stop the hook-install timer (if still polling) before tearing down.
    if(g_hwndMain)KillTimer(g_hwndMain,TIMER_ID_INSTALL_HOOK);
    g_unityReady=false;g_gameLoaded=false;
    if(g_pIPO){g_pIPO->UIDeactivate();g_pIPO->InPlaceDeactivate();g_pIPO->Release();g_pIPO=nullptr;}
    if(g_pDisp){g_pDisp->Release();g_pDisp=nullptr;}
    if(g_pOleObj){g_pOleObj->Close(OLECLOSE_NOSAVE);g_pOleObj->Release();g_pOleObj=nullptr;}
    if(g_pSite){g_pSite->Release();g_pSite=nullptr;}
    // Disable hooks before the runtime DLL is unloaded by CoFreeUnusedLibrariesEx.
    if(g_rtHookTried){
        RemoveExistingRtHooks();
        g_rtHookTried=false;
    }
    CoFreeUnusedLibrariesEx(0,0);
    if(g_hwndMain)InvalidateRect(g_hwndMain,nullptr,TRUE);
}
static bool UnityCreate(HWND hwnd,const wchar_t*srcUrl){
    // Local path -> file:// URL: the WWW class resolves relative bundle paths
    // against this base URL (multi-bundle games).
    wchar_t fileUrl[MAX_PATH*2]={};
    if(PathIsURL(srcUrl)!=TRUE){
        // srcUrl is a bare Windows path like D:\...\main.unity3d
        wcscpy(fileUrl,L"file:///");
        size_t pos=8; // len("file:///")
        for(const wchar_t*p=srcUrl;*p&&pos<(MAX_PATH*2)-1;p++){
            fileUrl[pos++]=(*p==L'\\')?L'/':*p;
        }
        fileUrl[pos]=L'\0';
    }else{
        wcsncpy(fileUrl,srcUrl,(MAX_PATH*2)-1);
        fileUrl[(MAX_PATH*2)-1]=L'\0';
    }

    // Store for OCX URL-resolve hook fallback (multi-bundle games).
    wcsncpy(g_gameFileUrl,fileUrl,_countof(g_gameFileUrl)-1);
    g_gameFileUrl[_countof(g_gameFileUrl)-1]=L'\0';

    g_pSite=new UnityClientSite(hwnd,fileUrl);
    HRESULT hr=CoCreateInstance(CLSID_UnityWebPlayer,nullptr,CLSCTX_INPROC_SERVER,
                                IID_IOleObject,(void**)&g_pOleObj);
    if(FAILED(hr)){g_pSite->Release();g_pSite=nullptr;return false;}
    g_pOleObj->SetClientSite(g_pSite);OleSetContainedObject(g_pOleObj,TRUE);
    g_pOleObj->QueryInterface(IID_IDispatch,(void**)&g_pDisp);

    // CoCreateInstance just loaded the OCX — patch all three hooks now.
    InstallOcxRefererHook();
    InstallOcxLoadLibraryHook();
    InstallOcxUrlResolveHook();

    // src must be a URL, not a bare path (see fileUrl above).
    UnitySetPropW(L"src",fileUrl);
    UnitySetPropW(L"backgroundcolor",L"000000");
    UnitySetPropW(L"bordercolor",L"000000");
    UnitySetPropW(L"disableContextMenu",L"false");
    UnitySetPropW(L"disableFullscreen",L"false");

    RECT rc={};GetClientRect(hwnd,&rc);
    hr=g_pOleObj->DoVerb(OLEIVERB_INPLACEACTIVATE,nullptr,g_pSite,0,hwnd,&rc);
    if(FAILED(hr)){UnityDestroy();return false;}
    g_pOleObj->QueryInterface(IID_IOleInPlaceObject,(void**)&g_pIPO);
    if(g_pIPO)g_pIPO->SetObjectRects(&rc,&rc);
    g_unityReady=true;return true;
}

// ---------------------------------------------------------------------------
//  OCX IAT hook: inject Referer into the Unity WebPlayer loader's URL bind.
//
//  The loader's BSCb doesn't implement IHttpNegotiate, so no Referer is sent
//  by default and CDNs requiring one return 403. We patch urlmon!
//  RegisterBindStatusCallback in the OCX's IAT and wrap its BSCb with a
//  UnityBindCallback that injects g_currentReferer via BeginningTransaction.
// ---------------------------------------------------------------------------

static HRESULT WINAPI HookRegisterBindStatusCallback(IBindCtx* pBC,
                                                     IBindStatusCallback* pBSCb,
                                                     IBindStatusCallback** ppBSCbPrev,
                                                     DWORD dwReserved)
{
    if(g_currentReferer[0] && pBSCb){
        UnityBindCallback* wrapped=new UnityBindCallback(g_currentReferer,pBSCb);
        HRESULT hr=g_origRegisterBindStatusCallback(pBC,wrapped,ppBSCbPrev,dwReserved);
        wrapped->Release();
        return hr;
    }
    return g_origRegisterBindStatusCallback(pBC,pBSCb,ppBSCbPrev,dwReserved);
}

// Patch OCX IAT. Idempotent — skips if already patched, repatches if OCX reloaded.
static void InstallOcxRefererHook(){
    HMODULE hOcx=GetModuleHandleW(L"UnityWebPluginAX.ocx");
    if(!hOcx)return;

    BYTE* base=reinterpret_cast<BYTE*>(hOcx);
    IMAGE_DOS_HEADER* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return;
    IMAGE_NT_HEADERS* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE)return;

    IMAGE_DATA_DIRECTORY* impDir=&nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!impDir->VirtualAddress)return;

    IMAGE_IMPORT_DESCRIPTOR* desc=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+impDir->VirtualAddress);
    for(;desc->Name;desc++){
        const char* modName=reinterpret_cast<const char*>(base+desc->Name);
        if(_stricmp(modName,"urlmon.dll")!=0)continue;

        IMAGE_THUNK_DATA* thunk=reinterpret_cast<IMAGE_THUNK_DATA*>(base+desc->FirstThunk);
        IMAGE_THUNK_DATA* origThunk=desc->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base+desc->OriginalFirstThunk)
            : thunk;
        for(;origThunk->u1.AddressOfData;origThunk++,thunk++){
            if(IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal))continue;
            IMAGE_IMPORT_BY_NAME* ibn=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+origThunk->u1.AddressOfData);
            if(strcmp(reinterpret_cast<const char*>(ibn->Name),"RegisterBindStatusCallback")!=0)continue;

            void* slot=&thunk->u1.Function;
            FARPROC current=reinterpret_cast<FARPROC>(thunk->u1.Function);
            // Already patched (OCX still loaded from last UnityCreate)? Nothing to do.
            if(current==reinterpret_cast<FARPROC>(&HookRegisterBindStatusCallback))return;

            DWORD oldProt=0;
            if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&oldProt))return;
            g_origRegisterBindStatusCallback=reinterpret_cast<HRESULT(WINAPI*)(IBindCtx*,IBindStatusCallback*,IBindStatusCallback**,DWORD)>(current);
            thunk->u1.Function=reinterpret_cast<ULONG_PTR>(&HookRegisterBindStatusCallback);
            VirtualProtect(slot,sizeof(void*),oldProt,&oldProt);
            return;
        }
    }
}

// LoadLibrary detours: install runtime hooks the moment webplayer_win.dll
// loads (the engine creates its D3D device milliseconds later — the 200 ms
// install timer would be far too late). All four variants are hooked because
// modules in the load chain import different ones.
static bool RtHookCheckName(const wchar_t* wideName)
{
    if (!wideName) return false;
    const wchar_t* base = wcsrchr(wideName, L'\\');
    if (!base) base = wideName; else base++;
    return _wcsicmp(base, L"webplayer_win.dll") == 0;
}
static void RtHookOnRuntimeLoaded()
{
    g_rtHookTried = false;
    InstallRuntimeHooks();
}
static HMODULE WINAPI HookedLoadLibraryW(LPCWSTR libFileName)
{
    HMODULE h = g_origLoadLibraryW ? g_origLoadLibraryW(libFileName) : nullptr;
    if (h && RtHookCheckName(libFileName))
        RtHookOnRuntimeLoaded();
    return h;
}
static HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR libFileName, HANDLE file, DWORD flags)
{
    typedef HMODULE (WINAPI *Fn)(LPCWSTR, HANDLE, DWORD);
    Fn orig = reinterpret_cast<Fn>(g_origLoadLibraryExW);
    HMODULE h = orig ? orig(libFileName, file, flags) : nullptr;
    if (h && RtHookCheckName(libFileName))
        RtHookOnRuntimeLoaded();
    return h;
}
static HMODULE WINAPI HookedLoadLibraryA(LPCSTR libFileName)
{
    typedef HMODULE (WINAPI *Fn)(LPCSTR);
    Fn orig = reinterpret_cast<Fn>(g_origLoadLibraryA);
    HMODULE h = orig ? orig(libFileName) : nullptr;
    if (h && libFileName) {
        wchar_t wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, libFileName, -1, wide, MAX_PATH);
        if (RtHookCheckName(wide))
            RtHookOnRuntimeLoaded();
    }
    return h;
}
static HMODULE WINAPI HookedLoadLibraryExA(LPCSTR libFileName, HANDLE file, DWORD flags)
{
    typedef HMODULE (WINAPI *Fn)(LPCSTR, HANDLE, DWORD);
    Fn orig = reinterpret_cast<Fn>(g_origLoadLibraryExA);
    HMODULE h = orig ? orig(libFileName, file, flags) : nullptr;
    if (h && libFileName) {
        wchar_t wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, libFileName, -1, wide, MAX_PATH);
        if (RtHookCheckName(wide))
            RtHookOnRuntimeLoaded();
    }
    return h;
}

// Process-wide inline hooks on the kernel32 LoadLibrary variants.
// webplayer_win.dll is loaded by npUnity3D32.dll (not the OCX), whose IAT we
// cannot reach — hooking the function bodies catches the load regardless of
// which module initiates it.
static void InstallOcxLoadLibraryHook()
{
    static bool installed = false;
    if (installed) return;

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (!hK32) return;

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) return;

    struct { const char* name; void* hook; void** orig; } entries[] = {
        { "LoadLibraryW",   reinterpret_cast<void*>(&HookedLoadLibraryW),   reinterpret_cast<void**>(&g_origLoadLibraryW) },
        { "LoadLibraryExW", reinterpret_cast<void*>(&HookedLoadLibraryExW), reinterpret_cast<void**>(&g_origLoadLibraryExW) },
        { "LoadLibraryA",   reinterpret_cast<void*>(&HookedLoadLibraryA),   reinterpret_cast<void**>(&g_origLoadLibraryA) },
        { "LoadLibraryExA", reinterpret_cast<void*>(&HookedLoadLibraryExA), reinterpret_cast<void**>(&g_origLoadLibraryExA) },
    };
    int okCount = 0;
    for (int i = 0; i < 4; i++) {
        void* p = reinterpret_cast<void*>(GetProcAddress(hK32, entries[i].name));
        if (!p) continue;
        MH_STATUS sc = MH_CreateHook(p, entries[i].hook, entries[i].orig);
        if (sc == MH_ERROR_ALREADY_CREATED) {
            MH_RemoveHook(p);
            sc = MH_CreateHook(p, entries[i].hook, entries[i].orig);
        }
        if (sc == MH_OK && MH_EnableHook(p) == MH_OK)
            okCount++;
    }
    installed = (okCount > 0);
}

// ---------------------------------------------------------------------------
// OCX IAT hook: urlmon!CreateURLMonikerEx. WWW downloads with relative URLs
// ("Shared/Shared.unity3d") have no page URL to resolve against when we host
// the OCX directly — combine them with the game's own file:// URL instead.
// ---------------------------------------------------------------------------
typedef HRESULT (WINAPI *CreateURLMonikerExFn)(LPMONIKER, LPCWSTR, LPMONIKER*, DWORD);
static CreateURLMonikerExFn g_origCreateURLMonikerEx = nullptr;

// Does the string start with a URL scheme (" ALPHA *( ALPHA / DIGIT / + - . ) :" )?
static bool WstrHasScheme(const wchar_t* s)
{
    if (!s || !*s) return false;
    const wchar_t* p = s;
    if (!(*p >= L'a' && *p <= L'z') && !(*p >= L'A' && *p <= L'Z')) return false;
    for (p++; *p; p++) {
        if (*p == L':') return true;
        if (!(*p >= L'a' && *p <= L'z') && !(*p >= L'A' && *p <= L'Z') &&
            !(*p >= L'0' && *p <= L'9') && *p != L'+' && *p != L'-' && *p != L'.')
            return false;
    }
    return false;
}

static HRESULT WINAPI HookedCreateURLMonikerEx(LPMONIKER pmkContext, LPCWSTR szURL,
                                               LPMONIKER* ppmk, DWORD dwFlags)
{
    wchar_t absBuf[MAX_PATH * 4];

    if (!pmkContext && szURL && g_gameFileUrl[0] && !WstrHasScheme(szURL)) {
        // Lazily resolve CoInternetCombineUrl from urlmon.dll.
        if (!g_pCoInternetCombineUrl) {
            HMODULE hUm = GetModuleHandleW(L"urlmon.dll");
            if (hUm)
                g_pCoInternetCombineUrl = reinterpret_cast<CoInternetCombineUrlFn>(
                    GetProcAddress(hUm, "CoInternetCombineUrl"));
        }
        if (g_pCoInternetCombineUrl) {
            DWORD cch = _countof(absBuf);
            HRESULT hr = g_pCoInternetCombineUrl(g_gameFileUrl, szURL, 0,
                                                 absBuf, cch, &cch, 0);
            if (SUCCEEDED(hr))
                szURL = absBuf;
        }
    }
    return g_origCreateURLMonikerEx(pmkContext, szURL, ppmk, dwFlags);
}

// Patch OCX IAT: urlmon!CreateURLMonikerEx -> HookedCreateURLMonikerEx.
// Idempotent — skips if already patched.
static void InstallOcxUrlResolveHook()
{
    HMODULE hOcx = GetModuleHandleW(L"UnityWebPluginAX.ocx");
    if (!hOcx) return;

    BYTE* base = reinterpret_cast<BYTE*>(hOcx);
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    IMAGE_DATA_DIRECTORY* impDir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!impDir->VirtualAddress) return;

    IMAGE_IMPORT_DESCRIPTOR* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + impDir->VirtualAddress);
    for (; desc->Name; desc++) {
        const char* modName = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(modName, "urlmon.dll") != 0) continue;

        IMAGE_THUNK_DATA* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        IMAGE_THUNK_DATA* origThunk = desc->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
            : thunk;
        for (; origThunk->u1.AddressOfData; origThunk++, thunk++) {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), "CreateURLMonikerEx") != 0) continue;

            void* slot = &thunk->u1.Function;
            FARPROC current = reinterpret_cast<FARPROC>(thunk->u1.Function);
            // Already patched (OCX still loaded from last UnityCreate)? Nothing to do.
            if (current == reinterpret_cast<FARPROC>(&HookedCreateURLMonikerEx)) return;

            DWORD oldProt = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) return;
            g_origCreateURLMonikerEx = reinterpret_cast<CreateURLMonikerExFn>(current);
            thunk->u1.Function = reinterpret_cast<ULONG_PTR>(&HookedCreateURLMonikerEx);
            VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
//  Runtime inline hooks (webplayer_win.dll)
// ---------------------------------------------------------------------------
// Signature-based icall lookup via Mono's registration table (names[] + impls[]).
// No per-version RVAs needed.

struct PeRanges {
    BYTE* textStart; BYTE* textEnd;
    BYTE* dataStart; BYTE* dataEnd;  // .rdata + .data combined
    BYTE* dataOnlyStart; BYTE* dataOnlyEnd;  // .data section alone
};

static bool GetPeRanges(HMODULE hMod, PeRanges* r)
{
    if (!hMod || !r) return false;
    BYTE* base = reinterpret_cast<BYTE*>(hMod);
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    r->textStart = r->textEnd = r->dataStart = r->dataEnd = nullptr;
    r->dataOnlyStart = r->dataOnlyEnd = nullptr;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char name[9] = {};
        memcpy(name, sec[i].Name, 8);
        if (strcmp(name, ".text") == 0) {
            r->textStart = base + sec[i].VirtualAddress;
            r->textEnd = r->textStart + sec[i].Misc.VirtualSize;
        } else if (strcmp(name, ".rdata") == 0 || strcmp(name, ".data") == 0) {
            if (!r->dataStart) r->dataStart = base + sec[i].VirtualAddress;
            BYTE* end = base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize;
            if (end > r->dataEnd) r->dataEnd = end;
            if (strcmp(name, ".data") == 0) {
                r->dataOnlyStart = base + sec[i].VirtualAddress;
                r->dataOnlyEnd = end;
            }
        }
    }
    return r->textStart && r->dataStart;
}

static bool IsTextPtr(DWORD v, const PeRanges& r)
{
    return (DWORD)(uintptr_t)r.textStart <= v && v < (DWORD)(uintptr_t)r.textEnd;
}
static bool IsDataPtr(DWORD v, const PeRanges& r)
{
    return (DWORD)(uintptr_t)r.dataStart <= v && v < (DWORD)(uintptr_t)r.dataEnd;
}
static bool LooksLikeStringPtr(DWORD v, const PeRanges& r)
{
    if (!IsDataPtr(v, r)) return false;
    BYTE b = *reinterpret_cast<BYTE*>(v);
    return b >= 0x20 && b < 0x7f;
}

// Find a NUL-terminated string in .rdata/.data.
static DWORD FindStringInData(const PeRanges& r, const char* target)
{
    size_t len = strlen(target);
    for (BYTE* p = r.dataStart; p + len + 1 <= r.dataEnd; p++) {
        if (memcmp(p, target, len) == 0 && p[len] == '\0') {
            return (DWORD)(uintptr_t)p;
        }
    }
    return 0;
}

// Locate a Mono icall implementation function by name.
// Returns the function pointer, or nullptr if not found.
static void* FindIcallImpl(HMODULE hRt, const char* icallName)
{
    PeRanges r;
    if (!GetPeRanges(hRt, &r)) return nullptr;

    // 1. Find the icall name string.
    DWORD strAddr = FindStringInData(r, icallName);
    if (!strAddr) return nullptr;

    // 2. Find the names[] entry pointing to it.
    DWORD namePtrAddr = 0;
    for (DWORD* p = reinterpret_cast<DWORD*>(r.dataStart);
         p < reinterpret_cast<DWORD*>(r.dataEnd); p++) {
        if (*p == strAddr) { namePtrAddr = (DWORD)(uintptr_t)p; break; }
    }
    if (!namePtrAddr) return nullptr;

    // 3. Scan back to find names[] array start (consecutive string ptrs).
    DWORD* arrStart = reinterpret_cast<DWORD*>(namePtrAddr);
    while (arrStart - 1 >= reinterpret_cast<DWORD*>(r.dataStart) &&
           LooksLikeStringPtr(*(arrStart - 1), r)) {
        arrStart--;
    }

    // 4. Scan forward to find names[] end, compute index.
    DWORD* arrEnd = reinterpret_cast<DWORD*>(namePtrAddr) + 1;
    while (arrEnd + 1 <= reinterpret_cast<DWORD*>(r.dataEnd)) {
        DWORD v = *arrEnd;
        if (v == 0 || !LooksLikeStringPtr(v, r)) break;
        arrEnd++;
    }
    DWORD idx = (reinterpret_cast<DWORD*>(namePtrAddr) - arrStart);

    // 5. Scan forward from arrEnd for impls[] (first block of .text ptrs).
    DWORD* implStart = arrEnd;
    while (implStart + 5 <= reinterpret_cast<DWORD*>(r.dataEnd)) {
        DWORD v = *implStart;
        if (IsTextPtr(v, r)) {
            // Confirm it's a real array by checking a neighbour is also a text ptr.
            bool ok = false;
            for (int k = 1; k < 5; k++) {
                DWORD v2 = *(implStart + k);
                if (IsTextPtr(v2, r)) { ok = true; break; }
                if (v2 == 0) break;
            }
            if (ok) break;
        }
        implStart++;
    }
    if (implStart + idx >= reinterpret_cast<DWORD*>(r.dataEnd)) return nullptr;

    DWORD impl = *(implStart + idx);
    if (!IsTextPtr(impl, r)) return nullptr;
    return reinterpret_cast<void*>(impl);
}

// Parse the first N E8 rel32 call targets from a function's bytes.
// Returns the number of calls found (up to maxOut).
static int ParseCallTargets(BYTE* funcAddr, int scanLen, DWORD* outTargets, int maxOut)
{
    int count = 0;
    for (int i = 0; i < scanLen && count < maxOut; ) {
        if (funcAddr[i] == 0xE8) {
            int rel = *reinterpret_cast<int*>(funcAddr + i + 1);
            outTargets[count++] = (DWORD)(uintptr_t)(funcAddr + i + 5 + rel);
            i += 5;
        } else {
            i++;
        }
    }
    return count;
}

// Locate "FF 15 xx" (indirect call through an absolute pointer) in a byte
// buffer; returns the offset or -1.
static int FindFF15(const BYTE* p, int len)
{
    for (int i = 0; i + 6 <= len; i++) {
        if (p[i] == 0xFF && p[i + 1] == 0x15) return i;
    }
    return -1;
}

// Trace impl -> std_str_to_mono -> mono_string_new by parsing E8 call opcodes.
// Fast path (Unity 4.6+/5.x): the first two calls inside the icall are the
// value getter and the std::string->MonoString helper, both within 20 bytes.
// Fallback (Unity 4.3-era layout): the icall starts with a thread-check and
// an error-report branch, so the helper sits far past the 20-byte window.
// Deep-scan BOTH string getters (get_absoluteURL + get_srcValue), keep only
// call targets they share, and accept the first common helper whose own
// first call is a recognizable mono-string entry point:
//   (a) 55 8B EC 5D E9 rel32 — frame-setup + tail-jump trampoline; the jump
//       target is the cdecl (const char*, int) entry (exactly the signature
//       MonoStrNewFn uses);
//   (b) FF 15 [ptr] within the first 16 bytes where ptr lives in .data — a
//       thunk calling through a runtime-resolved function pointer; it takes
//       just (const char*) and the extra cdecl length argument is ignored.
static void* FindMonoStrNewFromImpl(HMODULE hRt, void* implFunc, void* implOther)
{
    BYTE* impl = reinterpret_cast<BYTE*>(implFunc);
    DWORD calls[3] = {};
    int n = ParseCallTargets(impl, 20, calls, 3);
    if (n >= 2) {
        // calls[0] = global_getter, calls[1] = std_str_to_mono
        BYTE* stdStrToMono = reinterpret_cast<BYTE*>(calls[1]);
        DWORD innerCalls[2] = {};
        int m = ParseCallTargets(stdStrToMono, 30, innerCalls, 2);
        if (m >= 1)
            return reinterpret_cast<void*>(innerCalls[0]);  // mono_string_new
        return nullptr;
    }

    // Fallback for the thread-check-prologue layout.
    if (!hRt || !implOther) return nullptr;
    PeRanges r;
    if (!GetPeRanges(hRt, &r)) return nullptr;

    const int MAXC = 10;
    DWORD deepA[MAXC]; int na = ParseCallTargets(impl, 128, deepA, MAXC);
    DWORD deepS[MAXC]; int ns = ParseCallTargets(reinterpret_cast<BYTE*>(implOther), 128, deepS, MAXC);

    for (int i = 0; i < na; i++) {
        DWORD t = deepA[i];
        if (!IsTextPtr(t, r)) continue;
        bool dup = false;
        for (int j = 0; j < i; j++)
            if (deepA[j] == t) { dup = true; break; }
        if (dup) continue;
        bool common = false;
        for (int j = 0; j < ns; j++)
            if (deepS[j] == t) { common = true; break; }
        if (!common) continue;

        DWORD tin[2] = {};
        int nt = ParseCallTargets(reinterpret_cast<BYTE*>(t), 40, tin, 2);
        if (nt < 1) continue;
        DWORD u = tin[0];
        if (!IsTextPtr(u, r)) continue;
        BYTE ub[32] = {};
        memcpy(ub, reinterpret_cast<void*>(u), sizeof(ub));

        // (a) frame-setup + tail-jump trampoline -> (ptr, len) entry
        if (ub[0] == 0x55 && ub[1] == 0x8B && ub[2] == 0xEC &&
            ub[3] == 0x5D && ub[4] == 0xE9) {
            int rel = *reinterpret_cast<int*>(ub + 5);
            DWORD v = u + 9 + (DWORD)rel;
            if (IsTextPtr(v, r)) return reinterpret_cast<void*>(v);
        }
        // (b) indirect call through a .data function pointer -> (const char*) thunk
        int off = FindFF15(ub, 16);
        if (off >= 0) {
            DWORD p = *reinterpret_cast<DWORD*>(ub + off + 2);
            if (r.dataOnlyStart && r.dataOnlyStart <= reinterpret_cast<BYTE*>(p) &&
                reinterpret_cast<BYTE*>(p) < r.dataOnlyEnd)
                return reinterpret_cast<void*>(u);
        }
    }
    return nullptr;
}

// Detour: return spoofed MonoString if set, else call original.
static volatile LONG g_absUrlCallCnt = 0;
static void* __cdecl HookedGetAbsoluteURL()
{
    InterlockedIncrement(&g_absUrlCallCnt);
    void* origRet = g_origAbsURL ? g_origAbsURL() : nullptr;
    if (g_spoofedUrl[0] && g_rtMonoStrNew) {
        return g_rtMonoStrNew(g_spoofedUrl, (int)strlen(g_spoofedUrl));
    }
    return origRet;
}

static volatile LONG g_srcValCallCnt = 0;
static void* __cdecl HookedGetSrcValue()
{
    InterlockedIncrement(&g_srcValCallCnt);
    void* origRet = g_origSrcVal ? g_origSrcVal() : nullptr;
    if (g_spoofedUrl[0] && g_rtMonoStrNew) {
        return g_rtMonoStrNew(g_spoofedUrl, (int)strlen(g_spoofedUrl));
    }
    return origRet;
}

// ---------------------------------------------------------------------------
//  Experimental frame-rate override: detours
// ---------------------------------------------------------------------------

typedef int (__cdecl *FpsGetterFn)();
typedef int (__cdecl *FpsSetterFn)(int);
typedef int (__cdecl *FpsLoopFn)();

// UnityWinWebLoop: counts completed pumps. The FPS force must not run before
// the first one — set_vSyncCount dereferences the graphics device (created
// inside the first call) with no null check, and the loader queries the FPS
// one tick BEFORE that call.
static int __cdecl HookedUnityWinWebLoop()
{
    FpsLoopFn orig = reinterpret_cast<FpsLoopFn>(g_rtOrigLoop);
    int r = orig ? orig() : 0;
    g_rtLoopCalls++;
    return r;
}

// UnityGetPlayerTargetFPS: the loader queries this after each pump, making it
// the perfect re-apply point — whatever the game wrote into its quality
// settings during the frame gets reverted one tick later at most. The icall
// wrappers no-op on unchanged values and run on the pump thread, so the
// per-query re-apply is cheap and race-free.
static int __cdecl HookedGetPlayerTargetFPS()
{
    if (g_fpsTarget > 0) {
        if (g_rtLoopCalls > 0) {
            FpsSetterFn setVsy = reinterpret_cast<FpsSetterFn>(g_rtOrigSetVsy);
            FpsSetterFn setTfr = reinterpret_cast<FpsSetterFn>(g_rtOrigSetTfr);
            if (setVsy) setVsy(0);                        // clear game's vSync cap
            if (setTfr) setTfr(ENGINE_TFR_ACTIVE);        // engine never self-throttles
            if ((InterlockedIncrement(&g_fpsQueryCount) % 60) == 0)
                FpsAdaptiveStep();
        }
        return g_reportFps;
    }
    FpsGetterFn orig = reinterpret_cast<FpsGetterFn>(g_rtOrigGetFPS);
    return orig ? orig() : 60;
}

// set_targetFrameRate icall — pin HIGH while active (loader timer paces).
static int __cdecl HookedSetTargetFrameRate(int fps)
{
    FpsSetterFn orig = reinterpret_cast<FpsSetterFn>(g_rtOrigSetTfr);
    return orig ? orig(g_fpsTarget > 0 ? ENGINE_TFR_ACTIVE : fps) : fps;
}

// set_vSyncCount icall — force 0 while active.
static int __cdecl HookedSetVSyncCount(int count)
{
    FpsSetterFn orig = reinterpret_cast<FpsSetterFn>(g_rtOrigSetVsy);
    return orig ? orig(g_fpsTarget > 0 ? 0 : count) : count;
}

// ---------------------------------------------------------------------------
//  Experimental frame-rate override: D3D9 interception
// ---------------------------------------------------------------------------

// IMMEDIATE: the engine's internal frame wait already CPU-waits for each
// slot; a vblank-synced Present would serialize with it and halve the rate.
// Tearing is the trade-off.
static DWORD FpsOverridePresentInterval()
{
    return D3DPRESENT_INTERVAL_IMMEDIATE;
}

// Write a hook pointer into a (read-only) COM vtable slot. Idempotent.
static bool PatchVtableSlot(void** slot, void* hookFn, void** savedOrig)
{
    if (!slot) return false;
    if (*slot == hookFn) return true;          // already ours
    DWORD oldProt = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt))
        return false;
    *savedOrig = *slot;
    *slot = hookFn;
    VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
    return true;
}
static void RestoreVtableSlot(void** slot, void* savedOrig)
{
    if (!slot || !savedOrig) return;
    DWORD oldProt = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        *slot = savedOrig;
        VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
    }
}

// IDirect3DDevice9::Present — pass-through.
static HRESULT WINAPI HookedD3D9Present(IDirect3DDevice9* dev, const RECT* src,
                                        const RECT* dst, HWND wnd, const RGNDATA* dirty)
{
    return g_origD3DPresent ? g_origD3DPresent(dev, src, dst, wnd, dirty) : E_FAIL;
}

// Reset — re-force the interval (fullscreen toggles / resizes go through here).
static HRESULT WINAPI HookedD3D9Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    if (pp && g_fpsTarget > 0)
        pp->PresentationInterval = FpsOverridePresentInterval();
    return g_origD3DReset ? g_origD3DReset(dev, pp) : E_FAIL;
}

// CreateDevice — force the interval, patch the device vtable (Reset + Present).
static HRESULT WINAPI HookedD3D9CreateDevice(IDirect3D9* d3d, UINT adapter,
                                             D3DDEVTYPE type, HWND hwnd, DWORD flags,
                                             D3DPRESENT_PARAMETERS* pp,
                                             IDirect3DDevice9** out)
{
    if (pp && g_fpsTarget > 0)
        pp->PresentationInterval = FpsOverridePresentInterval();
    HRESULT hr = g_origD3DCreateDevice
        ? g_origD3DCreateDevice(d3d, adapter, type, hwnd, flags, pp, out)
        : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) {
        void** vt = *reinterpret_cast<void***>(*out);
        if (PatchVtableSlot(&vt[16], reinterpret_cast<void*>(&HookedD3D9Reset),
                            reinterpret_cast<void**>(&g_origD3DReset)))
            g_devVtSlotReset = &vt[16];
        if (PatchVtableSlot(&vt[17], reinterpret_cast<void*>(&HookedD3D9Present),
                            reinterpret_cast<void**>(&g_origD3DPresent)))
            g_devVtSlotPresent = &vt[17];
    }
    return hr;
}

// Direct3DCreate9 — patch the returned object's CreateDevice vtable entry.
static IDirect3D9* WINAPI HookedDirect3DCreate9(UINT sdkVersion)
{
    IDirect3D9* d3d = g_origD3DCreate9 ? g_origD3DCreate9(sdkVersion) : nullptr;
    if (d3d && g_fpsTarget > 0) {
        void** vt = *reinterpret_cast<void***>(d3d);
        if (PatchVtableSlot(&vt[16], reinterpret_cast<void*>(&HookedD3D9CreateDevice),
                            reinterpret_cast<void**>(&g_origD3DCreateDevice)))
            g_d3d9VtSlotCreate = &vt[16];
    }
    return d3d;
}

// ---------------------------------------------------------------------------
//  DXGI hooks (D3D11 path)
// ---------------------------------------------------------------------------

// Sync interval 0 (immediate) — same rationale as the D3D9 interval above.
static UINT FpsOverrideSyncInterval()
{
    return 0u;
}

// IDXGISwapChain::Present — force the sync interval.
static HRESULT WINAPI HookedSwapChainPresent(IDXGISwapChain* sc, UINT SyncInterval,
                                             UINT Flags)
{
    if (g_fpsTarget > 0)
        SyncInterval = FpsOverrideSyncInterval();
    return g_origSwapChainPresent ? g_origSwapChainPresent(sc, SyncInterval, Flags)
                                  : E_FAIL;
}

// CreateSwapChainForHwnd — patch the returned swap chain's Present.
static HRESULT WINAPI HookedDxgiCreateSwapChainForHwnd(
    void* factory, IUnknown* device, HWND hwnd,
    const void* desc, const void* fullscreenDesc,
    IDXGIOutput* restrictToOutput, IDXGISwapChain1** out)
{
    DxgiCreateSwapChainForHwndFn orig =
        reinterpret_cast<DxgiCreateSwapChainForHwndFn>(g_origDxgiCreateSwapChain);
    HRESULT hr = orig
        ? orig(factory, device, hwnd, desc, fullscreenDesc,
               restrictToOutput, out)
        : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) {
        void** vt = *reinterpret_cast<void***>(*out);
        if (PatchVtableSlot(&vt[8], reinterpret_cast<void*>(&HookedSwapChainPresent),
                            reinterpret_cast<void**>(&g_origSwapChainPresent)))
            g_scVtSlotPresent = &vt[8];
    }
    return hr;
}

static void PatchDxgiFactoryVtable(void* factory)
{
    void** vt = *reinterpret_cast<void***>(factory);
    if (PatchVtableSlot(&vt[15], reinterpret_cast<void*>(&HookedDxgiCreateSwapChainForHwnd),
                        reinterpret_cast<void**>(&g_origDxgiCreateSwapChain)))
        g_dxgiFtSlotCreateSwapChain = &vt[15];
}

// dxgi.dll!CreateDXGIFactory / CreateDXGIFactory1
static HRESULT WINAPI HookedCreateDxgiFactory(REFIID riid, void** ppFactory)
{
    HRESULT hr = g_origCreateDxgiFactory
        ? g_origCreateDxgiFactory(riid, ppFactory) : E_FAIL;
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        PatchDxgiFactoryVtable(*ppFactory);
    return hr;
}
static HRESULT WINAPI HookedCreateDxgiFactory1(REFIID riid, void** ppFactory)
{
    HRESULT hr = g_origCreateDxgiFactory1
        ? g_origCreateDxgiFactory1(riid, ppFactory) : E_FAIL;
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        PatchDxgiFactoryVtable(*ppFactory);
    return hr;
}

// Spoofed URL = referer host + file name, e.g.
//   https://www.4399.com/ + trn2.unity3d -> https://www.4399.com/trn2.unity3d
// Empty for URL games (no spoof).
static void BuildSpoofedUrl(const wchar_t* path, const wchar_t* referer)
{
    g_spoofedUrl[0] = '\0';

    // With a referer: build an https:// URL from its host.
    if (path && path[0] && referer && referer[0] && PathIsURL(path) != TRUE) {
        const wchar_t* p = wcsstr(referer, L"://");
        if (!p) p = referer; else p += 3;
        wchar_t host[256] = {};
        size_t i = 0;
        while (*p && *p != L'/' && *p != L'?' && *p != L'#' && i < 255) host[i++] = *p++;
        host[i] = L'\0';
        if (host[0]) {
            // Extract filename from path.
            const wchar_t* fn = wcsrchr(path, L'\\');
            const wchar_t* fn2 = wcsrchr(path, L'/');
            if (fn2 > fn) fn = fn2;
            fn = fn ? fn + 1 : path;
            if (fn[0]) {
                wchar_t urlW[1024];
                _snwprintf(urlW, 1023, L"https://%s/%s", host, fn);
                urlW[1023] = L'\0';
                WideCharToMultiByte(CP_UTF8, 0, urlW, -1, g_spoofedUrl, sizeof(g_spoofedUrl), nullptr, nullptr);
                g_spoofedUrl[sizeof(g_spoofedUrl) - 1] = '\0';
            }
        }
    }

    // Without a referer: use the file:// URL (WWW relative-path base).
    if (!g_spoofedUrl[0] && path && path[0] && PathIsURL(path) != TRUE) {
        wchar_t fileUrl[MAX_PATH * 2] = {};
        wcscpy(fileUrl, L"file:///");
        size_t pos = 8;
        for (const wchar_t* p = path; *p && pos < _countof(fileUrl) - 1; p++)
            fileUrl[pos++] = (*p == L'\\') ? L'/' : *p;
        fileUrl[pos] = L'\0';
        WideCharToMultiByte(CP_UTF8, 0, fileUrl, -1, g_spoofedUrl, sizeof(g_spoofedUrl), nullptr, nullptr);
        g_spoofedUrl[sizeof(g_spoofedUrl) - 1] = '\0';
    }
}

// Install runtime inline hooks on get_absoluteURL/get_srcValue, plus the
// experimental frame-rate override when g_fpsTarget > 0.
// Idempotent per UnityCreate cycle; g_rtHookTried resets in UnityDestroy.
// The two feature sets are independent: a failed URL-hook lookup only
// disables spoofing, never the frame-rate override (and vice versa).
static bool InstallRuntimeHooks()
{
    if (g_rtHookTried)
        return g_rtHookAbsURL != nullptr || g_rtHookTgtGetFPS != nullptr;

    HMODULE hRt = GetModuleHandleW(L"webplayer_win.dll");
    if (!hRt) return false;  // not loaded yet — timer retries; don't set tried
    g_rtHookTried = true;
    g_rtLoopCalls = 0;       // engine has not pumped any frame yet

    static bool mhInited = false;
    if (!mhInited) {
        // OCX URL hook may have initialised MinHook already — that's fine.
        MH_STATUS s = MH_Initialize();
        if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
            return false;
        mhInited = true;
    }

    // ---- URL spoof hooks (optional) ----
    bool urlInstalled = false;
    void* pAbsURL = FindIcallImpl(hRt, "UnityEngine.Application::get_absoluteURL");
    void* pSrcVal = FindIcallImpl(hRt, "UnityEngine.Application::get_srcValue");
    if (pAbsURL && pSrcVal) {
        // Trace impl -> std_str_to_mono -> mono_string_new_len (with a
        // fallback for the Unity 4.3-era thread-check icall prologue).
        g_rtMonoStrNew = reinterpret_cast<MonoStrNewFn>(
            FindMonoStrNewFromImpl(hRt, pAbsURL, pSrcVal));
        if (g_rtMonoStrNew) {
            RemoveExistingRtHooks();

            MH_STATUS s1 = MH_CreateHook(pAbsURL, reinterpret_cast<void*>(&HookedGetAbsoluteURL),
                                         reinterpret_cast<void**>(&g_origAbsURL));
            if (s1 == MH_ERROR_ALREADY_CREATED) {
                MH_RemoveHook(pAbsURL);
                s1 = MH_CreateHook(pAbsURL, reinterpret_cast<void*>(&HookedGetAbsoluteURL),
                                   reinterpret_cast<void**>(&g_origAbsURL));
            }
            if (s1 == MH_OK) {
                g_rtHookTgtAbsURL = pAbsURL;

                MH_STATUS s2 = MH_CreateHook(pSrcVal, reinterpret_cast<void*>(&HookedGetSrcValue),
                                             reinterpret_cast<void**>(&g_origSrcVal));
                if (s2 == MH_ERROR_ALREADY_CREATED) {
                    MH_RemoveHook(pSrcVal);
                    s2 = MH_CreateHook(pSrcVal, reinterpret_cast<void*>(&HookedGetSrcValue),
                                       reinterpret_cast<void**>(&g_origSrcVal));
                }
                if (s2 == MH_OK) {
                    g_rtHookTgtSrcVal = pSrcVal;
                    urlInstalled = true;
                } else {
                    // half-created set: roll the URL hooks back and go on
                    MH_DisableHook(pAbsURL); MH_RemoveHook(pAbsURL);
                    g_rtHookTgtAbsURL = nullptr; g_origAbsURL = nullptr;
                }
            }
        }
    }

    // ---- Experimental: frame-rate override (g_fpsTarget from registry) ----
    // Hooks are optional: a missing symbol (very old runtimes) only disables
    // the feature, never the URL spoofing above.
    if (g_fpsTarget > 0) {
        g_fpsQueryCount = 0;
        if (!g_qpcFreq.QuadPart)
            QueryPerformanceFrequency(&g_qpcFreq);
        // Adaptive compensation init: assume the floor is one monitor refresh
        // period; the EMA corrects this within a couple of seconds of play.
        {
            DEVMODE dm = {};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) &&
                dm.dmDisplayFrequency >= 30 && dm.dmDisplayFrequency <= 1000)
                g_monitorRefreshHz = dm.dmDisplayFrequency;
            g_baseTickMs = 1000.0 / g_monitorRefreshHz;
            g_reportFps = ComputeReportFpsFor(g_baseTickMs);
            QueryPerformanceCounter(&g_windowStart);
        }

        // D3D9/DXGI interception — hooking the export bodies, since the
        // runtimes' GetProcAddress is a delay-load import (IAT patches get
        // wiped on first call). Pre-loading the DLLs is harmless.
        {
            HMODULE hD3d9 = GetModuleHandleW(L"d3d9.dll");
            if (!hD3d9) hD3d9 = LoadLibraryW(L"d3d9.dll");
            if (hD3d9) {
                void* pCreate9 = reinterpret_cast<void*>(GetProcAddress(hD3d9, "Direct3DCreate9"));
                if (pCreate9) {
                    MH_STATUS sd = MH_CreateHook(pCreate9,
                                                 reinterpret_cast<void*>(&HookedDirect3DCreate9),
                                                 reinterpret_cast<void**>(&g_origD3DCreate9));
                    if (sd == MH_ERROR_ALREADY_CREATED) {
                        MH_RemoveHook(pCreate9);
                        sd = MH_CreateHook(pCreate9,
                                           reinterpret_cast<void*>(&HookedDirect3DCreate9),
                                           reinterpret_cast<void**>(&g_origD3DCreate9));
                    }
                    if (sd == MH_OK)
                        g_rtHookTgtD3D9 = pCreate9;
                }
            }
        }

        // DXGI factory exports (D3D11 path).
        {
            HMODULE hDxgi = GetModuleHandleW(L"dxgi.dll");
            if (!hDxgi) hDxgi = LoadLibraryW(L"dxgi.dll");
            if (hDxgi) {
                void* pF  = reinterpret_cast<void*>(GetProcAddress(hDxgi, "CreateDXGIFactory"));
                void* pF1 = reinterpret_cast<void*>(GetProcAddress(hDxgi, "CreateDXGIFactory1"));
                if (pF) {
                    MH_STATUS sx = MH_CreateHook(pF,
                                                 reinterpret_cast<void*>(&HookedCreateDxgiFactory),
                                                 reinterpret_cast<void**>(&g_origCreateDxgiFactory));
                    if (sx == MH_ERROR_ALREADY_CREATED) {
                        MH_RemoveHook(pF);
                        sx = MH_CreateHook(pF,
                                           reinterpret_cast<void*>(&HookedCreateDxgiFactory),
                                           reinterpret_cast<void**>(&g_origCreateDxgiFactory));
                    }
                    if (sx == MH_OK)
                        g_rtHookTgtDxgiF = pF;
                }
                if (pF1) {
                    MH_STATUS sx = MH_CreateHook(pF1,
                                                 reinterpret_cast<void*>(&HookedCreateDxgiFactory1),
                                                 reinterpret_cast<void**>(&g_origCreateDxgiFactory1));
                    if (sx == MH_ERROR_ALREADY_CREATED) {
                        MH_RemoveHook(pF1);
                        sx = MH_CreateHook(pF1,
                                           reinterpret_cast<void*>(&HookedCreateDxgiFactory1),
                                           reinterpret_cast<void**>(&g_origCreateDxgiFactory1));
                    }
                    if (sx == MH_OK)
                        g_rtHookTgtDxgiF1 = pF1;
                }
            }
        }

        void* pGetFPS = reinterpret_cast<void*>(GetProcAddress(hRt, "UnityGetPlayerTargetFPS"));
        void* pLoop   = reinterpret_cast<void*>(GetProcAddress(hRt, "UnityWinWebLoop"));
        void* pSetTfr = FindIcallImpl(hRt, "UnityEngine.Application::set_targetFrameRate");
        void* pSetVsy = FindIcallImpl(hRt, "UnityEngine.QualitySettings::set_vSyncCount");

        if (pGetFPS && pLoop && pSetTfr && pSetVsy) {
            MH_STATUS sf = MH_CreateHook(pGetFPS, reinterpret_cast<void*>(&HookedGetPlayerTargetFPS),
                                         reinterpret_cast<void**>(&g_rtOrigGetFPS));
            if (sf == MH_ERROR_ALREADY_CREATED) {
                MH_RemoveHook(pGetFPS);
                sf = MH_CreateHook(pGetFPS, reinterpret_cast<void*>(&HookedGetPlayerTargetFPS),
                                   reinterpret_cast<void**>(&g_rtOrigGetFPS));
            }
            if (sf == MH_OK) {
                g_rtHookTgtGetFPS = pGetFPS;

                sf = MH_CreateHook(pLoop, reinterpret_cast<void*>(&HookedUnityWinWebLoop),
                                   reinterpret_cast<void**>(&g_rtOrigLoop));
                if (sf == MH_ERROR_ALREADY_CREATED) {
                    MH_RemoveHook(pLoop);
                    sf = MH_CreateHook(pLoop, reinterpret_cast<void*>(&HookedUnityWinWebLoop),
                                       reinterpret_cast<void**>(&g_rtOrigLoop));
                }
                if (sf == MH_OK) {
                    g_rtHookTgtLoop = pLoop;

                    sf = MH_CreateHook(pSetTfr, reinterpret_cast<void*>(&HookedSetTargetFrameRate),
                                       reinterpret_cast<void**>(&g_rtOrigSetTfr));
                    if (sf == MH_ERROR_ALREADY_CREATED) {
                        MH_RemoveHook(pSetTfr);
                        sf = MH_CreateHook(pSetTfr, reinterpret_cast<void*>(&HookedSetTargetFrameRate),
                                           reinterpret_cast<void**>(&g_rtOrigSetTfr));
                    }
                    if (sf == MH_OK) {
                        g_rtHookTgtSetTfr = pSetTfr;

                        sf = MH_CreateHook(pSetVsy, reinterpret_cast<void*>(&HookedSetVSyncCount),
                                           reinterpret_cast<void**>(&g_rtOrigSetVsy));
                        if (sf == MH_ERROR_ALREADY_CREATED) {
                            MH_RemoveHook(pSetVsy);
                            sf = MH_CreateHook(pSetVsy, reinterpret_cast<void*>(&HookedSetVSyncCount),
                                               reinterpret_cast<void**>(&g_rtOrigSetVsy));
                        }
                        if (sf == MH_OK) {
                            g_rtHookTgtSetVsy = pSetVsy;
                        } else {
                            // incomplete set: roll back everything so the
                            // one-shot force never uses a half-installed set
                            MH_DisableHook(pSetTfr); MH_RemoveHook(pSetTfr);
                            g_rtHookTgtSetTfr = nullptr; g_rtOrigSetTfr = nullptr;
                            MH_DisableHook(pLoop); MH_RemoveHook(pLoop);
                            g_rtHookTgtLoop = nullptr; g_rtOrigLoop = nullptr;
                            MH_DisableHook(pGetFPS); MH_RemoveHook(pGetFPS);
                            g_rtHookTgtGetFPS = nullptr; g_rtOrigGetFPS = nullptr;
                        }
                    } else {
                        MH_DisableHook(pLoop); MH_RemoveHook(pLoop);
                        g_rtHookTgtLoop = nullptr; g_rtOrigLoop = nullptr;
                        MH_DisableHook(pGetFPS); MH_RemoveHook(pGetFPS);
                        g_rtHookTgtGetFPS = nullptr; g_rtOrigGetFPS = nullptr;
                    }
                } else {
                    MH_DisableHook(pGetFPS); MH_RemoveHook(pGetFPS);
                    g_rtHookTgtGetFPS = nullptr; g_rtOrigGetFPS = nullptr;
                }
            }
        }
    }

    // Nothing installed at all? Report failure (no retry — same as before).
    if (!urlInstalled && !g_rtHookTgtGetFPS && !g_rtHookTgtD3D9 &&
        !g_rtHookTgtDxgiF && !g_rtHookTgtDxgiF1)
        return false;

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { RemoveExistingRtHooks(); return false; }

    if (urlInstalled) {
        g_rtHookAbsURL = reinterpret_cast<void*>(g_origAbsURL);
        g_rtHookSrcVal = reinterpret_cast<void*>(g_origSrcVal);
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Game loading – the central function
// ---------------------------------------------------------------------------
static void LoadFileOrUrl(const wchar_t*pathArg,const wchar_t*refererArg)
{
    wchar_t path[MAX_PATH*2];
    wcsncpy(path,pathArg,(MAX_PATH*2)-1);path[(MAX_PATH*2)-1]=L'\0';

    bool isUrl=(PathIsURL(path)==TRUE);

    // Snapshot referer into a local buffer first: ReloadGame passes
    // g_currentReferer as refererArg, so clearing g_currentReferer below
    // would also zero out refererArg if we read it afterwards.
    wchar_t refSnap[MAX_PATH*2]={};
    if(refererArg && refererArg[0]){
        wcsncpy(refSnap,refererArg,(MAX_PATH*2)-1);
        refSnap[(MAX_PATH*2)-1]=L'\0';
    }
    g_currentReferer[0]=L'\0';
    if(refSnap[0]){
        wcsncpy(g_currentReferer,refSnap,(sizeof(g_currentReferer)/sizeof(wchar_t))-1);
        g_currentReferer[(sizeof(g_currentReferer)/sizeof(wchar_t))-1]=L'\0';
    }

    if(!CheckAndWarnSavePath(path))return;

    SetStatus(LS("STATUS_LOADING"));UpdateWindow(g_hwndMain);

    BundleInfo info;
    if(isUrl)info=ReadBundleFromURL(path);
    else      info=ReadBundleFromFile(path);

    wcsncpy(g_currentPath,path,(sizeof(g_currentPath)/sizeof(wchar_t))-1);
    g_currentPath[(sizeof(g_currentPath)/sizeof(wchar_t))-1]=L'\0';

    UnityDestroy();

    if(info.valid){
        wchar_t verW[36]={};
        int vi=0; for(;info.version[vi]&&vi<35;vi++) verW[vi]=(wchar_t)(unsigned char)info.version[vi];
        verW[vi]=L'\0';

        wchar_t msg[220];
        _snwprintf(msg,219,LS("STATUS_LOADING_VERSION"),
                  verW,ChannelForVersion(info.major,info.minor));
        msg[219]=0;
        SetStatus(msg);UpdateWindow(g_hwndMain);
        SwitchRuntime(info.major,info.minor);
        CoFreeUnusedLibrariesEx(0,0);Sleep(150);
    }

    // For local files with a referer, spoof absoluteURL/srcValue so URL-based
    // anti-piracy checks pass. URL games are left untouched.
    BuildSpoofedUrl(path,refSnap);

    if(!UnityCreate(g_hwndMain,path)){
        SetStatus(LS("STATUS_CREATE_FAILED"));
        g_currentPath[0]=L'\0';
        return;
    }

    // webplayer_win.dll loads lazily after DoVerb returns. Poll via timer
    // until it appears, then install inline hooks. If it is already in
    // memory (reload in the same process — CoFreeUnusedLibrariesEx may not
    // actually unload it), install right away: the engine re-initializes its
    // D3D device within milliseconds of the new load starting.
    if (GetModuleHandleW(L"webplayer_win.dll")) {
        g_rtHookTried = false;
        InstallRuntimeHooks();
    }
    g_hookRetryCnt=0;
    SetTimer(g_hwndMain,TIMER_ID_INSTALL_HOOK,HOOK_RETRY_INTERVAL_MS,nullptr);
    g_gameLoaded=true;
    MruAdd(path,g_currentReferer,path);RebuildFileMenu();
}

static void ReloadGame(){
    if(!g_currentPath[0])return;
    LoadFileOrUrl(g_currentPath,g_currentReferer);
}
static void CloseGame(){
    g_currentPath[0]=L'\0';
    g_currentReferer[0]=L'\0';
    UnityDestroy();
    SetStatus(LS("STATUS_IDLE"));
}

// ---------------------------------------------------------------------------
//  Fullscreen toggle
// ---------------------------------------------------------------------------
static void ToggleFullscreen(){
    if(!g_fullscreen){
        GetWindowRect(g_hwndMain,&g_savedRect);
        DWORD s=GetWindowLong(g_hwndMain,GWL_STYLE);
        SetWindowLong(g_hwndMain,GWL_STYLE,
            s&~(WS_CAPTION|WS_THICKFRAME|WS_SYSMENU|WS_MAXIMIZEBOX|WS_MINIMIZEBOX));
        g_savedMenu=GetMenu(g_hwndMain);
        SetMenu(g_hwndMain,nullptr);
        HMONITOR hm=MonitorFromWindow(g_hwndMain,MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi={};mi.cbSize=sizeof(mi);GetMonitorInfo(hm,&mi);
        SetWindowPos(g_hwndMain,HWND_TOPMOST,
                     mi.rcMonitor.left,mi.rcMonitor.top,
                     mi.rcMonitor.right-mi.rcMonitor.left,
                     mi.rcMonitor.bottom-mi.rcMonitor.top,
                     SWP_FRAMECHANGED|SWP_NOOWNERZORDER);
        g_fullscreen=true;
    }else{
        DWORD s=GetWindowLong(g_hwndMain,GWL_STYLE);
        SetWindowLong(g_hwndMain,GWL_STYLE,
            s|WS_CAPTION|WS_THICKFRAME|WS_SYSMENU|WS_MAXIMIZEBOX|WS_MINIMIZEBOX);
        if(g_savedMenu){SetMenu(g_hwndMain,g_savedMenu);g_savedMenu=nullptr;}
        SetWindowPos(g_hwndMain,HWND_NOTOPMOST,
                     g_savedRect.left,g_savedRect.top,
                     g_savedRect.right-g_savedRect.left,g_savedRect.bottom-g_savedRect.top,
                     SWP_FRAMECHANGED|SWP_NOOWNERZORDER);
        g_fullscreen=false;
    }
}

// ---------------------------------------------------------------------------
//  Dialogs
// ---------------------------------------------------------------------------
static wchar_t g_openResult[MAX_PATH*2]={};
static wchar_t g_openReferer[MAX_PATH*2]={};
INT_PTR CALLBACK OpenDlgProc(HWND hDlg,UINT msg,WPARAM wp,LPARAM){
    switch(msg){
    case WM_INITDIALOG:
        SetWindowText(hDlg,LS("OPEN_TITLE"));
        SetDlgItemText(hDlg,IDC_OPEN_URLLABEL,LS("OPEN_URL_LABEL"));
        SetDlgItemText(hDlg,IDC_OPEN_EXAMPLE,LS("OPEN_URL_EXAMPLE"));
        SetDlgItemText(hDlg,IDC_OPEN_SEPARATOR,LS("OPEN_BROWSE_SEPARATOR"));
        SetDlgItemText(hDlg,IDC_BROWSE,LS("OPEN_BROWSE_BTN"));
        SetDlgItemText(hDlg,IDC_ADV_GROUP,LS("OPEN_ADV_GROUP"));
        SetDlgItemText(hDlg,IDC_ADV_REF_LABEL,LS("OPEN_ADV_REF_LABEL"));
        SetDlgItemText(hDlg,IDOK,LS("OPEN_OK_BTN"));
        SetDlgItemText(hDlg,IDCANCEL,LS("OPEN_CANCEL_BTN"));
        if(g_openResult[0])SetDlgItemText(hDlg,IDC_URLEDIT,g_openResult);
        if(g_openReferer[0])SetDlgItemText(hDlg,IDC_REFEDIT,g_openReferer);
        EnableWindow(GetDlgItem(hDlg,IDOK),g_openResult[0]!=L'\0');return TRUE;
    case WM_COMMAND:
        switch(LOWORD(wp)){
        case IDC_URLEDIT:
            if(HIWORD(wp)==EN_CHANGE){
                wchar_t tmp[4];GetDlgItemText(hDlg,IDC_URLEDIT,tmp,4);
                EnableWindow(GetDlgItem(hDlg,IDOK),tmp[0]!=L'\0');}break;
        case IDC_BROWSE:{
            OPENFILENAME ofn={};wchar_t file[MAX_PATH]={};
            wchar_t filter[300]; {
                size_t pos=0;
                const wchar_t* parts[4]={LS("OPEN_FILEDLG_FILTER_NAME"),L"*.unity3d",
                                          LS("OPEN_FILEDLG_FILTER_ALL"),L"*.*"};
                for(int i=0;i<4;i++){
                    size_t l=wcslen(parts[i]);
                    if(pos+l+1>=sizeof(filter)/sizeof(wchar_t))break;
                    wcscpy(filter+pos,parts[i]);pos+=l;filter[pos++]=L'\0';
                }
                if(pos<sizeof(filter)/sizeof(wchar_t))filter[pos]=L'\0';
            }
            ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=hDlg;
            ofn.lpstrFilter=filter;
            ofn.lpstrFile=file;ofn.nMaxFile=MAX_PATH;
            ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;ofn.lpstrTitle=LS("OPEN_FILEDLG_TITLE");
            if(GetOpenFileName(&ofn)){
                SetDlgItemText(hDlg,IDC_URLEDIT,file);
                EnableWindow(GetDlgItem(hDlg,IDOK),TRUE);}break;}
        case IDOK:{
            wchar_t buf[MAX_PATH*2];GetDlgItemText(hDlg,IDC_URLEDIT,buf,MAX_PATH*2);
            if(buf[0]){
                wcsncpy(g_openResult,buf,(MAX_PATH*2)-1);g_openResult[(MAX_PATH*2)-1]=0;
                wchar_t ref[MAX_PATH*2];GetDlgItemText(hDlg,IDC_REFEDIT,ref,MAX_PATH*2);
                wcsncpy(g_openReferer,ref,(MAX_PATH*2)-1);g_openReferer[(MAX_PATH*2)-1]=0;
                EndDialog(hDlg,IDOK);}break;}
        case IDCANCEL:EndDialog(hDlg,IDCANCEL);break;}return TRUE;}return FALSE;
}
static bool ShowOpenDialog(){
    g_openResult[0]=L'\0';
    g_openReferer[0]=L'\0';
    return DialogBox(g_hInst,MAKEINTRESOURCE(IDD_OPEN),g_hwndMain,OpenDlgProc)==IDOK&&g_openResult[0];
}
INT_PTR CALLBACK AboutDlgProc(HWND hDlg,UINT msg,WPARAM wp,LPARAM){
    if(msg==WM_INITDIALOG){
        SetWindowText(hDlg,LS("ABOUT_TITLE"));
        wchar_t verLine[64];_snwprintf(verLine,63,L"%s %s",APP_NAME,APP_VERSION);verLine[63]=0;
        SetDlgItemText(hDlg,IDC_ABOUT_VERSION,verLine);
        SetDlgItemText(hDlg,IDC_ABOUT_TAGLINE,LS("ABOUT_TAGLINE"));
        SetDlgItemText(hDlg,IDOK,LS("ABOUT_OK_BTN"));
        return TRUE;
    }
    if(msg==WM_COMMAND&&(LOWORD(wp)==IDOK||LOWORD(wp)==IDCANCEL))EndDialog(hDlg,0);
    return FALSE;
}
INT_PTR CALLBACK DownloadDlgProc(HWND hDlg,UINT msg,WPARAM wp,LPARAM){
    switch(msg){
    case WM_INITDIALOG:{
        SetWindowText(hDlg,LS("DOWNLOAD_TITLE"));
        SetDlgItemText(hDlg,IDC_DL_NOTFOUND,LS("DOWNLOAD_NOTFOUND"));
        SetDlgItemText(hDlg,IDC_DL_EXPECTED_LBL,LS("DOWNLOAD_EXPECTED_LABEL"));
        SetDlgItemText(hDlg,IDC_DL_BODY,LS("DOWNLOAD_BODY"));
        wchar_t urlLine[300];_snwprintf(urlLine,299,L"%s %s",LS("DOWNLOAD_URL_LABEL"),RUNTIME_DL_URL);urlLine[299]=0;
        SetDlgItemText(hDlg,IDC_DL_URLLABEL,urlLine);
        SetDlgItemText(hDlg,IDC_DL_BROWSER,LS("DOWNLOAD_OPENPAGE_BTN"));
        SetDlgItemText(hDlg,IDOK,LS("DOWNLOAD_LATER_BTN"));
        SetDlgItemText(hDlg,IDCANCEL,LS("DOWNLOAD_CANCEL_BTN"));
        wchar_t p[MAX_PATH];_snwprintf(p,MAX_PATH-1,L"%s\\Runtime",g_exeDir);p[MAX_PATH-1]=0;
        SetDlgItemText(hDlg,IDC_DL_PATH,p);return TRUE;}
    case WM_COMMAND:switch(LOWORD(wp)){
        case IDC_DL_BROWSER:
            ShellExecute(hDlg,L"open",RUNTIME_DL_URL,nullptr,nullptr,SW_SHOWNORMAL);break;
        case IDOK:case IDCANCEL:EndDialog(hDlg,LOWORD(wp));break;}return TRUE;}return FALSE;
}
INT_PTR CALLBACK ToolsWarningDlgProc(HWND hDlg,UINT msg,WPARAM wp,LPARAM){
    if(msg==WM_INITDIALOG){
        SetWindowText(hDlg,LS("TOOLSWARN_TITLE"));
        SetDlgItemText(hDlg,IDC_TW_BODY,LS("TOOLSWARN_BODY"));
        SetDlgItemText(hDlg,IDC_TW_NOASKAGAIN,LS("TOOLSWARN_NOASKAGAIN"));
        SetDlgItemText(hDlg,IDOK,LS("TOOLSWARN_ENABLE_BTN"));
        SetDlgItemText(hDlg,IDCANCEL,LS("TOOLSWARN_CANCEL_BTN"));
        return TRUE;
    }
    if(msg==WM_COMMAND){
        WORD id=LOWORD(wp);
        if(id==IDOK||id==IDCANCEL){EndDialog(hDlg,id);return TRUE;}
    }
    return FALSE;
}

// Experimental Features dialog (v1.4). Currently holds the frame-rate
// override; future experimental toggles will be appended here.
INT_PTR CALLBACK ExperimentalDlgProc(HWND hDlg,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_INITDIALOG:{
        SetWindowText(hDlg,LS("EXP_TITLE"));
        SetDlgItemText(hDlg,IDC_EXP_FPS_GROUP,LS("EXP_FPS_GROUP"));
        SetDlgItemText(hDlg,IDC_EXP_FPS_LABEL,LS("EXP_FPS_LABEL"));
        SetDlgItemText(hDlg,IDC_EXP_FPS_ZERO, LS("EXP_FPS_ZERO"));
        SetDlgItemText(hDlg,IDC_EXP_FPS_HINT, LS("EXP_FPS_HINT"));
        SetDlgItemText(hDlg,IDC_EXP_FPS_APPLY,LS("EXP_FPS_APPLY_BTN"));
        SetDlgItemText(hDlg,IDOK,             LS("EXP_CLOSE_BTN"));
        wchar_t val[16];_snwprintf(val,15,L"%d",g_fpsTarget);val[15]=0;
        SetDlgItemText(hDlg,IDC_EXP_FPSEDIT,val);
        wchar_t st[128];
        if(g_fpsTarget>0)_snwprintf(st,127,LS("EXP_FPS_STATUS_ON"),g_fpsTarget);
        else wcscpy(st,LS("EXP_FPS_STATUS_OFF"));
        st[127]=0;
        SetDlgItemText(hDlg,IDC_EXP_FPS_STATUS,st);
        return TRUE;}
    case WM_COMMAND:{
        WORD id=LOWORD(wp);
        if(id==IDOK||id==IDCANCEL){EndDialog(hDlg,id);return TRUE;}
        if(id==IDC_EXP_FPS_APPLY&&HIWORD(wp)==BN_CLICKED){
            wchar_t val[32]={};
            GetDlgItemText(hDlg,IDC_EXP_FPSEDIT,val,31);
            int fps=_wtoi(val);
            if(fps<0||fps>1000){
                MessageBox(hDlg,LS("EXP_FPS_BAD_VALUE"),LS("EXP_TITLE"),MB_OK|MB_ICONWARNING);
                return TRUE;
            }
            g_fpsTarget=fps;
            SettingsSaveFpsTarget();
            wchar_t st[128];
            if(g_fpsTarget>0)_snwprintf(st,127,LS("EXP_FPS_STATUS_ON"),g_fpsTarget);
            else wcscpy(st,LS("EXP_FPS_STATUS_OFF"));
            st[127]=0;
            SetDlgItemText(hDlg,IDC_EXP_FPS_STATUS,st);
            MessageBox(hDlg,LS("EXP_FPS_SAVED"),LS("EXP_TITLE"),MB_OK|MB_ICONINFORMATION);
            return TRUE;
        }
        return FALSE;}
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
//  Main window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK MainWndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:
        DragAcceptFiles(hwnd,TRUE);PostMessage(hwnd,WM_POSTINIT,0,0);return 0;

    case WM_POSTINIT:{
        if(!IsRuntimePackagePresent()){
            SetStatus(LS("STATUS_RUNTIME_MISSING_PROMPT"));
            ShowWindow(hwnd,SW_SHOW);UpdateWindow(hwnd);
            DialogBox(g_hInst,MAKEINTRESOURCE(IDD_DOWNLOAD),hwnd,DownloadDlgProc);
            if(!IsRuntimePackagePresent()){
                SetStatus(LS("STATUS_RUNTIME_MISSING"));return 0;}
        }
        if(!IsWebPlayerInstalled()){
            SetStatus(LS("STATUS_INITIALIZING"));
            InvalidateRect(hwnd,nullptr,TRUE);UpdateWindow(hwnd);
            if(!SilentInstallWebPlayer()){
                SetStatus(LS("STATUS_INIT_FAILED"));return 0;}
        }
        SetStatus(LS("STATUS_IDLE"));
        RebuildFileMenu();
        RebuildToolsMenu();
        if(g_pendingFile[0]){
            wchar_t tmp[MAX_PATH*2];wcsncpy(tmp,g_pendingFile,(MAX_PATH*2)-1);tmp[(MAX_PATH*2)-1]=0;
            wchar_t ref[MAX_PATH*2];wcsncpy(ref,g_pendingReferer,(MAX_PATH*2)-1);ref[(MAX_PATH*2)-1]=0;
            g_pendingFile[0]=L'\0';g_pendingReferer[0]=L'\0';
            LoadFileOrUrl(tmp,ref);}
        return 0;}

    case WM_LOADFILE:{
        wchar_t*p=reinterpret_cast<wchar_t*>(lp);if(p){LoadFileOrUrl(p);delete[]p;}return 0;}

    case WM_COPYDATA:{
        // Second instance forwarded its command line so we load it here.
        COPYDATASTRUCT*cds=reinterpret_cast<COPYDATASTRUCT*>(lp);
        if(cds&&cds->dwData==0x55465031&&cds->cbData>=sizeof(wchar_t)&&cds->lpData){
            const wchar_t*arg=reinterpret_cast<const wchar_t*>(cds->lpData);
            wchar_t game[MAX_PATH*2]={},ref[MAX_PATH*2]={};
            ParseCmdArg(arg,game,_countof(game),ref,_countof(ref));
            if(game[0])LoadFileOrUrl(game,ref[0]?ref:nullptr);
        }
        return 0;}

    case WM_INITMENUPOPUP:{
        HMENU hFile=GetSubMenu(GetMenu(hwnd),0);
        HMENU hCtrl=GetSubMenu(GetMenu(hwnd),2);
        HMENU hLang=hCtrl?GetSubMenu(hCtrl,1):nullptr;
        if((HMENU)wp==hFile){
            RebuildFileMenu();
            UINT ena=g_gameLoaded?MF_ENABLED:MF_GRAYED;
            EnableMenuItem(hFile,IDM_FILE_RELOAD,MF_BYCOMMAND|ena);
            EnableMenuItem(hFile,IDM_FILE_CLOSE, MF_BYCOMMAND|ena);
        } else if((HMENU)wp==hLang){
            RebuildLanguageMenu();
        }
        return 0;}

    case WM_COMMAND:{
        WORD id=LOWORD(wp);
        if(id>=IDM_RECENT_0&&id<IDM_RECENT_0+MRU_MAX){
            int idx=id-IDM_RECENT_0;if(idx<g_mruCount)
                LoadFileOrUrl(g_mruList[idx],g_mruReferer[idx]);return 0;}
        if(id>=IDM_TOOLS_ITEM_0&&id<IDM_TOOLS_ITEM_0+TOOLS_MAX){
            int idx=id-IDM_TOOLS_ITEM_0;if(idx<g_toolsCount)LaunchTool(g_toolsList[idx]);return 0;}
        if(id==IDM_LANG_BUILTIN_EN){ApplyLanguage(hwnd,L"");return 0;}
        if(id>=IDM_LANG_ITEM_0&&id<IDM_LANG_ITEM_0+LANG_MAX){
            int idx=id-IDM_LANG_ITEM_0;if(idx<g_langFileCount)ApplyLanguage(hwnd,g_langFiles[idx].code);return 0;}
        switch(id){
        case IDM_FILE_OPEN:   if(ShowOpenDialog())LoadFileOrUrl(g_openResult,g_openReferer);break;
        case IDM_FILE_RELOAD: ReloadGame();  break;
        case IDM_FILE_CLOSE:  CloseGame();   break;
        case IDM_FILE_EXIT:   DestroyWindow(hwnd);break;
        case IDM_VIEW_FULLSCREEN:ToggleFullscreen();break;
        case IDM_CTRL_EXPERIMENTAL:
            DialogBox(g_hInst,MAKEINTRESOURCE(IDD_EXPERIMENTAL),hwnd,ExperimentalDlgProc);break;
        case IDM_TOOLS_ENABLE:
            if(DialogBox(g_hInst,MAKEINTRESOURCE(IDD_TOOLS_WARNING),hwnd,ToolsWarningDlgProc)==IDOK)
                EnableTools();
            break;
        case IDM_TOOLS_REFRESH:
            ScanToolsFolder();RebuildToolsMenu();break;
        case IDM_HELP_REPO:
            ShellExecute(hwnd,L"open",GITHUB_URL,nullptr,nullptr,SW_SHOWNORMAL);break;
        case IDM_HELP_CLEARDATA: {
            int r=MessageBox(hwnd,LS("MSG_CLEARDATA_CONFIRM"),LS("MSG_CLEARDATA_TITLE"),
                             MB_YESNO|MB_ICONQUESTION);
            if(r==IDYES){
                ClearUserData();
                MessageBox(hwnd,LS("MSG_CLEARDATA_DONE"),LS("MSG_CLEARDATA_TITLE"),MB_OK|MB_ICONINFORMATION);
            }
            break;
        }
        case IDM_HELP_ABOUT:
            DialogBox(g_hInst,MAKEINTRESOURCE(IDD_ABOUT),hwnd,AboutDlgProc);break;}
        return 0;}

    case WM_DROPFILES:{
        HDROP hd=(HDROP)wp;
        if(DragQueryFile(hd,0xFFFFFFFF,nullptr,0)>0){
            wchar_t*buf=new wchar_t[MAX_PATH];
            DragQueryFile(hd,0,buf,MAX_PATH);
            PostMessage(hwnd,WM_LOADFILE,0,(LPARAM)buf);}
        DragFinish(hd);return 0;}

    case WM_SIZE:if(g_unityReady)UnityResize(LOWORD(lp),HIWORD(lp));return 0;

    case WM_PAINT:{
        PAINTSTRUCT ps;HDC hdc=BeginPaint(hwnd,&ps);
        if(!g_gameLoaded){RECT rc;GetClientRect(hwnd,&rc);PaintStatus(hdc,rc);}
        EndPaint(hwnd,&ps);return 0;}

    case WM_ERASEBKGND:if(g_gameLoaded)return 1;return 0;

    case WM_KEYDOWN:if(wp==VK_F11){ToggleFullscreen();return 0;}break;

    case WM_TIMER:
        if(wp==TIMER_ID_INSTALL_HOOK){
            g_hookRetryCnt++;
            bool ok=InstallRuntimeHooks();
            if(ok||g_rtHookTried||g_hookRetryCnt>=HOOK_MAX_RETRIES){
                KillTimer(hwnd,TIMER_ID_INSTALL_HOOK);
            }
        }
        return 0;

    case WM_DESTROY:
        if(g_savedMenu){DestroyMenu(g_savedMenu);g_savedMenu=nullptr;}
        UnityDestroy();
        PostQuitMessage(0);return 0;}
    return DefWindowProc(hwnd,msg,wp,lp);
}

// ---------------------------------------------------------------------------
//  unitywp:// protocol self-registration (HKCU, no admin needed)
//  URL form:  unitywp://<gameURL>[|<referer>]
// ---------------------------------------------------------------------------
static void RegisterUnityWpProtocol(){
    wchar_t exe[MAX_PATH*2];
    GetModuleFileName(nullptr,exe,MAX_PATH*2);

    const wchar_t* base=L"Software\\Classes\\unitywp";
    HKEY hk;
    if(RegCreateKeyEx(HKEY_CURRENT_USER,base,0,nullptr,0,KEY_WRITE,nullptr,&hk,nullptr)!=ERROR_SUCCESS)return;
    const wchar_t* proto=L"URL:UFunPlayer Protocol";
    RegSetValueEx(hk,nullptr,0,REG_SZ,(const BYTE*)proto,(DWORD)(wcslen(proto)+1)*2);
    const wchar_t emptyStr[]=L"";
    RegSetValueEx(hk,L"URL Protocol",0,REG_SZ,(const BYTE*)emptyStr,sizeof(wchar_t));
    RegCloseKey(hk);

    wchar_t sub[64];
    wcscpy(sub,base);wcscat(sub,L"\\DefaultIcon");
    if(RegCreateKeyEx(HKEY_CURRENT_USER,sub,0,nullptr,0,KEY_WRITE,nullptr,&hk,nullptr)==ERROR_SUCCESS){
        RegSetValueEx(hk,nullptr,0,REG_SZ,(const BYTE*)exe,(DWORD)(wcslen(exe)+1)*2);
        RegCloseKey(hk);
    }
    wcscpy(sub,base);wcscat(sub,L"\\shell\\open\\command");
    if(RegCreateKeyEx(HKEY_CURRENT_USER,sub,0,nullptr,0,KEY_WRITE,nullptr,&hk,nullptr)==ERROR_SUCCESS){
        wchar_t cmd[MAX_PATH*2+16];
        _snwprintf(cmd,(sizeof(cmd)/sizeof(wchar_t))-1,L"\"%s\" \"%%1\"",exe);
        cmd[(sizeof(cmd)/sizeof(wchar_t))-1]=0;
        RegSetValueEx(hk,nullptr,0,REG_SZ,(const BYTE*)cmd,(DWORD)(wcslen(cmd)+1)*2);
        RegCloseKey(hk);
    }
}

// URL-decode (%XX -> char) in place, including multi-byte UTF-8 sequences
// (browsers percent-encode the protocol '|' separator and CJK paths).
static void UrlDecodeInPlace(wchar_t* s){
    if(!s)return;
    // Phase 1: build a UTF-8 byte buffer (%XX, ASCII, or encoded wchar_t).
    int maxOut=(int)wcslen(s);  // decoded output is never longer than input
    char buf[2048];
    int blen=0;
    for(const wchar_t* r=s; *r && blen<(int)sizeof(buf)-4; ){
        if(*r==L'%' && r[1] && r[2]){
            wchar_t hex[3]={r[1],r[2],0};
            wchar_t* end=nullptr;
            long v=wcstol(hex,&end,16);
            if(end==hex+2){ buf[blen++]=(char)(unsigned char)v; r+=3; continue; }
        }
        if(*r<0x80){
            buf[blen++]=(char)*r;
        }else{
            blen+=WideCharToMultiByte(CP_UTF8,0,r,1,buf+blen,4,nullptr,nullptr);
        }
        r++;
    }
    // Phase 2: UTF-8 bytes -> wchar_t in place.
    int wlen=MultiByteToWideChar(CP_UTF8,0,buf,blen,s,maxOut);
    s[wlen]=L'\0';
}

// Parse a command-line argument: unitywp://URL[|referer], plain URL, or path.
// The '|' separator may arrive percent-encoded as %7C.
static void ParseCmdArg(const wchar_t* arg,wchar_t* outGame,size_t gameCap,wchar_t* outRef,size_t refCap){
    outGame[0]=L'\0';if(outRef)outRef[0]=L'\0';
    if(!arg||!arg[0])return;

    const wchar_t* wp=nullptr;
    if(_wcsnicmp(arg,L"unitywp://",10)==0)wp=arg+10;
    else if(_wcsnicmp(arg,L"unitywp:",8)==0)wp=arg+8;   // tolerate missing slashes
    else wp=arg;   // plain URL / path

    // Find the separator: literal '|' or encoded "%7C".
    const wchar_t* bar=wcsstr(wp,L"|");
    if(!bar) bar=wcsstr(wp,L"%7C");
    if(bar){
        size_t glen=(size_t)(bar-wp);if(glen>=gameCap)glen=gameCap-1;
        wcsncpy(outGame,wp,glen);outGame[glen]=L'\0';
        if(outRef){
            const wchar_t* after = (bar[0]==L'|') ? bar+1 : bar+3;
            wcsncpy(outRef,after,refCap-1);outRef[refCap-1]=L'\0';
            UrlDecodeInPlace(outRef);   // referer likely has %2F, %3A, ...
        }
    }else{
        wcsncpy(outGame,wp,gameCap-1);outGame[gameCap-1]=L'\0';
    }
    // Browsers percent-encode local paths (e.g. backslash -> %5C, non-ASCII -> %XX UTF-8 bytes).
    // Decode so ReadBundleFromFile can open the file. For URLs this is a no-op
    // (scheme/host have no %XX).
    UrlDecodeInPlace(outGame);
}

int WINAPI wWinMain(HINSTANCE hInst,HINSTANCE,LPWSTR,int nShow){
    g_hInst=hInst;
    GetModuleFileName(nullptr,g_exeDir,MAX_PATH);PathRemoveFileSpec(g_exeDir);

    RegisterUnityWpProtocol();

    if(__argc>=2&&__wargv[1][0]){
        ParseCmdArg(__wargv[1],g_pendingFile,sizeof(g_pendingFile)/sizeof(wchar_t),
                    g_pendingReferer,sizeof(g_pendingReferer)/sizeof(wchar_t));
    }
    MruLoad();
    SettingsLoad();
    if (g_toolsEnabled) ScanToolsFolder();

    ScanLangsFolder();
    {
        wchar_t savedLangCode[64] = {};
        HKEY hk = nullptr;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            DWORD sz = sizeof(savedLangCode), type = 0;
            if (RegQueryValueEx(hk, L"Language", nullptr, &type, (BYTE*)savedLangCode, &sz) != ERROR_SUCCESS
                    || type != REG_SZ)
                savedLangCode[0] = L'\0';
            RegCloseKey(hk);
        }
        ApplyLanguage(nullptr, savedLangCode);
        SetStatus(LS("STATUS_IDLE"));
    }

    // Single-instance: forward the command line to the existing window instead
    // of launching a second runtime.
    g_hSingleInstance = CreateMutexW(nullptr, TRUE, L"Global\\UFunPlayerSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"UFunPlayerWnd", nullptr);
        if (existing) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
            // Forward command line to existing instance via WM_COPYDATA.
            if (__argc >= 2 && __wargv[1][0]) {
                size_t len = wcslen(__wargv[1]) + 1;
                COPYDATASTRUCT cds = {};
                cds.dwData = 0x55465031;  // "UFP1"
                cds.cbData = (DWORD)(len * sizeof(wchar_t));
                cds.lpData = (PVOID)__wargv[1];
                SendMessageW(existing, WM_COPYDATA, (WPARAM)nullptr, (LPARAM)&cds);
            }
        }
        if (g_hSingleInstance) { CloseHandle(g_hSingleInstance); g_hSingleInstance = nullptr; }
        return 0;
    }

    OleInitialize(nullptr);

    WNDCLASSEX wc={};wc.cbSize=sizeof(wc);wc.style=CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc=MainWndProc;wc.hInstance=hInst;
    wc.hIcon=LoadIcon(hInst,MAKEINTRESOURCE(IDI_MAINICON));
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    wc.lpszMenuName=MAKEINTRESOURCE(IDR_MAINMENU);
    wc.lpszClassName=L"UFunPlayerWnd";
    wc.hIconSm=LoadIcon(hInst,MAKEINTRESOURCE(IDI_SMALLICON));
    RegisterClassEx(&wc);
    g_hAccel=LoadAccelerators(hInst,MAKEINTRESOURCE(IDR_ACCEL));

    g_hwndMain=CreateWindowEx(WS_EX_ACCEPTFILES,L"UFunPlayerWnd",
        APP_NAME L" " APP_VERSION,WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,860,660,nullptr,nullptr,hInst,nullptr);
    if(!g_hwndMain){OleUninitialize();return 1;}
    ApplyMenuLanguage();
    ShowWindow(g_hwndMain,nShow);UpdateWindow(g_hwndMain);

    MSG msg;
    while(GetMessage(&msg,nullptr,0,0)){
        if(!TranslateAccelerator(g_hwndMain,g_hAccel,&msg)){
            TranslateMessage(&msg);DispatchMessage(&msg);}
    }
    OleUninitialize();
    // Tear down any remaining MinHook state (no-op if never initialised).
    MH_Uninitialize();
    if (g_hSingleInstance) { CloseHandle(g_hSingleInstance); g_hSingleInstance = nullptr; }
    return (int)msg.wParam;
}