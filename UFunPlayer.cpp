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
#include <string.h>
#include <wchar.h>
#include <stdlib.h>

#include "resource.h"

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------
#define APP_NAME       L"UFunPlayer"
#define APP_VERSION    L"1.3"
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
static wchar_t g_currentCacheFile[MAX_PATH]    = {};   // our cache copy of the loaded .unity3d (empty if local/no-referer)
static HANDLE  g_hSingleInstance = nullptr;            // mutex preventing multiple launches

// MRU list
static wchar_t g_mruList[MRU_MAX][MAX_PATH * 2];
static wchar_t g_mruReferer[MRU_MAX][MAX_PATH * 2];
static wchar_t g_mruRealSrc[MRU_MAX][MAX_PATH * 2]; // 实际传给Unity的路径(带referer时为缓存路径)
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
static void UnityDestroy();
static bool UnityCreate(HWND,const wchar_t*);
static void UnityResize(int,int);
static void LoadFileOrUrl(const wchar_t*,const wchar_t*refererArg=nullptr);
static void ParseCmdArg(const wchar_t* arg,wchar_t* outGame,size_t gameCap,wchar_t* outRef,size_t refCap);
static void ReloadGame();
static void CloseGame();
static const wchar_t* GetCachePath(const wchar_t* url);
static bool DownloadToCustomCache(const wchar_t* url,const wchar_t* referer,wchar_t* outPath,size_t outCap);
static void PurgeCacheDir();
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
//  Unity Web Player ActiveX downloads its src URL through the URL Moniker
//  binding layer (URLDownloadToCacheFile / IMoniker::BindToStorage), which
//  queries the container's IServiceProvider for SID_SBindHost -> IBindHost.
//  We implement IBindHost by wrapping every bind context with our own
//  IBindStatusCallback + IHttpNegotiate, whose BeginningTransaction() injects
//  the Referer header. This mirrors what Trident does for <object> in a real
//  browser: the host supplies the referer, not the control.
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

// UnityBindCallback is still used by our own downloads (URLOpenBlockingStreamW
// and the custom cache downloader) to inject the Referer header. The control
// itself no longer needs IBindHost: when a referer is set we feed the control a
// local file path, so it never issues a network request of its own.

class UnityClientSite : public IOleClientSite, public IOleInPlaceSite
{
public:
    LONG           m_refs;
    HWND           m_hwnd;
    UnityFrameSite m_frame;

