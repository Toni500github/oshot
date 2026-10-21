#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <csignal>
#include <cstring>
#include "tray.hpp"

using namespace Tray;

#define WM_TRAY_CALLBACK_MESSAGE (WM_USER + 1)
#define WM_TRAY_QUIT_SIGNAL (WM_USER + 2)
#define WC_TRAY_CLASS_NAME "TRAY"
#define ID_TRAY_FIRST 1000

static WNDCLASSEX wc;
static NOTIFYICONDATA nid;
static HWND hwnd;
static HMENU hmenu = NULL;
static UINT id = ID_TRAY_FIRST;
static std::function<void(int)> hotkeyCallback;
static std::function<void()> quitSignalCallback;
static std::function<void()> iconClickCallback;
// Broadcast when Explorer (re)starts, the icon must be added again or it's gone for good
static UINT WM_TASKBAR_CREATED = 0;
static bool exited = false;

static LRESULT CALLBACK _tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	if (WM_TASKBAR_CREATED != 0 && msg == WM_TASKBAR_CREATED)
	{
		if (!exited)
			Shell_NotifyIcon(NIM_ADD, &nid);
		return 0;
	}

	switch (msg)
	{
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_HOTKEY:
		if (hotkeyCallback)
			hotkeyCallback((int)wparam);
		return 0;
	case WM_TRAY_QUIT_SIGNAL:
		if (quitSignalCallback)
			quitSignalCallback();
		return 0;
	case WM_TRAY_CALLBACK_MESSAGE:
		if (lparam == WM_LBUTTONUP && iconClickCallback)
		{
			// Like opening the menu: gives the foreground to whatever the callback shows
			SetForegroundWindow(hwnd);
			iconClickCallback();
			return 0;
		}
		if (lparam == WM_LBUTTONUP || lparam == WM_RBUTTONUP)
		{
			POINT p;
			GetCursorPos(&p);
			SetForegroundWindow(hwnd);
			WORD cmd = TrackPopupMenu(hmenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, hwnd, NULL);
			// Without this the menu doesn't close when clicking elsewhere the
			// second time it's opened (KB135788)
			PostMessage(hwnd, WM_NULL, 0, 0);
			SendMessage(hwnd, WM_COMMAND, cmd, 0);
			return 0;
		}
		break;
	case WM_COMMAND:
		if (wparam >= ID_TRAY_FIRST)
		{
			MENUITEMINFO item;
			item.cbSize = sizeof(MENUITEMINFO);
			item.fMask =  MIIM_ID | MIIM_DATA;

			if (GetMenuItemInfo(hmenu, wparam, FALSE, &item))
			{
				TrayMenu* clickedMenuItem = (TrayMenu*)item.dwItemData;
				if (clickedMenuItem != nullptr && clickedMenuItem->onClicked != nullptr)
					clickedMenuItem->onClicked(clickedMenuItem);
			}
			return 0;
		}
		break;
	}
	return DefWindowProc(hwnd, msg, wparam, lparam);
}

static HMENU _tray_menu(std::vector<TrayMenu*> menuItems)
{
	HMENU hmenu = CreatePopupMenu();
	for (TrayMenu* tm : menuItems)
	{
		if (tm->text == "-") 
			InsertMenu(hmenu, id, MF_SEPARATOR, TRUE, "");
		else
		{
			MENUITEMINFO item;
			memset(&item, 0, sizeof(item));
			item.cbSize = sizeof(MENUITEMINFO);
			item.fMask = MIIM_ID | MIIM_TYPE | MIIM_STATE | MIIM_DATA;
			item.fType = 0;
			item.fState = 0;

			if (tm->subMenu.size() > 0) 
			{
				item.fMask = item.fMask | MIIM_SUBMENU;
				item.hSubMenu = _tray_menu(tm->subMenu);
			}

			if (!tm->isEnabled)
				item.fState |= MFS_DISABLED;

			if (tm->isChecked)
				item.fState |= MFS_CHECKED;

			item.wID = id;
			item.dwTypeData = (LPSTR)tm->text.c_str();
			item.dwItemData = (ULONG_PTR)tm;

			InsertMenuItem(hmenu, id, TRUE, &item);
		}

		id++;
	}

	return hmenu;
}

