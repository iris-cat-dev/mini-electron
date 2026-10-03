
#include "runtime/electron/node_bindings.h"

#include "runtime/electron/browser/api/window_interface.h"
#include "runtime/electron/browser/api/window_list.h"
#include "runtime/electron/browser/api/menu_event_notif.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/common/hide_wnd_help.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libnode/src/node_buffer.h"
#include "third_party/libuv/include/uv.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include <set>
#include <cstring>

namespace atom {

class Menu;

class MenuItem {
public:
    enum MenuItemType { ActionType, CheckableActionType, SeparatorType, SubmenuType };

    MenuItem(v8::Isolate* isolate, Menu* menu);

    ~MenuItem();

    v8::Isolate* getIsolate() const
    {
        return m_isolate;
    }

    Menu* getMenu() const
    {
        return m_menu;
    }

    void setType(MenuItemType type)
    {
        m_type = type;
    }

    MenuItemType getType() const
    {
        return m_type;
    }

    Menu* getSubMenu() const
    {
        return m_subMenu;
    }

    void setSubMenu(Menu* subMenu);

    void setLabel(const std::string label)
    {
        m_label = label;
    }

    void setEnabled(bool b)
    {
        m_isEnabled = b;
    }


    bool isEnabled() const
    {
        return m_isEnabled;
    }

    void setVisible(bool visible)
    {
        m_isVisible = visible;
    }

    bool isVisible() const
    {
        return m_isVisible;
    }

    void setAccelerator(const std::string& accelerator);
    bool matchesAccelerator(UINT virtualKeyCode) const;
    void setChecked(bool b)
    {
        m_isChecked = b;
    }

    bool getChecked() const
    {
        return m_isChecked;
    }

    void setClickCallback(v8::Local<v8::Value> callback)
    {
        m_clickCallbackValue.Reset(m_isolate, callback);
    }

    v8::Local<v8::Value> getClickCallbackValue() const
    {
        return m_clickCallbackValue.Get(m_isolate);
    }

    void setId(int id)
    {
        m_id = id;
    }

    int getId() const
    {
        return m_id;
    }

    UINT getAction() const
    {
        return m_action;
    }

    void insertPlatformMenu(size_t pos, HMENU hMenu) const;

