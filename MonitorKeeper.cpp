// MonitorKeeper.cpp 
//
// Author: Garr Godfrey
// License: MIT License
// License Summary: Free but author takes no responsibility.
//
//  Monitor Keeper is a simple app that runs as an application on the task bar that will restore windows
// to their original locations when a monitor because available. When a monitor is turned off, or HDMI is unplugged,
// Windows 7 and higher detect that and will rearrange all application windows onto the remaining monitor(s).
// If the monitor is reconnected, the applications stay on the single monitor, requiring the user to
// move them back manually. This application moves them back automatically.
//
// Limitations:
//		- Windows are only repositions when the number of monitors INCREASES. There is no way to specify a layout
//			to use on a single monitor, for example (although this would not be a difficult change)
//		- Support is limited to 5 monitors. This is arbitrary and done simply to limit the storage for each application.
//		- If application is run as a standard user, it cannot move any applications that are running as a privileged user.
//			If you run into this, you can run this program as administrator, perhaps using Task Scheduler to launch it at login.
//		- Window position is only saved while application is running. There is no persistent storage of position (say, between reboots)
//		- Windows will return to their state when the number of monitors was most recently seen. So, a window may go from minimize to
//			maximized or be a different size once the second (or third) monitor is plugged back in.
//
// DEMO:
//		To quickly test the functionality, go to Display Settings... in windows (right click on desktop), with two monitors, change
// the setting for "Multiple Displays" from "Extend These Displays" to "Duplicate These Displays".  Compare when running Monitor
// Keeper and when not.


#include "MonitorKeeper.h"


#define MAX_MONITORS 5
#define MIN_MONITORTORESTORE 2

#define LOGBUFFERSIZE  (32*1024)
#define MAX_LOADSTRING 100
// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name


//
// class representing the data we save for each top level window.
//
class SavedWindowData {
public:
    SavedWindowData() {
        m_wndClass[0] = '\0';
        m_hwnd = NULL;
        m_nUnusedCount = 1;
    }

    int					m_nUnusedCount;
    WINDOWPLACEMENT		m_windowPlacement[MAX_MONITORS - MIN_MONITORTORESTORE + 1];
    HWND				m_hwnd;
    TCHAR				m_wndClass[40];  // window class, for verification.

    BOOL SetData(HWND hwnd, int NumMonitors)
    {
        m_hwnd = hwnd;
        m_nUnusedCount = 0;
        RealGetWindowClass(hwnd, m_wndClass, sizeof(m_wndClass) / sizeof(TCHAR));

        if (NumMonitors < MIN_MONITORTORESTORE || NumMonitors > MAX_MONITORS) return false;  // too many monitors or not enought
        m_windowPlacement[NumMonitors - MIN_MONITORTORESTORE].length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(hwnd, &(m_windowPlacement[NumMonitors - MIN_MONITORTORESTORE]));
        return true;
    }

    void RestoreWindow(int NumMonitors)
    {
        TCHAR szTempClass[40];
        if (NumMonitors >= MIN_MONITORTORESTORE && NumMonitors <= MAX_MONITORS) {
            if (IsWindow(m_hwnd) &&
                m_windowPlacement[NumMonitors - MIN_MONITORTORESTORE].length == sizeof(WINDOWPLACEMENT)) {
                // verify window class
                RealGetWindowClass(m_hwnd, szTempClass, sizeof(szTempClass) / sizeof(TCHAR));
                if (lstrcmp(szTempClass, m_wndClass) == 0) {
                    WINDOWPLACEMENT * place = &(m_windowPlacement[NumMonitors - MIN_MONITORTORESTORE]);
                    // don't worry about "minimized position", it is a concept from Windows 3.0.
                    place->flags = WPF_ASYNCWINDOWPLACEMENT;

                    if (place->showCmd == SW_MAXIMIZE) {
                        // we need to treat this special, first restore it to the correct position,
                        // then maximize. Otherwise, it will just maximize it on the current screen
                        // and ingore the coordinates.
                        place->showCmd = SW_SHOWNOACTIVATE;
                        SetWindowPlacement(m_hwnd, &(m_windowPlacement[NumMonitors - MIN_MONITORTORESTORE]));
                        place->showCmd = SW_MAXIMIZE;
                    }
                    else if (place->showCmd == SW_MINIMIZE || place->showCmd == SW_SHOWMINIMIZED) {
                        place->showCmd = SW_SHOWMINNOACTIVE;
                    }
                    else if (place->showCmd == SW_NORMAL) {
                        place->showCmd = SW_SHOWNOACTIVATE;
                    }
                    SetWindowPlacement(m_hwnd, &(m_windowPlacement[NumMonitors - MIN_MONITORTORESTORE]));
                }
            }
        }
    }
};


