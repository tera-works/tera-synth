// HSynth desktop host - Win32 + OpenGL 3 + Dear ImGui.
//
// usage: hsynth_desktop [--data DIR] [--factory DIR] [--autotest OUTDIR]
//   --data      user data folder (library, presets, settings)   default: desktop/data
//   --factory   factory content (presets + library/factory)     default: android/app/src/main/assets
//   --autotest  run a scripted UI/audio smoke test, write screenshots, a WAV and report.txt to OUTDIR, then exit
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <GL/gl.h>

#include <cstdio>
#include <string>
#include <vector>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "platform.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
HGLRC g_rc = nullptr;
HDC g_dc = nullptr;
int g_width = 1600, g_height = 900;
App* g_app = nullptr;

std::string narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

LRESULT WINAPI wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                g_width = LOWORD(lParam);
                g_height = HIWORD(lParam);
            }
            return 0;
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wParam;
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::string> files;
            for (UINT i = 0; i < n; ++i) {
                wchar_t buf[MAX_PATH * 4];
                const UINT len = DragQueryFileW(drop, i, buf, (UINT)(sizeof buf / sizeof buf[0]));
                files.push_back(narrow(std::wstring(buf, len)));
            }
            DragFinish(drop);
            if (g_app) g_app->onDropFiles(files);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // no ALT menu
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool createGl(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    const int pf = ChoosePixelFormat(dc, &pfd);
    if (pf == 0 || !SetPixelFormat(dc, pf, &pfd)) return false;
    ReleaseDC(hwnd, dc);
    g_dc = GetDC(hwnd);
    g_rc = wglCreateContext(g_dc);
    return g_rc && wglMakeCurrent(g_dc, g_rc);
}

void setVsync(int interval) {
    using Fn = BOOL(WINAPI*)(int);
    if (auto f = (Fn)(void*)wglGetProcAddress("wglSwapIntervalEXT")) f(interval);
}

// Save the back buffer as a 24-bit BMP (used by --autotest).
void saveScreenshot(const std::string& path) {
    const int w = g_width, h = g_height;
    std::vector<unsigned char> px((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, 0x80E0 /*GL_BGR*/, GL_UNSIGNED_BYTE, px.data());
    const int rowSize = (w * 3 + 3) & ~3;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + (DWORD)(rowSize * h);
    ih.biSize = sizeof ih;
    ih.biWidth = w;
    ih.biHeight = h;  // bottom-up, matches glReadPixels
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    FILE* f = _wfopen(std::wstring(path.begin(), path.end()).c_str(), L"wb");
    if (!f) return;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    std::vector<unsigned char> row((size_t)rowSize, 0);
    for (int y = 0; y < h; ++y) {
        std::copy(px.begin() + (size_t)y * w * 3, px.begin() + (size_t)(y + 1) * w * 3, row.begin());
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
}

void loadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    const float size = 17.0f * scale;
    ImFont* base = nullptr;
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        base = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", size);
    if (!base) {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&cfg);
    }
    // Japanese glyphs for file names / messages
    const char* jp[] = {"C:\\Windows\\Fonts\\meiryo.ttc", "C:\\Windows\\Fonts\\YuGothM.ttc", "C:\\Windows\\Fonts\\msgothic.ttc"};
    for (const char* f : jp) {
        if (GetFileAttributesA(f) == INVALID_FILE_ATTRIBUTES) continue;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(f, size, &cfg, io.Fonts->GetGlyphRangesJapanese());
        break;
    }
}

void applyStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.FrameRounding = 4;
    st.GrabRounding = 4;
    st.TabRounding = 4;
    st.WindowPadding = ImVec2(10, 8);
    st.FramePadding = ImVec2(8, 5);
    st.ItemSpacing = ImVec2(8, 6);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.067f, 0.078f, 0.10f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.10f, 0.13f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.20f, 0.25f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.17f, 0.20f, 0.25f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.30f, 0.38f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.31f, 0.76f, 0.97f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.31f, 0.76f, 0.97f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.16f, 0.36f, 0.50f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.16f, 0.30f, 0.42f, 1.0f);
    st.ScaleAllSizes(scale);
}

std::string argValue(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i + 1 < argc; ++i)
        if (wcscmp(argv[i], name) == 0) return narrow(argv[i + 1]);
    return {};
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string dataDir = argValue(argc, argv, L"--data");
    std::string factoryDir = argValue(argc, argv, L"--factory");
    const std::string autotest = argValue(argc, argv, L"--autotest");
    if (dataDir.empty()) dataDir = HSYNTH_DEFAULT_DATA_DIR;
    if (factoryDir.empty()) factoryDir = HSYNTH_DEFAULT_FACTORY_DIR;

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = {sizeof wc, CS_OWNDC, wndProc, 0, 0, inst, LoadIconW(inst, L"APPICON"), LoadCursor(nullptr, IDC_ARROW),
                      nullptr, nullptr, L"HSynthDesktop", nullptr};
    RegisterClassExW(&wc);
    const float sysScale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"HSynth (desktop)", WS_OVERLAPPEDWINDOW, 60, 40, (int)(1500 * sysScale),
                              (int)(900 * sysScale), nullptr, nullptr, inst, nullptr);
    if (!createGl(hwnd)) {
        MessageBoxW(hwnd, L"OpenGL could not be initialised.", L"HSynth", MB_ICONERROR);
        return 1;
    }
    setVsync(1);
    ShowWindow(hwnd, autotest.empty() ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    DragAcceptFiles(hwnd, TRUE);

    App app(utf8ToPath(dataDir), utf8ToPath(factoryDir));
    g_app = &app;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // keyboard navigation stays off: letter keys play notes
    ImGui_ImplWin32_InitForOpenGL(hwnd);
    ImGui_ImplOpenGL3_Init();

    float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    app.init(scale);
    if (app.uiScaleOverride() > 0) scale = app.uiScaleOverride();
    app.setDpiScale(scale);
    loadFonts(scale);
    applyStyle(scale);
    if (!autotest.empty()) app.startAutotest(utf8ToPath(autotest));

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(hwnd)) {
            Sleep(10);
            continue;
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        glViewport(0, 0, g_width, g_height);
        glClearColor(0.067f, 0.078f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        const std::string shot = app.takeScreenshotRequest();
        if (!shot.empty()) saveScreenshot(shot);
        SwapBuffers(g_dc);
        if (app.wantsQuit()) done = true;
    }

    app.shutdown();
    g_app = nullptr;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(g_rc);
    ReleaseDC(hwnd, g_dc);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    LocalFree(argv);
    return 0;
}
