#pragma once

#include <nanogui/button.h>
#include <GLFW/glfw3.h>
#include <functional>
#include <string>

namespace tev {

// Custom button class that handles right-clicks for crop operations
class CropButton : public nanogui::Button {
public:
    CropButton(nanogui::Widget* parent, const std::string& caption = "Untitled", int icon = 0)
        : Button(parent, caption, icon) {}

    // Function to set the callback for right-click
    void setRightClickCallback(const std::function<void()>& callback) {
        mRightClickCallback = callback;
    }

    // Override the mouse button event to handle right-clicks
    virtual bool mouse_button_event(const nanogui::Vector2i &p, int button, bool down, int modifiers) override {
        if (button == GLFW_MOUSE_BUTTON_RIGHT && down && mRightClickCallback) {
            mRightClickCallback();
            return true;
        }

        // Let the parent handle normal left-clicks
        return Button::mouse_button_event(p, button, down, modifiers);
    }

private:
    std::function<void()> mRightClickCallback = nullptr;
};

}  // namespace tev
