// slideshow.c - fullscreen slideshow with aspect ratio and continuous arrow navigation
// Pure Windows API + dynamic GDI+, no extra SDK headers required
// Compile: tcc slideshow.c -lgdi32 -lole32 -loleaut32 -lcomdlg32
// Copyright (c) 2025 Shpati Koleka. MIT License. 

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_FILES 10000

#define INI_FILE L"slideshow.ini"
WCHAR folderPath[MAX_PATH] = L".";
WCHAR duration[16] = L"10";

WCHAR fileList[MAX_FILES][MAX_PATH];
int fileCount = 0;
int currentIndex = 0;
int durationSec;
ULONG_PTR gdiplusToken;
HWND hwndMain;
UINT_PTR timerId;
UINT_PTR keyTimerId = 0;
int keyDirection = 0; // -1 = left, 1 = right
HANDLE hDirChangeThread = NULL;
HANDLE hStopEvent = NULL;
WCHAR watchedPath[MAX_PATH];

// ---------------- Minimal GDI+ declarations ----------------
typedef float REAL;  // FIX: TinyCC doesn't know REAL
typedef struct {
    UINT32 GdiplusVersion;
    void* DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} GdiplusStartupInput;

typedef enum { UnitPixel = 2 } GpUnit;
typedef void GpImage;
typedef void GpGraphics;
typedef unsigned long GpStatus;
typedef ULONG_PTR ARGB;

GpStatus (WINAPI *GdiplusStartup)(ULONG_PTR*, const GdiplusStartupInput*, void*);
GpStatus (WINAPI *GdiplusShutdown)(ULONG_PTR);
GpStatus (WINAPI *GdipCreateFromHDC)(HDC, GpGraphics**);
GpStatus (WINAPI *GdipDeleteGraphics)(GpGraphics*);
GpStatus (WINAPI *GdipDrawImageRect)(GpGraphics*, GpImage*, REAL, REAL, REAL, REAL);
GpStatus (WINAPI *GdipLoadImageFromFile)(const WCHAR*, GpImage**);
GpStatus (WINAPI *GdipDisposeImage)(GpImage*);
GpStatus (WINAPI *GdipGetImageWidth)(GpImage*, UINT*);
GpStatus (WINAPI *GdipGetImageHeight)(GpImage*, UINT*);

void LoadGDIPlus() {
    HMODULE h = LoadLibraryW(L"gdiplus.dll");
    #define LOAD(fn) fn = (void*)GetProcAddress(h, #fn)
    LOAD(GdiplusStartup);
    LOAD(GdiplusShutdown);
    LOAD(GdipCreateFromHDC);
    LOAD(GdipDeleteGraphics);
    LOAD(GdipDrawImageRect);
    LOAD(GdipLoadImageFromFile);
    LOAD(GdipDisposeImage);
    LOAD(GdipGetImageWidth);
    LOAD(GdipGetImageHeight);
    #undef LOAD
}

// ---------------- Utility functions ----------------
WCHAR* FindExtensionW(WCHAR *fname) {
    WCHAR *dot = NULL;
    while (*fname) {
        if (*fname == L'.') dot = fname;
        fname++;
    }
    return dot ? dot : fname;
}

BOOL IsImageFile(const WCHAR *name) {
    WCHAR ext[16];
    lstrcpyW(ext, FindExtensionW((WCHAR*)name));
    CharLowerW(ext);
    return (lstrcmpW(ext, L".jpg") == 0 ||
            lstrcmpW(ext, L".jpeg") == 0 ||
            lstrcmpW(ext, L".png") == 0 ||
            lstrcmpW(ext, L".bmp") == 0 ||
            lstrcmpW(ext, L".gif") == 0);
}

void AddFile(const WCHAR *path) {
    if (fileCount < MAX_FILES) {
        lstrcpyW(fileList[fileCount++], path);
    }
}

