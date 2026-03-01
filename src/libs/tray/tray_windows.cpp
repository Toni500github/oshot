#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include "tray.hpp"

using namespace Tray;

#define WM_TRAY_CALLBACK_MESSAGE (WM_USER + 1)
#define WC_TRAY_CLASS_NAME "TRAY"
#define ID_TRAY_FIRST 1000

static WNDCLASSEX wc;
static NOTIFYICONDATA nid;
static HWND hwnd;
static HMENU hmenu = NULL;
static UINT id = ID_TRAY_FIRST;

static LRESULT CALLBACK _tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_TRAY_CALLBACK_MESSAGE:
		if (lparam == WM_LBUTTONUP || lparam == WM_RBUTTONUP)
		{
			POINT p;
			GetCursorPos(&p);
			SetForegroundWindow(hwnd);
			WORD cmd = TrackPopupMenu(hmenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, hwnd, NULL);
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
	MSG msg;
	if (blocking)
		GetMessage(&msg, hwnd, 0, 0);
	else
		PeekMessage(&msg, hwnd, 0, 0, PM_REMOVE);
	
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

	HICON icon;
	ExtractIconEx(trayIcon->iconFileIco.c_str(), 0, NULL, &icon, 1);
	if (nid.hIcon) 
		DestroyIcon(nid.hIcon);
	nid.hIcon = icon;

	if(trayIcon->tooltip.length() > 0) 
	{
		strncpy(nid.szTip, trayIcon->tooltip.c_str(), sizeof(nid.szTip));
		nid.uFlags |= NIF_TIP;
	}

	Shell_NotifyIcon(NIM_MODIFY, &nid);

	if (prevmenu != NULL)
		DestroyMenu(prevmenu);
}

void TrayMaker::Exit()
{
	Shell_NotifyIcon(NIM_DELETE, &nid);
	if (nid.hIcon != 0)
		DestroyIcon(nid.hIcon);
	if (hmenu != 0)
		DestroyMenu(hmenu);

	PostQuitMessage(0);
	UnregisterClass(WC_TRAY_CLASS_NAME, GetModuleHandle(NULL));
}
#endif
