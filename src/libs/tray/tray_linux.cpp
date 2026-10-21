#if !defined(_WIN32) && !defined(__APPLE__)
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <csignal>

#if __has_include(<libayatana-appindicator/app-indicator.h>)
#  include <libayatana-appindicator/app-indicator.h>
#else
#  include <libappindicator/app-indicator.h>
#endif

#include "tray.hpp"

// StatusNotifierItem id, must be unique per app or hosts may merge/hide icons
#define TRAY_APPINDICATOR_ID "oshot"

static AppIndicator *indicator = NULL;
static bool loop_result = true;
static std::function<void()> quitSignalCallback;
static std::function<void()> iconClickCallback;

using namespace Tray;

static void _tray_menu_cb(GtkMenuItem *item, gpointer data)
{
	TrayMenu *clickedMenuItem = (TrayMenu *)data;
	if (clickedMenuItem != nullptr && clickedMenuItem->onClicked != nullptr)
		clickedMenuItem->onClicked(clickedMenuItem);
}

// SNI Activate(), sent by the host on a primary click
static void _tray_activate_cb(AppIndicator *, gint, gint, gpointer)
{
	if (iconClickCallback)
		iconClickCallback();
}

static void _tray_status_icon_activate_cb(GtkStatusIcon *, gpointer)
{
	if (iconClickCallback)
		iconClickCallback();
}

// Without a StatusNotifier host (XEmbed trays: dwm, i3bar, stalonetray, ...)
// AppIndicator falls back to a GtkStatusIcon whose left click opens the menu.
// Keep everything else it set up, the right click menu included, but make the
// left click ours.
static GtkStatusIcon *(*parentFallback)(AppIndicator *) = NULL;

static GtkStatusIcon *_tray_fallback(AppIndicator *self)
{
	GtkStatusIcon *icon = parentFallback(self);
	if (icon != NULL)
	{
		g_signal_handlers_disconnect_matched(icon, G_SIGNAL_MATCH_ID, g_signal_lookup("activate", GTK_TYPE_STATUS_ICON), 0, NULL, NULL, NULL);
		g_signal_connect(icon, "activate", G_CALLBACK(_tray_status_icon_activate_cb), NULL);
	}
	return icon;
}

static GtkMenuShell *_tray_menu(std::vector<TrayMenu *> menuItems)
{
	GtkMenuShell *menu = (GtkMenuShell *)gtk_menu_new();

	for (TrayMenu *tm : menuItems)
	{
		GtkWidget *item;
		if (tm->text == "-")
			item = gtk_separator_menu_item_new();
		else
		{
			if (tm->subMenu.size() > 0)
			{
				item = gtk_menu_item_new_with_label(tm->text.c_str());
				gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), GTK_WIDGET(_tray_menu(tm->subMenu)));
			}
			else if (tm->hasCheckbox)
			{
				item = gtk_check_menu_item_new_with_label(tm->text.c_str());
				gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), !!tm->isChecked);
			}
			else
			{
				item = gtk_menu_item_new_with_label(tm->text.c_str());
			}

			gtk_widget_set_sensitive(item, tm->isEnabled);
			if (tm->onClicked != nullptr)
			{
				g_signal_connect(item, "activate", G_CALLBACK(_tray_menu_cb), tm);
			}
		}

		gtk_widget_show(item);
		gtk_menu_shell_append(menu, item);
	}

	return menu;
}

bool TrayMaker::Initialize(TrayIcon* toInitialize)
{
	if (gtk_init_check(0, NULL) == FALSE)
		return false;

	// Before creating the indicator, it may fall back on its first check for a
	// host. Without a click callback the left click keeps opening the menu.
	AppIndicatorClass *klass = (AppIndicatorClass *)g_type_class_ref(APP_INDICATOR_TYPE);
	if (toInitialize->onClicked && klass->fallback != NULL && parentFallback == NULL)
	{
		parentFallback = klass->fallback;
		klass->fallback = _tray_fallback;
	}

	indicator = app_indicator_new(TRAY_APPINDICATOR_ID, toInitialize->iconFilePng.c_str(), APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
	app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
	// Only Ayatana AppIndicator 0.6+ has it. Without a handler Activate()
	// fails and the host opens the menu instead, so don't connect one for nothing
	if (toInitialize->onClicked && g_signal_lookup("activate", G_OBJECT_TYPE(indicator)) != 0)
		g_signal_connect(indicator, "activate", G_CALLBACK(_tray_activate_cb), NULL);

	trayIcon = toInitialize;
	Update();
	
	return true;
}

bool TrayMaker::Loop(bool blocking)
{
	gtk_main_iteration_do(blocking);
	return loop_result;
}

void TrayMaker::Update()
{
	app_indicator_set_icon(indicator, trayIcon->iconFilePng.c_str());
	iconClickCallback = trayIcon->onClicked;
	// GTK is all about reference counting, so previous menu should be destroyed
	// here
	app_indicator_set_menu(indicator, GTK_MENU(_tray_menu(trayIcon->menu)));
}

void TrayMaker::Exit()
{
	loop_result = false;
	Wake();
}

void* Tray::GetNativeHandle()
{
	return nullptr;
}

void Tray::SetHotkeyCallback(std::function<void(int)>) {}

void Tray::Wake()
{
	// Thread-safe; makes a blocking gtk_main_iteration_do() return
	g_main_context_wakeup(NULL);
}

static gboolean _tray_on_signal(gpointer)
{
	if (quitSignalCallback)
		quitSignalCallback();
	return G_SOURCE_CONTINUE;
}

void Tray::SetQuitSignalCallback(std::function<void()> cb)
{
	quitSignalCallback = std::move(cb);
	// Dispatched from the GTK main loop, so the callback isn't limited to
	// async-signal-safe functions and the blocking loop wakes up for it
	g_unix_signal_add(SIGINT, _tray_on_signal, NULL);
	g_unix_signal_add(SIGTERM, _tray_on_signal, NULL);
	g_unix_signal_add(SIGHUP, _tray_on_signal, NULL);
}

void Tray::SetReopenCallback(std::function<void()>) {}
#endif
