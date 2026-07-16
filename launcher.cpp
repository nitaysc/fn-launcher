// Launcher v2.0 — Smooth Professional UI
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <TlHelp32.h>
#include <ShellAPI.h>
#include <ShlObj.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <cmath>
#include <stdio.h>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

#define GITHUB_USER    "nitaysc"
#define GITHUB_REPO    "fn-launcher"
#define CHEAT_FOLDER   "fn-cheat"
#define LAUNCHER_EXE   "FortniteESP.exe"
#define VERSION_FILE   "version.txt"
#define DRIVER_SERVICE "xHunters"
#define DRIVER_SYS     "ACvalun.sys"
#define APP_TITLE      "FN Cheat"

// Colors
static const COLORREF CLR_BG      = RGB(12, 12, 16);
static const COLORREF CLR_CARD    = RGB(20, 20, 28);
static const COLORREF CLR_BORDER  = RGB(35, 35, 48);
static const COLORREF CLR_ACCENT  = RGB(0, 110, 220);
static const COLORREF CLR_ACCENT2 = RGB(0, 150, 245);
static const COLORREF CLR_ACCENT_H= RGB(30, 140, 250);
static const COLORREF CLR_TEXT    = RGB(220, 220, 228);
static const COLORREF CLR_SUBTEXT = RGB(140, 140, 155);
static const COLORREF CLR_GREEN   = RGB(40, 200, 80);
static const COLORREF CLR_GREEN_D = RGB(20, 120, 50);
static const COLORREF CLR_BTN_BG  = RGB(30, 30, 40);
static const COLORREF CLR_BTN_HOV = RGB(45, 45, 60);
static const COLORREF CLR_TITLE_BG= RGB(8, 8, 12);

// Layout
static const int WIN_W = 480, WIN_H = 340;
static const int TITLE_H = 32;
static const int CARD_T = 100, CARD_B = WIN_H - 42;
static const int CARD_L = 20, CARD_R = WIN_W - 20;

enum { B_CLOSE = 1, B_MIN, B_LAUNCH, B_UPDATE, B_GITHUB };

struct Btn { RECT rc; int id; };
static HWND g_hWnd;
static HFONT g_fntBig, g_fntMed, g_fntSmall;
static std::vector<Btn> g_btns;
static int g_hotBtn = 0, g_pushBtn = 0;
static bool g_running = true, g_ready = false, g_updating = false, g_dragging = false;
static int g_progress = 0;
static char g_status[128] = "Initializing...";
static char g_localVer[32] = "0", g_latestVer[32] = "0", g_downloadUrl[512] = "";
static bool g_updateAvail = false, g_launchEnabled = false;

// Animation state
static int g_animTick = 0;
static float g_dispProgress = 0.0f;
struct Ripple { bool active; float x, y, r, maxR; } g_ripple = {};
static RECT g_rcDot, g_rcProg, g_rcLaunch;

// GDI+ for GitHub icon
static GdiplusStartupInput g_gdiInp;
static ULONG_PTR g_gdiTok;
static Image* g_ghImg = NULL;
static int g_ghLoadTick = -100; // frame to retry load

// Cheat process state
static bool g_cheatLaunched = false;
static bool g_cheatRunning = false;

