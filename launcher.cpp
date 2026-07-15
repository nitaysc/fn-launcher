// Launcher.cpp - FN Cheat Launcher v1.0
// Win32 GUI: auto-downloads cheat, installs driver, launches overlay
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>
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
// Admin requirement via embedded manifest (launcher.exe.manifest)
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

// ============================================================
// Config - change these for your GitHub repo
// ============================================================
#define GITHUB_USER    "nitaysc"
#define GITHUB_REPO    "fn-launcher"
#define CHEAT_FOLDER   "fn-cheat"
#define LAUNCHER_EXE   "FortniteESP.exe"
#define CHEAT_ZIP      "cheat.zip"
#define VERSION_FILE   "version.txt"
#define DRIVER_SERVICE "xhunter1"
#define DRIVER_SYS     "xhunter1.sys"
#define CURRENT_VER    "1.0"
#define APP_TITLE      "FN Cheat Launcher v" CURRENT_VER

// ============================================================
// Window controls
// ============================================================
#define IDC_STATUS     1001
#define IDC_PROGRESS   1002
#define IDC_VERSION    1003
#define IDC_LAUNCH     1004
#define IDC_UPDATE     1005
#define IDC_WEBLINK    1006

// ============================================================
// Globals
// ============================================================
HWND g_hWnd = NULL;
HWND g_hStatus = NULL;
HWND g_hProgress = NULL;
HWND g_hVersion = NULL;
HWND g_hLaunch = NULL;
HWND g_hUpdate = NULL;
HWND g_hWebLink = NULL;

char g_localVer[32] = "0";
char g_latestVer[32] = "0";
char g_downloadUrl[512] = "";
bool g_ready = false;
bool g_updateAvail = false;
bool g_isAdmin = false;

// ============================================================
// Helpers
// ============================================================
bool IsAdmin()
{
    BOOL isElevated = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION te = {};
        DWORD sz = sizeof(te);
        if (GetTokenInformation(hToken, TokenElevation, &te, sz, &sz))
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

// Fetch URL content into string using WinHTTP
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
            NULL, NULL, NULL,
            WINHTTP_FLAG_SECURE);
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

// Simple JSON field extraction
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

// Unzip using Shell COM (native Windows, no deps)
bool UnzipTo(const char* zipPath, const char* destPath)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool ok = false;

    wchar_t wzp[MAX_PATH], wdp[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, zipPath, -1, wzp, MAX_PATH);
    MultiByteToWideChar(CP_UTF8, 0, destPath, -1, wdp, MAX_PATH);

    // Ensure destination exists
    SHCreateDirectoryExW(NULL, wdp, NULL);

    // Use Shell COM to unzip
    IShellDispatch* pShell = NULL;
    HRESULT hr = CoCreateInstance(CLSID_Shell, NULL, CLSCTX_INPROC_SERVER,
        IID_IShellDispatch, (void**)&pShell);
    if (SUCCEEDED(hr) && pShell) {
        VARIANT vDir;
        VariantInit(&vDir);
        vDir.vt = VT_BSTR;
        vDir.bstrVal = SysAllocString(wzp);
        Folder* pFolder = NULL;
        hr = pShell->NameSpace(vDir, &pFolder);
        if (SUCCEEDED(hr) && pFolder) {
            FolderItems* pItems = NULL;
            pFolder->Items(&pItems);
            if (pItems) {
                VARIANT vDest;
                VariantInit(&vDest);
                vDest.vt = VT_BSTR;
                vDest.bstrVal = SysAllocString(wdp);
                Folder* pDest = NULL;
                pShell->NameSpace(vDest, &pDest);
                if (pDest) {
                    VARIANT vSrc, vOpt;
                    VariantInit(&vSrc);
                    vSrc.vt = VT_DISPATCH;
                    vSrc.pdispVal = pItems;
                    VariantInit(&vOpt);
                    vOpt.vt = VT_I4;
                    vOpt.lVal = 0x14; // no UI + yes to all
                    pDest->CopyHere(vSrc, vOpt);
                    ok = true;
                    pDest->Release();
                }
                VariantClear(&vDest);
                pItems->Release();
            }
            pFolder->Release();
        }
        VariantClear(&vDir);
        pShell->Release();
    }

    CoUninitialize();
    return ok;
}

// ============================================================
// UI Update helpers (called from worker thread)
// ============================================================
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
    sprintf_s(buf, "Local: %s  |  Latest: %s", local, latest);
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