void ScanFolder(const WCHAR *path) {
    WIN32_FIND_DATAW fd;
    WCHAR search[MAX_PATH];
    wsprintfW(search, L"%s\\*", path);
    HANDLE h = FindFirstFileW(search, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (lstrcmpW(fd.cFileName, L".") == 0 || lstrcmpW(fd.cFileName, L"..") == 0) continue;
            WCHAR full[MAX_PATH];
            wsprintfW(full, L"%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                ScanFolder(full);
            } else if (IsImageFile(fd.cFileName)) {
                AddFile(full);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void CreateDefaultINI() {
    WCHAR cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);  // Wide string version
    HANDLE hFile = CreateFileW(L"slideshow.ini", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WCHAR buf[512];
        // Format wide string with wide cwd
        wsprintfW(buf, L"[SETTINGS]\r\nPATH=%s\r\nDURATION=%s\r\n", cwd, duration);
        // WriteFile expects number of bytes, so multiply length by sizeof(WCHAR)
        WriteFile(hFile, buf, lstrlenW(buf) * sizeof(WCHAR), &written, NULL);
        FlushFileBuffers(hFile);
        CloseHandle(hFile);
    }
}

void LoadSettings() {
    WCHAR absIniPath[MAX_PATH];
    GetFullPathNameW(L"slideshow.ini", MAX_PATH, absIniPath, NULL);

    if (GetFileAttributesW(absIniPath) == INVALID_FILE_ATTRIBUTES) {
        CreateDefaultINI();
    }

    WritePrivateProfileStringW(NULL, NULL, NULL, absIniPath);

    GetPrivateProfileStringW(L"SETTINGS", L"PATH", folderPath, folderPath, MAX_PATH, absIniPath);
    GetPrivateProfileStringW(L"SETTINGS", L"DURATION", duration, duration, 16, absIniPath);

    durationSec = _wtoi(duration);
    if (durationSec <= 0) durationSec = 5;
}

// ---------------- Image drawing ----------------
void ShowImage(HDC hdc, const WCHAR *filename, RECT *rc) {
    GpImage *img = NULL;
    if (GdipLoadImageFromFile(filename, &img) != 0 || !img) return;

    UINT w, h;
    GdipGetImageWidth(img, &w);
    GdipGetImageHeight(img, &h);

    int scrW = rc->right - rc->left;
    int scrH = rc->bottom - rc->top;
    double imgRatio = (double)w / (double)h;
    double scrRatio = (double)scrW / (double)scrH;
    int drawW, drawH;
    if (imgRatio > scrRatio) {
        drawW = scrW;
        drawH = (int)(scrW / imgRatio);
    } else {
        drawH = scrH;
        drawW = (int)(scrH * imgRatio);
    }
    int offsetX = (scrW - drawW) / 2;
    int offsetY = (scrH - drawH) / 2;

    GpGraphics *g = NULL;
    GdipCreateFromHDC(hdc, &g);
    // Fill background with black
    HBRUSH brush = CreateSolidBrush(RGB(0,0,0));
    FillRect(hdc, rc, brush);
    DeleteObject(brush);
    GdipDrawImageRect(g, img, (REAL)offsetX, (REAL)offsetY, (REAL)drawW, (REAL)drawH);
    GdipDeleteGraphics(g);
    GdipDisposeImage(img);
}

void ShowCurrentImage(HWND hwnd) {
    InvalidateRect(hwnd, NULL, TRUE);
}

// ---------------- Window procedure ----------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // Create compatible memory DC and bitmap
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right - rc.left, rc.bottom - rc.top);
        HBITMAP oldBmp = SelectObject(memDC, memBmp);

        // Fill background black in memDC
        HBRUSH brush = CreateSolidBrush(RGB(0,0,0));
        FillRect(memDC, &rc, brush);
        DeleteObject(brush);

        // Draw the image onto memDC instead of hdc
        if (fileCount > 0) {
            ShowImage(memDC, fileList[currentIndex], &rc);
        }

        // Blit from memDC to screen hdc
        BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, memDC, 0, 0, SRCCOPY);

        // Cleanup
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
    } break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostQuitMessage(0);
        } else if (wParam == VK_RIGHT) {
            keyDirection = 1;
            currentIndex = (currentIndex + 1) % fileCount;
            ShowCurrentImage(hwnd);
            if (!keyTimerId) keyTimerId = SetTimer(hwnd, 2, 150, NULL);
        } else if (wParam == VK_LEFT) {
            keyDirection = -1;
            currentIndex = (currentIndex - 1 + fileCount) % fileCount;
            ShowCurrentImage(hwnd);
            if (!keyTimerId) keyTimerId = SetTimer(hwnd, 2, 150, NULL);
        }
        break;

    case WM_KEYUP:
        if ((wParam == VK_RIGHT && keyDirection == 1) ||
            (wParam == VK_LEFT && keyDirection == -1)) {
            keyDirection = 0;
            if (keyTimerId) {
                KillTimer(hwnd, 2);
                keyTimerId = 0;
            }
        }
        break;

    case WM_APP + 1:
        // Folder content changed: rescan folder, update file list
        fileCount = 0;
        ScanFolder(folderPath);
        if (fileCount == 0) currentIndex = 0;
        else if (currentIndex >= fileCount) currentIndex = fileCount - 1;
        InvalidateRect(hwnd, NULL, TRUE);
    break;

    case WM_TIMER:
        if (wParam == 1) { // auto-advance timer
            currentIndex = (currentIndex + 1) % fileCount;
            ShowCurrentImage(hwnd);
        } else if (wParam == 2 && keyDirection != 0) { // key repeat timer
            currentIndex = (currentIndex + keyDirection + fileCount) % fileCount;
            ShowCurrentImage(hwnd);
        }
        break;

    case WM_DESTROY:
        SetEvent(hStopEvent);
        if (hDirChangeThread) {
            WaitForSingleObject(hDirChangeThread, INFINITE);
            CloseHandle(hDirChangeThread);
        }
        if (hStopEvent) CloseHandle(hStopEvent);

        KillTimer(hwnd, timerId);
        if (keyTimerId) KillTimer(hwnd, 2);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