    void clear();

private:
    MenuItemType m_type;
    HMENU m_hSubMenu;
    Menu* m_subMenu;
    std::string m_label;
    bool m_isEnabled;
    bool m_isVisible;
    bool m_isChecked;
    bool m_acceleratorValid;
    bool m_acceleratorControl;
    bool m_acceleratorShift;
    bool m_acceleratorAlt;
    bool m_acceleratorSuper;
    bool m_acceleratorPlus;
    bool m_acceleratorMinus;
    UINT m_acceleratorKey;
    UINT m_action;
    v8::Persistent<v8::Value> m_clickCallbackValue;
    Menu* m_menu;
    int m_id;
    v8::Isolate* m_isolate;
};

//////////////////////////////////////////////////////////////////////////

class Menu : public mate::EventEmitter<Menu> {
public:
    explicit Menu(v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
        : m_hideWndHelp(nullptr)
    {
        gin_helper::Wrappable<Menu>::InitWith(isolate, wrapper);
        //m_menuTemplate = nullptr;
        m_hMenu = NULL;
        m_isItemNeedRebuilt = false;
        m_isAppOrPopupMenu = kNoInit;
        m_liveMenus->insert(this);
    }

    virtual ~Menu() override
    {
        OutputDebugStringA("~Menu\n");
        m_liveMenus->erase(this);
        ::DestroyMenu(m_hMenu);
        m_hMenu = nullptr;

        if (m_appMenu == this)
            m_appMenu = nullptr;
    }

    static void init(v8::Isolate* isolate, v8::Local<v8::Object> target)
    {
        v8::Local<v8::Context> context = isolate->GetCurrentContext();
        m_liveMenuItem = new std::set<MenuItem*>();
        m_liveMenus = new std::set<Menu*>();
        v8::Local<v8::FunctionTemplate> prototype = v8::FunctionTemplate::New(isolate, newFunction);

        prototype->SetClassName(v8::String::NewFromUtf8(isolate, "Menu").ToLocalChecked());
        gin_helper::ObjectTemplateBuilder builder(isolate, prototype->InstanceTemplate());
        builder.SetMethod("_sendActionToFirstResponder", &Menu::sendActionToFirstResponderApi);
        builder.SetMethod("_insert", &Menu::_insertApi);
        builder.SetMethod("_append", &Menu::_appendeApi);
        builder.SetMethod("_popup", &Menu::_popupApi);
        builder.SetMethod("_clear", &Menu::_clearApi);
        builder.SetMethod("getItemCount", &Menu::getItemCountApi);
        builder.SetMethod("quit", &Menu::nullFunction);

        v8::Local<v8::Function> menuConstructor = prototype->GetFunction(context).ToLocalChecked();
        menuConstructor->Set(
            context,
            v8::String::NewFromUtf8(isolate, "_setApplicationMenu").ToLocalChecked(),
            v8::Function::New(context, setApplicationMenuApi).ToLocalChecked()).Check();
        constructor.Reset(isolate, menuConstructor);
        target->Set(context, v8::String::NewFromUtf8(isolate, "Menu").ToLocalChecked(), menuConstructor).Check();
    }

    void nullFunction()
    {
        DebugBreak();
    }

    // Set or clear the global menubar on every existing window.
    static void setApplicationMenuApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        v8::Isolate* isolate = args.GetIsolate();
        if (args.Length() != 1) {
            isolate->ThrowException(v8::Exception::TypeError(
                v8::String::NewFromUtf8(isolate, "Invalid menu").ToLocalChecked()));
            return;
        }

        HMENU hmenuBar = nullptr;
        if (!MenuEventNotif::getNativeMenu(args[0], &hmenuBar))
            return;

        Menu* menu = nullptr;
        if (!args[0]->IsNull()
            && !gin_helper::Converter<Menu*>::FromV8(isolate, args[0], &menu)) {
            return;
        }

        WindowList::iterator winIt = WindowList::getInstance()->begin();
        for (; winIt != WindowList::getInstance()->end(); ++winIt)
            (*winIt)->setNativeMenu(hmenuBar);

        m_appMenu = menu;
    }

    void sendActionToFirstResponderApi(const std::string& action)
    {
        DebugBreak();
    }

    size_t getItemCountApi() const
    {
        return m_items.size();
    }

    void _appendeApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
    }

    void _insertApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        if (2 != args.Length())
            return;

        v8::Isolate* isolate = args.GetIsolate();
        if (!args[0]->IsUint32())
            return;
        v8::Local<v8::Context> context = isolate->GetCurrentContext();
        size_t pos = args[0]->ToUint32(context).ToLocalChecked()->Value();

        v8::Object* v8Obj = v8::Object::Cast(*args[1]);
        v8::MaybeLocal<v8::Array> v8MaybeObjProps = v8Obj->GetOwnPropertyNames(
            context, static_cast<v8::PropertyFilter>(v8::ALL_PROPERTIES | v8::SKIP_SYMBOLS));
        if (v8MaybeObjProps.IsEmpty())
            return;
        v8::Local<v8::Array> v8ObjProps = v8MaybeObjProps.ToLocalChecked();

        size_t size = v8ObjProps->Length();

        std::string label;
        std::string role;

        MenuItem* item = new MenuItem(isolate, this);