//
// Other global information we need for our application in this class, as
// well as methods that operate on the saved data.
//
class InstanceData {
public:
    InstanceData() {
        _Hook = NULL;
#ifdef _DEBUG
        _LogInfo[0] = '\0';
#endif
        _WindowDataLength = 32;
        _NumMonitors = 1;
        _WindowData = new SavedWindowData[_WindowDataLength];
        _MainWnd = NULL;
        InChangingState = false;
    }

    ~InstanceData()
    {
        Shutdown();
    }

    void Shutdown() {
        if (_Hook != NULL) UnhookWinEvent(_Hook);
        _Hook = NULL;

        if (_WindowData != NULL) {
            delete[] _WindowData;
        }
    }

    static InstanceData  g_Instance;

    //
    // there is a primiative log window in debug mode.
    //
    void LogMessage(LPCTSTR str)
    {
#ifdef _DEBUG
        int len = lstrlen(_LogInfo);
        int newlen = lstrlen(str);
        if (len + newlen >= LOGBUFFERSIZE)
        {
            len = 0;
        }
        lstrcpy(_LogInfo + len, str);
        if (_MainWnd != NULL) {
            SetScrollPos(_MainWnd, SB_VERT, 10000, true);
            InvalidateRect(_MainWnd, NULL, TRUE);
        }
#endif
    }

    //
    // we are not notified on a window destroy, so we marked windows
    // if we haven't seen them. If we don't see it 3 times in a row,
    // we will reuse it's position in the array.
    //
    void TagWindowsUnused()
    {
        int i;
        for (i = 0; i < _WindowDataLength; i++)
        {
            // don't count to the point we rollover
            if (_WindowData[i].m_hwnd != NULL && _WindowData[i].m_nUnusedCount < 100)
            {
                _WindowData[i].m_nUnusedCount++;
            }
        }
    }

    //
    // restore all the top level windows
    //
    void RestoreWindowPositions(int monitors)
    {
        int i;
        for (i = 0; i < _WindowDataLength; i++)
        {
            // don't count to the point we rollover
            if (_WindowData[i].m_hwnd != NULL && _WindowData[i].m_nUnusedCount <= 2)
            {
                _WindowData[i].RestoreWindow(monitors);
            }
        }

    }

    //
    // find slow for the window we found.
    SavedWindowData * FindWindowSlot(HWND hwnd)
    {
        //
        // find existing HWND in array
        int i;
        for (i = 0; i < _WindowDataLength; i++)
        {
            if (_WindowData[i].m_hwnd == hwnd) {
                return &(_WindowData[i]);
            }
        }

        //
        // find an unused slot.
        for (i = 0; i < _WindowDataLength; i++)
        {
            if (_WindowData[i].m_hwnd == NULL ||
                _WindowData[i].m_nUnusedCount > 2) {
                return &(_WindowData[i]);
            }
        }

        //
        // hmmm, all used, need to reallocate.
        i = _WindowDataLength;
        int newlength = _WindowDataLength + 32;
        SavedWindowData * newdata = new SavedWindowData[newlength];
        for (i = 0; i < _WindowDataLength; i++) {
            newdata[i] = _WindowData[i];
        }
        delete[] _WindowData;
        _WindowData = newdata;
        _WindowDataLength = newlength;

        // i === old _WindowDataLength
        return &(_WindowData[i]);
    }

    HWINEVENTHOOK		_Hook;
    SavedWindowData * _WindowData;
    int					_WindowDataLength;
    int					_NumMonitors;
    HWND				_MainWnd;
    BOOL				InChangingState;
#ifdef _DEBUG
    TCHAR				_LogInfo[LOGBUFFERSIZE];
#endif
};


/*static*/ InstanceData  InstanceData::g_Instance;

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // TODO: Place code here.

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_MONITORKEEPER, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_MONITORKEEPER));

    MSG msg;

    // Main message loop:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int)msg.wParam;
}



//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MONITORKEEPER));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_MONITORKEEPER);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}


LPCTSTR TranslateShowCommand(int nShowCmd)
{
    switch (nShowCmd)
    {
    case SW_RESTORE:
    case SW_SHOWNORMAL:
        return _T("SW_SHOWNORMAL");
    case SW_MAXIMIZE:
        return _T("SW_MAXIMIZE");
    case SW_MINIMIZE:
    case SW_SHOWMINIMIZED:
        return _T("SW_MINIMIZE");
    case SW_SHOWNOACTIVATE:
        return _T("SW_SHOWNOACTIVATE");
    case SW_SHOWMINNOACTIVE:
        return _T("SW_SHOWMINNOACTIVE");
    default:
        return _T("Unknown");
    }
}

