#ifdef __linux__
#include <gtk/gtk.h>

#if __has_include(<libayatana-appindicator/app-indicator.h>)
#  include <libayatana-appindicator/app-indicator.h>
#else
#  include <libappindicator/app-indicator.h>
#endif

#include "tray.hpp"

#define TRAY_APPINDICATOR_ID "tray-id"

static AppIndicator *indicator = NULL;
static bool loop_result = true;

using namespace Tray;

static void _tray_menu_cb(GtkMenuItem *item, gpointer data)
{
	TrayMenu *clickedMenuItem = (TrayMenu *)data;
	if (clickedMenuItem != nullptr && clickedMenuItem->onClicked != nullptr)
		clickedMenuItem->onClicked(clickedMenuItem);
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

	indicator = app_indicator_new(TRAY_APPINDICATOR_ID, toInitialize->iconFilePng.c_str(), APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
	app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);

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
	// GTK is all about reference counting, so previous menu should be destroyed
	// here
	app_indicator_set_menu(indicator, GTK_MENU(_tray_menu(trayIcon->menu)));
}

void TrayMaker::Exit()
{
	loop_result = false;
}
#endif
