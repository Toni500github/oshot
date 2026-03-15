// macOS Metal backend for oshot.
// Compiled as Objective-C++ (.mm) so it can use Metal/Cocoa APIs freely
// while the rest of the project stays plain C++.
#ifdef __APPLE__

#  import <Metal/Metal.h>
#  import <QuartzCore/CAMetalLayer.h>

#  define GLFW_INCLUDE_NONE
#  define GLFW_EXPOSE_NATIVE_COCOA
#  include <GLFW/glfw3.h>
#  include <GLFW/glfw3native.h>

#  include "imgui/imgui.h"
#  include "imgui/imgui_impl_glfw.h"
#  include "imgui/imgui_impl_metal.h"

#  undef fract1
#  include "config.hpp"
#  include "screen_capture.hpp"
#  include "screenshot_tool.hpp"
#  include "socket.hpp"
#  include "tool_icons.h"
#  include "util.hpp"

void glfw_error_callback(int error, const char* description);
void glfw_drop_callback(GLFWwindow*, int count, const char** paths);
void extern_glfwTerminate()
{
    glfwTerminate();
}
void extern_glfwSwapInterval(int v)
{
    glfwSwapInterval(v);
}

GLFWwindow* window = nullptr;

using namespace spdlog;

void minimize_window()
{
    glfwIconifyWindow(window);
    glfwPollEvents();  // flush
}

void maximize_window()
{
    glfwRestoreWindow(window);
    glfwFocusWindow(window);
}

static id<MTLTexture> create_metal_texture(id<MTLDevice> device, const uint8_t* data, int w, int h)
{
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                    width:w
                                                                                   height:h
                                                                                mipmapped:NO];

    id<MTLTexture> tex = [device newTextureWithDescriptor:desc];

    MTLRegion region = { { 0, 0, 0 }, { (NSUInteger)w, (NSUInteger)h, 1 } };

    [tex replaceRegion:region mipmapLevel:0 withBytes:data bytesPerRow:w * 4];

    return tex;
}

