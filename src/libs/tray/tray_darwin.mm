#include "tray.hpp"
#include <Cocoa/Cocoa.h>
#include <csignal>

using namespace Tray;
static NSApplication* app;
static NSStatusBar* statusBar;
static NSStatusItem* statusItem;
static bool wantsExit = false;
static std::function<void()> reopenCallback;
static std::function<void()> quitSignalCallback;
static dispatch_source_t signalSources[3];

@interface AppDelegate: NSObject <NSApplicationDelegate>
    - (IBAction)menuCallback:(id)sender;
    - (void)handleReopen:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)reply;
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
    - (void)handleReopen:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)reply
    {
		if (reopenCallback)
			reopenCallback();
    }
@end

// Kept alive for the whole process and set as every menu item's explicit
// target: NSApp's delegate isn't ours to keep, GLFW replaces it in glfwInit()
// and clears it in glfwTerminate(), which made nil-targeted items dead
// after the first capture.
static AppDelegate* delegate = nil;

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
			[menuItem setTarget:delegate];
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
	if (delegate == nil)
		delegate = [[AppDelegate alloc] init];
    app = [NSApplication sharedApplication];
    [app setDelegate:delegate];
    statusBar = [NSStatusBar systemStatusBar];
    statusItem = [statusBar statusItemWithLength:NSVariableStatusItemLength];
    trayIcon = toInitialize;
	Update();
	// No activateIgnoringOtherApps: when started at login it stole keyboard
	// focus from whatever the user was typing in
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
    Wake();
}

void* Tray::GetNativeHandle()
{
	return nullptr;
}

void Tray::SetHotkeyCallback(std::function<void(int)>) {}

void Tray::Wake()
{
	if (app == nil)
		return;
	NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
	                                    location:NSZeroPoint
	                               modifierFlags:0
	                                   timestamp:0
	                                windowNumber:0
	                                     context:nil
	                                     subtype:0
	                                       data1:0
	                                       data2:0];
	[app postEvent:event atStart:YES];
}

void Tray::SetQuitSignalCallback(std::function<void()> cb)
{
	quitSignalCallback = std::move(cb);

	// Dispatch sources deliver the signal on the main queue, which the tray
	// loop's run loop services, so the callback isn't limited to
	// async-signal-safe functions
	const int sigs[3] = { SIGINT, SIGTERM, SIGHUP };
	for (int i = 0; i < 3; ++i)
	{
		if (signalSources[i])
			continue;
		signal(sigs[i], SIG_IGN);
		signalSources[i] = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, sigs[i], 0, dispatch_get_main_queue());
		dispatch_source_set_event_handler(signalSources[i], ^{
			if (quitSignalCallback)
				quitSignalCallback();
		});
		dispatch_resume(signalSources[i]);
	}
}

void Tray::SetReopenCallback(std::function<void()> cb)
{
	reopenCallback = std::move(cb);
	if (delegate == nil)
		delegate = [[AppDelegate alloc] init];

	// An LSUIElement app that's already running gets a "reopen" Apple Event
	// instead of a second instance, without a handler nothing would happen
	[[NSAppleEventManager sharedAppleEventManager]
		setEventHandler:delegate
		    andSelector:@selector(handleReopen:withReplyEvent:)
		  forEventClass:kCoreEventClass
		     andEventID:kAEReopenApplication];
}
