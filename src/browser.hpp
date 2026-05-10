#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define WINVER       0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <WebView2.h>
#include <winhttp.h>
#include <Urlmon.h>

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cstdint>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "shell32.lib")

namespace FewMB {

// ─── Window class names ───────────────────────────────────────────────────────
inline constexpr wchar_t kMainClass[]      = L"FewMBrowser";
inline constexpr wchar_t kTabClass[]       = L"FewMTabBar";
inline constexpr wchar_t kAssistClass[]    = L"FewMAssistant";
inline constexpr wchar_t kSettingsClass[]  = L"FewMSettings";
inline constexpr wchar_t kDownloadsClass[] = L"FewMDownloads";
inline constexpr wchar_t kMenuClass[]      = L"FewMMenu";

// ─── Layout constants ─────────────────────────────────────────────────────────
inline constexpr int kToolbarH = 48;
inline constexpr int kTabBarH  = 36;
inline constexpr int kStatusH  = 24;
inline constexpr int kTabW     = 160;
inline constexpr int kTabGap   = 4;
inline constexpr int kBtnSz    = 32;

// ─── Data types ───────────────────────────────────────────────────────────────
struct Bookmark {
    int         id{};
    std::wstring url;
    std::wstring title;
    std::wstring folder;
    int64_t     createdAt{};
    int         visitCount{};
};

struct HistoryItem {
    int          id{};
    std::wstring url;
    std::wstring title;
    int64_t      visitedAt{};
    int          visitCount{};
};

struct DownloadItem {
    std::string  id;
    std::wstring url;
    std::wstring filename;
    std::wstring savePath;
    int64_t      totalBytes{};
    int64_t      downloadedBytes{};
    double       progress{};
    int          status{};   // 0=pending 1=active 2=paused 3=done 4=error
    int64_t      startTime{};
};

// ─── Tab ──────────────────────────────────────────────────────────────────────
struct Tab {
    explicit Tab(int i) : id(i) {}
    ~Tab() { if (webView) { webView->Close(); webView->Release(); } }

    Tab(const Tab&)            = delete;
    Tab& operator=(const Tab&) = delete;

    int              id{};
    std::wstring     url;
    std::wstring     title;
    bool             isLoading{false};
    double           progress{0.0};
    bool             canGoBack{false};
    bool             canGoForward{false};
    IWebView2WebView* webView{nullptr};
};

// ─── Theme ────────────────────────────────────────────────────────────────────
enum class Theme : int { System = 0, Light = 1, Dark = 2 };

// ─── Browser ─────────────────────────────────────────────────────────────────
class Browser {
public:
    static Browser& get();

    Browser(const Browser&)            = delete;
    Browser& operator=(const Browser&) = delete;

    // Lifecycle
    void init();
    void run();
    void shutdown();

    // Tabs
    Tab*  createTab(const std::wstring& url = L"");
    void  closeTab(int id);
    Tab*  getTab(int id);
    Tab*  getActiveTab();
    void  setActiveTab(int id);
    const std::vector<std::shared_ptr<Tab>>& getTabs() const { return tabs_; }
    int   getActiveTabId() const { return activeTabId_; }

    // Navigation
    void navigate(const std::wstring& input);
    void navigateBack();
    void navigateForward();
    void refresh();

    // Panels
    void showAssistant();   void hideAssistant();
    void showSettings();    void hideSettings();
    void showDownloads();   void hideDownloads();
    void showBookmarks();   void hideBookmarks();
    void showHistory();     void hideHistory();
    void showMenu();

    bool isAssistantVisible()  const { return panelFlags_.assistant; }
    bool isSettingsVisible()   const { return panelFlags_.settings;  }
    bool isDownloadsVisible()  const { return panelFlags_.downloads; }
    bool isBookmarksVisible()  const { return panelFlags_.bookmarks; }
    bool isHistoryVisible()    const { return panelFlags_.history;   }

    // Context menu
    void showContextMenu(int x, int y);

    // Split view
    void setSplitMode(int mode);
    int  getSplitMode() const { return splitMode_; }

    // Theme
    void  setTheme(Theme t);
    Theme getTheme()   const { return theme_; }
    bool  isDarkMode() const;

    // Accessors
    HWND getWindow()  const { return hwnd_; }
    HWND getTabBar()  const { return tabBarWnd_; }

