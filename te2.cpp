#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <iostream>
#include <vector>

std::vector<uint8_t> captureScreenRGBA() {
    std::vector<uint8_t> rgbaData;
    
    // Get the main display
    CGDirectDisplayID displayID = CGMainDisplayID();
    
    // Create a screenshot of the display
    CGImageRef screenshot = CGDisplayCreateImage(displayID);
    
    if (!screenshot) {
        std::cerr << "Failed to create screenshot" << std::endl;
        return rgbaData;
    }
    
    // Get image dimensions
    size_t width = CGImageGetWidth(screenshot);
    size_t height = CGImageGetHeight(screenshot);
    size_t bytesPerRow = CGImageGetBytesPerRow(screenshot);
    
    // Create bitmap context
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(
        nullptr, width, height, 8, bytesPerRow, colorSpace,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big
    );
    
    if (!context) {
        std::cerr << "Failed to create bitmap context" << std::endl;
        CGImageRelease(screenshot);
        CGColorSpaceRelease(colorSpace);
        return rgbaData;
    }
    
    // Draw the screenshot into the context
    CGRect rect = CGRectMake(0, 0, width, height);
    CGContextDrawImage(context, rect, screenshot);
    
    // Get the raw RGBA data
    uint8_t* data = static_cast<uint8_t*>(CGBitmapContextGetData(context));
    
    if (data) {
        // Copy the data to vector
        size_t dataSize = height * bytesPerRow;
        rgbaData.assign(data, data + dataSize);
    }
    
    // Clean up
    CGContextRelease(context);
    CGImageRelease(screenshot);
    CGColorSpaceRelease(colorSpace);
    
    return rgbaData;
}

// Save RGBA data to a file (PNG format)
bool saveRGBAAsPNG(const std::vector<uint8_t>& rgbaData, 
                   size_t width, size_t height,
                   const std::string& filename) {
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(
        const_cast<uint8_t*>(rgbaData.data()),
        width, height, 8, width * 4, colorSpace,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big
    );
    
    if (!context) {
        CGColorSpaceRelease(colorSpace);
        return false;
    }
    
    CGImageRef image = CGBitmapContextCreateImage(context);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, 
        reinterpret_cast<const UInt8*>(filename.c_str()),
        filename.length(),
        false
    );
    
    // Create PNG destination
    CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
        url, kUTTypePNG, 1, nullptr
    );
    
    if (!destination) {
        CGImageRelease(image);
        CGContextRelease(context);
        CGColorSpaceRelease(colorSpace);
        CFRelease(url);
        return false;
    }
    
    // Add image to destination
    CGImageDestinationAddImage(destination, image, nullptr);
    
    // Finalize the write
    bool success = CGImageDestinationFinalize(destination);
    
    // Clean up
    CFRelease(destination);
    CGImageRelease(image);
    CGContextRelease(context);
    CGColorSpaceRelease(colorSpace);
    CFRelease(url);
    
    return success;
}

int main (int argc, char *argv[]) {
    saveRGBAAsPNG(captureScreenRGBA(), 1280, 800, "test.png");
    return 0;
}