// ============================================================
// Driver management
// ============================================================
bool InstallDriver()
{
    std::string sysPath = GetLocalPath(DRIVER_SYS);
    if (!FileExists(sysPath.c_str())) {
        SetStatus("Driver file not found");
        return false;
    }

    // Check if service exists
    char buf[512];
    sprintf_s(buf, "sc query " DRIVER_SERVICE);
    if (RunCmd(buf)) {
        // Service exists, just try to start
        sprintf_s(buf, "sc start " DRIVER_SERVICE);
        RunCmd(buf, false);
        return true;
    }

    // Create service
    sprintf_s(buf, "sc create " DRIVER_SERVICE " binPath= \"%s\" type= kernel",
        sysPath.c_str());
    if (!RunCmd(buf)) {
        SetStatus("Failed to create driver service");
        return false;
    }

    // Start service
    sprintf_s(buf, "sc start " DRIVER_SERVICE);
    RunCmd(buf, false);
    return true;
}

// ============================================================
// Worker thread
// ============================================================
void WorkerThread()
{
    SetStatus("Initializing...");
    std::string localDir = GetLocalPath("");
    std::string verPath = GetLocalPath(VERSION_FILE);

    // Create directory
    SHCreateDirectoryExA(NULL, localDir.c_str(), NULL);

    // Read local version
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

    // Check GitHub for latest version
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
                // Write version file
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

    // Install driver
    SetStatus("Starting driver...");
    InstallDriver();
    Sleep(300);

    SetProgress(100);
    g_ready = true;
    SetStatus("Ready! Click Launch to start.");
    EnableLaunch(true);
    ShowUpdateBtn(g_updateAvail);
}

// ============================================================
// Window Procedure
// ============================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_LAUNCH) {
            // Launch the cheat
            std::string exePath = GetLocalPath(LAUNCHER_EXE);
            if (FileExists(exePath.c_str())) {
                ShellExecuteA(NULL, "open", exePath.c_str(), NULL,
                    GetLocalPath("").c_str(), SW_SHOW);
            } else {
                SetStatus("Cheat not found. Run update first.");
            }
        }
        if (id == IDC_UPDATE) {
            g_ready = false;
            EnableLaunch(false);
            ShowUpdateBtn(false);
            SetProgress(0);
            std::thread(WorkerThread).detach();
        }
        if (id == IDC_WEBLINK) {
            char url[256];
            sprintf_s(url, "https://github.com/%s/%s/releases", GITHUB_USER, GITHUB_REPO);
            ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOW);
        }
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// ============================================================
// WinMain
// ============================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow)
{
    // Check admin
    if (!IsAdmin()) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        ShellExecuteA(NULL, "runas", exePath, NULL, NULL, SW_SHOW);
        return 0;
    }
    g_isAdmin = true;

    // Init common controls
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icex);

    // Register window class
    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "FNLauncher";
    RegisterClassExA(&wc);

    // Create window
    g_hWnd = CreateWindowExA(0, "FNLauncher", APP_TITLE,
        WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        100, 100, 460, 240,
        NULL, NULL, hInst, NULL);
    if (!g_hWnd) return 1;

    // Center on screen
    RECT rc;
    GetWindowRect(g_hWnd, &rc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(g_hWnd, NULL, (sw - 440) / 2, (sh - 210) / 2, 440, 210, SWP_NOZORDER);

    // Create controls
    int y = 20;
    g_hStatus = CreateWindowExA(0, "STATIC", "Initializing...",
        WS_CHILD | WS_VISIBLE,
        20, y, 400, 20, g_hWnd, NULL, hInst, NULL);
    y += 30;

    g_hProgress = CreateWindowExA(0, PROGRESS_CLASSA, NULL,
        WS_CHILD | WS_VISIBLE,
        20, y, 400, 22, g_hWnd, NULL, hInst, NULL);
    SendMessage(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    y += 35;

    g_hVersion = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE,
        20, y, 400, 18, g_hWnd, NULL, hInst, NULL);
    y += 30;

    g_hLaunch = CreateWindowExA(0, "BUTTON", "Launch Game",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        20, y, 130, 36, g_hWnd, (HMENU)IDC_LAUNCH, hInst, NULL);
    EnableWindow(g_hLaunch, FALSE);

    g_hUpdate = CreateWindowExA(0, "BUTTON", "Update Available",
        WS_CHILD | BS_PUSHBUTTON,
        165, y, 130, 36, g_hWnd, (HMENU)IDC_UPDATE, hInst, NULL);
    ShowWindow(g_hUpdate, SW_HIDE);

    g_hWebLink = CreateWindowExA(0, "BUTTON", "GitHub",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        310, y, 110, 36, g_hWnd, (HMENU)IDC_WEBLINK, hInst, NULL);
    y += 48;

    // Footer text
    char footer[128];
    sprintf_s(footer, "FN Cheat Launcher - github.com/%s/%s", GITHUB_USER, GITHUB_REPO);
    HWND hFooter = CreateWindowExA(0, "STATIC", footer,
        WS_CHILD | WS_VISIBLE,
        20, y, 400, 16, g_hWnd, NULL, hInst, NULL);
    (void)hFooter;

    // Start worker in background
    std::thread(WorkerThread).detach();

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