        for (size_t i = 0; i < size; ++i) {
            v8::MaybeLocal<v8::Value> keyNameValueMaybe = v8ObjProps->Get(context, i);
            if (keyNameValueMaybe.IsEmpty())
                continue;
            v8::Local<v8::Value> keyNameValue = keyNameValueMaybe.ToLocalChecked();
            v8::MaybeLocal<v8::Value> outValueMaybe = v8Obj->Get(context, keyNameValue);
            if (outValueMaybe.IsEmpty())
                continue;
            v8::Local<v8::Value> outValue = outValueMaybe.ToLocalChecked();

            std::string keyNameStr;
            if (!gin_helper::Converter<std::string>::FromV8(isolate, keyNameValue, &keyNameStr))
                return;

            std::string type;
            if ("type" == keyNameStr && outValue->IsString()) {
                v8::String::Utf8Value utf8(isolate, outValue->ToString(context).ToLocalChecked());
                type = *utf8;
            }
            if ("separator" == type)
                item->setType(MenuItem::SeparatorType);
            if ("checkbox" == type || "radio" == type)
                item->setType(MenuItem::CheckableActionType);

            if ("label" == keyNameStr && outValue->IsString()) {
                v8::String::Utf8Value utf8(isolate, outValue->ToString(context).ToLocalChecked());
                label = *utf8;
            }

            if ("role" == keyNameStr && outValue->IsString()) {
                v8::String::Utf8Value utf8(isolate, outValue->ToString(context).ToLocalChecked());
                role = *utf8;
            }

            if ("enabled" == keyNameStr && outValue->IsBoolean()) {
                item->setEnabled(outValue->ToBoolean(isolate)->Value());
            }

            if ("visible" == keyNameStr && outValue->IsBoolean()) {
                item->setVisible(outValue->ToBoolean(isolate)->Value());
            }

            if ("_accelerator" == keyNameStr && outValue->IsString()) {
                v8::String::Utf8Value utf8(isolate, outValue->ToString(context).ToLocalChecked());
                item->setAccelerator(*utf8);
            }

            if ("checked" == keyNameStr && outValue->IsBoolean()) {
                item->setChecked(outValue->ToBoolean(isolate)->Value());
            }

            if ("submenu" == keyNameStr) {
                Menu* subMenu = nullptr;
                if (!gin_helper::Converter<Menu*>::FromV8(isolate, outValue, &subMenu))
                    subMenu = nullptr;
                if (subMenu)
                    item->setSubMenu(subMenu);
            }

            if ("click" == keyNameStr && outValue->IsFunction()) {
                item->setClickCallback(outValue);
            }
        }

        if (label.empty())
            label = role;
        item->setLabel(label);