    // Updates
    void checkForUpdates();
    bool isUpdateAvailable()         const { return updateAvailable_; }
    std::wstring getLatestVersion()  const { return latestVersion_;   }
    void downloadAndInstallUpdate();

    // JS execution
    void executeJs(const std::wstring& script);

    // Config
    std::wstring getAppDataPath();
    void saveConfig();
    void loadConfig();

    // Bookmarks
    void addBookmark(const std::wstring& url, const std::wstring& title, const std::wstring& folder = L"");
    void removeBookmark(int id);
    std::vector<Bookmark> getBookmarks() const;
    std::vector<Bookmark> searchBookmarks(const std::wstring& query) const;

    // History
    void addToHistory(const std::wstring& url, const std::wstring& title);
    std::vector<HistoryItem> getHistory(int limit = 100) const;
    std::vector<HistoryItem> searchHistory(const std::wstring& query) const;
    void clearHistory(int64_t before = 0);

    // Downloads
    std::string startDownload(const std::wstring& url);
    void pauseDownload(const std::string& id);
    void resumeDownload(const std::string& id);
    void cancelDownload(const std::string& id);
    std::vector<DownloadItem> getDownloads() const;

    // Assistant
    std::wstring processAssistantMessage(const std::wstring& msg);

private:
    Browser();
    ~Browser();

    // Window procedures
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK TabBarProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK AssistantProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK SettingsProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK DownloadsProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK MenuProc(HWND, UINT, WPARAM, LPARAM);

    // Initialisation helpers
    void registerWindowClasses();
    void createMainWindow();
    void createTabBar();
    void createToolbar();

    // Update helpers
    void updateLayout();
    void updateTabBar();
    void updateTitleBar();
    void updateAddressBar();
    void updateStatusBar();
    void applyTheme();

    // WebView2 helpers
    bool createWebViewForTab(Tab* tab, const std::wstring& url);
    void attachWebViewCallbacks(Tab* tab);

    // URL helpers
    std::wstring resolveUrl(const std::wstring& input) const;

    // String utils
    static std::string  w2a(const std::wstring& w);
    static std::wstring a2w(const std::string& a);

    // Logging
    void log(const std::string& msg, int level = 0);
    std::wstring timestamp() const;

    // ── Windows ──────────────────────────────────────────────────────────────
    HWND hwnd_          = nullptr;
    HWND tabBarWnd_     = nullptr;
    HWND addressBar_    = nullptr;
    HWND backBtn_       = nullptr;
    HWND forwardBtn_    = nullptr;
    HWND refreshBtn_    = nullptr;
    HWND homeBtn_       = nullptr;
    HWND menuBtn_       = nullptr;
    HWND assistantBtn_  = nullptr;
    HWND bookmarksBtn_  = nullptr;
    HWND downloadsBtn_  = nullptr;
    HWND settingsBtn_   = nullptr;
    HWND contentWnd_    = nullptr;
    HWND statusWnd_     = nullptr;
    HWND assistantWnd_  = nullptr;
    HWND settingsWnd_   = nullptr;
    HWND downloadsWnd_  = nullptr;

    // ── Tabs ─────────────────────────────────────────────────────────────────
    std::vector<std::shared_ptr<Tab>> tabs_;
    std::mutex                        tabsMutex_;
    std::atomic<int>                  nextTabId_{1};
    std::atomic<int>                  activeTabId_{0};
    std::atomic<bool>                 running_{false};

    // ── Panel visibility ──────────────────────────────────────────────────────
    struct PanelFlags {
        bool assistant  = false;
        bool settings   = false;
        bool downloads  = false;
        bool bookmarks  = false;
        bool history    = false;
    } panelFlags_;

    // ── Appearance ────────────────────────────────────────────────────────────
    Theme theme_     = Theme::Dark;
    int   splitMode_ = 0;

    // ── Update ────────────────────────────────────────────────────────────────
    bool         updateAvailable_{false};
    std::wstring latestVersion_;

    // ── Data ──────────────────────────────────────────────────────────────────
    std::vector<Bookmark>    bookmarks_;
    std::vector<HistoryItem> history_;
    std::vector<DownloadItem> downloads_;

    // ── Assistant ─────────────────────────────────────────────────────────────
    std::wstring assistantHistory_;

    // ── GDI resources ────────────────────────────────────────────────────────
    HFONT  uiFont_   = nullptr;
    HBRUSH bgBrush_  = nullptr;
};

} // namespace FewMB
