// HSynth desktop host - macOS: AppKit window + Metal + Dear ImGui.
//
// usage: hsynth_desktop [--data DIR] [--factory DIR] [--autotest OUTDIR] [--mute]
//   same options as the Windows host (see main_win32.cpp).
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include <cstdio>
#include <string>
#include <vector>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"
#include "platform.h"

namespace {
App* g_app = nullptr;
std::string g_dataDir, g_factoryDir, g_autotest;
bool g_mute = false;

void loadFonts(float pixelScale) {
    ImGuiIO& io = ImGui::GetIO();
    const float size = 17.0f * pixelScale;
    ImFont* base = nullptr;
    const char* latin[] = {"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc",
                           "/Library/Fonts/Arial Unicode.ttf"};
    for (const char* f : latin) {
        if (FILE* fp = fopen(f, "rb")) {
            fclose(fp);
            base = io.Fonts->AddFontFromFileTTF(f, size);
            if (base) break;
        }
    }
    if (!base) {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * pixelScale;
        io.Fonts->AddFontDefault(&cfg);
    }
    // Japanese glyphs for file names / messages
    const char* jp[] = {"/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc", "/System/Library/Fonts/Hiragino Sans GB.ttc",
                        "/System/Library/Fonts/AppleSDGothicNeo.ttc"};
    for (const char* f : jp) {
        FILE* fp = fopen(f, "rb");
        if (!fp) continue;
        fclose(fp);
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(f, size, &cfg, io.Fonts->GetGlyphRangesJapanese());
        break;
    }
    io.FontGlobalScale = 1.0f / pixelScale;  // fonts are rasterised at Retina resolution
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

// BGRA8 drawable texture -> 24-bit BMP (used by --autotest).
void saveScreenshot(id<MTLTexture> tex, const std::string& path) {
    const NSUInteger w = tex.width, h = tex.height;
    std::vector<uint8_t> px(w * h * 4);
    [tex getBytes:px.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    const uint32_t rowSize = ((uint32_t)w * 3 + 3) & ~3u;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    auto put16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    auto put32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    fwrite("BM", 1, 2, f);
    put32(54 + rowSize * (uint32_t)h); put32(0); put32(54);
    put32(40); put32((uint32_t)w); put32((uint32_t)h); put16(1); put16(24); put32(0); put32(rowSize * (uint32_t)h);
    put32(2835); put32(2835); put32(0); put32(0);
    std::vector<uint8_t> row(rowSize, 0);
    for (NSUInteger y = 0; y < h; ++y) {
        const uint8_t* src = px.data() + (h - 1 - y) * w * 4;  // BMP is bottom-up
        for (NSUInteger x = 0; x < w; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(row.data(), 1, rowSize, f);
    }
    fclose(f);
}
}  // namespace

// ---------------------------------------------------------------- view

@interface HSynthView : MTKView
@end

@implementation HSynthView
- (instancetype)initWithFrame:(NSRect)frame device:(id<MTLDevice>)device {
    if ((self = [super initWithFrame:frame device:device])) [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    return self;
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender { return NSDragOperationCopy; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray<NSURL*>* urls = [sender.draggingPasteboard readObjectsForClasses:@[NSURL.class]
                                                                     options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    std::vector<std::string> files;
    for (NSURL* u in urls) files.push_back(u.path.UTF8String);
    if (g_app && !files.empty()) g_app->onDropFiles(files);
    return YES;
}
- (BOOL)acceptsFirstResponder { return YES; }
@end

// ---------------------------------------------------------------- renderer

@interface HSynthRenderer : NSObject <MTKViewDelegate>
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@end

@implementation HSynthRenderer
- (void)drawInMTKView:(MTKView*)view {
    if (!g_app) return;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)view.bounds.size.width, (float)view.bounds.size.height);
    const CGFloat scale = view.window ? view.window.backingScaleFactor : 1.0;
    io.DisplayFramebufferScale = ImVec2((float)scale, (float)scale);

    id<MTLCommandBuffer> cmd = [self.queue commandBuffer];
    MTLRenderPassDescriptor* rp = view.currentRenderPassDescriptor;
    if (!rp) {
        [cmd commit];
        return;
    }
    ImGui_ImplMetal_NewFrame(rp);
    ImGui_ImplOSX_NewFrame(view);
    ImGui::NewFrame();
    g_app->frame();
    ImGui::Render();

    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.067, 0.078, 0.10, 1.0);
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmd, enc);
    [enc endEncoding];
    id<CAMetalDrawable> drawable = view.currentDrawable;
    const std::string shot = g_app->takeScreenshotRequest();
    [cmd presentDrawable:drawable];
    [cmd commit];
    if (!shot.empty()) {
        [cmd waitUntilCompleted];
        saveScreenshot(drawable.texture, shot);
    }
    if (g_app->wantsQuit()) [NSApp terminate:nil];
}
- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {}
@end

// ---------------------------------------------------------------- app delegate

@interface HSynthAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) HSynthView* view;
@property(nonatomic, strong) HSynthRenderer* renderer;
@end

@implementation HSynthAppDelegate {
    App* _app;
}
- (void)applicationDidFinishLaunching:(NSNotification*)n {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSRect frame = NSMakeRect(60, 60, 1500, 900);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"HSynth (desktop)";
    self.window.delegate = self;
    self.view = [[HSynthView alloc] initWithFrame:frame device:device];
    self.view.framebufferOnly = NO;  // screenshots read the drawable
    self.view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    self.view.preferredFramesPerSecond = 60;
    self.renderer = [HSynthRenderer new];
    self.renderer.queue = [device newCommandQueue];
    self.view.delegate = self.renderer;
    self.window.contentView = self.view;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view];
    if (g_autotest.empty()) [self.window zoom:nil];

    _app = new App(utf8ToPath(g_dataDir), utf8ToPath(g_factoryDir));
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // keyboard navigation stays off: letter keys play notes
    ImGui_ImplMetal_Init(device);
    ImGui_ImplOSX_Init(self.view);

    _app->setAudioMuted(g_mute);
    float scale = 1.0f;  // ImGui works in points on macOS
    _app->init(scale);
    if (_app->uiScaleOverride() > 0) scale = _app->uiScaleOverride();
    _app->setDpiScale(scale);
    loadFonts((float)self.window.backingScaleFactor * scale);
    applyStyle(scale);
    if (!g_autotest.empty()) _app->startAutotest(utf8ToPath(g_autotest));
    g_app = _app;
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { return YES; }
- (void)applicationWillTerminate:(NSNotification*)n {
    if (!_app) return;
    g_app = nullptr;
    _app->shutdown();
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplOSX_Shutdown();
    ImGui::DestroyContext();
    delete _app;
    _app = nullptr;
}
@end

int main(int argc, const char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--data") g_dataDir = next();
        else if (a == "--factory") g_factoryDir = next();
        else if (a == "--autotest") g_autotest = next();
        else if (a == "--mute") g_mute = true;
    }
    if (g_dataDir.empty()) g_dataDir = HSYNTH_DEFAULT_DATA_DIR;
    if (g_factoryDir.empty()) g_factoryDir = HSYNTH_DEFAULT_FACTORY_DIR;
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        HSynthAppDelegate* delegate = [HSynthAppDelegate new];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