bool TrayMaker::Initialize(TrayIcon *toInitialize)
{
	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.lpfnWndProc = _tray_wnd_proc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = WC_TRAY_CLASS_NAME;
	if (!RegisterClassEx(&wc))
		return false;

	WM_TASKBAR_CREATED = RegisterWindowMessage("TaskbarCreated");

	hwnd = CreateWindowEx(0, WC_TRAY_CLASS_NAME, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	if (hwnd == NULL)
		return false;

	UpdateWindow(hwnd);

	memset(&nid, 0, sizeof(nid));
	nid.cbSize = sizeof(NOTIFYICONDATA);
	nid.hWnd = hwnd;
	nid.uID = 0;
	nid.uFlags = NIF_ICON | NIF_MESSAGE;
	nid.uCallbackMessage = WM_TRAY_CALLBACK_MESSAGE;
	Shell_NotifyIcon(NIM_ADD, &nid);

	trayIcon = toInitialize;
	Update();

	return true;
}

bool TrayMaker::Loop(bool blocking)
{
	MSG msg = {};
	// No hwnd filter: WM_QUIT and other thread messages have no window
	if (blocking)
	{
		const BOOL ret = GetMessage(&msg, NULL, 0, 0);
		if (ret == 0 || ret == -1)
			return false;
	}
	else if (!PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
	{
		return true;
	}

	if (msg.message == WM_QUIT)
		return false;

	TranslateMessage(&msg);
	DispatchMessage(&msg);
	return true;
}

void TrayMaker::Update()
{
	HMENU prevmenu = hmenu;
	id = ID_TRAY_FIRST;
	hmenu = _tray_menu(trayIcon->menu);
	SendMessage(hwnd, WM_INITMENUPOPUP, (WPARAM)hmenu, 0);
	iconClickCallback = trayIcon->onClicked;

	HICON icon = NULL;
	ExtractIconEx(trayIcon->iconFileIco.c_str(), 0, NULL, &icon, 1);
	if (nid.hIcon) 
		DestroyIcon(nid.hIcon);
	nid.hIcon = icon;

	if(trayIcon->tooltip.length() > 0) 
	{
		strncpy(nid.szTip, trayIcon->tooltip.c_str(), sizeof(nid.szTip) - 1);
		nid.szTip[sizeof(nid.szTip) - 1] = '\0';
		nid.uFlags |= NIF_TIP;
	}

	Shell_NotifyIcon(NIM_MODIFY, &nid);

	if (prevmenu != NULL)
		DestroyMenu(prevmenu);
}

void* Tray::GetNativeHandle()
{
	return hwnd;
}

void Tray::SetHotkeyCallback(std::function<void(int)> cb)
{
	hotkeyCallback = std::move(cb);
}

void Tray::Wake()
{
	if (hwnd)
		PostMessage(hwnd, WM_NULL, 0, 0);
}

// The CRT runs SIGINT/SIGTERM handlers on a separate thread, so just hand it
// over to the tray thread. The handler is reset after each delivery.
static void _tray_on_signal(int sig)
{
	signal(sig, _tray_on_signal);
	if (hwnd)
		PostMessage(hwnd, WM_TRAY_QUIT_SIGNAL, 0, 0);
}

void Tray::SetQuitSignalCallback(std::function<void()> cb)
{
	quitSignalCallback = std::move(cb);
	signal(SIGINT, _tray_on_signal);
	signal(SIGTERM, _tray_on_signal);
}

void Tray::SetReopenCallback(std::function<void()>) {}

// Idempotent and callable from any thread: the window is closed through its
// own message loop (WM_CLOSE -> DestroyWindow -> WM_DESTROY -> WM_QUIT),
// since PostQuitMessage() only reaches the calling thread's queue.
void TrayMaker::Exit()
{
	if (exited)
		return;
	exited = true;

	Shell_NotifyIcon(NIM_DELETE, &nid);
	if (nid.hIcon != 0)
	{
		DestroyIcon(nid.hIcon);
		nid.hIcon = 0;
	}
	if (hmenu != 0)
	{
		DestroyMenu(hmenu);
		hmenu = 0;
	}

	if (hwnd)
		PostMessage(hwnd, WM_CLOSE, 0, 0);
}
#endif