// ---------- helpers ----------
static bool IsAdmin()
{
    BOOL e = FALSE; HANDLE h; DWORD sz;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &h)) {
        TOKEN_ELEVATION t{}; sz = sizeof(t);
        if (GetTokenInformation(h, TokenElevation, &t, sz, &sz)) e = t.TokenIsElevated;
        CloseHandle(h);
    } return e;
}
static bool RunCmd(const char* c, bool wait = true)
{
    STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(NULL, (LPSTR)c, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return false;
    if (wait) WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return true;
}
static bool FileExists(const char* p) { DWORD a = GetFileAttributesA(p); return a != -1 && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static std::string GetLocalPath(const char* sub = "")
{
    char p[MAX_PATH] = {}; SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, p);
    strcat_s(p, "\\" CHEAT_FOLDER); if (sub && sub[0]) { strcat_s(p, "\\"); strcat_s(p, sub); } return p;
}
static std::string HttpGet(const char* url)
{
    std::string r; HINTERNET s = WinHttpOpen(L"Launcher/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!s) return r;
    HINTERNET c = WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (c) {
        HINTERNET q = WinHttpOpenRequest(c, L"GET", L"/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest", NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
        if (q) { WinHttpSendRequest(q, NULL, 0, NULL, 0, 0, 0); WinHttpReceiveResponse(q, NULL);
            char b[1024]; DWORD rd; while (WinHttpReadData(q, b, sizeof(b)-1, &rd) && rd > 0) { b[rd]=0; r+=b; } WinHttpCloseHandle(q); }
        WinHttpCloseHandle(c);
    } WinHttpCloseHandle(s); return r;
}
static std::string ExtractJson(const std::string& j, const char* f)
{
    char s[128]; sprintf_s(s, "\"%s\":\"", f); size_t p = j.find(s);
    if (p == -1) return ""; p += strlen(s);
    size_t e = j.find("\"", p); return e == -1 ? "" : j.substr(p, e-p);
}
static bool UnzipTo(const char* zip, const char* dst)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); bool ok = false;
    wchar_t wz[MAX_PATH], wd[MAX_PATH];
    MultiByteToWideChar(CP_UTF8,0,zip,-1,wz,MAX_PATH); MultiByteToWideChar(CP_UTF8,0,dst,-1,wd,MAX_PATH);
    SHCreateDirectoryExW(NULL, wd, NULL);
    IShellDispatch* sh = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_Shell,NULL,CLSCTX_INPROC_SERVER,IID_IShellDispatch,(void**)&sh)) && sh) {
        VARIANT vD; VariantInit(&vD); vD.vt=VT_BSTR; vD.bstrVal=SysAllocString(wz); Folder* f = NULL;
        if (SUCCEEDED(sh->NameSpace(vD,&f)) && f) { FolderItems* its = NULL; f->Items(&its);
            if (its) { VARIANT vDt; VariantInit(&vDt); vDt.vt=VT_BSTR; vDt.bstrVal=SysAllocString(wd); Folder* fd = NULL;
                sh->NameSpace(vDt,&fd); if (fd) { VARIANT vs,vo; VariantInit(&vs); vs.vt=VT_DISPATCH; vs.pdispVal=its;
                    VariantInit(&vo); vo.vt=VT_I4; vo.lVal=0x14; fd->CopyHere(vs,vo); ok=true; fd->Release(); }
                VariantClear(&vDt); its->Release(); } f->Release(); }
        VariantClear(&vD); sh->Release();
    } CoUninitialize(); return ok;
}
static bool EnsureDriver(const char* svc, const char* sys, const char* disp)
{
    std::string sp = GetLocalPath(sys); if (!FileExists(sp.c_str())) return false;
    char b[1024]; sprintf_s(b, "sc query %s", svc);
    if (RunCmd(b)) { sprintf_s(b, "sc start %s", svc); RunCmd(b, false); return true; }
    sprintf_s(b, "sc create %s binPath= \"%s\" type= kernel start= auto DisplayName= \"%s\"", svc, sp.c_str(), disp ? disp : svc);
    if (!RunCmd(b)) return false; sprintf_s(b, "sc start %s", svc); RunCmd(b, false); return true;
}