    explicit UnityClientSite(HWND hwnd)
        : m_refs(1), m_hwnd(hwnd), m_frame(this) {}

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
    STDMETHODIMP GetMoniker(DWORD, DWORD, IMoniker**) override { return E_NOTIMPL; }
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
    HKEY hk = nullptr;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, REG_SETTINGS_KEY, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD val = 0, sz = sizeof(val), type = 0;
        if (RegQueryValueEx(hk, L"ToolsEnabled", nullptr, &type, (BYTE*)&val, &sz) == ERROR_SUCCESS
                && type == REG_DWORD)
            g_toolsEnabled = (val != 0);
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
        L"(Tools will need to be re-enabled).\n\nContinue?");
    SetString("MSG_CLEARDATA_DONE",    L"User data has been cleared.");
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
static void UnityDestroy(){
    g_unityReady=false;g_gameLoaded=false;
    if(g_pIPO){g_pIPO->UIDeactivate();g_pIPO->InPlaceDeactivate();g_pIPO->Release();g_pIPO=nullptr;}
    if(g_pDisp){g_pDisp->Release();g_pDisp=nullptr;}
    if(g_pOleObj){g_pOleObj->Close(OLECLOSE_NOSAVE);g_pOleObj->Release();g_pOleObj=nullptr;}
    if(g_pSite){g_pSite->Release();g_pSite=nullptr;}
    CoFreeUnusedLibrariesEx(0,0);
    if(g_hwndMain)InvalidateRect(g_hwndMain,nullptr,TRUE);
}
static bool UnityCreate(HWND hwnd,const wchar_t*srcUrl){
    g_pSite=new UnityClientSite(hwnd);
    HRESULT hr=CoCreateInstance(CLSID_UnityWebPlayer,nullptr,CLSCTX_INPROC_SERVER,
                                IID_IOleObject,(void**)&g_pOleObj);
    if(FAILED(hr)){g_pSite->Release();g_pSite=nullptr;return false;}
    g_pOleObj->SetClientSite(g_pSite);OleSetContainedObject(g_pOleObj,TRUE);
    g_pOleObj->QueryInterface(IID_IDispatch,(void**)&g_pDisp);

    UnitySetPropW(L"src",srcUrl);
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
//  Custom cache for referer-based loads
//
//  When a referer is set we cannot hand the URL straight to the control: some
//  Unity ActiveX builds fetch src via raw wininet (ignoring any host-supplied
//  bind hook) and get 403 -> "Invalid data file". So we download the .unity3d
//  ourselves (with the referer injected) to a per-URL file under
//  %APPDATA%\UFunPlayer\cache\<hash>.unity3d and load the control from that
//  local path. The cache is wiped on close/exit so it never wastes disk.
// ---------------------------------------------------------------------------

// FNV-1a 64-bit over the URL bytes -> stable 16-hex-digit file name.
// 64-bit is overkill for a per-user cache but makes collisions a non-issue:
// even with 1 billion distinct URLs the collision odds are ~3e-12.
static unsigned long long HashUrl64(const wchar_t* url){
    if(!url)return 0;
    unsigned long long h=14695981039346656037ULL;
    for(const wchar_t* p=url;*p;p++){
        unsigned int c=(unsigned int)*p;
        h^=c; h*=1099511628211ULL;
        c>>=8;
        if(c){ h^=c; h*=1099511628211ULL; }
    }
    return h;
}

// Returns a static buffer with the cache file path for the given URL and
// makes sure the cache directory exists.
static const wchar_t* GetCachePath(const wchar_t* url){
    static wchar_t cachePath[MAX_PATH];
    wchar_t appData[MAX_PATH]={};
    if(FAILED(SHGetFolderPathW(nullptr,CSIDL_APPDATA,nullptr,0,appData)) || !appData[0]){
        wchar_t* env=_wgetenv(L"APPDATA");
        if(env)wcsncpy(appData,env,MAX_PATH-1);
    }
    _snwprintf(cachePath,MAX_PATH-1,L"%s\\UFunPlayer\\cache\\%016llX.unity3d",
               appData,HashUrl64(url));
    cachePath[MAX_PATH-1]=L'\0';

    // Ensure %APPDATA%\UFunPlayer\cache exists.
    wchar_t dir[MAX_PATH];
    wcsncpy(dir,cachePath,MAX_PATH-1);dir[MAX_PATH-1]=L'\0';
    PathRemoveFileSpecW(dir);              // strip "<hash>.unity3d"
    SHCreateDirectoryExW(nullptr,dir,nullptr);
    return cachePath;
}

// Downloads `url` (with referer injected) into our cache file for that URL.
// On success copies the cache path into outPath and returns true. The cache
// dir is created by GetCachePath; any pre-existing file is overwritten by
// URLDownloadToFileW so reloads always fetch fresh bytes.
static bool DownloadToCustomCache(const wchar_t* url,const wchar_t* referer,
                                  wchar_t* outPath,size_t outCap){
    if(!url || !url[0])return false;
    const wchar_t* cachePath=GetCachePath(url);

    UnityBindCallback* cb=new UnityBindCallback(referer?referer:L"",nullptr);
    HRESULT hr=URLDownloadToFileW(nullptr,url,cachePath,0,cb);
    cb->Release();
    if(FAILED(hr) || !PathFileExistsW(cachePath))return false;

    if(outPath && outCap){
        wcsncpy(outPath,cachePath,outCap-1);outPath[outCap-1]=L'\0';
    }
    return true;
}

// Wipe the whole cache directory (called on exit).
static void PurgeCacheDir(){
    wchar_t appData[MAX_PATH]={};
    if(FAILED(SHGetFolderPathW(nullptr,CSIDL_APPDATA,nullptr,0,appData)) || !appData[0]){
        wchar_t* env=_wgetenv(L"APPDATA");
        if(env)wcsncpy(appData,env,MAX_PATH-1);
    }
    wchar_t dir[MAX_PATH];
    _snwprintf(dir,MAX_PATH-1,L"%s\\UFunPlayer\\cache",appData);
    dir[MAX_PATH-1]=L'\0';
    if(!PathFileExistsW(dir))return;
    DeleteFolderContents(dir);
    RemoveDirectoryW(dir);
}

// ---------------------------------------------------------------------------
//  Game loading – the central function
// ---------------------------------------------------------------------------
static void LoadFileOrUrl(const wchar_t*pathArg,const wchar_t*refererArg)
{
    wchar_t path[MAX_PATH*2];
    wcsncpy(path,pathArg,(MAX_PATH*2)-1);path[(MAX_PATH*2)-1]=L'\0';

    bool isUrl=(PathIsURL(path)==TRUE);

    // Set referer before probing/loading so both paths see it.
    g_currentReferer[0]=L'\0';
    if(refererArg && refererArg[0]){
        wcsncpy(g_currentReferer,refererArg,(sizeof(g_currentReferer)/sizeof(wchar_t))-1);
        g_currentReferer[(sizeof(g_currentReferer)/sizeof(wchar_t))-1]=L'\0';
    }

    // Drop any cache file left by the previous game; ReloadGame re-downloads.
    if(g_currentCacheFile[0]){
        DeleteFileW(g_currentCacheFile);
        g_currentCacheFile[0]=L'\0';
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

    // When a referer is set, download the .unity3d into our own cache file
    // (referer injected) and load the control from that local path. This
    // sidesteps the control's own network fetch entirely, so 403s from CDNs
    // that check the referer can't happen. Save-path impact is acceptable:
    // these games couldn't load at all without a referer before, so there's
    // no prior save data to lose.
    const wchar_t* loadSrc=path;
    if(isUrl && g_currentReferer[0]){
        wchar_t cacheFile[MAX_PATH]={};
        if(DownloadToCustomCache(path,g_currentReferer,cacheFile,MAX_PATH)){
            loadSrc=cacheFile;
            wcsncpy(g_currentCacheFile,cacheFile,(sizeof(g_currentCacheFile)/sizeof(wchar_t))-1);
            g_currentCacheFile[(sizeof(g_currentCacheFile)/sizeof(wchar_t))-1]=L'\0';
        }
    }

    if(!UnityCreate(g_hwndMain,loadSrc)){
        SetStatus(LS("STATUS_CREATE_FAILED"));
        g_currentPath[0]=L'\0';
        if(g_currentCacheFile[0]){DeleteFileW(g_currentCacheFile);g_currentCacheFile[0]=L'\0';}
        return;
    }
    g_gameLoaded=true;
    MruAdd(path,g_currentReferer,loadSrc);RebuildFileMenu();
}

static void ReloadGame(){
    if(!g_currentPath[0])return;
    // LoadFileOrUrl deletes the old cache file and re-downloads.
    LoadFileOrUrl(g_currentPath,g_currentReferer);
}
static void CloseGame(){
    if(g_currentCacheFile[0]){
        DeleteFileW(g_currentCacheFile);
        g_currentCacheFile[0]=L'\0';
    }
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

    case WM_DESTROY:
        if(g_savedMenu){DestroyMenu(g_savedMenu);g_savedMenu=nullptr;}
        UnityDestroy();
        PurgeCacheDir();   // wipe the whole cache dir on exit
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

// URL-decode (%XX -> char) in place. Browsers percent-encode '|' as %7C when
// launching a custom protocol, so the referer separator arrives encoded.
static void UrlDecodeInPlace(wchar_t* s){
    if(!s)return;
    wchar_t* r=s;   // read
    wchar_t* w=s;   // write
    while(*r){
        if(*r==L'%' && r[1] && r[2]){
            wchar_t hex[3]={r[1],r[2],0};
            wchar_t* end=nullptr;
            long v=wcstol(hex,&end,16);
            if(end==hex+2){ *w++=(wchar_t)v; r+=3; continue; }
        }
        *w++=*r++;
    }
    *w=L'\0';
}

// Parse a command-line argument into game URL/path + optional referer.
// Supports:  unitywp://URL            -> URL (referer empty)
//            unitywp://URL|referer    -> URL + referer
//            plain URL or local path  -> as-is (referer empty)
// The '|' separator may arrive percent-encoded as %7C (browsers do this).
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

    // Single-instance lock: different instances loading different Unity
    // versions would clobber the shared runtime switch. Instead of nagging,
    // silently bring the existing window to the foreground and forward the
    // command line so the existing instance loads the requested game.
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
    if (g_hSingleInstance) { CloseHandle(g_hSingleInstance); g_hSingleInstance = nullptr; }
    return (int)msg.wParam;
}