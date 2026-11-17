// The macOS menu bar (06-gui.md): NSMenus built from Gui::menus(), the
// table the in-window menu bar draws on Linux and keys() matches. An item
// keeps its place in that table; a click or its key equivalent looks the
// entry up again and posts its run to the UI loop, so menus and keys share
// one code path. Built with ARC.
#import <Cocoa/Cocoa.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "gui.hpp"
#include "update.hpp"

using anton::gui::Cmd;
using anton::gui::Gui;
using anton::gui::Menu;

static Gui* g_gui = nullptr;

static NSString* ns(const std::string& s) {
  NSString* n = [NSString stringWithUTF8String:s.c_str()];
  return n ? n : @"";
}

// The entry at path (indexes from the top), if it's still the one labelled so.
static const Cmd* find(const std::vector<Menu>& ms, NSArray<NSNumber*>* path, NSString* label) {
  if (path.count < 2 || path[0].unsignedLongValue >= ms.size()) return nullptr;
  const std::vector<Cmd>* items = &ms[path[0].unsignedLongValue].items;
  const Cmd* c = nullptr;
  for (NSUInteger i = 1; i < path.count; ++i) {
    size_t at = path[i].unsignedLongValue;
    if (!items || at >= items->size()) return nullptr;
    c = &(*items)[at];
    items = &c->sub;
  }
  return c && [ns(c->label) isEqualToString:label] ? c : nullptr;
}

@interface AntonMenus : NSObject <NSMenuDelegate>
@end

static AntonMenus* g_target = nil;

// "Mod+K" -> "k" with Cmd; "Shift+Mod+R" -> "r" with Shift and Cmd. Plain
// keys ("H", "Enter") aren't key equivalents: the menu would take them from
// the text inputs. They're drawn after a tab instead.
static bool key_equivalent(const std::string& key, NSString** eq, NSEventModifierFlags* mask) {
  std::string k = key;
  NSEventModifierFlags m = 0;
  for (bool more = true; more;) {
    more = false;
    for (auto [prefix, flag] : {std::pair{"Shift+", NSEventModifierFlagShift}, {"Ctrl+", NSEventModifierFlagControl},
                                {"Mod+", NSEventModifierFlagCommand}})
      if (k.size() > strlen(prefix) && k.rfind(prefix, 0) == 0) {
        m |= flag;
        k = k.substr(strlen(prefix));
        more = true;
      }
  }
  if (!(m & NSEventModifierFlagCommand) || k.size() != 1) return false;
  *eq = ns(std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(k[0])))));
  *mask = m;
  return true;
}

static void fill(NSMenu* menu, const std::vector<Cmd>& items, NSArray<NSNumber*>* prefix) {
  [menu removeAllItems];
  NSFont* font = [NSFont menuFontOfSize:0];
  CGFloat widest = 0;
  for (auto& c : items) widest = MAX(widest, [ns(c.label) sizeWithAttributes:@{NSFontAttributeName : font}].width);
  for (size_t i = 0; i < items.size(); ++i) {
    const Cmd& c = items[i];
    if (c.label.empty()) {
      [menu addItem:[NSMenuItem separatorItem]];
      continue;
    }
    NSArray<NSNumber*>* path = [prefix arrayByAddingObject:@(i)];
    NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:ns(c.label) action:nil keyEquivalent:@""];
    if (!c.run && !c.sub.empty()) {
      NSMenu* sub = [[NSMenu alloc] initWithTitle:ns(c.label)];
      fill(sub, c.sub, path);
      it.submenu = sub;
    } else {
      it.target = g_target;
      it.action = @selector(fire:);
      it.representedObject = @{@"path" : path, @"label" : ns(c.label)};
      it.state = c.check == 1 ? NSControlStateValueOn : NSControlStateValueOff;
      NSString* eq = nil;
      NSEventModifierFlags mask = 0;
      if (key_equivalent(c.key, &eq, &mask)) {
        it.keyEquivalent = eq;
        it.keyEquivalentModifierMask = mask;
      } else if (!c.key.empty()) {
        NSString* hint = c.key == "Enter" ? @"↵" : ns(c.key);
        NSMutableParagraphStyle* ps = [[NSMutableParagraphStyle alloc] init];
        ps.tabStops = @[ [[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentRight location:widest + 48 options:@{}] ];
        NSString* s = [NSString stringWithFormat:@"%@\t%@", ns(c.label), hint];
        NSMutableAttributedString* t = [[NSMutableAttributedString alloc]
            initWithString:s
                attributes:@{NSFontAttributeName : font, NSParagraphStyleAttributeName : ps}];
        [t addAttribute:NSForegroundColorAttributeName
                  value:[NSColor secondaryLabelColor]
                  range:NSMakeRange(s.length - hint.length, hint.length)];
        it.attributedTitle = t;
      }
    }
    [menu addItem:it];
  }
}