// ---------- drawing ----------
static COLORREF LerpC(COLORREF a, COLORREF b, float t)
{
    if (t <= 0) return a; if (t >= 1) return b;
    return RGB((int)(GetRValue(a)+(GetRValue(b)-GetRValue(a))*t),
               (int)(GetGValue(a)+(GetGValue(b)-GetGValue(a))*t),
               (int)(GetBValue(a)+(GetBValue(b)-GetBValue(a))*t));
}
static void DrawGradH(HDC hdc, int x, int y, int w, int h, COLORREF a, COLORREF b)
{
    if (w <= 0 || h <= 0) return;
    COLOR16 r1=(COLOR16)(GetRValue(a)<<8), g1=(COLOR16)(GetGValue(a)<<8), b1=(COLOR16)(GetBValue(a)<<8);
    COLOR16 r2=(COLOR16)(GetRValue(b)<<8), g2=(COLOR16)(GetGValue(b)<<8), b2=(COLOR16)(GetBValue(b)<<8);
    TRIVERTEX v[2] = { {x,y,r1,g1,b1,0}, {x+w,y+h,r2,g2,b2,0} };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(hdc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);
}
static void DrawTxt(HDC hdc, const char* t, int x, int y, int w, int h, COLORREF c, HFONT f, UINT al = DT_LEFT)
{
    SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, c);
    HFONT of = (HFONT)SelectObject(hdc, f);
    RECT r = { x, y, x+w, y+h }; DrawTextA(hdc, t, -1, &r, al | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, of);
}
static void FillR(HDC hdc, RECT rc, COLORREF c) { HBRUSH b=CreateSolidBrush(c); FillRect(hdc,&rc,b); DeleteObject(b); }
static void FrameR(HDC hdc, RECT rc, COLORREF c)
{
    HPEN p=CreatePen(PS_SOLID,1,c), op=(HPEN)SelectObject(hdc,p);
    HBRUSH ob=(HBRUSH)SelectObject(hdc,GetStockObject(NULL_BRUSH));
    Rectangle(hdc,rc.left,rc.top,rc.right,rc.bottom);
    SelectObject(hdc,op); SelectObject(hdc,ob); DeleteObject(p);
}
static int HitTest(int x, int y) { POINT p={x,y}; for (auto& b:g_btns) if (PtInRect(&b.rc,p)) return b.id; return 0; }
static void Inval(int id) { for (auto& b:g_btns) if (b.id==id) { InvalidateRect(g_hWnd,&b.rc,FALSE); break; } }
static void InvalAll() { InvalidateRect(g_hWnd, NULL, FALSE); }

// GDI fallback: simplified Octocat silhouette (used when PNG not available)
static void DrawOctocat(HDC hdc, int x, int y, int s)
{
    HBRUSH br = CreateSolidBrush(CLR_TEXT);
    HPEN pn = CreatePen(PS_SOLID, 0, CLR_TEXT);
    SelectObject(hdc, br); SelectObject(hdc, pn);
    int cx = x + s/2, cy = y + s/2 + 1;
    int r = s * 3 / 8;
    Ellipse(hdc, cx-r, cy-r, cx+r, cy+r); // head
    float e = r * 0.6f, eh = r * 0.7f;
    POINT el[] = {{cx-(int)(e*0.3f),cy-(int)(r*0.3f)},{cx-(int)(e*1.3f),cy-(int)(r+eh)},{cx+(int)(e*0.2f),cy-(int)(r*0.6f)}};
    POINT er[] = {{cx+(int)(e*0.3f),cy-(int)(r*0.3f)},{cx+(int)(e*1.3f),cy-(int)(r+eh)},{cx-(int)(e*0.2f),cy-(int)(r*0.6f)}};
    Polygon(hdc, el, 3); Polygon(hdc, er, 3);
    DeleteObject(br); DeleteObject(pn);
}

// ---------- worker ----------
static void Worker()
{
    SHCreateDirectoryExA(NULL, GetLocalPath("").c_str(), NULL);
    FILE* f = NULL;
    std::string vp = GetLocalPath(VERSION_FILE);
    if (fopen_s(&f, vp.c_str(), "r") == 0 && f) {
        if (fgets(g_localVer, sizeof(g_localVer), f)) {
            size_t len = strlen(g_localVer);
            if (len > 0 && g_localVer[len-1] == '\n') g_localVer[len-1] = 0;
        } fclose(f);
    }
    strcpy_s(g_status, "Checking for updates..."); g_progress = 5; InvalAll();
    std::string j = HttpGet("https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest");
    if (!j.empty()) {
        strncpy_s(g_latestVer, ExtractJson(j,"tag_name").c_str(), sizeof(g_latestVer)-1);
        strncpy_s(g_downloadUrl, ExtractJson(j,"browser_download_url").c_str(), sizeof(g_downloadUrl)-1);
    }
    g_progress = 10; InvalAll();
    g_updateAvail = (strcmp(g_localVer, g_latestVer) != 0);
    bool needDL = !FileExists(GetLocalPath(LAUNCHER_EXE).c_str()) || g_updateAvail;
    if (needDL && g_downloadUrl[0]) {
        strcpy_s(g_status, "Downloading..."); g_progress = 15; InvalAll();
        std::string zp = GetLocalPath("update.zip");
        if (SUCCEEDED(URLDownloadToFileA(NULL, g_downloadUrl, zp.c_str(), 0, NULL))) {
            g_progress = 55; strcpy_s(g_status, "Extracting..."); InvalAll();
            if (UnzipTo(zp.c_str(), GetLocalPath("").c_str())) {
                g_progress = 80; Sleep(100);
                if (fopen_s(&f, vp.c_str(), "w") == 0 && f) { fprintf(f, "%s", g_latestVer); fclose(f); }
                strcpy_s(g_localVer, g_latestVer); InvalAll();
                g_updateAvail = false;
            } else strcpy_s(g_status, "Extract failed");
        } else { strcpy_s(g_status, "Download failed"); InvalAll(); g_launchEnabled = FileExists(GetLocalPath(LAUNCHER_EXE).c_str()); Inval(B_LAUNCH); g_updating = false; return; }
        DeleteFileA(zp.c_str());
    }
    g_progress = 85; strcpy_s(g_status, "Starting drivers..."); InvalAll();
    EnsureDriver("vigembus","ViGEmBus.sys","Nefarius Virtual Gamepad Emulation Service"); Sleep(100);
    EnsureDriver(DRIVER_SERVICE,DRIVER_SYS,"xHunters kernel driver"); Sleep(100);
    g_progress = 100; g_ready = true; g_launchEnabled = true; g_updating = false;
    strcpy_s(g_status, "Ready"); InvalAll();
}