        m_items.insert(m_items.begin() + pos, item);
        m_isItemNeedRebuilt = true;
    }

    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        v8::Isolate* isolate = args.GetIsolate();
        if (args.IsConstructCall()) {
            new Menu(isolate, args.This());
            args.GetReturnValue().Set(args.This());
            return;
        }
    }

    void _popupApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        v8::Isolate* isolate = args.GetIsolate();
        v8::Local<v8::Context> context = isolate->GetCurrentContext();

        HWND owner = nullptr;
        const bool hasOwner = args.Length() > 0 && !args[0]->IsNullOrUndefined();
        if (hasOwner) {
            if (!node::Buffer::HasInstance(args[0])
                || node::Buffer::Length(args[0]) < sizeof(HWND)) {
                isolate->ThrowException(v8::Exception::TypeError(
                    v8::String::NewFromUtf8(isolate, "Invalid BrowserWindow handle").ToLocalChecked()));
                return;
            }
            std::memcpy(&owner, node::Buffer::Data(args[0]), sizeof(HWND));
            if (!::IsWindow(owner))
                return;
        } else {
            Menu* self = this;
            if (!m_hideWndHelp) {
                m_hideWndHelp = new HideWndHelp(L"HideParentWindowClass",
                    [self](HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) -> LRESULT {
                        return self->hideWndProc(hWnd, uMsg, wParam, lParam);
                    });
            }
            owner = m_hideWndHelp->getWnd();
        }

        HMENU popup = buildMenus(false);
        if (!popup || !::IsWindow(owner))
            return;

        POINT pt;
        if (args.Length() >= 3 && args[1]->IsInt32() && args[2]->IsInt32()) {
            pt.x = args[1]->ToInt32(context).ToLocalChecked()->Value();
            pt.y = args[2]->ToInt32(context).ToLocalChecked()->Value();
        } else {
            ::GetCursorPos(&pt);
        }

        ::SetForegroundWindow(owner);
        const UINT command = ::TrackPopupMenu(popup,
            TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            pt.x, pt.y, 0, owner, nullptr);
        if (command)
            MenuEventNotif::onMenuCommon(WM_COMMAND, MAKEWPARAM(command, 0), 0);
        if (::IsWindow(owner))
            ::PostMessageW(owner, WM_NULL, 0, 0);
    }

    void _clearApi()
    {
        clear();
    }
    void clear()
    {
        if (m_hMenu)
            ::DestroyMenu(m_hMenu);
        m_hMenu = nullptr;
        for (size_t i = 0; i < m_items.size(); ++i) {
            MenuItem* it = m_items[i];
            it->clear();
            delete it;
        }
        m_items.clear();
    }

    void onCommon(MenuItem* item, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        if (!item || !item->isEnabled() || !item->isVisible())
            return;

        if (MenuItem::CheckableActionType == item->getType()) {
            MENUITEMINFOW info = { 0 };
            info.cbSize = sizeof(MENUITEMINFOW);
            info.fMask = MIIM_STATE;
            if (::GetMenuItemInfoW(m_hMenu, item->getAction(), FALSE, &info)) {
                if (item->getChecked()) {
                    info.fState &= ~MFS_CHECKED;
                    item->setChecked(false);
                } else {
                    info.fState |= MFS_CHECKED;
                    item->setChecked(true);
                }
                ::SetMenuItemInfoW(m_hMenu, item->getAction(), FALSE, &info);
            }
        }

        v8::Local<v8::Value> focusedWindow = WindowInterface::getFocusedWindow(isolate());
        v8::Local<v8::Value> focusedWebContents = WindowInterface::getFocusedContents(isolate());
        item->getMenu()->mate::EventEmitter<Menu>::emit(
            "click", item->getClickCallbackValue(), focusedWindow, focusedWebContents);
    }

    static bool dispatchAccelerator(HMENU nativeMenu, UINT virtualKeyCode)
    {
        if (!nativeMenu)
            return false;
        for (Menu* menu : *m_liveMenus) {
            if (menu->m_hMenu != nativeMenu)
                continue;
            MenuItem* item = menu->findAccelerator(virtualKeyCode);
            if (!item)
                return false;
            item->getMenu()->onCommon(item, WM_COMMAND, MAKEWPARAM(item->getAction(), 0), 0);
            return true;
        }
        return false;
    }

    static Menu* getAppMenu()
    {
        return m_appMenu;
    }

public:
    static gin_helper::WrapperInfo kWrapperInfo;
    static v8::Persistent<v8::Function> constructor;
    static std::set<MenuItem*>* m_liveMenuItem;
    static std::set<Menu*>* m_liveMenus;

    HideWndHelp* m_hideWndHelp;

    friend class MenuItem;

    HMENU buildMenus(bool isAppOrPopupMenu)
    {
        if (!m_isItemNeedRebuilt)
            return m_hMenu;
        m_isItemNeedRebuilt = false;

        AppOrPopupType appOrPopupMenuType = isAppOrPopupMenu ? kIsApp : kIsPopup;
        if (kNoInit == m_isAppOrPopupMenu)
            m_isAppOrPopupMenu = appOrPopupMenuType;
        else if (m_isAppOrPopupMenu != appOrPopupMenuType)
            return nullptr;

        if (m_hMenu)
            ::DestroyMenu(m_hMenu);
        m_hMenu = nullptr;

        return buildMenu(this, isAppOrPopupMenu);
    }

