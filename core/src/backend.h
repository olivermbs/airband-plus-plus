#pragma once
#include <string>

namespace backend {
    int init(std::string resDir = "");
    void beginFrame();
    void render(bool vsync = true);

    // Caps how often the UI is redrawn, 0 for no limit
    void setFrameRateLimit(int fps);
    void getMouseScreenPos(double& x, double& y);
    void setMouseScreenPos(double x, double y);
    int renderLoop();
    int end();
}