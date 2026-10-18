/*
 * Copyright 2026 Toni500
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 * disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 * following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 * products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS” AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "platform.hpp"
#if OSHOT_METAL
// macOS Metal backend for oshot.
// Compiled as Objective-C++ (.mm) so it can use Metal/Cocoa APIs freely
// while the rest of the project stays plain C++.

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
#  include "tool_icons.h"
#  include "util.hpp"

void apply_imgui_theme();
void glfw_error_callback(int error, const char* description);
void glfw_drop_callback(GLFWwindow*, int count, const char** paths);

GLFWwindow* window = nullptr;

// The thread running the frame loop, the only one that may touch the window
static std::thread::id window_thread;

static void minimize_window_()
{
    glfwIconifyWindow(window);
    glfwPollEvents();  // flush
}

static bool hide_window_()
{
    if (!window || std::this_thread::get_id() != window_thread || !glfwGetWindowAttrib(window, GLFW_VISIBLE) ||
        glfwGetWindowAttrib(window, GLFW_ICONIFIED))
        return false;

    glfwIconifyWindow(window);
    glfwPollEvents();  // flush
    return true;
}

static void show_window_()
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

bool app_quitting();  // main.cpp

int run_main_tool()
{
    window_thread = std::this_thread::get_id();
    register_window_callbacks(minimize_window_, hide_window_, show_window_, nullptr, glfwTerminate, glfwSwapInterval);
    id<MTLDevice>       device                = nil;
    id<MTLCommandQueue> commandQueue          = nil;
    id<MTLTexture>      current_frame_texture = nil;

    // vsync is controlled via the CAMetalLayer's displaySyncEnabled property instead.
    g_ss_tool.SetOnCancel([&]() {
        spdlog::info("Canceled screenshot");
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    });
    g_ss_tool.SetReadPixels([&](const region_t& region) {
        const point_t    o             = g_ss_tool.GetImageOrigin();
        const NSUInteger bytes_per_row = NSUInteger(region.w) * 4;
        const NSUInteger buffer_size   = bytes_per_row * NSUInteger(region.h);

        id<MTLBuffer> readback = [device newBufferWithLength:buffer_size options:MTLResourceStorageModeShared];

        id<MTLCommandBuffer>      cb   = [commandQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:current_frame_texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(region.x + int(o.x), region.y + int(o.y), 0)
                          sourceSize:MTLSizeMake(NSUInteger(region.w), NSUInteger(region.h), 1)
                            toBuffer:readback
                   destinationOffset:0
              destinationBytesPerRow:bytes_per_row
            destinationBytesPerImage:buffer_size];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        std::vector<unsigned char> pixels(buffer_size);
        std::memcpy(pixels.data(), readback.contents, buffer_size);
        for (size_t i = 0; i + 3 < pixels.size(); i += 4)
            std::swap(pixels[i], pixels[i + 2]);  // BGRA -> RGBA

        return capture_result_t{ .data = std::move(pixels), .w = region.w, .h = region.h };
    });
    g_ss_tool.SetOnComplete([&](SavingOp op, const capture_result_t& img, ImageExt ext) {
        MUST_OK(save_image(op, img, ext), {
            // The overlay stays open; create_dialog() keeps the error in front of it
            if (op == SavingOp::Clipboard)
                error("Failed to copy the image: {}", _r.error_v());
            else
                error("Failed to save as {}: {}", g_config->File.image_out_type.first, _r.error_v());
            return false;
        });

        glfwSetWindowShouldClose(window, GLFW_TRUE);
        return true;
    });
    g_ss_tool.SetOnImageReload([&](const capture_result_t& cap) {
        // Release old texture automatically via ARC
        id<MTLTexture> newTex = create_metal_texture(device, cap.data.data(), cap.w, cap.h);

        g_ss_tool.SetBackendTexture((__bridge void*)newTex);
    });

    // Before the window exists, so the screenshot is taken right away and without the overlay
    MUST_OK(g_ss_tool.Start(), {
        error("Failed to start capture: {}", _r.error_v());
        return EXIT_FAILURE;
    });

    // Set up dialogs only now that the screenshot is taken, so they can't delay it
    init_dialogs();

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

    // Fullscreen on the monitor: macOS keeps a plain window below the menu bar, which
    // drew the screenshot one menu bar too low, and GLFW_FLOATING is still under the
    // Dock. GLFW's fullscreen covers both (NSMainMenuWindowLevel + 1) and keeps the
    // video mode, since it's the current one
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
    device = MTLCreateSystemDefaultDevice();
    if (!device)
    {
        error("Metal is not supported on this device");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    commandQueue = [device newCommandQueue];
    if (!commandQueue)
    {
        error("Failed to create Metal command queue");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    // Attach a CAMetalLayer to the GLFW window's content view
    NSWindow*     nswin      = glfwGetCocoaWindow(window);
    CAMetalLayer* layer      = [CAMetalLayer layer];
    layer.device             = device;
    layer.pixelFormat        = MTLPixelFormatBGRA8Unorm;
    layer.displaySyncEnabled = YES;  // vsync
    layer.framebufferOnly    = NO;   // required so drawable.texture can be used as a blit source below

    nswin.contentView.layer      = layer;
    nswin.contentView.wantsLayer = YES;

    // ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    const std::string ini = g_config->GetConfigDirPath() + "/imgui.ini";
    ImGuiIO&          io  = ImGui::GetIO();
    io.IniFilename        = ini.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplGlfw_InitForOther(window, true);  // "Other" = non-GL backend
    ImGui_ImplMetal_Init(device);

    build_font_atlas(io);
    apply_imgui_theme();

    // Start the overlay window
    MUST_OK(g_ss_tool.StartWindow(), {
        error("Failed to start tool window: {}", _r.error_v());
        if (!g_is_systray)
            glfwTerminate();
        return EXIT_FAILURE;
    });

    // Create Metal texture from screenshot buffer
    capture_result_t& cap = g_ss_tool.GetRawScreenshot();

    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                    width:cap.w
                                                                                   height:cap.h
                                                                                mipmapped:NO];

    id<MTLTexture> metalTexture = [device newTextureWithDescriptor:desc];

    MTLRegion region = { { 0, 0, 0 }, { (NSUInteger)cap.w, (NSUInteger)cap.h, 1 } };

    [metalTexture replaceRegion:region mipmapLevel:0 withBytes:cap.data.data() bytesPerRow:cap.w * 4];

    // Pass to ImGui
    g_ss_tool.SetBackendTexture((__bridge void*)metalTexture);

    g_ss_tool.SetToolTexture(
        ToolType::Rectangle,
        (__bridge void*)create_metal_texture(device, ICON_SQUARE_RGBA, ICON_SQUARE_W, ICON_SQUARE_H));
    g_ss_tool.SetToolTexture(
        ToolType::RectangleFilled,
        (__bridge void*)create_metal_texture(device, ICON_RECT_FILLED_RGBA, ICON_RECT_FILLED_W, ICON_RECT_FILLED_H));
    g_ss_tool.SetToolTexture(ToolType::CircleFilled,
                             (__bridge void*)create_metal_texture(
                                 device, ICON_CIRCLE_FILLED_RGBA, ICON_CIRCLE_FILLED_W, ICON_CIRCLE_FILLED_H));
    g_ss_tool.SetToolTexture(
        ToolType::ToggleTextTools,
        (__bridge void*)create_metal_texture(device, ICON_TEXT_TOOLS_RGBA, ICON_TEXT_TOOLS_W, ICON_TEXT_TOOLS_H));
    g_ss_tool.SetToolTexture(ToolType::Line,
                             (__bridge void*)create_metal_texture(device, ICON_LINE_RGBA, ICON_LINE_W, ICON_LINE_H));
    g_ss_tool.SetToolTexture(
        ToolType::Circle, (__bridge void*)create_metal_texture(device, ICON_CIRCLE_RGBA, ICON_CIRCLE_W, ICON_CIRCLE_H));
    g_ss_tool.SetToolTexture(ToolType::Arrow,
                             (__bridge void*)create_metal_texture(device, ICON_ARROW_RGBA, ICON_ARROW_W, ICON_ARROW_H));
    g_ss_tool.SetToolTexture(
        ToolType::Pencil, (__bridge void*)create_metal_texture(device, ICON_PENCIL_RGBA, ICON_PENCIL_W, ICON_PENCIL_H));
    g_ss_tool.SetToolTexture(ToolType::Text,
                             (__bridge void*)create_metal_texture(device, ICON_TEXT_RGBA, ICON_TEXT_W, ICON_TEXT_H));

    // Render loop
    MTLRenderPassDescriptor* rpd         = [MTLRenderPassDescriptor new];
    bool                     clean_frame = false;
    // app_quitting(): tray Quit or a signal while capturing
    while (!glfwWindowShouldClose(window) && g_ss_tool.IsActive() && !app_quitting())
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

        current_frame_texture = drawable.texture;

        // Configure render pass
        rpd.colorAttachments[0].texture     = drawable.texture;
        rpd.colorAttachments[0].loadAction  = MTLLoadActionClear;
        rpd.colorAttachments[0].clearColor  = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;

        ImGui_ImplMetal_NewFrame(rpd);
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        g_ss_tool.RenderOverlay();

        ImGui::Render();

        // Encode + submit
        id<MTLCommandBuffer>        cb  = [commandQueue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
        [enc pushDebugGroup:@"oshot"];
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
        [enc popDebugGroup];
        [enc endEncoding];

        // Save/copy or scan requested: render one more frame with only the screenshot
        // and the annotations, blit it off the drawable, and never present it
        if (clean_frame)
        {
            [cb commit];
            [cb waitUntilCompleted];
            clean_frame = false;
            g_ss_tool.OnCleanFrame();
        }
        else if (g_ss_tool.WantsCleanFrame())
        {
            clean_frame = true;
            continue;  // this frame still has the overlay baked in; drop it unpresented
        }
        else
        {
            [cb presentDrawable:drawable];
            [cb commit];
        }
    }

    // Cleanup
    ImGui_ImplMetal_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return EXIT_SUCCESS;
}

#endif  // OSHOT_METAL