int run_main_tool(const std::string& imgui_ini_path)
{
    id<MTLDevice>  device;
    ScreenshotTool ss_tool;

    // vsync is controlled via the CAMetalLayer's displaySyncEnabled property instead.
    ss_tool.SetOnCancel([&]() {
        fmt::println(stderr, "Cancelled screenshot");
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    });
    ss_tool.SetOnComplete([&](SavingOp op, const Result<capture_result_t>& result) {
        if (!result.ok())
        {
            error("Screenshot failed: {}", result.error());
            glfwSetWindowShouldClose(window, GLFW_TRUE);
            return;
        }

        const Result<>& res = save_png(op, result.get());
        if (!res.ok())
            error("Failed to save as PNG: {}", res.error());

        glfwSetWindowShouldClose(window, GLFW_TRUE);
    });
    ss_tool.SetOnImageReload([&](const capture_result_t& cap) {
        // Release old texture automatically via ARC
        id<MTLTexture> newTex = create_metal_texture(device, cap.data.data(), cap.w, cap.h);

        ss_tool.SetBackendTexture((__bridge void*)newTex);
    });

    // Setup Screenshot Tool
    // Calling it before starting the window so that
    // we can capture at the exact moment we launch
    {
        const Result<>& res = ss_tool.Start();
        if (!res.ok())
        {
            error("Failed to start capture: {}", res.error());
            return EXIT_FAILURE;
        }
    }

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return EXIT_FAILURE;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // skip GL context

#  if !DEBUG
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE);
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
#  endif

    GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode    = glfwGetVideoMode(monitor);

    // Pass nullptr as the monitor to get a borderless windowed overlay
    // instead of exclusive fullscreen. This ensures GLFW_DECORATED,
    // GLFW_FLOATING, and GLFW_AUTO_ICONIFY hints actually take effect
    // (they are silently ignored for exclusive fullscreen windows), and
    // keeps mouse/keyboard events scoped to this monitor so they don't
    // bleed in from the other display on multi-monitor setups.
    window = glfwCreateWindow(mode->width, mode->height, "oshot", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwSetDropCallback(window, glfw_drop_callback);

    g_scr_w = mode->width;
    g_scr_h = mode->height;

    // Metal device + command queue
    device = MTLCreateSystemDefaultDevice();
    if (!device)
    {
        error("Metal is not supported on this device");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    id<MTLCommandQueue> commandQueue = [device newCommandQueue];

    // Attach a CAMetalLayer to the GLFW window's content view
    NSWindow*     nswin      = glfwGetCocoaWindow(window);
    CAMetalLayer* layer      = [CAMetalLayer layer];
    layer.device             = device;
    layer.pixelFormat        = MTLPixelFormatBGRA8Unorm;
    layer.displaySyncEnabled = YES;  // vsync

    nswin.contentView.layer      = layer;
    nswin.contentView.wantsLayer = YES;

    // ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = imgui_ini_path.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplGlfw_InitForOther(window, true);  // "Other" = non-GL backend
    ImGui_ImplMetal_Init(device);

    ImFontConfig font_cfg;

    for (const std::string& font : g_config->File.fonts)
    {
        const fs::path& path = get_font_path(font);
        if (path.empty())
        {
            io.Fonts->AddFontDefault(&font_cfg);
        }
        else if (fs::exists(path))
        {
            io.Fonts->AddFontFromFileTTF(path.string().c_str(), 16.0f, &font_cfg);
        }
        else
        {
            if (!font.empty())
                ::warn("Font '{}' is not found", font);
            continue;
        }

        // this value is false by default, and we can't set it to true without adding atleast one font first.
        // so, after we add the first font, this will be true (and will stay true).
        // MergeMode fills the gap in previous fonts with glyphs from this font, for example, adding Arabic glyphs to a non-Arabic font.
        font_cfg.MergeMode = true;
    }

    // Start the overlay window
    {
        const Result<>& res = ss_tool.StartWindow();
        if (!res.ok())
        {
            error("Failed to start tool window: {}", res.error());
            glfwTerminate();
            return EXIT_FAILURE;
        }
    }

    // Create Metal texture from screenshot buffer
    capture_result_t& cap = ss_tool.GetRawScreenshot();

    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                    width:cap.w
                                                                                   height:cap.h
                                                                                mipmapped:NO];

    id<MTLTexture> metalTexture = [device newTextureWithDescriptor:desc];

    MTLRegion region = { { 0, 0, 0 }, { (NSUInteger)cap.w, (NSUInteger)cap.h, 1 } };

    [metalTexture replaceRegion:region mipmapLevel:0 withBytes:cap.data.data() bytesPerRow:cap.w * 4];

    // Pass to ImGui
    ss_tool.SetBackendTexture((__bridge void*)metalTexture);

    ss_tool.SetToolTexture(
        ToolType::Rectangle,
        (__bridge void*)create_metal_texture(device, ICON_SQUARE_RGBA, ICON_SQUARE_W, ICON_SQUARE_H));
    ss_tool.SetToolTexture(
        ToolType::RectangleFilled,
        (__bridge void*)create_metal_texture(device, ICON_RECT_FILLED_RGBA, ICON_RECT_FILLED_W, ICON_RECT_FILLED_H));
    ss_tool.SetToolTexture(ToolType::CircleFilled,
                           (__bridge void*)create_metal_texture(
                               device, ICON_CIRCLE_FILLED_RGBA, ICON_CIRCLE_FILLED_W, ICON_CIRCLE_FILLED_H));
    ss_tool.SetToolTexture(
        ToolType::ToggleTextTools,
        (__bridge void*)create_metal_texture(device, ICON_TEXT_TOOLS_RGBA, ICON_TEXT_TOOLS_W, ICON_TEXT_TOOLS_H));
    ss_tool.SetToolTexture(ToolType::Line,
                           (__bridge void*)create_metal_texture(device, ICON_LINE_RGBA, ICON_LINE_W, ICON_LINE_H));
    ss_tool.SetToolTexture(
        ToolType::Circle, (__bridge void*)create_metal_texture(device, ICON_CIRCLE_RGBA, ICON_CIRCLE_W, ICON_CIRCLE_H));
    ss_tool.SetToolTexture(ToolType::Arrow,
                           (__bridge void*)create_metal_texture(device, ICON_ARROW_RGBA, ICON_ARROW_W, ICON_ARROW_H));
    ss_tool.SetToolTexture(
        ToolType::Pencil, (__bridge void*)create_metal_texture(device, ICON_PENCIL_RGBA, ICON_PENCIL_W, ICON_PENCIL_H));
    ss_tool.SetToolTexture(ToolType::Text,
                           (__bridge void*)create_metal_texture(device, ICON_TEXT_RGBA, ICON_TEXT_W, ICON_TEXT_H));

    // Render loop
    MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor new];

    while (!glfwWindowShouldClose(window) && ss_tool.IsActive())
    {
        glfwPollEvents();

        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0)
        {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        // Keep the CAMetalLayer drawable size in sync with the framebuffer
        int fb_w, fb_h;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        layer.drawableSize = CGSizeMake(fb_w, fb_h);

        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable)
            continue;

        // Configure render pass
        rpd.colorAttachments[0].texture     = drawable.texture;
        rpd.colorAttachments[0].loadAction  = MTLLoadActionClear;
        rpd.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;

        ImGui_ImplMetal_NewFrame(rpd);
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ss_tool.RenderOverlay();

        ImGui::Render();

        // Encode + submit
        id<MTLCommandBuffer>        cb  = [commandQueue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
        [enc pushDebugGroup:@"oshot"];

        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);

        [enc popDebugGroup];
        [enc endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];
    }

    // Cleanup
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    g_sender->Close();

    return EXIT_SUCCESS;
}

#endif  // __APPLE__