private:
    static HMENU buildMenu(Menu* menu, bool isAppOrPopupMenu)
    {
        size_t size = menu->m_items.size();
        if (0 == size)
            return nullptr;

        menu->m_hMenu = ((isAppOrPopupMenu) ? ::CreateMenu() : ::CreatePopupMenu());

        for (size_t i = 0; i < size; ++i) {
            MenuItem* item = menu->m_items[i];
            item->insertPlatformMenu(i, menu->m_hMenu);
        }
        return menu->m_hMenu;
    }

    MenuItem* findAccelerator(UINT virtualKeyCode)
    {
        for (MenuItem* item : m_items) {
            if (!item->isEnabled() || !item->isVisible())
                continue;
            if (item->getType() == MenuItem::SubmenuType) {
                Menu* submenu = item->getSubMenu();
                MenuItem* match = submenu ? submenu->findAccelerator(virtualKeyCode) : nullptr;
                if (match)
                    return match;
            } else if (item->matchesAccelerator(virtualKeyCode)) {
                return item;
            }
        }
        return nullptr;
    }

    int findItemIndex(MenuItem* item)
    {
        for (size_t i = 0; i < m_items.size(); ++i) {
            MenuItem* it = m_items[i];
            if (it == item)
                return i;
        }
        return -1;
    }

    LRESULT hideWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        switch (uMsg) {
        case WM_COMMAND: {
            MenuEventNotif::onMenuCommon(uMsg, wParam, lParam);
            return 0;
        }
        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
        return 0;
    }

    HMENU m_hMenu;
    std::vector<MenuItem*> m_items;
    bool m_isItemNeedRebuilt;

    static Menu* m_appMenu;

    enum AppOrPopupType {
        kNoInit,
        kIsApp,
        kIsPopup,
    };
    AppOrPopupType m_isAppOrPopupMenu;
};

//////////////////////////////////////////////////////////////////////////

static int s_menuItemCount = 1;

MenuItem::MenuItem(v8::Isolate* isolate, Menu* menu)
{
    m_isolate = isolate;
    m_type = ActionType;
    m_hSubMenu = nullptr;
    m_isEnabled = true;
    m_isVisible = true;
    m_isChecked = false;
    m_acceleratorValid = false;
    m_acceleratorControl = false;
    m_acceleratorShift = false;
    m_acceleratorAlt = false;
    m_acceleratorSuper = false;
    m_acceleratorPlus = false;
    m_acceleratorMinus = false;
    m_acceleratorKey = 0;
    m_action = s_menuItemCount++;
    m_menu = menu;
    m_subMenu = nullptr;
    m_id = 0;
    Menu::m_liveMenuItem->insert(this);
}

MenuItem::~MenuItem()
{
    Menu::m_liveMenuItem->erase(this);
}

static std::string lowerAcceleratorToken(const std::string& token)
{
    std::string result = token;
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z')
            character += 'a' - 'A';
    }
    return result;
}

