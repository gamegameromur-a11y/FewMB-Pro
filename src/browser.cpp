#include "browser.hpp"

#include <thread>
#include <random>
#include <cctype>

namespace FewMB {

// ─── Singleton ────────────────────────────────────────────────────────────────
Browser& Browser::get() {
    static Browser instance;
    return instance;
}

Browser::Browser() = default;

Browser::~Browser() {
    shutdown();
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────
void Browser::init() {
    log("FewMB initializing");

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    loadConfig();
    registerWindowClasses();
    createMainWindow();
    createTabBar();
    createToolbar();
    applyTheme();
    checkForUpdates();

    log("FewMB ready");
}

void Browser::run() {
    running_ = true;
    MSG msg{};
    while (running_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void Browser::shutdown() {
    if (!running_.exchange(false)) return;
    saveConfig();
    {
        std::lock_guard lock(tabsMutex_);
        tabs_.clear();          // Tab dtor closes + releases WebView2
    }
    if (uiFont_)  { DeleteObject(uiFont_);  uiFont_  = nullptr; }
    if (bgBrush_) { DeleteObject(bgBrush_); bgBrush_ = nullptr; }
    log("FewMB shutdown");
}

// ─── Window registration ──────────────────────────────────────────────────────
void Browser::registerWindowClasses() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    auto reg = [&](const wchar_t* name, WNDPROC proc, COLORREF bg, UINT style = 0) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style        = style;
        wc.lpfnWndProc  = proc;
        wc.hInstance    = hInst;
        wc.lpszClassName= name;
        wc.hIcon        = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hCursor      = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground= CreateSolidBrush(bg);
        RegisterClassExW(&wc);
    };

    reg(kMainClass,      WndProc,       RGB(30,30,30));
    reg(kTabClass,       TabBarProc,    RGB(25,25,25));
    reg(kAssistClass,    AssistantProc, RGB(35,35,35));
    reg(kSettingsClass,  SettingsProc,  RGB(40,40,40));
    reg(kDownloadsClass, DownloadsProc, RGB(40,40,40));
    reg(kMenuClass,      MenuProc,      RGB(45,45,45), CS_DROPSHADOW);
}

// ─── Window creation ──────────────────────────────────────────────────────────
void Browser::createMainWindow() {
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    const int w  = sw * 85 / 100;
    const int h  = sh * 85 / 100;

    hwnd_ = CreateWindowExW(
        0, kMainClass, L"FewMB",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        (sw - w) / 2, (sh - h) / 2, w, h,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);

    if (!hwnd_) { log("Failed to create main window", 4); return; }

    // Content area
    contentWnd_ = CreateWindowExW(
        0, L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, kTabBarH + kToolbarH, w, h - kTabBarH - kToolbarH - kStatusH,
        hwnd_, reinterpret_cast<HMENU>(200), GetModuleHandleW(nullptr), nullptr);

    // Status bar
    statusWnd_ = CreateWindowExW(
        0, L"STATIC", L"Ready",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, h - kStatusH, w, kStatusH,
        hwnd_, reinterpret_cast<HMENU>(201), GetModuleHandleW(nullptr), nullptr);

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}

void Browser::createTabBar() {
    RECT r; GetClientRect(hwnd_, &r);

    tabBarWnd_ = CreateWindowExW(
        0, kTabClass, nullptr,
        WS_CHILD | WS_VISIBLE,
        0, 0, r.right, kTabBarH,
        hwnd_, reinterpret_cast<HMENU>(300), GetModuleHandleW(nullptr), nullptr);

    SetWindowLongPtrW(tabBarWnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}

void Browser::createToolbar() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    RECT r; GetClientRect(hwnd_, &r);

    constexpr DWORD BS = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP;
    constexpr DWORD ES = WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP;

    int x = 8, y = kTabBarH + 8;

    auto btn = [&](const wchar_t* lbl, int id, int w = kBtnSz) -> HWND {
        HWND h = CreateWindowExW(0, L"BUTTON", lbl, BS,
            x, y, w, kBtnSz, hwnd_, reinterpret_cast<HMENU>(id), hInst, nullptr);
        x += w + 4;
        return h;
    };

    backBtn_     = btn(L"\x2190", 1);
    forwardBtn_  = btn(L"\x2192", 2);
    refreshBtn_  = btn(L"\x21BB", 3);
    homeBtn_     = btn(L"\x2302", 4);

    // Address bar stretches between nav buttons and action buttons
    const int addrLeft = x + 4;
    const int addrRight = r.right - (5 * (kBtnSz + 4)) - 16;
    addressBar_ = CreateWindowExW(0, L"EDIT", nullptr, ES,
        addrLeft, kTabBarH + 10, addrRight - addrLeft, kBtnSz,
        hwnd_, reinterpret_cast<HMENU>(100), hInst, nullptr);

    // Address bar subclass — handle Enter key
    SetWindowSubclass(addressBar_,
        [](HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) -> LRESULT {
            if (m == WM_KEYDOWN && w == VK_RETURN) {
                wchar_t buf[2048]{};
                GetWindowTextW(h, buf, 2048);
                Browser::get().navigate(buf);
                return 0;
            }
            return DefSubclassProc(h, m, w, l);
        }, 0, 0);

    // Right-side action buttons (right-to-left)
    x = r.right - 8;
    auto rbtn = [&](const wchar_t* lbl, int id) -> HWND {
        x -= kBtnSz;
        HWND h = CreateWindowExW(0, L"BUTTON", lbl, BS,
            x, y, kBtnSz, kBtnSz, hwnd_, reinterpret_cast<HMENU>(id), hInst, nullptr);
        x -= 4;
        return h;
    };

    settingsBtn_  = rbtn(L"\x2699", 10);
    bookmarksBtn_ = rbtn(L"\x2605", 11);
    downloadsBtn_ = rbtn(L"\x2193", 12);
    assistantBtn_ = rbtn(L"\x2728", 13);
    menuBtn_      = rbtn(L"\x2630", 14);

    // Create initial tab
    createTab(L"https://www.google.com");
}

// ─── Layout ───────────────────────────────────────────────────────────────────
void Browser::updateLayout() {
    if (!hwnd_) return;
    RECT r; GetClientRect(hwnd_, &r);

    // Tab bar
    SetWindowPos(tabBarWnd_, nullptr, 0, 0, r.right, kTabBarH, SWP_NOZORDER | SWP_NOACTIVATE);

    // Address bar stretches
    const int addrLeft  = 8 + 4 * (kBtnSz + 4) + 4;
    const int addrRight = r.right - 5 * (kBtnSz + 4) - 12;
    SetWindowPos(addressBar_, nullptr, addrLeft, kTabBarH + 10, addrRight - addrLeft, kBtnSz,
        SWP_NOZORDER | SWP_NOACTIVATE);

    // Right-side toolbar buttons
    int bx = r.right - 8;
    for (HWND* btn : { &settingsBtn_, &bookmarksBtn_, &downloadsBtn_, &assistantBtn_, &menuBtn_ }) {
        bx -= kBtnSz;
        SetWindowPos(*btn, nullptr, bx, kTabBarH + 8, kBtnSz, kBtnSz, SWP_NOZORDER | SWP_NOACTIVATE);
        bx -= 4;
    }

    // Content
    const int contentY = kTabBarH + kToolbarH;
    const int contentH = r.bottom - contentY - kStatusH;
    SetWindowPos(contentWnd_, nullptr, 0, contentY, r.right, contentH, SWP_NOZORDER | SWP_NOACTIVATE);

    // Status
    SetWindowPos(statusWnd_, nullptr, 0, r.bottom - kStatusH, r.right, kStatusH, SWP_NOZORDER | SWP_NOACTIVATE);

    // WebView2 bounds
    if (Tab* tab = getActiveTab(); tab && tab->webView) {
        RECT cr; GetClientRect(contentWnd_, &cr);
        tab->webView->put_Bounds(cr);
    }
}

// ─── Theme ────────────────────────────────────────────────────────────────────
bool Browser::isDarkMode() const {
    if (theme_ == Theme::Light) return false;
    if (theme_ == Theme::Dark)  return true;
    DWORD val = 1;
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, 0, nullptr, 0); // just touch it
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD sz = sizeof(val);
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(&val), &sz);
        RegCloseKey(key);
    }
    return val == 0;  // 0 = dark
}

