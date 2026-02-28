#include "tray.hpp"
#include <Cocoa/Cocoa.h>

using namespace Tray;
static NSApplication* app;
static NSStatusBar* statusBar;
static NSStatusItem* statusItem;
bool wantsExit = false;

@interface AppDelegate: NSObject <NSApplicationDelegate>
    - (IBAction)menuCallback:(id)sender;
@end
@implementation AppDelegate{}
    - (IBAction)menuCallback:(id)sender
    {
		TrayMenu* menu = (TrayMenu*)[[sender representedObject] pointerValue];
		if (menu != nullptr && menu->onClicked != nullptr)
		{
			menu->onClicked(menu);
		}
    }
@end

static NSMenu* _tray_menu(std::vector<TrayMenu*> menuItems)
{
	NSMenu* menu = [[NSMenu alloc] init];
    [menu setAutoenablesItems:FALSE];

	for (TrayMenu* tm : menuItems)
	{
		if (tm->text == "-") 
			[menu addItem:[NSMenuItem separatorItem]];
		else
		{
            NSMenuItem* menuItem = [[NSMenuItem alloc]
                initWithTitle:[NSString stringWithUTF8String:tm->text.c_str()]
                action:@selector(menuCallback:)
                keyEquivalent:@""];
			[menuItem setEnabled:(tm->isEnabled ? TRUE : FALSE)];
            [menuItem setState:(tm->isChecked ? 1 : 0)];
            [menuItem setRepresentedObject:[NSValue valueWithPointer:tm]];
            [menu addItem:menuItem];
            if (tm->subMenu.size() > 0) {
                [menu setSubmenu:_tray_menu(tm->subMenu) forItem:menuItem];
            }
		}
	}

	return menu;
}

bool TrayMaker::Initialize(TrayIcon* toInitialize)
{
	AppDelegate *delegate = [[AppDelegate alloc] init];
    app = [NSApplication sharedApplication];
    [app setDelegate:delegate];
    statusBar = [NSStatusBar systemStatusBar];
    statusItem = [statusBar statusItemWithLength:NSVariableStatusItemLength];
    trayIcon = toInitialize;
	Update();
    [app activateIgnoringOtherApps:TRUE];
	return true;
}

bool TrayMaker::Loop(bool blocking)
{
	NSDate* until = (blocking ? [NSDate distantFuture] : [NSDate distantPast]);
    NSEvent* event = [app nextEventMatchingMask:ULONG_MAX untilDate:until
        inMode:[NSString stringWithUTF8String:"kCFRunLoopDefaultMode"] dequeue:TRUE];
    if (event) {
        [app sendEvent:event];
    }

	return !wantsExit;
}

void TrayMaker::Update()
{
    // Load icon from the absolute path stored in trayIcon->iconFilePng.
    // NSBundle-based lookup only works inside a .app bundle; using the path
    // directly works both from the terminal and from a bundled app.
    NSString* pngPath = [NSString stringWithUTF8String:trayIcon->iconFilePng.c_str()];
    NSImage*  image   = [[NSImage alloc] initWithContentsOfFile:pngPath];

    if (image)
    {
        [image setSize:NSMakeSize(16, 16)];
        statusItem.button.image = image;
    }
    else
    {
        // Fallback: show a text label so the tray item is always visible
        statusItem.button.title = [NSString stringWithUTF8String:trayIcon->tooltip.c_str()];
    }

    [statusItem setMenu:_tray_menu(trayIcon->menu)];
}

void TrayMaker::Exit()
{
    wantsExit = true;
}
