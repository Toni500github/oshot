#ifndef TRAY_H
#define TRAY_H

#include <string>
#include <vector>
#include <functional>

namespace Tray 
{
	struct TrayMenu
	{
		std::string text = "";
		bool isEnabled = true;
		bool isChecked = false;
		bool hasCheckbox = false;		// Only used with linux/libAppIndicator. Whether the entry even has a checkbox

		std::function<void(TrayMenu*)> onClicked = nullptr;
		std::vector<TrayMenu*> subMenu;
	};

	struct TrayIcon
	{
		std::string iconFilePng = "";	// OSX, Linux. File name of icon (same folder as execution dir)
		std::string iconFileIco = "";	// Windows. File name of icon (same folder as execution dir)
		std::string tooltip = "";		// Only used on Windows, hover tooltip

		std::vector<TrayMenu*> menu;
	};

	class TrayMaker
	{
	private:
		TrayIcon* trayIcon;

	public:
		bool Initialize(TrayIcon* toInitialize);
		bool Loop(bool blocking);
		void Update();
		void Exit();
	};

	static TrayMaker trayMaker;

	// oshot addition (Windows only): the tray window handle, used to register
	// global hotkeys whose WM_HOTKEY then arrives in the tray message loop
	void* GetNativeHandle();
	void SetHotkeyCallback(std::function<void(int)> cb);
}


#endif /* TRAY_H */