void Browser::setTheme(Theme t) {
    theme_ = t;
    applyTheme();
    saveConfig();
}

void Browser::applyTheme() {
    const bool dark = isDarkMode();
    const COLORREF bgCol   = dark ? RGB(30,30,30) : RGB(240,240,240);
    const COLORREF tabCol  = dark ? RGB(25,25,25) : RGB(220,220,220);
    const COLORREF statCol = dark ? RGB(45,45,45) : RGB(230,230,230);

    if (bgBrush_) DeleteObject(bgBrush_);
    bgBrush_ = CreateSolidBrush(bgCol);

    SetClassLongPtrW(hwnd_,      GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(CreateSolidBrush(bgCol)));
    SetClassLongPtrW(tabBarWnd_, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(CreateSolidBrush(tabCol)));

    InvalidateRect(hwnd_, nullptr, TRUE);
    if (tabBarWnd_) InvalidateRect(tabBarWnd_, nullptr, TRUE);
}

// ─── WndProc ──────────────────────────────────────────────────────────────────
LRESULT CALLBACK Browser::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Browser* app = reinterpret_cast<Browser*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (!app && msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<Browser*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_SIZE:
        app->updateLayout();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1:  app->navigateBack();    break;
        case 2:  app->navigateForward(); break;
        case 3:  app->refresh();         break;
        case 4:  app->navigate(L"https://www.google.com"); break;
        case 10: app->panelFlags_.settings  ? app->hideSettings()  : app->showSettings();  break;
        case 11: app->panelFlags_.bookmarks ? app->hideBookmarks() : app->showBookmarks(); break;
        case 12: app->panelFlags_.downloads ? app->hideDownloads() : app->showDownloads(); break;
        case 13: app->panelFlags_.assistant ? app->hideAssistant() : app->showAssistant(); break;
        case 14: app->showMenu(); break;
        case 1000: app->createTab(); break;
        }
        return 0;

    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            switch (wParam) {
            case 'T': app->createTab(); break;
            case 'W': if (Tab* t = app->getActiveTab()) app->closeTab(t->id); break;
            case 'L': SetFocus(app->addressBar_); SendMessageW(app->addressBar_, EM_SETSEL, 0, -1); break;
            case 'H': app->showHistory();   break;
            case 'D': app->showDownloads(); break;
            case 'P': app->showSettings();  break;
            case 'R': app->refresh();       break;
            }
        }
        if (wParam == VK_F5)  app->refresh();
        return 0;

    case WM_RBUTTONDOWN:
        app->showContextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const bool dark = app->isDarkMode();
        SetBkColor(dc,   dark ? RGB(45,45,45)    : RGB(255,255,255));
        SetTextColor(dc, dark ? RGB(220,220,220) : RGB(30,30,30));
        return reinterpret_cast<LRESULT>(app->bgBrush_);
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        // toolbar background
        RECT toolbarR{ 0, kTabBarH, ps.rcPaint.right, kTabBarH + kToolbarH };
        FillRect(ps.hdc, &toolbarR,
            CreateSolidBrush(app->isDarkMode() ? RGB(35,35,35) : RGB(245,245,245)));
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        app->shutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── Tab bar ─────────────────────────────────────────────────────────────────
LRESULT CALLBACK Browser::TabBarProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Browser* app = reinterpret_cast<Browser*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_LBUTTONDOWN: {
        const int mx = GET_X_LPARAM(lParam);
        int tx = kTabGap;
        std::lock_guard lock(app->tabsMutex_);
        for (auto& tab : app->tabs_) {
            if (mx >= tx && mx < tx + kTabW - 20)
                { app->setActiveTab(tab->id); return 0; }
            if (mx >= tx + kTabW - 20 && mx < tx + kTabW)
                { app->closeTab(tab->id); return 0; }
            tx += kTabW + kTabGap;
        }
        // "+" button
        if (mx >= tx && mx < tx + 28) app->createTab();
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        const bool dark = app->isDarkMode();

        HBRUSH bg = CreateSolidBrush(dark ? RGB(25,25,25) : RGB(220,220,220));
        FillRect(dc, &r, bg);
        DeleteObject(bg);

        SetBkMode(dc, TRANSPARENT);
        int tx = kTabGap;

        {
            std::lock_guard lock(app->tabsMutex_);
            for (auto& tab : app->tabs_) {
                const bool active = (tab->id == app->activeTabId_);
                RECT tr{ tx, 2, tx + kTabW, r.bottom - 2 };

                HBRUSH tabBg = CreateSolidBrush(
                    active ? (dark ? RGB(40,40,40) : RGB(255,255,255))
                           : (dark ? RGB(30,30,30) : RGB(235,235,235)));
                FillRect(dc, &tr, tabBg);
                DeleteObject(tabBg);

                // Title
                SetTextColor(dc, active ? (dark ? RGB(255,255,255) : RGB(20,20,20))
                                        : (dark ? RGB(160,160,160) : RGB(100,100,100)));
                std::wstring title = tab->title.empty() ? L"New Tab" : tab->title;
                if (title.size() > 20) title = title.substr(0, 17) + L"...";
                RECT textR{ tx + 8, 0, tx + kTabW - 28, r.bottom };
                DrawTextW(dc, title.c_str(), -1, &textR, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

                // Close "x"
                SetTextColor(dc, dark ? RGB(180,180,180) : RGB(100,100,100));
                RECT closeR{ tx + kTabW - 22, 0, tx + kTabW - 4, r.bottom };
                DrawTextW(dc, L"\xD7", -1, &closeR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                // Active underline accent
                if (active) {
                    HPEN pen = CreatePen(PS_SOLID, 2, RGB(100,150,255));
                    HPEN old = reinterpret_cast<HPEN>(SelectObject(dc, pen));
                    MoveToEx(dc, tx,          r.bottom - 2, nullptr);
                    LineTo  (dc, tx + kTabW,  r.bottom - 2);
                    SelectObject(dc, old);
                    DeleteObject(pen);
                }

                tx += kTabW + kTabGap;
            }
        }

        // "+" button
        SetTextColor(dc, dark ? RGB(200,200,200) : RGB(80,80,80));
        RECT plusR{ tx + 4, 0, tx + 24, r.bottom };
        DrawTextW(dc, L"+", -1, &plusR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── Tab management ───────────────────────────────────────────────────────────
Tab* Browser::createTab(const std::wstring& url) {
    auto tab = std::make_shared<Tab>(nextTabId_++);
    tab->url = url.empty() ? L"about:blank" : url;

    {
        std::lock_guard lock(tabsMutex_);
        tabs_.push_back(tab);
    }

    if (activeTabId_ == 0) activeTabId_ = tab->id;
    createWebViewForTab(tab.get(), tab->url);
    updateTabBar();

    log("Tab created: " + std::to_string(tab->id));
    return tab.get();
}

bool Browser::createWebViewForTab(Tab* tab, const std::wstring& url) {
    if (!contentWnd_) return false;

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, tab, url](HRESULT envHr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(envHr) || !env) return envHr;

                env->CreateCoreWebView2Controller(
                    contentWnd_,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this, tab, url](HRESULT ctrlHr, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (FAILED(ctrlHr) || !ctrl) return ctrlHr;

                            ICoreWebView2* wv2 = nullptr;
                            ctrl->get_CoreWebView2(&wv2);
                            // Store as legacy interface for compat
                            wv2->QueryInterface(IID_PPV_ARGS(&tab->webView));
                            if (wv2) wv2->Release();

                            RECT cr; GetClientRect(contentWnd_, &cr);
                            ctrl->put_Bounds(cr);
                            ctrl->put_IsVisible(tab->id == activeTabId_);

                            attachWebViewCallbacks(tab);

                            if (!url.empty() && url != L"about:blank")
                                tab->webView->Navigate(url.c_str());

                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    return SUCCEEDED(hr);
}

void Browser::attachWebViewCallbacks(Tab* tab) {
    if (!tab || !tab->webView) return;

    EventRegistrationToken tok{};

    tab->webView->add_NavigationStarting(
        Microsoft::WRL::Callback<IWebView2NavigationStartingEventHandler>(
            [this, tab](IWebView2WebView*, IWebView2NavigationStartingEventArgs* args) -> HRESULT {
                tab->isLoading = true;
                wchar_t* uri = nullptr;
                args->get_Uri(&uri);
                if (uri) {
                    tab->url = uri;
                    CoTaskMemFree(uri);
                    if (tab->id == activeTabId_) updateAddressBar();
                }
                updateStatusBar();
                return S_OK;
            }).Get(), &tok);

    tab->webView->add_NavigationCompleted(
        Microsoft::WRL::Callback<IWebView2NavigationCompletedEventHandler>(
            [this, tab](IWebView2WebView*, IWebView2NavigationCompletedEventArgs*) -> HRESULT {
                tab->isLoading = false;
                if (tab->id == activeTabId_) {
                    wchar_t* title = nullptr;
                    tab->webView->get_Title(&title);
                    if (title) {
                        tab->title = title;
                        CoTaskMemFree(title);
                        updateTitleBar();
                    }
                    addToHistory(tab->url, tab->title);
                    updateTabBar();
                }
                updateStatusBar();
                return S_OK;
            }).Get(), &tok);

    tab->webView->add_TitleChanged(
        Microsoft::WRL::Callback<IWebView2TitleChangedEventHandler>(
            [this, tab](IWebView2WebView*, IWebView2TitleChangedEventArgs* args) -> HRESULT {
                wchar_t* title = nullptr;
                args->get_Title(&title);
                if (title) {
                    tab->title = title;
                    CoTaskMemFree(title);
                    if (tab->id == activeTabId_) updateTitleBar();
                    updateTabBar();
                }
                return S_OK;
            }).Get(), &tok);
}

void Browser::closeTab(int id) {
    int nextId = 0;
    {
        std::lock_guard lock(tabsMutex_);
        auto it = std::find_if(tabs_.begin(), tabs_.end(),
                               [id](auto& t){ return t->id == id; });
        if (it == tabs_.end()) return;
        tabs_.erase(it);
        log("Tab closed: " + std::to_string(id));

        if (!tabs_.empty()) nextId = tabs_.front()->id;
    }

    if (tabs_.empty()) {
        createTab();
    } else if (activeTabId_ == id) {
        activeTabId_ = nextId;
        updateAddressBar();
        updateLayout();
    }
    updateTabBar();
}

Tab* Browser::getTab(int id) {
    std::lock_guard lock(tabsMutex_);
    for (auto& t : tabs_) if (t->id == id) return t.get();
    return nullptr;
}

Tab* Browser::getActiveTab() {
    return getTab(activeTabId_);
}

void Browser::setActiveTab(int id) {
    // Hide current WebView
    if (Tab* prev = getActiveTab(); prev && prev->webView)
        prev->webView->put_IsVisible(FALSE);

    activeTabId_ = id;

    // Show new WebView
    if (Tab* next = getActiveTab(); next && next->webView) {
        next->webView->put_IsVisible(TRUE);
        RECT cr; GetClientRect(contentWnd_, &cr);
        next->webView->put_Bounds(cr);
    }

    updateAddressBar();
    updateTitleBar();
    updateTabBar();
    updateStatusBar();
}

// ─── Navigation ───────────────────────────────────────────────────────────────
std::wstring Browser::resolveUrl(const std::wstring& input) const {
    if (input.find(L"://")  != std::wstring::npos) return input;
    if (input.find(L"about:") == 0)                return input;
    if (input.find(L'.')     != std::wstring::npos) return L"https://" + input;
    // Treat as search query
    std::wstring encoded;
    for (wchar_t c : input) {
        if (iswalnum(c) || c == L'-' || c == L'_' || c == L'.' || c == L'~')
            encoded += c;
        else {
            wchar_t buf[8];
            swprintf(buf, 8, L"%%%02X", static_cast<unsigned>(c));
            encoded += buf;
        }
    }
    return L"https://www.google.com/search?q=" + encoded;
}

void Browser::navigate(const std::wstring& input) {
    const std::wstring url = resolveUrl(input);
    if (Tab* tab = getActiveTab(); tab) {
        tab->url = url;
        SetWindowTextW(addressBar_, url.c_str());
        if (tab->webView) tab->webView->Navigate(url.c_str());
    }
}

void Browser::navigateBack()    { if (Tab* t = getActiveTab(); t && t->webView) t->webView->GoBack();    }
void Browser::navigateForward() { if (Tab* t = getActiveTab(); t && t->webView) t->webView->GoForward(); }
void Browser::refresh()         { if (Tab* t = getActiveTab(); t && t->webView) t->webView->Reload();    }
void Browser::executeJs(const std::wstring& script) {
    if (Tab* t = getActiveTab(); t && t->webView)
        t->webView->ExecuteScript(script.c_str(), nullptr);
}

// ─── Address / title / status update ─────────────────────────────────────────
void Browser::updateAddressBar() {
    if (Tab* t = getActiveTab())
        SetWindowTextW(addressBar_, t->url.c_str());
}

void Browser::updateTitleBar() {
    if (Tab* t = getActiveTab())
        SetWindowTextW(hwnd_, (L"FewMB \u2014 " + t->title).c_str());
}

void Browser::updateTabBar() {
    if (tabBarWnd_) InvalidateRect(tabBarWnd_, nullptr, TRUE);
}

void Browser::updateStatusBar() {
    if (Tab* t = getActiveTab())
        SetWindowTextW(statusWnd_, t->isLoading ? L"Loading..." : L"Ready");
    else
        SetWindowTextW(statusWnd_, L"Ready");
}

// ─── Panels ───────────────────────────────────────────────────────────────────
void Browser::showAssistant() {
    panelFlags_.assistant = true;
    if (!assistantWnd_) {
        assistantWnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            kAssistClass, L"FewMB Assistant",
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_THICKFRAME,
            100, 100, 480, 620,
            hwnd_, nullptr, GetModuleHandleW(nullptr), this);
        SetWindowLongPtrW(assistantWnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    ShowWindow(assistantWnd_, SW_SHOW);
    SetForegroundWindow(assistantWnd_);
}
void Browser::hideAssistant() {
    panelFlags_.assistant = false;
    if (assistantWnd_) ShowWindow(assistantWnd_, SW_HIDE);
}

void Browser::showSettings() {
    panelFlags_.settings = true;
    if (!settingsWnd_) {
        settingsWnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW, kSettingsClass, L"Settings",
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
            200, 150, 420, 380,
            hwnd_, nullptr, GetModuleHandleW(nullptr), this);
        SetWindowLongPtrW(settingsWnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    ShowWindow(settingsWnd_, SW_SHOW);
}
void Browser::hideSettings() {
    panelFlags_.settings = false;
    if (settingsWnd_) ShowWindow(settingsWnd_, SW_HIDE);
}

void Browser::showDownloads() {
    panelFlags_.downloads = true;
    if (!downloadsWnd_) {
        downloadsWnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW, kDownloadsClass, L"Downloads",
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
            200, 150, 500, 420,
            hwnd_, nullptr, GetModuleHandleW(nullptr), this);
        SetWindowLongPtrW(downloadsWnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    ShowWindow(downloadsWnd_, SW_SHOW);
}
void Browser::hideDownloads() {
    panelFlags_.downloads = false;
    if (downloadsWnd_) ShowWindow(downloadsWnd_, SW_HIDE);
}

void Browser::showBookmarks() { panelFlags_.bookmarks = true;  log("Bookmarks opened"); }
void Browser::hideBookmarks() { panelFlags_.bookmarks = false; }
void Browser::showHistory()   { panelFlags_.history   = true;  log("History opened");   }
void Browser::hideHistory()   { panelFlags_.history   = false; }

void Browser::showMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1,  L"New Tab\tCtrl+T");
    AppendMenuW(menu, MF_SEPARATOR,  0, nullptr);
    AppendMenuW(menu, MF_STRING, 3,  L"Bookmarks");
    AppendMenuW(menu, MF_STRING, 4,  L"History\tCtrl+H");
    AppendMenuW(menu, MF_STRING, 5,  L"Downloads\tCtrl+D");
    AppendMenuW(menu, MF_STRING, 6,  L"Settings\tCtrl+P");
    AppendMenuW(menu, MF_SEPARATOR,  0, nullptr);
    AppendMenuW(menu, MF_STRING, 7,  L"Exit");

    POINT pt; GetCursorPos(&pt);
    int sel = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    switch (sel) {
    case 1: createTab(); break;
    case 3: showBookmarks(); break;
    case 4: showHistory();   break;
    case 5: showDownloads(); break;
    case 6: showSettings();  break;
    case 7: PostMessageW(hwnd_, WM_CLOSE, 0, 0); break;
    }
}

void Browser::showContextMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1001, L"Back\tAlt+Left");
    AppendMenuW(menu, MF_STRING, 1002, L"Forward\tAlt+Right");
    AppendMenuW(menu, MF_STRING, 1003, L"Refresh\tF5");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1004, L"Select All");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1007, L"Inspect\tCtrl+Shift+I");
    AppendMenuW(menu, MF_STRING, 1008, L"View Source\tCtrl+U");

    POINT pt{ x, y }; ClientToScreen(hwnd_, &pt);
    int sel = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    switch (sel) {
    case 1001: navigateBack();    break;
    case 1002: navigateForward(); break;
    case 1003: refresh();         break;
    }
}

// ─── Split mode ───────────────────────────────────────────────────────────────
void Browser::setSplitMode(int mode) {
    splitMode_ = mode;
    updateLayout();
}

// ─── Assistant window proc ────────────────────────────────────────────────────
LRESULT CALLBACK Browser::AssistantProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Browser* app = reinterpret_cast<Browser*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    static HWND chatArea = nullptr, inputArea = nullptr;

    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandleW(nullptr);
        CreateWindowExW(0, L"STATIC", L"FewMB Assistant",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 480, 28, hwnd, nullptr, hi, nullptr);
        chatArea = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            4, 32, 472, 500, hwnd, reinterpret_cast<HMENU>(1), hi, nullptr);
        inputArea = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE,
            4, 536, 400, 56, hwnd, reinterpret_cast<HMENU>(2), hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Send",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            408, 536, 68, 56, hwnd, reinterpret_cast<HMENU>(3), hi, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 3 && app) {
            wchar_t buf[4096]{};
            GetWindowTextW(inputArea, buf, 4096);
            if (wcslen(buf) > 0) {
                std::wstring input = buf;
                app->assistantHistory_ += L"You: " + input + L"\n\n";
                std::wstring resp = app->processAssistantMessage(input);
                app->assistantHistory_ += L"FewMB: " + resp + L"\n\n";
                SetWindowTextW(chatArea, app->assistantHistory_.c_str());
                SetWindowTextW(inputArea, L"");
                SendMessageW(chatArea, WM_VSCROLL, SB_BOTTOM, 0);
            }
        }
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, RGB(35,35,35)); SetTextColor(dc, RGB(220,220,220));
        return reinterpret_cast<LRESULT>(CreateSolidBrush(RGB(35,35,35)));
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        if (app) app->panelFlags_.assistant = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── Settings window proc ─────────────────────────────────────────────────────
LRESULT CALLBACK Browser::SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Browser* app = reinterpret_cast<Browser*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandleW(nullptr);
        CreateWindowExW(0, L"STATIC", L"Appearance",
            WS_CHILD | WS_VISIBLE, 12, 12, 200, 20, hwnd, nullptr, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Dark",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            12, 36, 120, 26, hwnd, reinterpret_cast<HMENU>(101), hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Light",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            12, 66, 120, 26, hwnd, reinterpret_cast<HMENU>(102), hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Follow system",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            12, 96, 180, 26, hwnd, reinterpret_cast<HMENU>(103), hi, nullptr);
        CreateWindowExW(0, L"STATIC", L"Data",
            WS_CHILD | WS_VISIBLE, 12, 136, 200, 20, hwnd, nullptr, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Clear history",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            12, 160, 160, 32, hwnd, reinterpret_cast<HMENU>(201), hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Check for updates",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            12, 200, 160, 32, hwnd, reinterpret_cast<HMENU>(301), hi, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (!app) return 0;
        switch (LOWORD(wParam)) {
        case 101: app->setTheme(Theme::Dark);   break;
        case 102: app->setTheme(Theme::Light);  break;
        case 103: app->setTheme(Theme::System); break;
        case 201: app->clearHistory(); MessageBoxW(hwnd, L"History cleared.", L"FewMB", MB_OK); break;
        case 301: app->checkForUpdates(); break;
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, RGB(40,40,40)); SetTextColor(dc, RGB(220,220,220));
        return reinterpret_cast<LRESULT>(CreateSolidBrush(RGB(40,40,40)));
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        if (app) app->panelFlags_.settings = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── Downloads window proc ───────────────────────────────────────────────────
LRESULT CALLBACK Browser::DownloadsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Browser* app = reinterpret_cast<Browser*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE:
        CreateWindowExW(0, L"STATIC", L"Downloads",
            WS_CHILD | WS_VISIBLE, 12, 12, 200, 24, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Open folder",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            12, 44, 160, 32, hwnd, reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == 1 && app)
            ShellExecuteW(nullptr, L"open", app->getAppDataPath().c_str(), nullptr, nullptr, SW_SHOW);
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, RGB(40,40,40)); SetTextColor(dc, RGB(220,220,220));
        return reinterpret_cast<LRESULT>(CreateSolidBrush(RGB(40,40,40)));
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        if (app) app->panelFlags_.downloads = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── Menu window proc (unused — using TrackPopupMenu instead) ────────────────
LRESULT CALLBACK Browser::MenuProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── Config ───────────────────────────────────────────────────────────────────
std::wstring Browser::getAppDataPath() {
    wchar_t path[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path);
    std::wstring p = std::wstring(path) + L"\\FewMB";
    CreateDirectoryW(p.c_str(), nullptr);
    return p;
}

void Browser::saveConfig() {
    std::wofstream f(getAppDataPath() + L"\\config.ini");
    if (!f) return;
    f << L"theme="     << static_cast<int>(theme_)    << L'\n';
    f << L"splitMode=" << splitMode_                  << L'\n';
}

void Browser::loadConfig() {
    std::wifstream f(getAppDataPath() + L"\\config.ini");
    if (!f) return;
    std::wstring line;
    while (std::getline(f, line)) {
        const auto pos = line.find(L'=');
        if (pos == std::wstring::npos) continue;
        const auto key = line.substr(0, pos);
        const auto val = line.substr(pos + 1);
        if (key == L"theme")     theme_     = static_cast<Theme>(_wtoi(val.c_str()));
        if (key == L"splitMode") splitMode_ = _wtoi(val.c_str());
    }
}

// ─── Bookmarks ────────────────────────────────────────────────────────────────
void Browser::addBookmark(const std::wstring& url, const std::wstring& title, const std::wstring& folder) {
    Bookmark bm;
    bm.id        = static_cast<int>(bookmarks_.size()) + 1;
    bm.url       = url;
    bm.title     = title;
    bm.folder    = folder;
    bm.createdAt = static_cast<int64_t>(GetTickCount64());
    bookmarks_.push_back(std::move(bm));
    log("Bookmark added: " + w2a(url));
}

void Browser::removeBookmark(int id) {
    bookmarks_.erase(
        std::remove_if(bookmarks_.begin(), bookmarks_.end(), [id](auto& b){ return b.id == id; }),
        bookmarks_.end());
}

std::vector<Bookmark> Browser::getBookmarks() const { return bookmarks_; }

std::vector<Bookmark> Browser::searchBookmarks(const std::wstring& q) const {
    std::vector<Bookmark> r;
    for (auto& b : bookmarks_)
        if (b.title.find(q) != std::wstring::npos || b.url.find(q) != std::wstring::npos)
            r.push_back(b);
    return r;
}

// ─── History ─────────────────────────────────────────────────────────────────
void Browser::addToHistory(const std::wstring& url, const std::wstring& title) {
    // Skip duplicates within last 5 entries
    if (!history_.empty() && history_.back().url == url) return;
    HistoryItem hi;
    hi.id        = static_cast<int>(history_.size()) + 1;
    hi.url       = url;
    hi.title     = title;
    hi.visitedAt = static_cast<int64_t>(GetTickCount64());
    history_.push_back(std::move(hi));
    // Keep last 10 000 items
    if (history_.size() > 10000) history_.erase(history_.begin(), history_.begin() + 1000);
}

std::vector<HistoryItem> Browser::getHistory(int limit) const {
    if (limit <= 0 || static_cast<int>(history_.size()) <= limit) return history_;
    return { history_.end() - limit, history_.end() };
}

std::vector<HistoryItem> Browser::searchHistory(const std::wstring& q) const {
    std::vector<HistoryItem> r;
    for (auto& h : history_)
        if (h.title.find(q) != std::wstring::npos || h.url.find(q) != std::wstring::npos)
            r.push_back(h);
    return r;
}

void Browser::clearHistory(int64_t before) {
    if (before == 0) { history_.clear(); return; }
    history_.erase(
        std::remove_if(history_.begin(), history_.end(), [before](auto& h){ return h.visitedAt < before; }),
        history_.end());
}

// ─── Downloads ────────────────────────────────────────────────────────────────
std::string Browser::startDownload(const std::wstring& url) {
    static std::mt19937 rng{ std::random_device{}() };
    const std::string id = "dl_" + std::to_string(rng());
    DownloadItem dl;
    dl.id        = id;
    dl.url       = url;
    dl.status    = 0;
    dl.startTime = static_cast<int64_t>(GetTickCount64());
    downloads_.push_back(std::move(dl));
    log("Download started: " + w2a(url));
    return id;
}

void Browser::pauseDownload(const std::string& id) {
    for (auto& d : downloads_) if (d.id == id) { d.status = 2; break; }
}
void Browser::resumeDownload(const std::string& id) {
    for (auto& d : downloads_) if (d.id == id) { d.status = 1; break; }
}
void Browser::cancelDownload(const std::string& id) {
    downloads_.erase(
        std::remove_if(downloads_.begin(), downloads_.end(), [&id](auto& d){ return d.id == id; }),
        downloads_.end());
}
std::vector<DownloadItem> Browser::getDownloads() const { return downloads_; }

// ─── Update checker ───────────────────────────────────────────────────────────
void Browser::checkForUpdates() {
    std::thread([this]() {
        HINTERNET session = WinHttpOpen(L"FewMB/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
        if (!session) return;

        HINTERNET conn = WinHttpConnect(session, L"api.github.com", 443, 0);
        HINTERNET req  = conn ? WinHttpOpenRequest(conn, L"GET",
            L"/repos/TOREofficial/FewMB/releases/latest",
            nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;

        if (req && WinHttpSendRequest(req, nullptr,0,nullptr,0,0,0)
               && WinHttpReceiveResponse(req, nullptr)) {
            std::string resp;
            char buf[4096]; DWORD bytes;
            while (WinHttpReadData(req, buf, sizeof(buf), &bytes) && bytes)
                resp.append(buf, bytes);

            const auto tagPos = resp.find("\"tag_name\"");
            if (tagPos != std::string::npos) {
                const auto vStart = resp.find("\"v", tagPos);
                const auto vEnd   = vStart != std::string::npos ? resp.find('"', vStart + 2) : std::string::npos;
                if (vEnd != std::string::npos) {
                    latestVersion_  = a2w(resp.substr(vStart + 1, vEnd - vStart - 2));
                    updateAvailable_= (latestVersion_ != L"1.0.0");
                    if (updateAvailable_) log("Update available: " + w2a(latestVersion_));
                }
            }
        }

        if (req)  WinHttpCloseHandle(req);
        if (conn) WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
    }).detach();
}

void Browser::downloadAndInstallUpdate() {
    log("Update download not yet implemented");
}

// ─── Assistant ────────────────────────────────────────────────────────────────
std::wstring Browser::processAssistantMessage(const std::wstring& msg) {
    std::wstring lower = msg;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

    struct Rule { const wchar_t* kw; const wchar_t* resp; };
    static constexpr Rule rules[] = {
        { L"hello",     L"Hello! I'm FewMB Assistant. How can I help you today?" },
        { L"merhaba",   L"Merhaba! FewMB Asistan burada. Nasil yardimci olabilirim?" },
        { L"help",      L"I can assist with: navigation, translations, code, explanations and general questions." },
        { L"yardim",    L"Ceviriler, kod aciklamalari, ozet veya genel sorular icin buradayim." },
        { L"weather",   L"I don't have live weather data, but I can search for it - just ask!" },
        { L"translate", L"Sure! Type: translate [text] to [language]" },
        { L"who are",   L"I'm FewMB Assistant, built into the FewMB browser." },
        { L"kimsin",    L"Ben FewMB Asistan, FewMB tarayicisina entegre yapay zeka asistaniyim." },
    };

    for (auto& r : rules)
        if (lower.find(r.kw) != std::wstring::npos)
            return r.resp;

    // Code detection
    static constexpr const wchar_t* codeKws[] = {
        L"function ", L"def ", L"class ", L"const ", L"var ", L"let ",
        L"if (", L"for (", L"while (", L"return ", L"import ", L"#include" };
    for (auto* kw : codeKws)
        if (msg.find(kw) != std::wstring::npos)
            return L"That looks like code! Ask me to explain, debug, or improve it.";

    return L"I'm here to help - try asking me to translate something, explain a concept, or navigate to a site.";
}

// ─── Logging ──────────────────────────────────────────────────────────────────
void Browser::log(const std::string& msg, int level) {
    const std::wstring path = getAppDataPath() + L"\\fewmb.log";
    std::wofstream f(path, std::ios::app);
    if (!f) return;
    const wchar_t* lvl = (level == 0) ? L"INFO" : (level == 4) ? L"ERROR" : L"WARN";
    f << timestamp() << L" [" << lvl << L"] " << a2w(msg) << L'\n';
}

std::wstring Browser::timestamp() const {
    const auto now = std::chrono::system_clock::now();
    const auto t   = std::chrono::system_clock::to_time_t(now);
    tm local{}; localtime_s(&local, &t);
    wchar_t buf[16]{};
    wcsftime(buf, 16, L"%H:%M:%S", &local);
    return buf;
}

// ─── String helpers ───────────────────────────────────────────────────────────
std::string Browser::w2a(const std::wstring& w) {
    if (w.empty()) return {};
    const int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), sz, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::wstring Browser::a2w(const std::string& a) {
    if (a.empty()) return {};
    const int sz = MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, nullptr, 0);
    std::wstring s(sz, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, a.c_str(), -1, s.data(), sz);
    if (!s.empty() && s.back() == L'\0') s.pop_back();
    return s;
}

} // namespace FewMB