DWORD WINAPI DirectoryWatcherThread(LPVOID param) {
    HANDLE hDir = CreateFileW(watchedPath,
                             FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL,
                             OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                             NULL);
    if (hDir == INVALID_HANDLE_VALUE) return 1;

    char buffer[1024];
    DWORD bytesReturned;
    OVERLAPPED ol = {0};
    HANDLE events[2] = { hStopEvent, NULL };
    events[1] = CreateEvent(NULL, FALSE, FALSE, NULL);
    ol.hEvent = events[1];

    while (1) {
        if (!ReadDirectoryChangesW(hDir, buffer, sizeof(buffer), TRUE,
                                   FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                                   &bytesReturned, &ol, NULL)) {
            break;
        }

        DWORD waitStatus = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (waitStatus == WAIT_OBJECT_0) {
            // Stop event triggered
            break;
        }
        // Directory changed - notify main thread
        PostMessage(hwndMain, WM_APP + 1, 0, 0);
    }

    CloseHandle(events[1]);
    CloseHandle(hDir);
    return 0;
}

// ---------------- Entry point ----------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmd, int nShow) {
    LoadSettings();
    ScanFolder(folderPath);

    if (fileCount == 0) {
        MessageBoxW(NULL, L"No images found.", L"Slideshow", MB_ICONERROR);
        return 0;
    }

    lstrcpyW(watchedPath, folderPath);
    hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    hDirChangeThread = CreateThread(NULL, 0, DirectoryWatcherThread, NULL, 0, NULL);

    LoadGDIPlus();
    GdiplusStartupInput gdiplusStartupInput = {1, NULL, FALSE, FALSE};
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"SlideShowClass";
    RegisterClassW(&wc);

    hwndMain = CreateWindowExW(0, wc.lpszClassName, L"Slideshow",
                               WS_POPUP, 0, 0,
                               GetSystemMetrics(SM_CXSCREEN),
                               GetSystemMetrics(SM_CYSCREEN),
                               NULL, NULL, hInstance, NULL);

    ShowWindow(hwndMain, SW_SHOWMAXIMIZED);

    // Set or reset timer with current durationSec
    if (timerId) KillTimer(hwndMain, timerId);
    timerId = SetTimer(hwndMain, 1, durationSec * 1000, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    return 0;
}