//
// Called by EnumDesktopWindows whenever a window changes state.
// This will capture a lot of events.
//
BOOL CALLBACK SaveWindowsCallback(
    _In_ HWND   hwnd,
    _In_ LPARAM lParam
)
{
    int monitors = (int)lParam;

    //
    // only track windows that are visible, don't have a parent, 
    // have at least one style that is in the OVERLAPPEDWINDOW style and
    // do not have the WS_EX_NOACTIVE style.
    //
    // we include WS_EX_TOOLBAR because they sometimes are useful and get
    // moved as well.
    //
    if (IsWindowVisible(hwnd) &&
        GetParent(hwnd) == NULL) {
        DWORD dwStyle = (DWORD)GetWindowLong(hwnd, GWL_STYLE);
        DWORD dwExStyle = (DWORD)GetWindowLong(hwnd, GWL_EXSTYLE);
        if (((dwStyle & (WS_OVERLAPPEDWINDOW)) != 0 ||
            (dwExStyle & WS_EX_APPWINDOW) != 0)
            &&
            (dwExStyle & (WS_EX_NOACTIVATE)) == 0)
        {
            SavedWindowData * pData = InstanceData::g_Instance.FindWindowSlot(hwnd);
            if (pData->SetData(hwnd, monitors))
            {
                TCHAR sz[128];
                wsprintf(sz, _T("Save Position for %s, monitors %d, x=%d, y=%d, show=%s\n"),
                    pData->m_wndClass, monitors, pData->m_windowPlacement[monitors - MIN_MONITORTORESTORE].rcNormalPosition.left,
                    pData->m_windowPlacement[monitors - MIN_MONITORTORESTORE].rcNormalPosition.top,
                    TranslateShowCommand(pData->m_windowPlacement[monitors - MIN_MONITORTORESTORE].showCmd));
                InstanceData::g_Instance.LogMessage(sz);
            }
        }
    }
    return true;
}

//
// Process when the number of monitors changes. If we have changed monitor count
// we attempt to restore /
//
void ProcessMonitors()
{
    int monitors = GetSystemMetrics(SM_CMONITORS);
    if (monitors > 1 && InstanceData::g_Instance._NumMonitors != monitors)
    {
        // restore windows.
        InstanceData::g_Instance.RestoreWindowPositions(monitors);
    }
    InstanceData::g_Instance._NumMonitors = monitors;
    InstanceData::g_Instance.InChangingState = false;
}


//
// called by hook for window changes
//
void ProcessDesktopWindows()
{
    TCHAR sz[128];
    int monitors = GetSystemMetrics(SM_CMONITORS);
    if (monitors != InstanceData::g_Instance._NumMonitors)
    {
        // we haven't completed our switch to change of monitors yet.
        // so don't save positions until we've repositioned things.
        return;
    }
    wsprintf(sz, _T("Monitors: %d\n"), monitors);
    InstanceData::g_Instance.TagWindowsUnused();
    InstanceData::g_Instance.LogMessage(sz);
    EnumDesktopWindows(NULL, SaveWindowsCallback, monitors);
}


//
// save windows positions after a slight delay
VOID CALLBACK SaveTimerCallback(
    _In_ HWND     hwnd,
    _In_ UINT     uMsg,
    _In_ UINT_PTR idEvent,
    _In_ DWORD    dwTime
)
{
    ProcessDesktopWindows();
    KillTimer(hwnd, idEvent);
}



//
// Our window hook, grabbing the event when the active window changes.
VOID CALLBACK WinEventProcCallback(HWINEVENTHOOK hWinEventHook, DWORD dwEvent, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime)
{
    if (InstanceData::g_Instance.InChangingState) return;
    if (hwnd != NULL &&
        (dwEvent == EVENT_SYSTEM_MOVESIZEEND ||
            dwEvent == EVENT_OBJECT_LOCATIONCHANGE))
    {
        // use our HWND so that this timer get replaced each time we call SetTimer.
        SetTimer(InstanceData::g_Instance._MainWnd, 2, 200, SaveTimerCallback);
    }
}


//
// reposition windows after a slight delay
VOID CALLBACK TimerCallback(
    _In_ HWND     hwnd,
    _In_ UINT     uMsg,
    _In_ UINT_PTR idEvent,
    _In_ DWORD    dwTime
)
{
    ProcessMonitors();
    KillTimer(hwnd, idEvent);
}