// ---------- window proc ----------
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM w, LPARAM lp)
{
    switch (msg) {
    case WM_NCCALCSIZE: return 0;
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ScreenToClient(hWnd, &pt);
        if (pt.y < TITLE_H) {
            if (pt.x >= CARD_R - 50) return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_ENTERSIZEMOVE: g_dragging = true; KillTimer(hWnd, 1); break;
    case WM_EXITSIZEMOVE: g_dragging = false; SetTimer(hWnd, 1, 50, NULL); InvalAll(); break;
    case WM_TIMER: {
        if (g_dragging) break;
        g_animTick++;

        // Smooth progress lerp
        float t = (float)g_progress;
        if (fabs(g_dispProgress - t) > 0.3f) {
            g_dispProgress += (t - g_dispProgress) * 0.15f;
            InvalidateRect(hWnd, &g_rcProg, FALSE);
        } else if ((int)g_dispProgress != g_progress) {
            g_dispProgress = t;
            InvalidateRect(hWnd, &g_rcProg, FALSE);
        }

        // Pulse dot (only when ready)
        if (g_ready) InvalidateRect(hWnd, &g_rcDot, FALSE);

        // Process monitor: check if cheat is still running (every ~2s = 40 ticks)
        if (g_cheatLaunched && g_animTick % 40 == 0) {
            bool found = false;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W pe = { sizeof(pe) };
                if (Process32FirstW(snap, &pe)) do {
                    if (wcsstr(pe.szExeFile, L"FortniteESP")) { found = true; break; }
                } while (Process32NextW(snap, &pe));
                CloseHandle(snap);
            }
            if (found != g_cheatRunning) {
                g_cheatRunning = found;
                if (!found) { g_cheatLaunched = false; strcpy_s(g_status, "Ready"); }
                InvalAll();
            }
        }

        // Ripple
        if (g_ripple.active) {
            g_ripple.r += 4.0f;
            if (g_ripple.r > g_ripple.maxR) {
                g_ripple.active = false;
                InvalidateRect(hWnd, &g_rcLaunch, FALSE);
            } else {
                RECT rr = { (int)(g_ripple.x - g_ripple.r - 6), (int)(g_ripple.y - g_ripple.r - 6),
                            (int)(g_ripple.x + g_ripple.r + 6), (int)(g_ripple.y + g_ripple.r + 6) };
                InvalidateRect(hWnd, &rr, FALSE);
                InvalidateRect(hWnd, &g_rcLaunch, FALSE);
            }
        }
        break;
    }
    case WM_MOUSEMOVE: {
        int id = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (id != g_hotBtn) { int o=g_hotBtn; g_hotBtn=id; if(o)Inval(o); if(id)Inval(id); }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE: { int o=g_hotBtn; g_hotBtn=0; if(o)Inval(o); break; }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int id = HitTest(x, y);
        if (!id) break;
        if (id == B_CLOSE) { PostQuitMessage(0); return 0; }
        if (id == B_MIN) { ShowWindow(hWnd, SW_MINIMIZE); return 0; }
        g_pushBtn = id; Inval(id);
        if (id == B_LAUNCH && g_launchEnabled) {
            g_ripple.active = true;
            g_ripple.x = (float)(g_rcLaunch.left + g_rcLaunch.right) * 0.5f;
            g_ripple.y = (float)(g_rcLaunch.top + g_rcLaunch.bottom) * 0.5f;
            g_ripple.r = 0; g_ripple.maxR = 110.0f;
            std::string exe = GetLocalPath(LAUNCHER_EXE);
            if (!FileExists(exe.c_str())) { strcpy_s(g_status, "Cheat not found."); InvalAll(); break; }
            WCHAR we[MAX_PATH], wd[MAX_PATH];
            MultiByteToWideChar(CP_UTF8,0,exe.c_str(),-1,we,MAX_PATH);
            wcscpy_s(wd,we); WCHAR* p = wcsrchr(wd,L'\\'); if(p)*p=0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
            if (snap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W pe = { sizeof(pe) };
                if (Process32FirstW(snap,&pe)) do { if (wcsstr(pe.szExeFile,L"FortniteESP")) { HANDLE hp=OpenProcess(PROCESS_TERMINATE,FALSE,pe.th32ProcessID); if(hp){TerminateProcess(hp,0);CloseHandle(hp);} } } while(Process32NextW(snap,&pe));
                CloseHandle(snap); Sleep(500);
            }
            char stopCmd[64], startCmd[64];
            sprintf_s(stopCmd, "sc stop %s", DRIVER_SERVICE);
            sprintf_s(startCmd, "sc start %s", DRIVER_SERVICE);
            RunCmd(stopCmd, false);
            Sleep(500);
            RunCmd(startCmd, false);
            Sleep(300);
            SHELLEXECUTEINFOW sei = { sizeof(sei) }; sei.lpFile=we; sei.lpDirectory=wd; sei.nShow=SW_SHOW;
            if (!ShellExecuteExW(&sei)) { strcpy_s(g_status, "Launch failed"); InvalAll(); break; }
            g_cheatLaunched = true; g_cheatRunning = true;
            strcpy_s(g_status, "Cheat launched"); InvalAll();
        }
        if (id == B_UPDATE && g_updateAvail && !g_updating) {
            g_updating = true; g_launchEnabled = false; g_updateAvail = false;
            Inval(B_LAUNCH); Inval(B_UPDATE);
            strcpy_s(g_status, "Updating..."); InvalAll();
            std::thread(Worker).detach();
        }
        if (id == B_GITHUB) { char u[256]; sprintf_s(u,"https://github.com/%s/%s/releases",GITHUB_USER,GITHUB_REPO); ShellExecuteA(NULL,"open",u,NULL,NULL,SW_SHOW); }
        break;
    }
    case WM_LBUTTONUP: { if(g_pushBtn){Inval(g_pushBtn);g_pushBtn=0;} break; }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);
        int W = WIN_W, H = WIN_H;

        // Background
        FillR(hdc, rc, CLR_BG);

        // Title bar
        FillR(hdc, {0,0,W,TITLE_H}, CLR_TITLE_BG);
        HPEN sp = CreatePen(PS_SOLID,1,CLR_ACCENT); SelectObject(hdc,sp);
        MoveToEx(hdc,0,TITLE_H,NULL); LineTo(hdc,W,TITLE_H); DeleteObject(sp);
        DrawTxt(hdc, APP_TITLE, 14,0,200,TITLE_H, CLR_TEXT, g_fntSmall, DT_LEFT);

        // Title buttons
        struct { int id;int x;int w; } tb[] = {{B_CLOSE,CARD_R-27,27},{B_MIN,CARD_R-54,27}};
        for (auto& t : tb) {
            if (g_hotBtn == t.id) FillR(hdc,{t.x,0,t.x+t.w,TITLE_H}, t.id==B_CLOSE?RGB(200,40,40):RGB(30,30,45));
            if (t.id == B_CLOSE) {
                HPEN xp = CreatePen(PS_SOLID,2,CLR_TEXT); SelectObject(hdc,xp);
                int cx=t.x+t.w/2, cy=TITLE_H/2, s=6;
                MoveToEx(hdc,cx-s,cy-s,NULL); LineTo(hdc,cx+s,cy+s);
                MoveToEx(hdc,cx+s,cy-s,NULL); LineTo(hdc,cx-s,cy+s);
                DeleteObject(xp);
            } else {
                HPEN mp = CreatePen(PS_SOLID,2,CLR_TEXT); SelectObject(hdc,mp);
                MoveToEx(hdc,t.x+6,TITLE_H/2,NULL); LineTo(hdc,t.x+t.w-6,TITLE_H/2);
                DeleteObject(mp);
            }
        }

        // Header
        DrawTxt(hdc, "FN Cheat Launcher", CARD_L,44,400,30, CLR_TEXT, g_fntBig, DT_LEFT);
        DrawTxt(hdc, "Fortnite ESP + Aimbot  |  Controller Support", CARD_L,74,400,16, CLR_SUBTEXT, g_fntSmall, DT_LEFT);

        // Card
        RECT card = {CARD_L,CARD_T,CARD_R,CARD_B};
        FillR(hdc, card, CLR_CARD); FrameR(hdc, card, CLR_BORDER);
        int cx = CARD_L + 24, cw = CARD_R - CARD_L - 48;

        // Status dot — animated pulse
        int dotClr = CLR_SUBTEXT, dotSz = 8;
        if (g_ready) {
            float ph = sinf(g_animTick * 0.12f) * 0.5f + 0.5f;
            dotSz = 6 + (int)(4 * ph);
            dotClr = LerpC(CLR_GREEN_D, CLR_GREEN, ph);
        }
        HBRUSH dbr = CreateSolidBrush(dotClr);
        HPEN dpn = CreatePen(PS_SOLID,0,dotClr);
        SelectObject(hdc,dbr); SelectObject(hdc,dpn);
        int off = (8-dotSz)/2;
        Ellipse(hdc, cx+off, CARD_T+16+off, cx+off+dotSz, CARD_T+16+off+dotSz);
        DeleteObject(dbr); DeleteObject(dpn);

        DrawTxt(hdc, "Status", cx+16, CARD_T+13, 50,14, CLR_SUBTEXT, g_fntSmall, DT_LEFT);
        DrawTxt(hdc, g_status, cx+70, CARD_T+13, cw-70,14, CLR_TEXT, g_fntSmall, DT_LEFT);

        // Progress bar (GradientFill — one call, instant)
        int pby = CARD_T + 42;
        FillR(hdc, {cx,pby,cx+cw,pby+6}, CLR_BORDER);
        int fw = (int)(cw * g_dispProgress / 100.0f);
        if (fw > 0) DrawGradH(hdc, cx, pby, fw, 6, CLR_ACCENT, CLR_ACCENT2);

        // Separator
        HPEN sep = CreatePen(PS_SOLID,1,CLR_BORDER); SelectObject(hdc,sep);
        MoveToEx(hdc,cx,CARD_T+56,NULL); LineTo(hdc,cx+cw,CARD_T+56); DeleteObject(sep);

        // Version
        char ver[128];
        if (g_localVer[0] && g_localVer[0]!='0') sprintf_s(ver,"Installed: v%s     Latest: v%s",g_localVer,g_latestVer[0]?g_latestVer:"...");
        else sprintf_s(ver,"Latest version: v%s",g_latestVer[0]?g_latestVer:"...");
        DrawTxt(hdc, ver, cx, CARD_T+63, cw,16, CLR_SUBTEXT, g_fntSmall, DT_LEFT);

        // LAUNCH GAME / RUNNING button
        int bw = cw-40, bx = cx+20, by = CARD_T+90;
        RECT lrc = {bx,by,bx+bw,by+40};
        bool lHot = (g_hotBtn == B_LAUNCH);
        bool lDisabled = !g_launchEnabled || g_cheatLaunched;
        if (lDisabled) {
            FillR(hdc, lrc, CLR_CARD); FrameR(hdc, lrc, CLR_BORDER);
            DrawTxt(hdc, g_cheatLaunched ? "ALREADY RUNNING" : "LAUNCH GAME", bx,by,bw,40, CLR_BORDER, g_fntMed, DT_CENTER);
        } else {
            DrawGradH(hdc, bx,by,bw,40, lHot?CLR_ACCENT_H:CLR_ACCENT, CLR_ACCENT2);
            DrawTxt(hdc, "LAUNCH GAME", bx,by,bw,40, RGB(255,255,255), g_fntMed, DT_CENTER);
            if (g_ripple.active) {
                float t = g_ripple.r / g_ripple.maxR;
                if (t <= 1.0f) {
                    HPEN rp = CreatePen(PS_SOLID,2,LerpC(RGB(255,255,255),CLR_BG,t));
                    SelectObject(hdc,rp); SelectObject(hdc,GetStockObject(NULL_BRUSH));
                    int rr = (int)g_ripple.r;
                    Ellipse(hdc,(int)g_ripple.x-rr,(int)g_ripple.y-rr,(int)g_ripple.x+rr,(int)g_ripple.y+rr);
                    DeleteObject(rp);
                }
            }
        }

        // Secondary buttons
        int b2y = CARD_T+144, b2w=120, b2gap=20, total=b2w*2+b2gap;
        int b2x = cx + (cw-total)/2;
        RECT urc = {b2x,b2y,b2x+b2w,b2y+34};
        bool uHot = (g_hotBtn==B_UPDATE), uVis = g_updateAvail||g_updating;
        if (uVis) { FillR(hdc,urc,uHot?CLR_BTN_HOV:CLR_BTN_BG); FrameR(hdc,urc,uHot?CLR_ACCENT:CLR_BORDER);
            DrawTxt(hdc, g_updating?"UPDATING...":"UPDATE", b2x,b2y,b2w,34, uHot?CLR_TEXT:CLR_SUBTEXT, g_fntSmall, DT_CENTER); }
        RECT grc = {b2x+b2w+b2gap,b2y,b2x+total,b2y+34};
        bool gHot = (g_hotBtn==B_GITHUB);
        FillR(hdc,grc,gHot?CLR_BTN_HOV:CLR_BTN_BG); FrameR(hdc,grc,gHot?CLR_ACCENT:CLR_BORDER);
        // GitHub icon: try GDI+ PNG, fall back to GDI-drawn Octocat
        int ghIconSize = 18, ghIconPad = 6;
        int ghTxtX = grc.left + ghIconPad + ghIconSize + 4;
        int ghTxtW = (grc.right - ghTxtX) - 4;
        // Retry loading PNG every 120 frames (~6s)
        if (!g_ghImg && g_animTick - g_ghLoadTick > 120) {
            g_ghLoadTick = g_animTick;
            char ip[MAX_PATH]; strcpy_s(ip, GetLocalPath("github.png").c_str());
            if (FileExists(ip)) {
                WCHAR wip[MAX_PATH] = {};
                MultiByteToWideChar(CP_UTF8,0,ip,-1,wip,MAX_PATH);
                Image* img = new Image(wip);
                if (img && img->GetLastStatus() == Ok) g_ghImg = img;
                else delete img;
            }
        }
        if (g_ghImg) {
            Graphics gfx(hdc);
            gfx.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            gfx.DrawImage(g_ghImg, grc.left+ghIconPad, grc.top+(34-ghIconSize)/2, ghIconSize, ghIconSize);
        } else {
            DrawOctocat(hdc, grc.left+ghIconPad, grc.top+(34-ghIconSize)/2, ghIconSize);
        }
        DrawTxt(hdc, "GITHUB", ghTxtX, grc.top, ghTxtW, 34, gHot?CLR_TEXT:CLR_SUBTEXT, g_fntSmall, DT_LEFT);

        // Footer note
        DrawTxt(hdc, "Launcher will start the cheat and bring Fortnite to the foreground",
            cx, CARD_B-20, cw,16, CLR_SUBTEXT, g_fntSmall, DT_CENTER);
        char ftr[128]; sprintf_s(ftr,"%s/%s  |  github.com",GITHUB_USER,GITHUB_REPO);
        DrawTxt(hdc, ftr, CARD_L, WIN_H-28, WIN_W-40,16, CLR_SUBTEXT, g_fntSmall, DT_LEFT);

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: g_running=false; PostQuitMessage(0); break;
    default: return DefWindowProc(hWnd,msg,w,lp);
    }
    return 0;
}

