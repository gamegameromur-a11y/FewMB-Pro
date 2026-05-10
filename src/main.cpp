#include "browser.hpp"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    FewMB::Browser::get().init();
    FewMB::Browser::get().run();

    CoUninitialize();
    return 0;
}
