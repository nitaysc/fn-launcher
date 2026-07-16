// Launcher.cpp - FN Cheat Launcher v1.0
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>
#include <TlHelp32.h>
#include <CommCtrl.h>
#include <ShellAPI.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <stdio.h>
#include <string>
#include <thread>


#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

#define GITHUB_USER    "nitaysc"
#define GITHUB_REPO    "fn-launcher"
#define CHEAT_FOLDER   "fn-cheat"
#define LAUNCHER_EXE   "FortniteESP.exe"
#define CHEAT_ZIP      "cheat.zip"
#define VERSION_FILE   "version.txt"
#define DRIVER_SERVICE "xhunter1"
#define DRIVER_SYS     "ACvalun.sys"
#define CURRENT_VER    "1.0"
#define APP_TITLE      L"FN Cheat"

#define IDC_STATUS     1001
#define IDC_PROGRESS   1002
#define IDC_VERSION    1003
#define IDC_LAUNCH     1004
#define IDC_UPDATE     1005
#define IDC_WEBLINK    1006
#define IDC_LOGO       1007

HWND g_hWnd = NULL;
HWND g_hStatus = NULL;
HWND g_hProgress = NULL;
HWND g_hVersion = NULL;
HWND g_hLaunch = NULL;
HWND g_hUpdate = NULL;
HWND g_hWebLink = NULL;
HFONT g_hTitleFont = NULL;
HFONT g_hNormFont = NULL;
HFONT g_hBtnFont = NULL;
HBRUSH g_bgBrush = NULL;
HBRUSH g_whiteBrush = NULL;

char g_localVer[32] = "0";
char g_latestVer[32] = "0";
char g_downloadUrl[512] = "";
bool g_ready = false;
bool g_updateAvail = false;
bool g_isAdmin = false;

// Colors
const COLORREF CLR_BG      = RGB(18, 18, 22);
const COLORREF CLR_ACCENT  = RGB(0, 120, 215);
const COLORREF CLR_TEXT    = RGB(220, 220, 225);
const COLORREF CLR_SUBTEXT = RGB(140, 140, 150);
const COLORREF CLR_GREEN   = RGB(60, 200, 80);

bool IsAdmin()
{
    BOOL isElevated = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION te = {};
        DWORD sz = sizeof(te), ret = 0;
        if (GetTokenInformation(hToken, TokenElevation, &te, sz, &ret))
            isElevated = te.TokenIsElevated;
        CloseHandle(hToken);
    }
    return isElevated != FALSE;
}

