#include <GLFW/glfw3.h>
#include <iostream>

void testGLFWConfig(int major, int minor, int profile, bool forward) {
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, major);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, minor);
    glfwWindowHint(GLFW_OPENGL_PROFILE, profile);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, forward);
    
    GLFWwindow* window = glfwCreateWindow(100, 100, "", NULL, NULL);
    if (window) {
        std::cout << "✓ OpenGL " << major << "." << minor 
                  << " Profile: " << (profile == 0x00032001 ? "Core" : "Any")
                  << " Forward: " << (forward ? "Yes" : "No") << std::endl;
        glfwDestroyWindow(window);
    } else {
        std::cout << "✗ OpenGL " << major << "." << minor << " Failed" << std::endl;
    }
}

int main() {
    glfwInit();
    
    std::cout << "Testing QEMU macOS OpenGL compatibility:" << std::endl;
    
    // Test different configurations
    testGLFWConfig(4, 1, GLFW_OPENGL_CORE_PROFILE, GL_TRUE);
    testGLFWConfig(3, 2, GLFW_OPENGL_CORE_PROFILE, GL_TRUE);
    testGLFWConfig(3, 2, GLFW_OPENGL_ANY_PROFILE, GL_FALSE);
    testGLFWConfig(2, 1, GLFW_OPENGL_ANY_PROFILE, GL_FALSE);
    testGLFWConfig(2, 0, GLFW_OPENGL_ANY_PROFILE, GL_FALSE);
    
    glfwTerminate();
    return 0;
}