HWINEVENTHOOK HookDisplayChange()
{
    return SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, NULL, WinEventProcCallback, 0, 0, WINEVENT_OUTOFCONTEXT);
}


//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance; // Store instance handle in our global variable

    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW & ~WS_VISIBLE,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

    ProcessDesktopWindows();
    InstanceData::g_Instance._NumMonitors = GetSystemMetrics(SM_CMONITORS);

    if (!hWnd)
    {
        return FALSE;
    }

    InstanceData::g_Instance._MainWnd = hWnd;
    InstanceData::g_Instance._Hook = HookDisplayChange();

    SetScrollRange(hWnd, SB_VERT, 0, 10000, false);

    NOTIFYICONDATA icon;
    //
    // create notify icon
    icon.cbSize = sizeof(icon);
    icon.hWnd = hWnd;
    icon.uID = 1;
    icon.szTip[0] = '\0';
    icon.uFlags = NIF_ICON | NIF_MESSAGE | NIM_SETVERSION;
    icon.uCallbackMessage = WM_USER + 100;
    icon.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MONITORKEEPER));
    icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIcon(NIM_ADD, &icon);

    return TRUE;
}




//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_DISPLAYCHANGE:
        InstanceData::g_Instance.LogMessage(_T("WM_DISPLAYCHANGE\n"));
        InstanceData::g_Instance.InChangingState = true;
        SetTimer(hWnd, 99, 500, TimerCallback);
        break;
    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        // Parse the menu selections:
        switch (wmId)
        {
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        case IDM_SHOWWINDOW:
            ShowWindow(hWnd, SW_RESTORE);
            UpdateWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
    break;
    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        return false;
    case (WM_USER + 100):
        // notify icon
    {
        //
        // pop up our context menu on the notify icon.
        //
        UINT nMsg = LOWORD(lParam);
        if (nMsg == WM_CONTEXTMENU || nMsg == WM_RBUTTONUP) {

            int x = LOWORD(wParam);
            int y = HIWORD(wParam);
            HMENU menu = GetMenu(hWnd);
            menu = GetSubMenu(menu, 1);
            RECT r;
            NOTIFYICONIDENTIFIER id;
            id.cbSize = sizeof(id);
            id.hWnd = hWnd;
            id.uID = 1;
            id.guidItem = GUID_NULL;
            Shell_NotifyIconGetRect(&id, &r);
            x += r.left;
            y += r.top;
            TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, x, y, 0, hWnd, NULL);
        }
    }
    break;
    case WM_VSCROLL:
    {
        UINT cmd = LOWORD(wParam);
        int pos = GetScrollPos(hWnd, SB_VERT);
        if (cmd == SB_BOTTOM) {
            pos = 10000;
        }
        else if (cmd == SB_TOP) {
            pos += 0;
        }
        else if (cmd == SB_PAGEDOWN) {
            pos += 1000;
        }
        else if (cmd == SB_PAGEUP) {
            pos -= 1000;
        }
        else if (cmd == SB_THUMBPOSITION) {
            pos = HIWORD(wParam);
        }
        else if (cmd == SB_THUMBTRACK) {
            pos = HIWORD(wParam);
        }
        else {
            break;
        }
        if (pos < 0) pos = 0;
        if (pos > 10000) pos = 10000;
        SetScrollPos(hWnd, SB_VERT, pos, true);
        InvalidateRect(hWnd, NULL, true);
    }
    break;
    case WM_PAINT:
    {
#ifdef _DEBUG
        PAINTSTRUCT ps;
        RECT r, r2;
        HDC hdc = BeginPaint(hWnd, &ps);
        GetClientRect(hWnd, &r);
        r2 = r;
        DrawText(hdc, InstanceData::g_Instance._LogInfo,
            -1, &r2, DT_LEFT | DT_NOPREFIX | DT_WORDBREAK | DT_CALCRECT);

        //
        // get scroll position.
        //
        int pos = GetScrollPos(hWnd, SB_VERT);
        pos = pos * (r2.bottom - r.bottom) / 10000;

        r.top = r.top - pos;
        DrawText(hdc, InstanceData::g_Instance._LogInfo,
            -1, &r, DT_LEFT | DT_NOPREFIX | DT_WORDBREAK);

        EndPaint(hWnd, &ps);
#endif
    }
    break;
    case WM_DESTROY:
    {
        // destory our notify icon.
        NOTIFYICONDATA icon;
        icon.cbSize = sizeof(icon);
        icon.hWnd = hWnd;
        icon.uID = 1;
        Shell_NotifyIcon(NIM_DELETE, &icon);
        PostQuitMessage(0);
    }
    break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