void MenuItem::setAccelerator(const std::string& accelerator)
{
    m_acceleratorValid = false;
    m_acceleratorControl = false;
    m_acceleratorShift = false;
    m_acceleratorAlt = false;
    m_acceleratorSuper = false;
    m_acceleratorPlus = false;
    m_acceleratorMinus = false;
    m_acceleratorKey = 0;

    bool hasKey = false;
    size_t start = 0;
    while (start <= accelerator.size()) {
        size_t end = accelerator.find('+', start);
        const bool isLast = end == std::string::npos;
        if (isLast)
            end = accelerator.size();
        std::string token = lowerAcceleratorToken(accelerator.substr(start, end - start));
        if (token.empty() && end == accelerator.size())
            token = "plus";

        if (token == "commandorcontrol" || token == "cmdorctrl"
            || token == "cmdorcontrol" || token == "controlorcommand"
            || token == "ctrl" || token == "control") {
            m_acceleratorControl = true;
        } else if (token == "shift") {
            m_acceleratorShift = true;
        } else if (token == "alt" || token == "option") {
            m_acceleratorAlt = true;
        } else if (token == "command" || token == "cmd" || token == "super") {
            m_acceleratorSuper = true;
        } else {
            UINT key = 0;
            if (token.size() == 1 && token[0] >= 'a' && token[0] <= 'z') {
                key = static_cast<UINT>(token[0] - 'a' + 'A');
            } else if (token.size() == 1 && token[0] >= '0' && token[0] <= '9') {
                key = static_cast<UINT>(token[0]);
            } else if (token == "=") {
                key = VK_OEM_PLUS;
            } else if (token == "plus" || token == "add") {
                key = VK_OEM_PLUS;
                m_acceleratorPlus = true;
            } else if (token == "-" || token == "minus" || token == "subtract") {
                key = VK_OEM_MINUS;
                m_acceleratorMinus = true;
            } else if (token == "space") {
                key = VK_SPACE;
            } else if (token == "tab") {
                key = VK_TAB;
            } else if (token == "backspace") {
                key = VK_BACK;
            } else if (token == "delete" || token == "del") {
                key = VK_DELETE;
            } else if (token == "insert" || token == "ins") {
                key = VK_INSERT;
            } else if (token == "enter" || token == "return") {
                key = VK_RETURN;
            } else if (token == "escape" || token == "esc") {
                key = VK_ESCAPE;
            } else if (token == "left") {
                key = VK_LEFT;
            } else if (token == "right") {
                key = VK_RIGHT;
            } else if (token == "up") {
                key = VK_UP;
            } else if (token == "down") {
                key = VK_DOWN;
            } else if (token == "home") {
                key = VK_HOME;
            } else if (token == "end") {
                key = VK_END;
            } else if (token == "pageup") {
                key = VK_PRIOR;
            } else if (token == "pagedown") {
                key = VK_NEXT;
            } else if (token.size() >= 2 && token[0] == 'f') {
                int number = 0;
                for (size_t i = 1; i < token.size(); ++i) {
                    if (token[i] < '0' || token[i] > '9') {
                        number = 0;
                        break;
                    }
                    number = number * 10 + token[i] - '0';
                }
                if (number >= 1 && number <= 24)
                    key = VK_F1 + number - 1;
            }

            if (!key || hasKey)
                return;
            m_acceleratorKey = key;
            hasKey = true;
        }

        if (isLast)
            break;
        start = end + 1;
    }
    m_acceleratorValid = hasKey;
}

bool MenuItem::matchesAccelerator(UINT virtualKeyCode) const
{
    if (!m_acceleratorValid)
        return false;

    bool keyMatches = virtualKeyCode == m_acceleratorKey;
    if (m_acceleratorPlus)
        keyMatches = virtualKeyCode == VK_OEM_PLUS || virtualKeyCode == VK_ADD;
    else if (m_acceleratorMinus)
        keyMatches = virtualKeyCode == VK_OEM_MINUS || virtualKeyCode == VK_SUBTRACT;
    if (!keyMatches)
        return false;

    const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool super = ((::GetKeyState(VK_LWIN) | ::GetKeyState(VK_RWIN)) & 0x8000) != 0;
    bool requiredShift = m_acceleratorShift;
    if (m_acceleratorPlus && virtualKeyCode == VK_OEM_PLUS)
        requiredShift = true;
    const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    return control == m_acceleratorControl
        && shift == requiredShift
        && alt == m_acceleratorAlt
        && super == m_acceleratorSuper;
}

void MenuItem::setSubMenu(Menu* subMenu)
{
    HMENU hSubMenu = subMenu->m_hMenu;
    m_hSubMenu = hSubMenu;
    m_subMenu = subMenu;
    m_type = SubmenuType;
}