// ---------- entry ----------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow)
{
    if (!IsAdmin()) { char p[MAX_PATH]; GetModuleFileNameA(NULL,p,MAX_PATH); ShellExecuteA(NULL,"runas",p,NULL,NULL,SW_SHOW); return 0; }
    SetProcessDPIAware();
    // Global crash handler (catches all unhandled exceptions)
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep)->LONG {
        char buf[512]; sprintf_s(buf, "Launcher crashed (0x%08X).\n\nThis usually happens when the cheat\ndriver or Fortnite interferes with the process.\nClick OK to exit.", ep->ExceptionRecord->ExceptionCode);
        MessageBoxA(NULL, buf, "FN Launcher Error", MB_OK|MB_ICONERROR);
        return EXCEPTION_EXECUTE_HANDLER;
    });
    g_fntBig   = CreateFontA(-20,0,0,0,FW_BOLD,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH|FF_DONTCARE,"Segoe UI");
    g_fntMed   = CreateFontA(-13,0,0,0,FW_SEMIBOLD,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH|FF_DONTCARE,"Segoe UI");
    g_fntSmall = CreateFontA(-11,0,0,0,FW_NORMAL,0,0,0,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH|FF_DONTCARE,"Segoe UI");
    int cx=CARD_L+24, cw=CARD_R-CARD_L-48, bw=cw-40, bx=cx+20, by=CARD_T+90;
    g_rcDot    = {cx-2, CARD_T+12, cx+120, CARD_T+30};
    g_rcProg   = {cx, CARD_T+40, cx+cw, CARD_T+50};
    g_rcLaunch = {bx, by, bx+bw, by+40};
    g_btns.push_back({{CARD_R-27,0,CARD_R,TITLE_H},B_CLOSE});
    g_btns.push_back({{CARD_R-54,0,CARD_R-27,TITLE_H},B_MIN});
    g_btns.push_back({g_rcLaunch,B_LAUNCH});
    int b2x = cx + (cw - (120*2+20))/2;
    g_btns.push_back({{b2x,CARD_T+144,b2x+120,CARD_T+178},B_UPDATE});
    g_btns.push_back({{b2x+140,CARD_T+144,b2x+260,CARD_T+178},B_GITHUB});

    WNDCLASSEXW wc = { sizeof(wc) }; wc.lpfnWndProc=WndProc; wc.hInstance=hInst;
    wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.hbrBackground=NULL; wc.lpszClassName=L"FNLauncher3";
    wc.hIcon=LoadIcon(NULL,IDI_APPLICATION); RegisterClassExW(&wc);
    int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
    g_hWnd = CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW, L"FNLauncher3",L"FN Cheat",WS_POPUP|WS_VISIBLE,
        (sw-WIN_W)/2,(sh-WIN_H)/2,WIN_W,WIN_H,NULL,NULL,hInst,NULL);
    if (!g_hWnd) return 1;
    MARGINS m = {0,0,0,1}; DwmExtendFrameIntoClientArea(g_hWnd,&m);
    DWORD da = 2; DwmSetWindowAttribute(g_hWnd,2,&da,sizeof(da));
    SetLayeredWindowAttributes(g_hWnd,0,0,LWA_ALPHA);
    for (int a=0;a<=255;a+=17) { SetLayeredWindowAttributes(g_hWnd,0,(BYTE)a,LWA_ALPHA); Sleep(6); }
    SetLayeredWindowAttributes(g_hWnd,0,255,LWA_ALPHA);
    // GDI+ for GitHub icon
    GdiplusStartup(&g_gdiTok, &g_gdiInp, NULL);
    // Background download of GitHub icon PNG (try multiple sources)
    std::string ghPath = GetLocalPath("github.png");
    if (!FileExists(ghPath.c_str()))
        std::thread([]() {
            const char* urls[] = {
                "https://github.githubassets.com/assets/GitHub-Mark-ea2971cee799.png",
                "https://pngimg.com/uploads/github/github_PNG15.png",
            };
            for (auto u : urls) {
                if (SUCCEEDED(URLDownloadToFileA(NULL, u, GetLocalPath("github.png").c_str(), 0, NULL)))
                    break;
            }
        }).detach();

    SetTimer(g_hWnd, 1, 50, NULL);
    std::thread(Worker).detach();
    // Message loop
    MSG msg;
    while (g_running && GetMessage(&msg,NULL,0,0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    KillTimer(g_hWnd,1);
    if (g_ghImg) delete g_ghImg;
    GdiplusShutdown(g_gdiTok);
    DeleteObject(g_fntBig); DeleteObject(g_fntMed); DeleteObject(g_fntSmall);
    return 0;
}