bool RunCmd(const char* cmd, bool wait = true)
{
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(NULL, (LPSTR)cmd, NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return false;
    if (wait) WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool DirExists(const char* path)
{
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
}

bool FileExists(const char* path)
{
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

std::string GetLocalPath(const char* sub = "")
{
    char path[MAX_PATH] = {};
    SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path);
    strcat_s(path, "\\");
    strcat_s(path, CHEAT_FOLDER);
    if (sub && sub[0]) {
        strcat_s(path, "\\");
        strcat_s(path, sub);
    }
    return std::string(path);
}

std::string HttpGet(const char* url)
{
    std::string result;
    HINTERNET hSession = WinHttpOpen(L"Launcher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        NULL, NULL, 0);
    if (!hSession) return result;
    HINTERNET hConnect = WinHttpConnect(hSession,
        L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
            L"/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest",
            NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
        if (hRequest) {
            WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0);
            WinHttpReceiveResponse(hRequest, NULL);
            char buf[1024];
            DWORD read = 0;
            while (WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &read) && read > 0) {
                buf[read] = 0;
                result += buf;
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return result;
}

std::string ExtractJsonStr(const std::string& json, const char* field)
{
    char search[128];
    sprintf_s(search, "\"%s\":\"", field);
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += strlen(search);
    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

bool UnzipTo(const char* zipPath, const char* destPath)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool ok = false;
    wchar_t wzp[MAX_PATH], wdp[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, zipPath, -1, wzp, MAX_PATH);
    MultiByteToWideChar(CP_UTF8, 0, destPath, -1, wdp, MAX_PATH);
    SHCreateDirectoryExW(NULL, wdp, NULL);
    IShellDispatch* pShell = NULL;
    HRESULT hr = CoCreateInstance(CLSID_Shell, NULL, CLSCTX_INPROC_SERVER,
        IID_IShellDispatch, (void**)&pShell);
    if (SUCCEEDED(hr) && pShell) {
        VARIANT vDir; VariantInit(&vDir); vDir.vt = VT_BSTR; vDir.bstrVal = SysAllocString(wzp);
        Folder* pFolder = NULL;
        hr = pShell->NameSpace(vDir, &pFolder);
        if (SUCCEEDED(hr) && pFolder) {
            FolderItems* pItems = NULL;
            pFolder->Items(&pItems);
            if (pItems) {
                VARIANT vDest; VariantInit(&vDest); vDest.vt = VT_BSTR; vDest.bstrVal = SysAllocString(wdp);
                Folder* pDest = NULL;
                pShell->NameSpace(vDest, &pDest);
                if (pDest) {
                    VARIANT vSrc, vOpt;
                    VariantInit(&vSrc); vSrc.vt = VT_DISPATCH; vSrc.pdispVal = pItems;
                    VariantInit(&vOpt); vOpt.vt = VT_I4; vOpt.lVal = 0x14;
                    pDest->CopyHere(vSrc, vOpt);
                    ok = true;
                    pDest->Release();
                }
                VariantClear(&vDest); pItems->Release();
            }
            pFolder->Release();
        }
        VariantClear(&vDir); pShell->Release();
    }
    CoUninitialize();
    return ok;
}

void SetStatus(const char* text)
{
    if (g_hStatus) SendMessageA(g_hStatus, WM_SETTEXT, 0, (LPARAM)text);
}

void SetProgress(int pct)
{
    if (g_hProgress) SendMessage(g_hProgress, PBM_SETPOS, pct, 0);
}

void SetVersionText(const char* local, const char* latest)
{
    char buf[256];
    if (local[0] && local[0] != '0')
        sprintf_s(buf, "v%s installed  \x95  v%s latest", local, latest);
    else
        sprintf_s(buf, "Latest: v%s", latest);
    if (g_hVersion) SendMessageA(g_hVersion, WM_SETTEXT, 0, (LPARAM)buf);
}

void EnableLaunch(bool enable)
{
    if (g_hLaunch) EnableWindow(g_hLaunch, enable ? TRUE : FALSE);
}

void ShowUpdateBtn(bool show)
{
    if (g_hUpdate) ShowWindow(g_hUpdate, show ? SW_SHOW : SW_HIDE);
}

bool EnsureDriver(const char* service, const char* sysFile, const char* display)
{
    std::string sysPath = GetLocalPath(sysFile);
    if (!FileExists(sysPath.c_str())) return false;

    char buf[1024];
    // Check if service exists
    sprintf_s(buf, "sc query %s", service);
    if (RunCmd(buf)) {
        // Exists, just start it
        sprintf_s(buf, "sc start %s", service);
        RunCmd(buf, false);
        return true;
    }
    // Create and start
    sprintf_s(buf, "sc create %s binPath= \"%s\" type= kernel start= auto DisplayName= \"%s\"",
        service, sysPath.c_str(), display ? display : service);
    if (!RunCmd(buf)) return false;
    sprintf_s(buf, "sc start %s", service);
    RunCmd(buf, false);
    return true;
}

void InstallDrivers()
{
    SetStatus("Installing gamepad driver...");
    EnsureDriver("vigembus", "ViGEmBus.sys", "Nefarius Virtual Gamepad Emulation Service");
    Sleep(200);
    SetStatus("Installing memory driver...");
    EnsureDriver(DRIVER_SERVICE, DRIVER_SYS, "xhunter1 kernel driver");
    Sleep(200);
}

void WorkerThread()
{
    SetStatus("Initializing...");
    std::string localDir = GetLocalPath("");
    std::string verPath = GetLocalPath(VERSION_FILE);
    SHCreateDirectoryExA(NULL, localDir.c_str(), NULL);
    FILE* f = NULL;
    if (fopen_s(&f, verPath.c_str(), "r") == 0 && f) {
        if (fgets(g_localVer, sizeof(g_localVer), f)) {
            size_t len = strlen(g_localVer);
            if (len > 0 && g_localVer[len - 1] == '\n') g_localVer[len - 1] = 0;
        }
        fclose(f);
    }
    SetStatus("Checking for updates...");
    Sleep(200);
    std::string json = HttpGet("https://api.github.com/repos/" GITHUB_USER "/" GITHUB_REPO "/releases/latest");
    if (!json.empty()) {
        std::string tag = ExtractJsonStr(json, "tag_name");
        std::string url = ExtractJsonStr(json, "browser_download_url");
        strncpy_s(g_latestVer, tag.c_str(), sizeof(g_latestVer) - 1);
        strncpy_s(g_downloadUrl, url.c_str(), sizeof(g_downloadUrl) - 1);
    }
    SetVersionText(g_localVer, g_latestVer[0] ? g_latestVer : g_localVer);
    g_updateAvail = (strcmp(g_localVer, g_latestVer) != 0);
    bool needDownload = (!FileExists(GetLocalPath(LAUNCHER_EXE).c_str()) || g_updateAvail);
    if (needDownload && g_downloadUrl[0]) {
        SetStatus("Downloading cheat files...");
        SetProgress(10);
        std::string zipPath = localDir + "\\update.zip";
        HRESULT hr = URLDownloadToFileA(NULL, g_downloadUrl, zipPath.c_str(), 0, NULL);
        if (SUCCEEDED(hr)) {
            SetProgress(50);
            SetStatus("Extracting...");
            if (UnzipTo(zipPath.c_str(), localDir.c_str())) {
                SetProgress(80);
                Sleep(200);
                if (fopen_s(&f, verPath.c_str(), "w") == 0 && f) {
                    fprintf(f, "%s", g_latestVer);
                    fclose(f);
                }
                strcpy_s(g_localVer, g_latestVer);
                SetVersionText(g_localVer, g_latestVer);
                g_updateAvail = false;
                ShowUpdateBtn(false);
            }
            DeleteFileA(zipPath.c_str());
        } else {
            SetStatus("Download failed. Check internet connection.");
            EnableLaunch(FileExists(GetLocalPath(LAUNCHER_EXE).c_str()));
            return;
        }
    }
    InstallDrivers();
    SetProgress(100);
    g_ready = true;
    SetStatus("Ready! Click Launch to start.");
    EnableLaunch(true);
    ShowUpdateBtn(g_updateAvail);
}

// Custom draw the logo banner at the top
void DrawBanner(HWND hWnd, HDC hdc, RECT rc)
{
    // Top accent bar
    RECT bar = rc;
    bar.bottom = bar.top + 2;
    HBRUSH accent = CreateSolidBrush(CLR_ACCENT);
    FillRect(hdc, &bar, accent);
    DeleteObject(accent);

    // Title text
    SetBkMode(hdc, TRANSPARENT);
    HFONT old = (HFONT)SelectObject(hdc, g_hTitleFont);
    SetTextColor(hdc, RGB(255, 255, 255));
    RECT tr = { 30, 20, rc.right - 30, 65 };
    DrawTextW(hdc, L"FN Cheat Launcher", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);

    // Subtitle
    old = (HFONT)SelectObject(hdc, g_hNormFont);
    SetTextColor(hdc, CLR_SUBTEXT);
    tr.top = 52; tr.bottom = 75;
    DrawTextW(hdc, L"Fortnite ESP + Aimbot  \xb7  ViGEm Controller", -1, &tr,
        DT_LEFT | DT_TOP | DT_SINGLELINE);
    SelectObject(hdc, old);
}

void DrawMainArea(HDC hdc, RECT rc)
{
    RECT area = { 20, 85, rc.right - 20, rc.bottom - 10 };
    HBRUSH bg = CreateSolidBrush(RGB(25, 25, 32));
    FillRect(hdc, &area, bg);
    DeleteObject(bg);

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(40, 40, 50));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, area.left, area.top, area.right, area.bottom);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, CLR_TEXT);
        return (LRESULT)g_whiteBrush;
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)g_whiteBrush;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_LAUNCH) {
            std::string exePath = GetLocalPath(LAUNCHER_EXE);
            if (!FileExists(exePath.c_str())) {
                SetStatus("Cheat not found. Run update first.");
                break;
            }
            WCHAR wExe[MAX_PATH], wDir[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, exePath.c_str(), -1, wExe, MAX_PATH);
            wcscpy_s(wDir, wExe);
            WCHAR* p = wcsrchr(wDir, L'\\');
            if (p) *p = 0;
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.lpFile = wExe;
            sei.lpDirectory = wDir;
            sei.nShow = SW_SHOW;
            // Kill any stale instance first
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W pe2 = { sizeof(pe2) };
                if (Process32FirstW(snap, &pe2)) do {
                    if (wcsstr(pe2.szExeFile, L"FortniteESP")) {
                        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe2.th32ProcessID);
                        if (hProc) { TerminateProcess(hProc, 0); CloseHandle(hProc); }
                    }
                } while (Process32NextW(snap, &pe2));
                CloseHandle(snap);
                Sleep(500);
            }
            // Restart driver to clear stale state
            RunCmd("sc stop xhunter1", false);
            Sleep(1000);
            RunCmd("sc start xhunter1", false);
            Sleep(500);
            if (!ShellExecuteExW(&sei)) {
                SetStatus("Launch failed");
                break;
            }
            SetStatus("Cheat launched");
            // Focus Fortnite window
            Sleep(1000);
            EnumWindows([](HWND hw, LPARAM) -> BOOL {
                wchar_t cls[64]; GetClassNameW(hw, cls, 64);
                if (wcscmp(cls, L"UnrealWindow") == 0) {
                    wchar_t title[128]; GetWindowTextW(hw, title, 128);
                    if (wcsstr(title, L"Fortnite") || wcsstr(title, L" 1")) {
                        SetForegroundWindow(hw);
                        return FALSE;
                    }
                }
                return TRUE;
            }, 0);
        }
        if (id == IDC_UPDATE) {
            g_ready = false;
            EnableLaunch(false);
            ShowUpdateBtn(false);
            SetProgress(0);
            SetStatus("Updating...");
            std::thread(WorkerThread).detach();
        }
        if (id == IDC_WEBLINK) {
            char url[256];
            sprintf_s(url, "https://github.com/%s/%s/releases", GITHUB_USER, GITHUB_REPO);
            ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOW);
        }
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        // Background
        FillRect(hdc, &rc, g_bgBrush);

        // Draw banner
        DrawBanner(hWnd, hdc, rc);

        // Draw main area border
        DrawMainArea(hdc, rc);

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow)
{
    if (!IsAdmin()) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        ShellExecuteA(NULL, "runas", exePath, NULL, NULL, SW_SHOW);
        return 0;
    }
    g_isAdmin = true;

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icex);

    // Create fonts
    LOGFONTW lf = {};
    lf.lfHeight = -22;
    lf.lfWeight = FW_BOLD;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    g_hTitleFont = CreateFontIndirectW(&lf);

    lf.lfHeight = -12;
    lf.lfWeight = FW_NORMAL;
    g_hNormFont = CreateFontIndirectW(&lf);

    lf.lfHeight = -13;
    lf.lfWeight = FW_SEMIBOLD;
    g_hBtnFont = CreateFontIndirectW(&lf);

    g_bgBrush = CreateSolidBrush(CLR_BG);
    g_whiteBrush = CreateSolidBrush(RGB(25, 25, 32));

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"FNLauncher";
    RegisterClassExW(&wc);

    int winW = 480, winH = 290;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    g_hWnd = CreateWindowExW(0, L"FNLauncher", APP_TITLE,
        WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        (sw - winW) / 2, (sh - winH) / 2, winW, winH,
        NULL, NULL, hInst, NULL);
    if (!g_hWnd) return 1;

    // Status text
    g_hStatus = CreateWindowExW(0, L"STATIC", L"Initializing...",
        WS_CHILD | WS_VISIBLE,
        35, 98, 410, 20, g_hWnd, NULL, hInst, NULL);
    SendMessage(g_hStatus, WM_SETFONT, (WPARAM)g_hNormFont, TRUE);

    // Progress bar (themed)
    g_hProgress = CreateWindowExW(0, L"msctls_progress32", NULL,
        WS_CHILD | WS_VISIBLE,
        35, 122, 410, 18, g_hWnd, (HMENU)IDC_PROGRESS, hInst, NULL);
    SendMessage(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(g_hProgress, PBM_SETBARCOLOR, 0, CLR_ACCENT);

    // Version info
    g_hVersion = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        35, 148, 410, 18, g_hWnd, NULL, hInst, NULL);
    SendMessage(g_hVersion, WM_SETFONT, (WPARAM)g_hNormFont, TRUE);

    // Launch button (prominent)
    g_hLaunch = CreateWindowExW(0, L"BUTTON", L"Launch Game",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        35, 178, 180, 42, g_hWnd, (HMENU)IDC_LAUNCH, hInst, NULL);
    SendMessage(g_hLaunch, WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);
    EnableWindow(g_hLaunch, FALSE);

    // Update button
    g_hUpdate = CreateWindowExW(0, L"BUTTON", L"Update Available",
        WS_CHILD | BS_PUSHBUTTON,
        230, 178, 130, 42, g_hWnd, (HMENU)IDC_UPDATE, hInst, NULL);
    SendMessage(g_hUpdate, WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);
    ShowWindow(g_hUpdate, SW_HIDE);

    // GitHub button
    g_hWebLink = CreateWindowExW(0, L"BUTTON", L"GitHub",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        375, 178, 80, 42, g_hWnd, (HMENU)IDC_WEBLINK, hInst, NULL);
    SendMessage(g_hWebLink, WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);

    // Footer
    HWND hFooter = CreateWindowExW(0, L"STATIC", L"nitaysc/fn-launcher  \xb7  github.com",
        WS_CHILD | WS_VISIBLE, 35, 238, 410, 16, g_hWnd, NULL, hInst, NULL);
    SendMessage(hFooter, WM_SETFONT, (WPARAM)g_hNormFont, TRUE);

    std::thread(WorkerThread).detach();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DeleteObject(g_hTitleFont);
    DeleteObject(g_hNormFont);
    DeleteObject(g_hBtnFont);
    DeleteObject(g_bgBrush);
    DeleteObject(g_whiteBrush);
    return 0;
}