void MenuItem::insertPlatformMenu(size_t pos, HMENU hMenu) const
{
    if (!m_isVisible)
        return;

    int count = ::GetMenuItemCount(hMenu);
    if (count < 0)
        return;

    MENUITEMINFO info = { 0 };
    info.cbSize = sizeof(MENUITEMINFO);

    if (m_type == SeparatorType) {
        info.fMask = MIIM_FTYPE;
        info.fType = MFT_SEPARATOR;
        ::InsertMenuItem(hMenu, count, TRUE, &info);
        return;
    }

    info.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE;
    info.fType = MFT_STRING;
    info.wID = m_action;

    if (m_type == SubmenuType) {
        info.fMask |= MIIM_SUBMENU;
        info.hSubMenu = Menu::buildMenu(m_subMenu, false); // m_hSubMenu;
    }

    std::u16string labelW = base::UTF8ToUTF16(m_label);
    if (!labelW.empty()) {
        info.fMask |= MIIM_STRING;
        info.cch = labelW.size();
        info.dwTypeData = (LPWSTR)(labelW.c_str());
    }

    info.fState |= m_isEnabled ? MFS_ENABLED : MFS_DISABLED;
    if (CheckableActionType == m_type)
        info.fState |= m_isChecked ? MFS_CHECKED : MFS_UNCHECKED;
    ::InsertMenuItem(hMenu, count, TRUE, &info);
}

void MenuItem::clear()
{
    if (m_hSubMenu)
        ::DestroyMenu(m_hSubMenu);
    m_hSubMenu = nullptr;
    if (m_subMenu)
        m_subMenu->clear();
    m_subMenu = nullptr;
}

bool MenuEventNotif::getNativeMenu(v8::Local<v8::Value> value, HMENU* menu)
{
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    if (!menu || value.IsEmpty()) {
        isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8(isolate, "Invalid menu").ToLocalChecked()));
        return false;
    }

    if (value->IsNull()) {
        *menu = nullptr;
        return true;
    }

    Menu* nativeMenu = nullptr;
    if (!gin_helper::Converter<Menu*>::FromV8(isolate, value, &nativeMenu)) {
        isolate->ThrowException(v8::Exception::TypeError(
            v8::String::NewFromUtf8(isolate, "Invalid menu").ToLocalChecked()));
        return false;
    }

    *menu = nativeMenu->buildMenus(true);
    return true;
}

void MenuEventNotif::onWindowDidCreated(WindowInterface* window)
{
    HMENU hmenuBar = nullptr;
    if (Menu::getAppMenu())
        hmenuBar = Menu::getAppMenu()->buildMenus(true);
    window->setNativeMenu(hmenuBar);
}

void MenuEventNotif::onMenuCommon(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    UINT menuID = LOWORD(wParam);
    for (std::set<MenuItem*>::const_iterator it = Menu::m_liveMenuItem->begin(); it != Menu::m_liveMenuItem->end(); ++it) {
        MenuItem* item = *it;
        if (item->getAction() == menuID) {
            item->getMenu()->onCommon(item, uMsg, wParam, lParam);
            return;
        }
    }
}

bool MenuEventNotif::onAccelerator(HMENU menu, UINT virtualKeyCode)
{
    return Menu::dispatchAccelerator(menu, virtualKeyCode);
}

v8::Persistent<v8::Function> Menu::constructor;
gin_helper::WrapperInfo Menu::kWrapperInfo = { gin_helper::GinEmbedder::kEmbedderNativeGin };
std::set<MenuItem*>* Menu::m_liveMenuItem = nullptr;
std::set<Menu*>* Menu::m_liveMenus = nullptr;

Menu* Menu::m_appMenu = nullptr;

static void initializeMenuApi(v8::Local<v8::Object> target, v8::Local<v8::Value> unused, v8::Local<v8::Context> context, const NodeNative* native)
{
    Menu::init(context->GetIsolate(), target);
}

static const char BrowserMenuNative[] = "exports = function {};";

static NodeNative nativeBrowserMenuNative { "Menu", BrowserMenuNative, sizeof(BrowserMenuNative) - 1 };

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(electron_browser_menu, initializeMenuApi, &nativeBrowserMenuNative)

}