@implementation AntonMenus
- (void)fire:(NSMenuItem*)item {
  if (!g_gui) return;
  NSDictionary* at = item.representedObject;
  auto ms = g_gui->menus();
  const Cmd* c = find(ms, at[@"path"], at[@"label"]);
  if (c && c->enabled && c->run) g_gui->post(c->run);  // runs in the loop, between frames
}

// Enabled and checked as things are now, before the menu shows and before
// a key equivalent fires.
- (BOOL)validateMenuItem:(NSMenuItem*)item {
  if (!g_gui || ![item.representedObject isKindOfClass:[NSDictionary class]]) return YES;
  NSDictionary* at = item.representedObject;
  auto ms = g_gui->menus();
  const Cmd* c = find(ms, at[@"path"], at[@"label"]);
  if (!c) return NO;
  item.state = c->check == 1 ? NSControlStateValueOn : NSControlStateValueOff;
  return c->enabled;
}

// A top-level menu is rebuilt each time it opens: the views, the plugins'
// actions and the labels that change (Enter/Exit Full Screen) are current.
- (void)menuNeedsUpdate:(NSMenu*)menu {
  if (!g_gui) return;
  auto ms = g_gui->menus();
  NSUInteger i = [[NSApp mainMenu] indexOfItemWithSubmenu:menu];
  if (i == NSNotFound || i >= ms.size()) return;
  fill(menu, ms[i].items, @[ @(i) ]);
}
@end

namespace anton::gui {

void mac_install_menu(Gui* g) {
  g_gui = g;
  g_target = [[AntonMenus alloc] init];
  // The app menu's bold title is the process's name.
  [[NSProcessInfo processInfo] setProcessName:@"anton"];
  NSMenu* bar = [[NSMenu alloc] init];
  auto ms = g->menus();
  for (size_t i = 0; i < ms.size(); ++i) {
    NSMenuItem* top = [[NSMenuItem alloc] initWithTitle:ns(ms[i].title) action:nil keyEquivalent:@""];
    NSMenu* menu = [[NSMenu alloc] initWithTitle:ns(ms[i].title)];
    menu.delegate = g_target;
    fill(menu, ms[i].items, @[ @(i) ]);  // now too, so the key equivalents work before a menu has opened
    top.submenu = menu;
    [bar addItem:top];
  }
  [NSApp setMainMenu:bar];
}

void mac_about() {
  [NSApp activateIgnoringOtherApps:YES];
  [NSApp orderFrontStandardAboutPanelWithOptions:@{
    NSAboutPanelOptionApplicationName : @"anton",
    NSAboutPanelOptionApplicationVersion : ns(kVersion),
    NSAboutPanelOptionVersion : @"",
  }];
}

void mac_hide() { [NSApp hide:nil]; }

void mac_toggle_fullscreen(GLFWwindow* w) { [glfwGetCocoaWindow(w) toggleFullScreen:nil]; }

bool mac_fullscreen(GLFWwindow* w) {
  NSWindow* win = glfwGetCocoaWindow(w);
  return win && (win.styleMask & NSWindowStyleMaskFullScreen);
}

}  // namespace anton::gui
