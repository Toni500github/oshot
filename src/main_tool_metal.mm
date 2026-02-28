// main_tool_metal.mm — macOS Metal backend for oshot
// Compiled as Objective-C++ (.mm) so it can use Metal/Cocoa APIs freely
// while the rest of the project stays plain C++.
#ifdef __APPLE__

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_metal.h"

#undef fract1
#include "config.hpp"
#include "screenshot_tool.hpp"
#include "screen_capture.hpp"
#include "socket.hpp"
#include "util.hpp"

void glfw_error_callback(int error, const char* description);
void glfw_drop_callback(GLFWwindow*, int count, const char** paths);

int run_main_tool(const std::string& imgui_ini_path)
{
    GLFWwindow* window = nullptr;

    ScreenshotTool ss_tool;

    // vsync disable is a no-op in the Metal path — vsync is controlled via
    // the CAMetalLayer's displaySyncEnabled property instead.
    ss_tool.SetOnCancel([&]() {
        fmt::println(stderr, "Cancelled screenshot");
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    });
    ss_tool.SetOnComplete([&](SavingOp op, const Result<capture_result_t>& result) {
        if (!result.ok())
            error("Screenshot failed: {}", result.error());
        else
        {
            const Result<>& res = save_png(op, result.get());
            if (!res.ok())
                error("Failed to save as PNG: {}", res.error());
        }
        glfwSetWindowShouldClose(window, GLFW_TRUE);
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

    // No OpenGL context, Metal owns the rendering
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return EXIT_FAILURE;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // skip GL context

#if !DEBUG
    glfwWindowHint(GLFW_DECORATED,    GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING,     GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUSED,      GLFW_TRUE);
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
#endif

    GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode    = glfwGetVideoMode(monitor);

    window = glfwCreateWindow(mode->width, mode->height, "oshot", monitor, nullptr);
    if (!window)
    {
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwSetDropCallback(window, glfw_drop_callback);

    g_scr_w = mode->width;
    g_scr_h = mode->height;

    // Metal device + command queue
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
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

    if (!g_config->File.font.empty())
    {
        const auto& path = get_font_path(g_config->File.font);
        if (!path.empty())
            io.FontDefault =
                io.Fonts->AddFontFromFileTTF(path.string().c_str(), 16.0f, nullptr, io.Fonts->GetGlyphRangesDefault());
    }

    ImGui_ImplGlfw_InitForOther(window, true);  // "Other" = non-GL backend
    ImGui_ImplMetal_Init(device);

    {
        const Result<>& res = ss_tool.StartWindow();
        if (!res.ok())
        {
            error("Failed to start tool window: {}", res.error());
            return EXIT_FAILURE;
        }
    }

    // Create Metal texture from screenshot buffer
    capture_result_t& cap = ss_tool.GetRawScreenshot();  // <-- add getter

    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                    width:cap.w
                                                                                   height:cap.h
                                                                                mipmapped:NO];

    id<MTLTexture> metalTexture = [device newTextureWithDescriptor:desc];

    MTLRegion region = { { 0, 0, 0 }, { (NSUInteger)cap.w, (NSUInteger)cap.h, 1 } };

    [metalTexture replaceRegion:region mipmapLevel:0 withBytes:cap.data.data() bytesPerRow:cap.w * 4];

    // Pass to ImGui
    ss_tool.SetBackendTexture((__bridge void*)metalTexture);

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
        rpd.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
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
