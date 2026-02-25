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
}


#endif /* TRAY_H */
