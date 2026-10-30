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

		// oshot: run on a primary (left) click on the icon, the menu then opens
		// with a secondary (right) click. On Linux StatusNotifier hosts (KDE,
		// XFCE, GNOME's extension) this needs Ayatana AppIndicator 0.6+, with
		// older versions any click opens the menu.
		std::function<void()> onClicked = nullptr;
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

	// oshot additions:

	// Windows only: the tray window handle, used to register global hotkeys
	// whose WM_HOTKEY then arrives in the tray message loop
	void* GetNativeHandle();
	void SetHotkeyCallback(std::function<void(int)> cb);

	// Make a blocking Loop() return so the caller can re-check its state.
	// Callable from any thread (on macOS: main thread only).
	void Wake();

	// Run `cb` on the tray thread when SIGINT/SIGTERM (and SIGHUP on unix) arrive,
	// instead of inside the signal handler. Call after Initialize().
	void SetQuitSignalCallback(std::function<void()> cb);

	// macOS only: run `cb` when the app is opened again while already running
	// (Finder, Launchpad, Spotlight, `open -a`). Other code (GLFW's init) can
	// replace the handler, so call this again after such code runs.
	void SetReopenCallback(std::function<void()> cb);

	// Whether the panel holding the icon is dark, so a white icon shows on it.
	// Windows reads the taskbar's theme, Linux/BSD the desktop's color-scheme
	// from the settings portal. false when it can't tell, and on macOS, which
	// tints template icons by itself.
	bool PanelIsDark();

	// Run `cb` on the tray thread when PanelIsDark() may have changed.
	// Windows and Linux/BSD only. Call after Initialize(), on the tray thread.
	void SetThemeChangeCallback(std::function<void()> cb);
}


#endif /* TRAY_H */
