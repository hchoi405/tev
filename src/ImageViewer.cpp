// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/ImageViewer.h>
#include <tev/CropButton.h>

#include <clip.h>

#include <nanogui/button.h>
#include <nanogui/colorwheel.h>
#include <nanogui/icons.h>
#include <nanogui/label.h>
#include <nanogui/layout.h>
#include <nanogui/messagedialog.h>
#include <nanogui/popupbutton.h>
#include <nanogui/screen.h>
#include <nanogui/textbox.h>
#include <nanogui/theme.h>
#include <nanogui/vscrollpanel.h>

#include <chrono>
#include <stdexcept>

using namespace nanogui;
using namespace std;

namespace tev {

static const int SIDEBAR_MIN_WIDTH = 230;
static const float CROP_MIN_SIZE = 3;

ImageViewer::ImageViewer(
    const shared_ptr<BackgroundImagesLoader>& imagesLoader,
    bool maximize,
    bool floatBuffer,
    bool /*supportsHdr*/
)
: nanogui::Screen{
    nanogui::Vector2i{1024, 799},
    "tev",
    true,
    maximize,
    false,
    true,
    true,
    floatBuffer
}, mImagesLoader{imagesLoader} {
    if (floatBuffer && !m_float_buffer) {
        tlog::warning() << "Failed to create floating point frame buffer.";
    }

    mSupportsHdr = m_float_buffer;

    // At this point we no longer need the standalone console (if it exists).
    toggleConsole();

    // Get monitor configuration to figure out how large the tev window may
    // maximally become.
    {
        int monitorCount;
        auto** monitors = glfwGetMonitors(&monitorCount);
        if (monitors && monitorCount > 0) {
            nanogui::Vector2i
                monitorMin{numeric_limits<int>::max(), numeric_limits<int>::max()},
                monitorMax{numeric_limits<int>::min(), numeric_limits<int>::min()};

            for (int i = 0; i < monitorCount; ++i) {
                nanogui::Vector2i pos, size;
                glfwGetMonitorWorkarea(monitors[i], &pos.x(), &pos.y(), &size.x(), &size.y());
                monitorMin = min(monitorMin, pos);
                monitorMax = max(monitorMax, pos + size);
            }

            mMaxSize = min(mMaxSize, max(monitorMax - monitorMin, nanogui::Vector2i{1024, 800}));
        }
    }

    m_background = Color{0.23f, 1.0f};

    mVerticalScreenSplit = new Widget{this};
    mVerticalScreenSplit->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

    auto horizontalScreenSplit = new Widget(mVerticalScreenSplit);
    horizontalScreenSplit->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill});

    mSidebar = new VScrollPanel{horizontalScreenSplit};
    mSidebar->set_fixed_width(SIDEBAR_MIN_WIDTH);

    auto tmp = new Widget{mSidebar};
    mHelpButton = new Button{tmp, "", FA_QUESTION};
    mHelpButton->set_change_callback([this](bool) { toggleHelpWindow(); });
    mHelpButton->set_font_size(15);
    mHelpButton->set_tooltip("Information about using tev.");
    mHelpButton->set_flags(Button::ToggleButton);

    mSidebarLayout = new Widget{tmp};
    mSidebarLayout->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 0, 0});

    mImageCanvas = new ImageCanvas{horizontalScreenSplit, pixel_ratio()};

    // Tonemapping sectionim
    {
        auto panel = new Widget{mSidebarLayout};
        panel->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 5});
        new Label{panel, "Tonemapping", "sans-bold", 25};
        panel->set_tooltip(
            "Various tonemapping options. Hover the individual controls to learn more!"
        );

        // Exposure label and slider
        {
            panel = new Widget{mSidebarLayout};
            panel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

            mExposureLabel = new Label{panel, "", "sans-bold", 15};

            mExposureSlider = new Slider{panel};
            mExposureSlider->set_range({-5.0f, 5.0f});
            mExposureSlider->set_callback([this](float value) {
                setExposure(value);
            });
            setExposure(0);

            panel->set_tooltip(
                "Exposure scales the brightness of an image prior to tonemapping by 2^Exposure.\n\n"
                "Keyboard shortcuts:\nE and Shift+E"
            );
        }

        // Offset/Gamma label and slider
        {
            panel = new Widget{mSidebarLayout};
            panel->set_layout(new GridLayout{Orientation::Vertical, 2, Alignment::Fill, 5, 0});

            mOffsetLabel = new Label{panel, "", "sans-bold", 15};

            mOffsetSlider = new Slider{panel};
            mOffsetSlider->set_range({-1.0f, 1.0f});
            mOffsetSlider->set_callback([this](float value) {
                setOffset(value);
            });
            setOffset(0);

            mGammaLabel = new Label{panel, "", "sans-bold", 15};

            mGammaSlider = new Slider{panel};
            mGammaSlider->set_range({0.01f, 5.0f});
            mGammaSlider->set_callback([this](float value) {
                setGamma(value);
            });
            setGamma(2.2f);

            panel->set_tooltip(
                "The offset is added to the image after exposure has been applied.\n"
                "Keyboard shortcuts: O and Shift+O\n\n"
                "Gamma is the exponent used when gamma-tonemapping.\n"
                "Keyboard shortcuts: G and Shift+G\n\n"
            );
        }
    }

    // Exposure/offset buttons
    {
        auto buttonContainer = new Widget{mSidebarLayout};
        buttonContainer->set_layout(
            new GridLayout{Orientation::Horizontal, mSupportsHdr ? 4 : 3, Alignment::Fill, 5, 2}
        );

        auto makeButton = [&](const string& name, function<void()> callback, int icon = 0, string tooltip = "") {
            auto button = new Button{buttonContainer, name, icon};
            button->set_font_size(15);
            button->set_callback(callback);
            button->set_tooltip(tooltip);
            return button;
        };

        mCurrentImageButtons.push_back(
            makeButton("Normalize", [this]() { normalizeExposureAndOffset(); }, 0, "Shortcut: N")
        );
        makeButton("Reset", [this]() { resetImage(); }, 0, "Shortcut: R");

        if (mSupportsHdr) {
            mClipToLdrButton = new Button{buttonContainer, "LDR", 0};
            mClipToLdrButton->set_font_size(15);
            mClipToLdrButton->set_change_callback([this](bool value) {
                mImageCanvas->setClipToLdr(value);
            });
            mClipToLdrButton->set_tooltip(
                "Clips the image to [0,1] as if displayed on an LDR screen.\n\n"
                "Shortcut: L"
            );
            mClipToLdrButton->set_flags(Button::ToggleButton);
        }

        auto popupBtn = new PopupButton{buttonContainer, "", FA_PAINT_BRUSH};
        popupBtn->set_font_size(15);
        popupBtn->set_chevron_icon(0);
        popupBtn->set_tooltip("Background Color");

        // Background color popup
        {
            auto popup = popupBtn->popup();
            popup->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 10});

            new Label{popup, "Background Color"};
            auto colorwheel = new ColorWheel{popup, mImageCanvas->backgroundColor()};
            colorwheel->set_color(popupBtn->background_color());

            new Label{popup, "Background Alpha"};
            auto bgAlphaSlider = new Slider{popup};
            bgAlphaSlider->set_range({0.0f, 1.0f});
            bgAlphaSlider->set_callback([this](float value) {
                auto col = mImageCanvas->backgroundColor();
                mImageCanvas->setBackgroundColor(Color{
                    col.r(),
                    col.g(),
                    col.b(),
                    value,
                });
            });

            bgAlphaSlider->set_value(0);

            colorwheel->set_callback([bgAlphaSlider, this](const Color& value) {
                //popupBtn->set_background_color(value);
                mImageCanvas->setBackgroundColor(Color{
                    value.r(),
                    value.g(),
                    value.b(),
                    bgAlphaSlider->value(),
                });
            });
        }
    }

    // Tonemap options
    {
        mTonemapButtonContainer = new Widget{mSidebarLayout};
        mTonemapButtonContainer->set_layout(new GridLayout{Orientation::Horizontal, 4, Alignment::Fill, 5, 2});

        auto makeTonemapButton = [&](const string& name, function<void()> callback) {
            auto button = new Button{mTonemapButtonContainer, name};
            button->set_flags(Button::RadioButton);
            button->set_font_size(15);
            button->set_callback(callback);
            return button;
        };

        makeTonemapButton("sRGB",  [this]() { setTonemap(ETonemap::SRGB); });
        makeTonemapButton("Gamma", [this]() { setTonemap(ETonemap::Gamma); });
        makeTonemapButton("FC",    [this]() { setTonemap(ETonemap::FalseColor); });
        makeTonemapButton("+/-",   [this]() { setTonemap(ETonemap::PositiveNegative); });

        setTonemap(ETonemap::SRGB);

        mTonemapButtonContainer->set_tooltip(
            "Tonemap operator selection:\n\n"

            "sRGB\n"
            "Linear to sRGB conversion\n\n"

            "Gamma\n"
            "Inverse power gamma correction\n\n"

            "FC\n"
            "False-color visualization\n\n"

            "+/-\n"
            "Positive=Green, Negative=Red"
        );
    }

    auto toggleChildrenVisibilityExceptFirst = [](Widget* parentPanel) {
        // Hide all children except the first one (which is the header panel)
        for (auto& child : parentPanel->children()) {
            if (child != parentPanel->children().front()) {
                child->set_visible(!child->visible());
            }
        }
    };

    // Helper for creating a show/hide button for panels
    auto createShowHideButton = [this, toggleChildrenVisibilityExceptFirst](Widget* parentPanel, const string& tooltip) -> Button* {
        // Assume the first child is the header panel
        auto headerPanel = parentPanel->children().front();

        auto button = new Button{headerPanel, "", FA_EYE};
        button->set_font_size(15);
        button->set_flags(Button::ToggleButton);
        button->set_pushed(false);
        button->set_tooltip(tooltip);
        button->set_change_callback([this, parentPanel, toggleChildrenVisibilityExceptFirst](bool value) {
            toggleChildrenVisibilityExceptFirst(parentPanel);
            updateLayout();
        });
        return button;
    };

    // Error metrics
    {
        mMetricButtonContainer = new Widget{mSidebarLayout};
        mMetricButtonContainer->set_layout(new GridLayout{Orientation::Horizontal, 5, Alignment::Fill, 5, 2});

        auto makeMetricButton = [&](const string& name, function<void()> callback) {
            auto button = new Button{mMetricButtonContainer, name};
            button->set_flags(Button::RadioButton);
            button->set_font_size(15);
            button->set_callback(callback);
            return button;
        };

        makeMetricButton("E",   [this]() { setMetric(EMetric::Error); });
        makeMetricButton("AE",  [this]() { setMetric(EMetric::AbsoluteError); });
        makeMetricButton("SE",  [this]() { setMetric(EMetric::SquaredError); });
        makeMetricButton("RAE", [this]() { setMetric(EMetric::RelativeAbsoluteError); });
        makeMetricButton("RSE", [this]() { setMetric(EMetric::RelativeSquaredError); });

        setMetric(EMetric::AbsoluteError);

        mMetricButtonContainer->set_tooltip(
            "Error metric selection. Given a reference image r and the selected image i, "
            "the following operators are available:\n\n"

            "E (Error)\n"
            "i - r\n\n"

            "AE (Absolute Error)\n"
            "|i - r|\n\n"

            "SE (Squared Error)\n"
            "(i - r)²\n\n"

            "RAE (Relative Absolute Error)\n"
            "|i - r| / (r + 0.01)\n\n"

            "RSE (Relative Squared Error)\n"
            "(i - r)² / (r² + 0.01)"
        );
    }

    // Copy size modifier
    {
        auto panel = new Widget{mSidebarLayout};
        panel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

        auto headerPanel = new Widget{panel};
        headerPanel->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 5});

        new Label{headerPanel, "Copy Resize", "sans-bold", 25};

        mCopyResizeShowHideButton = createShowHideButton(panel, "Show/Hide copy resize box");

        auto clipResizePanel = new Widget{panel};
        clipResizePanel->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 5, 2});

        auto selectResizeMode = [this](EClipResizeMode mode) { mClipResizeMode = mode; };

        auto neareastButton = new Button{clipResizePanel, "Nearest"};
        neareastButton->set_flags(Button::RadioButton);
        neareastButton->set_font_size(15);
        neareastButton->set_callback([selectResizeMode]() { selectResizeMode(EClipResizeMode::Nearest); });

        auto linearButton = new Button{clipResizePanel, "Bilinear"};
        linearButton->set_flags(Button::RadioButton);
        linearButton->set_font_size(15);
        linearButton->set_callback([selectResizeMode]() { selectResizeMode(EClipResizeMode::Bilinear); });

        // Default
        if (mClipResizeMode == EClipResizeMode::Nearest) {
            neareastButton->set_pushed(true);
        } else if (mClipResizeMode == EClipResizeMode::Bilinear) {
            linearButton->set_pushed(true);
        }

        auto inputPanel = new Widget{panel};
        inputPanel->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 5, 2});

        mCopyResizeXTextBox = new TextBox{inputPanel};
        mCopyResizeXTextBox->set_editable(true);
        mCopyResizeXTextBox->set_value("1");
        mCopyResizeXTextBox->set_format("[-]?[0-9]*\\.?[0-9]*");
        mCopyResizeXTextBox->set_font_size(15);

        mCopyResizeYTextBox = new TextBox{inputPanel};
        mCopyResizeYTextBox->set_editable(true);
        mCopyResizeYTextBox->set_value("1");
        mCopyResizeYTextBox->set_format("[-]?[0-9]*\\.?[0-9]*");
        mCopyResizeYTextBox->set_font_size(15);

        toggleChildrenVisibilityExceptFirst(panel);
    }

    // Crop box
    {
        // Main panel for the Crop section, using a vertical layout to stack elements
        auto panel = new Widget{mSidebarLayout};
        panel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

        auto headerPanel = new Widget{panel};
        headerPanel->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 5});
        // Add the label in the first row
        new Label{headerPanel, "Crop", "sans-bold", 25};
        // Create a button to toggle the visibility of the crop box
        mCropShowHideButton = createShowHideButton(panel, "Show/Hide crop box");

        // Create a child panel for the input fields, arranged horizontally
        auto inputPanel = new Widget{panel}; // This widget is the container for the input fields
        inputPanel->set_layout(new GridLayout{Orientation::Horizontal, 4, Alignment::Fill, 4, 1});

        // Min X text box
        auto minXPanel = new Widget{inputPanel};
        minXPanel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
        new Label{minXPanel, "Min X", "sans"};
        mCropXminTextBox = new TextBox{minXPanel};
        mCropXminTextBox->set_editable(true);
        mCropXminTextBox->set_value(std::to_string(mCurrCrop ? mCurrCrop->min.x() : 0));
        mCropXminTextBox->set_font_size(15);

        // Max X text box
        auto maxXPanel = new Widget{inputPanel};
        maxXPanel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
        new Label(maxXPanel, "Max X", "sans");
        mCropXmaxTextBox = new TextBox{maxXPanel};
        mCropXmaxTextBox->set_editable(true);
        mCropXmaxTextBox->set_value(std::to_string(mCurrCrop ? mCurrCrop->max.x() : 0));
        mCropXmaxTextBox->set_font_size(15);

        // Min Y text box
        auto minYPanel = new Widget{inputPanel};
        minYPanel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
        new Label(minYPanel, "Min Y", "sans");
        mCropYminTextBox = new TextBox{minYPanel};
        mCropYminTextBox->set_editable(true);
        mCropYminTextBox->set_value(std::to_string(mCurrCrop ? mCurrCrop->min.y() : 0));
        mCropYminTextBox->set_font_size(15);

        // Max Y text box
        auto maxYPanel = new Widget{inputPanel};
        maxYPanel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
        new Label(maxYPanel, "Max Y", "sans");
        mCropYmaxTextBox = new TextBox{maxYPanel};
        mCropYmaxTextBox->set_editable(true);
        mCropYmaxTextBox->set_value(std::to_string(mCurrCrop ? mCurrCrop->max.y() : 0));
        mCropYmaxTextBox->set_font_size(15);

        // Helper function to validate crop dimensions
        auto validateCrop = [](int minX, int minY, int maxX, int maxY) -> std::optional<std::string> {
            // Check if min coordinates are greater than or equal to max coordinates
            if (minX >= maxX) {
                return "Min X " + std::to_string(minX) + " must be less than Max X " + std::to_string(maxX);
            }
            if (minY >= maxY) {
                return "Min Y " + std::to_string(minY) + " must be less than Max Y " + std::to_string(maxY);
            }

            // All validations passed
            return std::nullopt;
        };

        // Callback for when the user modifies any of the text boxes
        auto updateCrop = [this, validateCrop]() -> bool {
            try {
                int minX = std::stoi(this->mCropXminTextBox->value());
                int minY = std::stoi(this->mCropYminTextBox->value());
                int maxX = std::stoi(this->mCropXmaxTextBox->value());
                int maxY = std::stoi(this->mCropYmaxTextBox->value());

                // Validate crop dimensions
                auto validationError = validateCrop(minX, minY, maxX, maxY);
                if (validationError) {
                    std::cerr << "Invalid crop: " << *validationError << std::endl;
                    return false;
                }

                // Update the crop box with the new values
                mImageCanvas->setCrop(Box2i(Vector2i(minX, minY), Vector2i(maxX, maxY)));

                // Update the width/height text boxes without triggering their callbacks
                if (!mUpdatingFromSizeFields) {
                    mUpdatingFromMinMax = true;
                    mCropWidthTextBox->set_value(std::to_string(maxX - minX));
                    mCropHeightTextBox->set_value(std::to_string(maxY - minY));
                    mUpdatingFromMinMax = false;
                }

                return true; // Callback successfully handled the change
            } catch (const std::exception& e) {
                std::cerr << "Invalid input: " << e.what() << std::endl;
                return false; // Return false to indicate failure
            }
        };

        // Alternative callback for width/height text boxes
        auto updateCropFromSize = [this, validateCrop]() -> bool {
            try {
                if (mUpdatingFromMinMax) {
                    return true; // Avoid recursive updates
                }

                int minX = std::stoi(this->mCropXminTextBox->value());
                int minY = std::stoi(this->mCropYminTextBox->value());
                int width = std::stoi(this->mCropWidthTextBox->value());
                int height = std::stoi(this->mCropHeightTextBox->value());

                // Validate crop dimensions using the calculated max values
                auto validationError = validateCrop(minX, minY, minX + width, minY + height);
                if (validationError) {
                    std::cerr << "Invalid crop: " << *validationError << std::endl;
                    return false;
                }

                // Update the crop box with the new values
                mUpdatingFromSizeFields = true;

                // Update max text boxes
                mCropXmaxTextBox->set_value(std::to_string(minX + width));
                mCropYmaxTextBox->set_value(std::to_string(minY + height));

                // Update the crop box
                mImageCanvas->setCrop(Box2i(Vector2i(minX, minY), Vector2i(minX + width, minY + height)));

                mUpdatingFromSizeFields = false;
                return true; // Callback successfully handled the change
            } catch (const std::exception& e) {
                std::cerr << "Invalid input: " << e.what() << std::endl;
                return false; // Return false to indicate failure
            }
        };

        // Set callbacks for the input boxes
        mCropXminTextBox->set_callback([updateCrop](const std::string&) { return updateCrop(); });
        mCropYminTextBox->set_callback([updateCrop](const std::string&) { return updateCrop(); });
        mCropXmaxTextBox->set_callback([updateCrop](const std::string&) { return updateCrop(); });
        mCropYmaxTextBox->set_callback([updateCrop](const std::string&) { return updateCrop(); });

        // Add width/height text boxes in a compact layout
        auto dimensionsPanel = new Widget{panel};
        dimensionsPanel->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 4, 1});

        // Width text box
        auto widthPanel = new Widget{dimensionsPanel};
        widthPanel->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 5});
        new Label{widthPanel, "Width", "sans", 15};
        mCropWidthTextBox = new TextBox{widthPanel};
        mCropWidthTextBox->set_editable(true);
        mCropWidthTextBox->set_value(std::to_string(mCurrCrop ? mCurrCrop->max.x() - mCurrCrop->min.x() : 0));
        mCropWidthTextBox->set_font_size(15);
        mCropWidthTextBox->set_fixed_width(55);

        // Height text box
        auto heightPanel = new Widget{dimensionsPanel};
        heightPanel->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 5});
        new Label{heightPanel, "Height", "sans", 15};
        mCropHeightTextBox = new TextBox{heightPanel};
        mCropHeightTextBox->set_editable(true);
        mCropHeightTextBox->set_value(std::to_string(mCurrCrop ? mCurrCrop->max.y() - mCurrCrop->min.y() : 0));
        mCropHeightTextBox->set_font_size(15);
        mCropHeightTextBox->set_fixed_width(55);

        // Set callbacks for width/height text boxes
        mCropWidthTextBox->set_callback([updateCropFromSize](const std::string&) { return updateCropFromSize(); });
        mCropHeightTextBox->set_callback([updateCropFromSize](const std::string&) { return updateCropFromSize(); });

        // Create a panel for the crop list file path
        auto cropFilePathPanel = new Widget{panel};
        cropFilePathPanel->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 5, 0});

        // Label for the crop list file path
        new Label{cropFilePathPanel, "Crop List File:", "sans-bold"};

        // Create a button to browse for a new crop list file
        auto browseButton = new Button{cropFilePathPanel, "", FA_FOLDER_OPEN};
        browseButton->set_font_size(15);
        browseButton->set_tooltip("Browse for a crop list file");

        // Create the text box to display and edit the crop list file path directly in the panel
        mCropListPathTextBox = new TextBox{cropFilePathPanel};
        mCropListPathTextBox->set_editable(true);

        // Convert initial relative path to absolute path if needed
        try {
            if (!mCropListFilename.empty() && !fs::path(mCropListFilename).is_absolute()) {
                mCropListFilename = fs::absolute(mCropListFilename).string();
            }
        } catch (const fs::filesystem_error& e) {
            std::cerr << "Error converting initial path to absolute: " << e.what() << std::endl;
        }

        mCropListPathTextBox->set_value(mCropListFilename);
        mCropListPathTextBox->set_font_size(15);
        mCropListPathTextBox->set_tooltip("Path to the crop list file");
        mCropListPathTextBox->set_alignment(TextBox::Alignment::Left);
        mCropListPathTextBox->set_fixed_width(mSidebar->fixed_width() - 130); // Give enough width for the text box

        auto cropWindowPanel = new Widget{panel};
        cropWindowPanel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 2, 1});

        auto cropButtonPanel = new Widget{cropWindowPanel};
        cropButtonPanel->set_layout(new GridLayout{Orientation::Horizontal, 1, Alignment::Fill});
        auto cropButtonAdd = new Button{cropButtonPanel, "Add", FA_PLUS};
        cropButtonAdd->set_font_size(15);
        cropButtonAdd->set_tooltip("Add current crop to the list");

        mCropListContainer = new VScrollPanel{cropWindowPanel};
        mCropListContainer->set_fixed_width(mSidebarLayout->fixed_width());

        auto cropListScrollContent = new Widget{mCropListContainer};
        cropListScrollContent->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

        mCropListFile = std::fstream(mCropListFilename, std::ios::in | std::ios::out);
        // Create the file if it doesn't exist
        if (!mCropListFile) {
            mCropListFile = std::fstream(mCropListFilename, std::ios::out);
        }

        auto addCropButtonCallback = [this, cropListScrollContent, validateCrop](int x1, int y1, int x2, int y2) {
            auto valid_out = validateCrop(x1, y1, x2, y2);
            if (valid_out) {
                std::cerr << "Invalid crop: " << *valid_out << std::endl;
                return;
            }

            auto cropWindow = new Box2i(Vector2i(x1, y1), Vector2i(x2, y2));

            // Create a container widget with horizontal layout for the crop button and delete button
            auto buttonContainer = new Widget{cropListScrollContent};
            buttonContainer->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill, 0, 2});

            // Create a shortened caption that fits within the available space
            std::string fullCaption = fmt::format("({}, {}) - ({}, {}) (w:{}, h:{})",
                x1, y1, x2, y2, x2 - x1, y2 - y1);

            // Create the crop button with shortened text if needed
            auto button = new Button{buttonContainer, fullCaption};
            button->set_font_size(12);
            button->set_tooltip(fullCaption); // Show full coordinates in tooltip

            button->set_callback([this, cropWindow]() {
                mImageCanvas->setCrop(*cropWindow);
                mCurrCrop = *cropWindow;

                // Update the text boxes with the new crop values
                mCropXminTextBox->set_value(std::to_string(cropWindow->min.x()));
                mCropYminTextBox->set_value(std::to_string(cropWindow->min.y()));
                mCropXmaxTextBox->set_value(std::to_string(cropWindow->max.x()));
                mCropYmaxTextBox->set_value(std::to_string(cropWindow->max.y()));
                mCropWidthTextBox->set_value(std::to_string(cropWindow->max.x() - cropWindow->min.x()));
                mCropHeightTextBox->set_value(std::to_string(cropWindow->max.y() - cropWindow->min.y()));
            });

            // Create the delete button
            auto deleteButton = new Button{buttonContainer, "", FA_TIMES};
            deleteButton->set_font_size(15);
            deleteButton->set_tooltip("Delete this crop");
            deleteButton->set_fixed_width(25);

            // Add callback to delete button
            deleteButton->set_callback([this, cropListScrollContent, buttonContainer, x1, y1, x2, y2]() {
                // First collect information about all OTHER crops before removing this one
                std::vector<std::tuple<int, int, int, int>> remainingCrops;

                // Store all crops except for this one
                for (auto& child : cropListScrollContent->children()) {
                    // Skip the container being removed
                    if (child == buttonContainer)
                        continue;

                    auto* cropButtonContainer = dynamic_cast<Widget*>(child);
                    if (!cropButtonContainer || cropButtonContainer->child_count() < 1)
                        continue;

                    // Access the crop button which is the first child in the container
                    auto* cropButton = dynamic_cast<Button*>(cropButtonContainer->child_at(0));
                    if (!cropButton)
                        continue;

                    int cx1, cy1, cx2, cy2;
                    // Try to parse coordinates from the tooltip which contains the full coordinates
                    if (sscanf(cropButton->tooltip().c_str(), "(%d, %d) - (%d, %d)", &cx1, &cy1, &cx2, &cy2) == 4) {
                        remainingCrops.emplace_back(cx1, cy1, cx2, cy2);
                    }
                    else if (sscanf(cropButton->caption().c_str(), "(%d, %d) - (%d, %d)", &cx1, &cy1, &cx2, &cy2) == 4) {
                        // Fallback to parsing from caption
                        remainingCrops.emplace_back(cx1, cy1, cx2, cy2);
                    }
                }

                // Remove this crop container from UI
                cropListScrollContent->remove_child(buttonContainer);

                // Rewrite the file with remaining crops
                if (mCropListFile.is_open()) {
                    mCropListFile.close();
                }

                // Reopen file for writing (clearing it)
                mCropListFile = std::fstream(mCropListFilename, std::ios::out);

                // Write all remaining crops
                for (const auto& [cx1, cy1, cx2, cy2] : remainingCrops) {
                    mCropListFile << cx1 << " " << cy1 << " " << cx2 << " " << cy2 << std::endl;
                }

                // Update layout
                updateLayout();
            });
        };

        // Setup callbacks for the file path text box and browse button
        mCropListPathTextBox->set_callback([this, cropListScrollContent, addCropButtonCallback](const std::string& newPath) -> bool {
            // Convert relative path to absolute path if needed
            std::string absolutePath = newPath;
            if (!newPath.empty() && !fs::path(newPath).is_absolute()) {
                try {
                    absolutePath = fs::absolute(newPath).string();
                } catch (const fs::filesystem_error& e) {
                    std::cerr << "Error converting to absolute path: " << e.what() << std::endl;
                    return false;
                }
            }

            if (absolutePath != mCropListFilename) {
                mCropListFilename = absolutePath;
                // Update the textbox to show the absolute path
                mCropListPathTextBox->set_value(absolutePath);

                // Close current file if open
                if (mCropListFile.is_open()) {
                    mCropListFile.close();
                }

                // Clear existing crop list UI
                while (cropListScrollContent->child_count() > 0) {
                    cropListScrollContent->remove_child_at(cropListScrollContent->child_count() - 1);
                }

                // Check if the file exists and try to load crop entries
                bool fileExists = fs::exists(toPath(absolutePath));

                if (fileExists) {
                    // Attempt to open the existing file for reading and writing
                    mCropListFile = std::fstream(mCropListFilename, std::ios::in | std::ios::out);

                    // Read contents from the file and add them to the UI
                    std::string line;
                    while (std::getline(mCropListFile, line)) {
                        std::istringstream iss(line);
                        int x1, y1, x2, y2;
                        if (!(iss >> x1 >> y1 >> x2 >> y2)) {
                            std::cerr << "Invalid crop window: " << line << std::endl;
                            continue;
                        }
                        addCropButtonCallback(x1, y1, x2, y2);
                    }

                    // File already open for reading and writing
                } else {
                    // Create a new file if it doesn't exist
                    mCropListFile = std::fstream(mCropListFilename, std::ios::out);
                    if (!mCropListFile) {
                        std::cerr << "Failed to create crop list file: " << mCropListFilename << std::endl;
                        return false;
                    }
                }

                // Update layout to reflect changes
                updateLayout();
            }
            return true;
        });

        // Add a callback to the browse button to open a file dialog
        browseButton->set_callback([this, cropListScrollContent, addCropButtonCallback]() {
            try {
                // First check if the file exists using a regular file dialog (not save mode)
                auto fileDialog = nanogui::file_dialog(
                    {{"txt", "Text File"}},
                    false); // false for open dialog mode, won't ask for replacement

                if (!fileDialog.empty()) {
                    std::string newPath = fileDialog;
                    mCropListPathTextBox->set_value(newPath);

                    // Clear existing crop list UI
                    while (cropListScrollContent->child_count() > 0) {
                        cropListScrollContent->remove_child_at(cropListScrollContent->child_count() - 1);
                    }

                    // Also reset the current crop selection
                    mImageCanvas->setCrop(std::nullopt);
                    mCurrCrop = std::nullopt;

                    // Check if the file exists
                    bool fileExists = fs::exists(toPath(newPath));

                    if (fileExists) {
                        // File exists, we'll open it
                        if (mCropListFile.is_open()) {
                            mCropListFile.close();
                        }

                        // Open the existing file for reading and writing
                        mCropListFile = std::fstream(newPath, std::ios::in | std::ios::out);

                        // Read and parse the crop entries
                        mCropListFile.seekg(0, std::ios::beg); // Reset position to beginning of file
                        std::string line;
                        while (std::getline(mCropListFile, line)) {
                            std::istringstream iss(line);
                            int x1, y1, x2, y2;
                            if (!(iss >> x1 >> y1 >> x2 >> y2)) {
                                std::cerr << "Invalid crop window: " << line << std::endl;
                                continue;
                            }
                            addCropButtonCallback(x1, y1, x2, y2);
                        }
                    } else {
                        // File doesn't exist, create it
                        mCropListFilename = newPath;
                        if (mCropListFile.is_open()) {
                            mCropListFile.close();
                        }
                        mCropListFile = std::fstream(mCropListFilename, std::ios::out);
                        if (!mCropListFile) {
                            std::cerr << "Failed to create crop list file: " << mCropListFilename << std::endl;
                        }
                    }

                    // Update the stored filename
                    mCropListFilename = newPath;

                    // Update layout
                    updateLayout();
                }
                // If fileDialog is empty, the user canceled the dialog, so we do nothing
            } catch (const std::exception& e) {
                // Handle any exceptions that may occur during file dialog
                std::cerr << "Error in file dialog: " << e.what() << std::endl;
                // Don't change the path if there was an error
            }
        });

        // Read each line and parse the crop window
        std::string line;
        while (std::getline(mCropListFile, line)) {
            std::istringstream iss(line);
            int x1, y1, x2, y2;
            if (!(iss >> x1 >> y1 >> x2 >> y2)) {
                std::cerr << "Invalid crop window: " << line << std::endl;
                continue;
            }
            addCropButtonCallback(x1, y1, x2, y2);
        }

        // Bind callbacks to the add button
        cropButtonAdd->set_callback([this, addCropButtonCallback, validateCrop]() {
            try {
                int minX = std::stoi(this->mCropXminTextBox->value());
                int minY = std::stoi(this->mCropYminTextBox->value());
                int maxX = std::stoi(this->mCropXmaxTextBox->value());
                int maxY = std::stoi(this->mCropYmaxTextBox->value());

                auto valid_out = validateCrop(minX, minY, maxX, maxY);
                if (valid_out) {
                    std::cerr << "Invalid crop: " << *valid_out << std::endl;
                    return;
                }

                // Update the crop box with the new values
                mImageCanvas->setCrop(Box2i(Vector2i(minX, minY), Vector2i(maxX, maxY)));
                // Add the crop window to the list
                addCropButtonCallback(minX, minY, maxX, maxY);
                // Update the layout
                updateLayout();
                // Update the crop list file
                std::fstream cropListFile(mCropListFilename, std::ios::app);
                cropListFile << minX << " " << minY << " " << maxX << " " << maxY << std::endl;
                mCropListContainer->set_scroll(1.f);
            } catch (const std::exception& e) { std::cerr << "Invalid input: " << e.what() << std::endl; }
        });

        toggleChildrenVisibilityExceptFirst(panel);
    }

    // Pixel locator
    {
        auto panel = new Widget{mSidebarLayout};
        panel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

        auto headerPanel = new Widget{panel};
        headerPanel->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Middle, 5, 0});

        new Label{headerPanel, "Pixel Locator", "sans-bold", 25};
        mPixelLocatorShowHideButton = createShowHideButton(panel, "Show/Hide pixel locator");

        //
        auto updateStatusText = [this](const Vector2i& pixelPos, float value, const string& type, const std::string& detail = "") {
            if (!mStatusLabel || !mCurrentImage) return;

            // Get the channels in the current group
            auto channels = mCurrentImage->channelsInGroup(mCurrentGroup);
            if (channels.empty()) return;

            std::string channelName = channels[0];
            if (channels.size() > 1) {
                channelName = mCurrentGroup;
            }

            std::string statusText = fmt::format(
                "{} Value Found\nPixel: ({}, {})\nValue: {:.6f}\nChannel: {}",
                type, pixelPos.x(), pixelPos.y(), value, channelName
            );

            if (!detail.empty()) {
                statusText += "\n" + detail;
            }

            mStatusLabel->set_caption(statusText);
            auto preferred_size = mStatusLabel->preferred_size(m_nvg_context);
            mStatusLabel->set_fixed_width(preferred_size.x());
            mStatusLabel->set_fixed_height(max(20, preferred_size.y()));
            updateLayout();
        };

        // Create buttons for different search criteria in a horizontal layout
        auto searchPanel = new Widget{panel};
        searchPanel->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 3, 1});

        auto findMaxButton = new Button{searchPanel, "Find Max"};
        findMaxButton->set_font_size(15);
        findMaxButton->set_tooltip("Find the pixel with the maximum value in the current channel/group");
        findMaxButton->set_callback([this, updateStatusText]() {
            if (!mCurrentImage) return;

            // Get current channel group
            const auto &channelImages = ImageCanvas::channelsFromImages(mCurrentImage, mCurrentReference, mCurrentGroup, mImageCanvas->metric(), 0); // 0 for lower priority
            if (channelImages.empty()) return;

            auto size = mCurrentImage->size();
            Vector2i maxPos{0, 0};
            float maxVal = -numeric_limits<float>::infinity();

            const Box2i region = mImageCanvas->cropInImageCoords();
            int cropMinX = 0, cropMaxX = size.x(), cropMinY = 0, cropMaxY = size.y();
            if (region.isValid()) {
                cropMinX = region.min.x();
                cropMaxX = region.max.x();
                cropMinY = region.min.y();
                cropMaxY = region.max.y();
            }

            // Find maximum value across all channels in the current group, within crop region if set
            for (const auto& channel : channelImages) {
                for (int y = cropMinY; y < cropMaxY; ++y) {
                    for (int x = cropMinX; x < cropMaxX; ++x) {
                        float val = channel.eval({x, y});
                        if (val > maxVal) {
                            maxVal = val;
                            maxPos = {x, y};
                        }
                    }
                }
            }

            // Focus on the found pixel
            focusPixel(maxPos);
            updateStatusText(maxPos, maxVal, "Maximum");
        });

        auto findMinButton = new Button{searchPanel, "Find Min"};
        findMinButton->set_font_size(15);
        findMinButton->set_tooltip("Find the pixel with the minimum value in the current channel/group");
        findMinButton->set_callback([this, updateStatusText]() {
            if (!mCurrentImage) return;

            // Get current channel group
            const auto &channelImages = ImageCanvas::channelsFromImages(mCurrentImage, mCurrentReference, mCurrentGroup, mImageCanvas->metric(), 0); // 0 for lower priority
            if (channelImages.empty()) return;

            auto size = mCurrentImage->size();
            Vector2i minPos{0, 0};
            float minVal = numeric_limits<float>::infinity();

            const Box2i region = mImageCanvas->cropInImageCoords();
            int cropMinX = 0, cropMaxX = size.x(), cropMinY = 0, cropMaxY = size.y();
            if (region.isValid()) {
                cropMinX = region.min.x();
                cropMaxX = region.max.x();
                cropMinY = region.min.y();
                cropMaxY = region.max.y();
            }

            // Find minimum value across all channels in the current group
            for (const auto& channel : channelImages) {
                for (int y = cropMinY; y < cropMaxY; ++y) {
                    for (int x = cropMinX; x < cropMaxX; ++x) {
                        float val = channel.eval({x, y});
                        if (val < minVal) {
                            minVal = val;
                            minPos = {x, y};
                        }
                    }
                }
            }

            // Focus on the found pixel
            focusPixel(minPos);
            updateStatusText(minPos, minVal, "Minimum");
        });

        // Range search - more compact layout with labels inline
        auto rangePanel = new Widget{panel};
        rangePanel->set_layout(new GridLayout{Orientation::Horizontal, 4, Alignment::Middle, 2, 1});

        // First row: labels and inputs side by side
        new Label{rangePanel, "Min:", "sans", 15};
        mRangeMinTextBox = new TextBox{rangePanel};
        mRangeMinTextBox->set_editable(true);
        mRangeMinTextBox->set_value("0.0");
        mRangeMinTextBox->set_format("[-]?[0-9]*\\.?[0-9]*");
        mRangeMinTextBox->set_font_size(15);
        mRangeMinTextBox->set_fixed_width(55);

        new Label{rangePanel, "Max:", "sans", 15};
        mRangeMaxTextBox = new TextBox{rangePanel};
        mRangeMaxTextBox->set_editable(true);
        mRangeMaxTextBox->set_value("1.0");
        mRangeMaxTextBox->set_format("[-]?[0-9]*\\.?[0-9]*");
        mRangeMaxTextBox->set_font_size(15);
        mRangeMaxTextBox->set_fixed_width(55);

        // Second row: Search buttons
        auto rangeButtonPanel = new Widget{panel};
        rangeButtonPanel->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 3, 1});

        mFindRangeButton = new Button{rangeButtonPanel, "Find First"};
        mFindRangeButton->set_font_size(15);
        mFindRangeButton->set_tooltip("Find the first pixel with value in the specified range");

        mFindNextRangeButton = new Button{rangeButtonPanel, "Find Next"};
        mFindNextRangeButton->set_font_size(15);
        mFindNextRangeButton->set_tooltip("Find the next pixel with value in the specified range");
        mFindNextRangeButton->set_enabled(false);

        // Store found pixels for "Find Next" functionality
        mFoundPixels.clear();
        mCurrentFoundPixelIdx = -1;

        mFindRangeButton->set_callback([this, updateStatusText]() {
            if (!mCurrentImage) return;

            try {
                float minVal = std::stof(mRangeMinTextBox->value());
                float maxVal = std::stof(mRangeMaxTextBox->value());

                // Swap if min > max
                if (minVal > maxVal) std::swap(minVal, maxVal);

                // Get current channel group
                const auto &channelImages = ImageCanvas::channelsFromImages(mCurrentImage, mCurrentReference, mCurrentGroup, mImageCanvas->metric(), 0); // 0 for lower priority
                if (channelImages.empty()) return;

                auto size = mCurrentImage->size();
                mFoundPixels.clear();

                const Box2i region = mImageCanvas->cropInImageCoords();
                int cropMinX = 0, cropMaxX = size.x(), cropMinY = 0, cropMaxY = size.y();
                if (region.isValid()) {
                    cropMinX = region.min.x();
                    cropMaxX = region.max.x();
                    cropMinY = region.min.y();
                    cropMaxY = region.max.y();
                }

                // Find all pixels in the given range
                for (const auto& channel : channelImages) {
                    for (int y = cropMinY; y < cropMaxY; ++y) {
                        for (int x = cropMinX; x < cropMaxX; ++x) {
                            float val = channel.eval({x, y});
                            if (val >= minVal && val <= maxVal) {
                                mFoundPixels.push_back({{x, y}, val});
                            }
                        }
                    }
                }

                // Sort found pixels by value
                std::sort(mFoundPixels.begin(), mFoundPixels.end(),
                    [](const auto& a, const auto& b) { return a.second < b.second; });

                if (!mFoundPixels.empty()) {
                    mCurrentFoundPixelIdx = 0;
                    mFindNextRangeButton->set_enabled(true);

                    // Focus on the first found pixel
                    focusPixel(mFoundPixels[0].first);
                    updateStatusText(
                        mFoundPixels[0].first,
                        mFoundPixels[0].second,
                        "Range",
                        fmt::format("{} of {}", 1, mFoundPixels.size())
                    );
                } else {
                    mFindNextRangeButton->set_enabled(false);
                    mStatusLabel->set_caption("No pixels found in the specified range");
                }
            } catch (const std::exception& e) {
                mStatusLabel->set_caption(fmt::format("Error: {}", e.what()));
            }
        });

        mFindNextRangeButton->set_callback([this, updateStatusText]() {
            if (mFoundPixels.empty() || mCurrentFoundPixelIdx < 0) return;

            mCurrentFoundPixelIdx = (mCurrentFoundPixelIdx + 1) % mFoundPixels.size();
            focusPixel(mFoundPixels[mCurrentFoundPixelIdx].first);
            updateStatusText(
                mFoundPixels[mCurrentFoundPixelIdx].first,
                mFoundPixels[mCurrentFoundPixelIdx].second,
                "Range",
                fmt::format("{} of {}", mCurrentFoundPixelIdx + 1, mFoundPixels.size())
            );
        });

        // Status label to show results - reduced height
        mStatusLabel = new Label{panel, "", "sans", 15};
        mStatusLabel->set_font_size(15);

        // Add a tooltip to the entire section
        panel->set_tooltip("Find pixels of interest in the image");

        toggleChildrenVisibilityExceptFirst(panel);
    }

    // Image selection
    {
        auto spacer = new Widget{mSidebarLayout};
        spacer->set_height(10);

        {
            auto panel = new Widget{mSidebarLayout};
            panel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});
            auto label = new Label{panel, "Images", "sans-bold", 25};
            label->set_tooltip(
                "Select images either by left-clicking on them or by pressing arrow/number keys on your keyboard.\n"
                "Right-clicking an image marks it as the 'reference' image. "
                "While a reference image is set, the currently selected image is not simply displayed, but compared to the reference image."
            );
        }

        // Histogram of selected image
        {
            auto panel = new Widget{mSidebarLayout};
            panel->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill, 5});

            mHistogram = new MultiGraph{panel, ""};
        }

        // Fuzzy filter of open images
        {
            auto panel = new Widget{mSidebarLayout};
            panel->set_layout(new GridLayout{Orientation::Horizontal, 2, Alignment::Fill, 5, 2});

            mFilter = new TextBox{panel, ""};
            mFilter->set_editable(true);
            mFilter->set_alignment(TextBox::Alignment::Left);
            mFilter->set_callback([this](const string& filter) {
                return setFilter(filter);
            });

            mFilter->set_placeholder("Find");
            mFilter->set_tooltip(fmt::format(
                "Filters visible images and channel groups according to a supplied string. "
                "The string must have the format 'image:group'. "
                "Only images whose name contains 'image' and groups whose name contains 'group' will be visible.\n\n"
                "Keyboard shortcut:\n{}+P",
                HelpWindow::COMMAND
            ));

            mRegexButton = new Button{panel, "", FA_SEARCH};
            mRegexButton->set_tooltip("Treat filter as regular expression");
            mRegexButton->set_pushed(false);
            mRegexButton->set_flags(Button::ToggleButton);
            mRegexButton->set_font_size(15);
            mRegexButton->set_change_callback([this](bool value) { setUseRegex(value); });
        }

        // Playback controls
        {
            auto playback = new Widget{mSidebarLayout};
            playback->set_layout(new GridLayout{Orientation::Horizontal, 4, Alignment::Fill, 5, 2});

            auto makePlaybackButton = [&](
                const string& name,
                bool enabled,
                function<void()> callback,
                int icon = 0,
                string tooltip = ""
            ) {
                auto button = new Button{playback, name, icon};
                button->set_callback(callback);
                button->set_tooltip(tooltip);
                button->set_font_size(15);
                button->set_enabled(enabled);
                return button;
            };

            mPlayButton = makePlaybackButton("", true, []{}, FA_PLAY, "Play (Space)");
            mPlayButton->set_flags(Button::ToggleButton);
            mPlayButton->set_change_callback([this](bool value) { setPlayingBack(value); });

            mAnyImageButtons.push_back(makePlaybackButton("", false, [this] {
                selectImage(nthVisibleImage(0));
            }, FA_FAST_BACKWARD, "Front (Home)"));

            mAnyImageButtons.push_back(makePlaybackButton("", false, [this] {
                selectImage(nthVisibleImage(mImages.size()));
            }, FA_FAST_FORWARD, "Back (End)"));

            mFpsTextBox = new IntBox<int>{playback, 24};
            mFpsTextBox->set_default_value("24");
            mFpsTextBox->set_units("fps");
            mFpsTextBox->set_editable(true);
            mFpsTextBox->set_alignment(TextBox::Alignment::Right);
            mFpsTextBox->set_min_max_values(1, 1000);
            mFpsTextBox->set_spinnable(true);
        }

        // Save, refresh, load, close
        {
            auto tools = new Widget{mSidebarLayout};
            tools->set_layout(new GridLayout{Orientation::Horizontal, 6, Alignment::Fill, 5, 1});

            auto makeImageButton = [&](
                const string& name,
                bool enabled,
                function<void()> callback,
                int icon = 0,
                string tooltip = ""
            ) {
                auto button = new Button{tools, name, icon};
                button->set_callback(callback);
                button->set_tooltip(tooltip);
                button->set_font_size(15);
                button->set_enabled(enabled);
                return button;
            };

            makeImageButton("", true, [this] {
                openImageDialog();
            }, FA_FOLDER, fmt::format("Open ({}+O)", HelpWindow::COMMAND));

            mCurrentImageButtons.push_back(makeImageButton("", false, [this] {
                saveImageDialog();
            }, FA_SAVE, fmt::format("Save ({}+S)", HelpWindow::COMMAND)));

            mCurrentImageButtons.push_back(makeImageButton("", false, [this] {
                reloadImage(mCurrentImage);
            }, FA_RECYCLE, fmt::format("Reload ({}+R or F5)", HelpWindow::COMMAND)));

            mAnyImageButtons.push_back(makeImageButton("A", false, [this] {
                reloadAllImages();
            }, 0, fmt::format("Reload All ({}+Shift+R or {}+F5)", HelpWindow::COMMAND, HelpWindow::COMMAND)));

            mWatchFilesForChangesButton = makeImageButton("W", true, {}, 0, "Watch image files and directories for changes and reload them automatically.");
            mWatchFilesForChangesButton->set_flags(Button::Flags::ToggleButton);
            mWatchFilesForChangesButton->set_change_callback([this](bool value) {
                setWatchFilesForChanges(value);
            });

            mCurrentImageButtons.push_back(makeImageButton("", false, [this] {
                auto* glfwWindow = screen()->glfw_window();
                // There is no explicit access to the currently pressed modifier keys here, so we
                // need to directly ask GLFW. In case this is needed more often, it may be worth
                // inheriting Button and overriding mouse_button_event (similar to ImageButton).
                if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) {
                    removeAllImages();
                } else {
                    removeImage(mCurrentImage);
                }
            }, FA_TIMES, fmt::format("Close ({}+W); Close All ({}+Shift+W)", HelpWindow::COMMAND, HelpWindow::COMMAND)));

            spacer = new Widget{mSidebarLayout};
            spacer->set_height(3);
        }

        // List of open images
        {
            mImageScrollContainer = new VScrollPanel{mSidebarLayout};
            mImageScrollContainer->set_fixed_width(mSidebarLayout->fixed_width());

            mScrollContent = new Widget{mImageScrollContainer};
            mScrollContent->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill});

            mImageButtonContainer = new Widget{mScrollContent};
            mImageButtonContainer->set_layout(new BoxLayout{Orientation::Vertical, Alignment::Fill});
        }
    }

    // Group selection
    {
        mFooter = new Widget{mVerticalScreenSplit};

        mGroupButtonContainer = new Widget{mFooter};
        mGroupButtonContainer->set_layout(new BoxLayout{Orientation::Horizontal, Alignment::Fill});
        mGroupButtonContainer->set_fixed_height(25);
        mFooter->set_fixed_height(25);
        mFooter->set_visible(false);
    }

    set_resize_callback([this](nanogui::Vector2i) { requestLayoutUpdate(); });

    selectImage(nullptr);
    selectReference(nullptr);

    if (!maximize) {
        this->set_size(nanogui::Vector2i(1024, 800));
        mDidFitToImage = 3;
    }

    updateLayout();
}

bool ImageViewer::mouse_button_event(const nanogui::Vector2i& p, int button, bool down, int modifiers) {
    redraw();

    // Check if the user performed mousedown on an imagebutton so we can mark it as being dragged.
    // This has to occur before Screen::mouse_button_event as the button would absorb the event.
    if (down) {
        if (mImageScrollContainer->contains(p - mSidebarLayout->parent()->position())) {
            auto& buttons = mImageButtonContainer->children();

            nanogui::Vector2i relMousePos = (absolute_position() + p) - mImageButtonContainer->absolute_position();

            for (size_t i = 0; i < buttons.size(); ++i) {
                const auto* imgButton = dynamic_cast<ImageButton*>(buttons[i]);
                if (imgButton->visible() && imgButton->contains(relMousePos) && !imgButton->textBoxVisible()) {
                    mDraggingStartPosition = relMousePos - imgButton->position();
                    mDragType = EMouseDragType::ImageButtonDrag;
                    mDraggedImageButtonId = i;
                    break;
                }
            }
        }
    }

    if (Screen::mouse_button_event(p, button, down, modifiers)) {
        return true;
    }

    // Hide caption textbox when the user performed mousedown on any other component
    if (down) {
        for (auto& b : mImageButtonContainer->children()) {
            dynamic_cast<ImageButton*>(b)->hideTextBox();
        }
    }

    auto* glfwWindow = screen()->glfw_window();
    if (down) {
        if (mDragType != EMouseDragType::ImageButtonDrag) {
            mDraggingStartPosition = p;
            if (canDragSidebarFrom(p)) {
                mDragType = EMouseDragType::SidebarDrag;
                return true;
            } else if (mImageCanvas->contains(p) && mCurrentImage) {
                mDragType = glfwGetKey(glfwWindow, GLFW_KEY_C) ? EMouseDragType::ImageCrop : EMouseDragType::ImageDrag;
                return true;
            }
        }
    } else {
        if (mDragType == EMouseDragType::ImageButtonDrag) {
            requestLayoutUpdate();
        } else if (mDragType == EMouseDragType::ImageCrop) {
            if (norm(mDraggingStartPosition - p) < CROP_MIN_SIZE) {
                // If the user did not drag the mouse far enough, we assume that they
                // wanted to reset the crop rather than create a new one.
                mImageCanvas->setCrop(std::nullopt);
                mCurrCrop = std::nullopt;
                requestLayoutUpdate();
            }
        }

        mDragType = EMouseDragType::None;
    }

    return false;
}

bool ImageViewer::mouse_motion_event(
    const nanogui::Vector2i& p,
    const nanogui::Vector2i& rel,
    int button,
    int modifiers
) {
    if (Screen::mouse_motion_event(p, rel, button, modifiers)) {
        return true;
    }

    // Only need high refresh rate responsiveness if tev is actually in focus.
    if (focused()) {
        redraw();
    }

    if (mDragType == EMouseDragType::SidebarDrag || canDragSidebarFrom(p)) {
        mSidebarLayout->set_cursor(Cursor::HResize);
        mImageCanvas->set_cursor(Cursor::HResize);
    } else {
        mSidebarLayout->set_cursor(Cursor::Arrow);
        mImageCanvas->set_cursor(Cursor::Arrow);
    }

    switch (mDragType) {
        case EMouseDragType::SidebarDrag:
            mSidebar->set_fixed_width(clamp(p.x(), SIDEBAR_MIN_WIDTH, m_size.x() - 10));
            requestLayoutUpdate();
            break;

        case EMouseDragType::ImageDrag: {
            nanogui::Vector2f relativeMovement = {rel};
            auto* glfwWindow = screen()->glfw_window();
            // There is no explicit access to the currently pressed modifier keys here, so we
            // need to directly ask GLFW.
            if (glfwGetKey(glfwWindow, GLFW_KEY_LEFT_SHIFT) || glfwGetKey(glfwWindow, GLFW_KEY_RIGHT_SHIFT)) {
                relativeMovement /= 10;
            } else if (glfwGetKey(glfwWindow, SYSTEM_COMMAND_LEFT) || glfwGetKey(glfwWindow, SYSTEM_COMMAND_RIGHT)) {
                relativeMovement /= std::log2(1.1f);
            }

            // If left mouse button is held, move the image with mouse movement
            if ((button & 1) != 0) {
                mImageCanvas->translate(relativeMovement);
            }

            // If middle mouse button is held, zoom in-out with up-down mouse movement
            if ((button & 4) != 0) {
                mImageCanvas->scale(relativeMovement.y() / 10.0f, Vector2f{mDraggingStartPosition});
            }

            break;
        }

        case EMouseDragType::ImageCrop: {
            Vector2i relStartMousePos = (absolute_position() + mDraggingStartPosition) - mImageCanvas->absolute_position();
            Vector2i relMousePos = (absolute_position() + p) - mImageCanvas->absolute_position();

            // Require a minimum movement to start cropping. Since this is measured in nanogui / screen space and not
            // image space, this does not prevent the cropping of smaller image regions. Just zoom in before cropping
            // smaller regions.
            if (norm(relStartMousePos - relMousePos) < CROP_MIN_SIZE) {
                return false;
            }

            auto startImageCoords = mImageCanvas->getDisplayWindowCoords(mCurrentImage.get(), relStartMousePos);
            auto imageCoords = mImageCanvas->getDisplayWindowCoords(mCurrentImage.get(), relMousePos);

            // sanitize the input crop
            Box2i crop = {{startImageCoords, imageCoords}};
            crop.max += Vector2i{1};

            // we do not need to worry about min/max ordering here, as setCrop sanitizes the input for us
            mImageCanvas->setCrop(crop);
            mCurrCrop = crop;
            requestLayoutUpdate();

            break;
        }

        case EMouseDragType::ImageButtonDrag: {
            auto& buttons = mImageButtonContainer->children();
            nanogui::Vector2i relMousePos = (absolute_position() + p) - mImageButtonContainer->absolute_position();

            TEV_ASSERT(mDraggedImageButtonId < buttons.size(), "Dragged image button id is out of bounds.");
            auto* draggedImgButton = dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId]);
            for (size_t i = 0; i < buttons.size(); ++i) {
                if (i == mDraggedImageButtonId) {
                    continue;
                }

                auto* imgButton = dynamic_cast<ImageButton*>(buttons[i]);
                if (imgButton->visible() && imgButton->contains(relMousePos)) {
                    nanogui::Vector2i pos = imgButton->position();
                    pos.y() += ((int)draggedImgButton->id() - (int)imgButton->id()) * imgButton->size().y();
                    imgButton->set_position(pos);
                    imgButton->mouse_enter_event(relMousePos, false);

                    moveImageInList(mDraggedImageButtonId, i);
                    mDraggedImageButtonId = i;
                    break;
                }
            }

            dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId])->set_position(
                relMousePos - mDraggingStartPosition
            );

            break;
        }

        case EMouseDragType::None:
            break;
    }

    return false;
}

bool ImageViewer::drop_event(const vector<string>& filenames) {
    if (Screen::drop_event(filenames)) {
        return true;
    }

    for (size_t i = 0; i < filenames.size(); ++i) {
        mImagesLoader->enqueue(toPath(filenames[i]), "", i == filenames.size() - 1);
    }

    // Make sure we gain focus after dragging files into here.
    focusWindow();
    redraw();
    return true;
}

bool ImageViewer::keyboard_event(int key, int scancode, int action, int modifiers) {
    if (Screen::keyboard_event(key, scancode, action, modifiers)) {
        return true;
    }

    redraw();

    int numGroups = mGroupButtonContainer->child_count();

    // Keybindings which should _not_ respond to repeats
    if (action == GLFW_PRESS) {
        // The checks for mod + GLFW_KEY_0 and GLFW_KEY_9 need to happen prior to checking for generic number
        // keys as they should take priority over group switching on Windows/Linux. No conflics on macOS.
        if (key == GLFW_KEY_0 && (modifiers & SYSTEM_COMMAND_MOD)) {
            mImageCanvas->resetTransform();
            return true;
        } else if (key == GLFW_KEY_F || (key == GLFW_KEY_9 && (modifiers & SYSTEM_COMMAND_MOD))) {
            if (mCurrentImage) {
                mImageCanvas->fitImageToScreen(*mCurrentImage);
            }
            return true;
        } else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
            int idx = (key - GLFW_KEY_1 + 10) % 10;
            if (modifiers & GLFW_MOD_SHIFT) {
                const auto& image = nthVisibleImage(idx);
                if (image) {
                    if (mCurrentReference == image) {
                        selectReference(nullptr);
                    } else {
                        selectReference(image);
                    }
                }
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (idx >= 0 && idx < numGroups) {
                    selectGroup(nthVisibleGroup(idx));
                }
            } else {
                const auto& image = nthVisibleImage(idx);
                if (image) {
                    selectImage(image);
                }
            }
            return true;
        } else if (key == GLFW_KEY_HOME || key == GLFW_KEY_END) {
            const auto& image = nthVisibleImage(key == GLFW_KEY_HOME ? 0 : mImages.size());
            if (modifiers & GLFW_MOD_SHIFT) {
                if (mCurrentReference == image) {
                    selectReference(nullptr);
                } else {
                    selectReference(image);
                }
            } else {
                selectImage(image);
            }
            return true;
#ifdef __APPLE__
        } else if (key == GLFW_KEY_ENTER) {
#else
        } else if (key == GLFW_KEY_F2) {
#endif
            if (mCurrentImage) {
                int id = imageId(mCurrentImage);
                dynamic_cast<ImageButton*>(mImageButtonContainer->child_at(id))->showTextBox();
                requestLayoutUpdate();
            }
            return true;
        } else if (key == GLFW_KEY_N) {
            normalizeExposureAndOffset();
            return true;
        } else if (key == GLFW_KEY_R) {
            if (modifiers & SYSTEM_COMMAND_MOD) {
                if (modifiers & GLFW_MOD_SHIFT) {
                    reloadAllImages();
                } else {
                    reloadImage(mCurrentImage);
                }
            } else {
                resetImage();
            }
            return true;
        } else if (key == GLFW_KEY_X) {
            // X for "eXplode channels
            if (mCurrentImage) {
                mCurrentImage->decomposeChannelGroup(mCurrentGroup);

                // Resets channel group buttons to include the now exploded channels
                selectImage(mCurrentImage);
            }

            if (mCurrentReference) {
                mCurrentReference->decomposeChannelGroup(mCurrentGroup);
                selectReference(mCurrentReference);
            }
        } else if (key == GLFW_KEY_B && (modifiers & SYSTEM_COMMAND_MOD)) {
            setUiVisible(!isUiVisible());
            return true;
        } else if (key == GLFW_KEY_O && (modifiers & SYSTEM_COMMAND_MOD)) {
            openImageDialog();
            return true;
        } else if (key == GLFW_KEY_S && (modifiers & SYSTEM_COMMAND_MOD)) {
            saveImageDialog();
            return true;
        } else if (key == GLFW_KEY_P && (modifiers & SYSTEM_COMMAND_MOD)) {
            mFilter->request_focus();
            return true;
        } else if (
            key == GLFW_KEY_H || /* question mark on US layout */ (
                key == GLFW_KEY_SLASH && (modifiers & GLFW_MOD_SHIFT)
            )
        ) {
            toggleHelpWindow();
            return true;
        } else if (key == GLFW_KEY_ENTER && modifiers & GLFW_MOD_ALT) {
            toggleMaximized();
            return true;
        } else if (key == GLFW_KEY_F5) {
            if (modifiers & SYSTEM_COMMAND_MOD) {
                reloadAllImages();
            } else {
                reloadImage(mCurrentImage);
            }
            return true;
        } else if (key == GLFW_KEY_F12) {
            // For debugging purposes.
            toggleConsole();
            return true;
        } else if (key == GLFW_KEY_SPACE) {
            setPlayingBack(!playingBack());
            return true;
        } else if (key == GLFW_KEY_L && mSupportsHdr) {
            mClipToLdrButton->set_pushed(!mClipToLdrButton->pushed());
            mImageCanvas->setClipToLdr(mClipToLdrButton->pushed());
            return true;
        } else if (key == GLFW_KEY_ESCAPE) {
            setFilter("");
            return true;
        } else if (key == GLFW_KEY_Q && (modifiers & SYSTEM_COMMAND_MOD)) {
            set_visible(false);
            return true;
        } else if (mCurrentImage && key == GLFW_KEY_C && (modifiers & SYSTEM_COMMAND_MOD)) {
            if (modifiers & GLFW_MOD_SHIFT) {
                if (clip::set_text(mCurrentImage->name())) {
                    tlog::success() << "Image path copied to clipboard.";
                } else {
                    tlog::error() << "Failed to copy image path to clipboard.";
                }
            } else if (auto imageSize = mImageCanvas->imageDataSize(); imageSize.x() > 0 && imageSize.y() > 0) {
                int resizedImageWidth = int(imageSize.x() * std::stof(mCopyResizeXTextBox->value()));
                int resizedImageHeight = int(imageSize.y() * std::stof(mCopyResizeYTextBox->value()));

                clip::image_spec imageMetadata;
                imageMetadata.width = resizedImageWidth;
                imageMetadata.height = resizedImageHeight;
                imageMetadata.bits_per_pixel = 32;
                imageMetadata.bytes_per_row = imageMetadata.bits_per_pixel / 8 * imageMetadata.width;

                imageMetadata.red_mask    = 0x000000ff;
                imageMetadata.green_mask  = 0x0000ff00;
                imageMetadata.blue_mask   = 0x00ff0000;
                imageMetadata.alpha_mask  = 0xff000000;
                imageMetadata.red_shift   = 0;
                imageMetadata.green_shift = 8;
                imageMetadata.blue_shift = 16;
                imageMetadata.alpha_shift = 24;

                auto resizeFunc = [this, &imageSize](const vector<float>& data) {
                    return resizeImageArray(data, imageSize.x(), imageSize.y());
                };

                auto imageDataResized = mImageCanvas->getLdrImageData(true, std::numeric_limits<int>::max(), resizeFunc);

                // // Print image array
                // for (int i = 0; i < imageSize.y(); i++) {
                //     for (int j = 0; j < imageSize.x(); j++) {
                //         printf("%f %f %f %f\n", floatData[4 * (i * imageSize.x() + j)], floatData[4 * (i * imageSize.x() + j) + 1],
                //         floatData[4 * (i * imageSize.x() + j) + 2], floatData[4 * (i * imageSize.x() + j) + 3]);
                //     }
                //     std::cout << std::endl;
                // }

                // auto imageDataResized = resizeImageArray(imageData, imageSize.x(), imageSize.y());

                clip::image image(imageDataResized.data(), imageMetadata);

                if (clip::set_image(image)) {
                    tlog::success() << "Image copied to clipboard.";
                } else {
                    tlog::error() << "Failed to copy image to clipboard.";
                }
            }
        } else if (key == GLFW_KEY_V && (modifiers & SYSTEM_COMMAND_MOD)) {
            // if (clip::has(clip::text_format())) {
            //     string path;
            //     if (!clip::get_text(path)) {
            //         tlog::error() << "Failed to paste text from clipboard.";
            //     } else {
            //         auto image = tryLoadImage(toPath(path), "");
            //         if (image) {
            //             addImage(image, true);
            //         } else {
            //             tlog::error() << fmt::format("Failed to load image from clipboard path: {}", path);
            //         }
            //     }
            // } else
            if (clip::has(clip::image_format())) {
                clip::image clipImage;
                if (!clip::get_image(clipImage)) {
                    tlog::error() << "Failed to paste image from clipboard.";
                } else {
                    tlog::info() << "Loading image from clipboard...";
                    stringstream imageStream;
                    imageStream
                        << "clip"
                        << string(reinterpret_cast<const char*>(&clipImage.spec()), sizeof(clip::image_spec))
                        << string(clipImage.data(), clipImage.spec().bytes_per_row * clipImage.spec().height)
                        ;

                    auto images = tryLoadImage(
                        fmt::format("clipboard ({})", ++mClipboardIndex),
                        imageStream,
                        ""
                    ).get();
                    if (images.empty()) {
                        tlog::error() << "Failed to load image from clipboard data.";
                    } else {
                        for (auto& image : images) {
                            addImage(image, true);
                        }
                    }
                }
            }
        }
    }

    // Keybindings which should respond to repeats
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (key == GLFW_KEY_KP_ADD || key == GLFW_KEY_EQUAL ||
            key == GLFW_KEY_KP_SUBTRACT || key == GLFW_KEY_MINUS) {
            float scaleAmount = 1.0f;
            if (modifiers & GLFW_MOD_SHIFT) {
                scaleAmount /= 10;
            } else if (modifiers & SYSTEM_COMMAND_MOD) {
                scaleAmount /= std::log2(1.1f);
            }

            if (key == GLFW_KEY_KP_SUBTRACT || key == GLFW_KEY_MINUS) {
                scaleAmount = -scaleAmount;
            }

            nanogui::Vector2f origin =
                nanogui::Vector2f{mImageCanvas->position()} +
                nanogui::Vector2f{mImageCanvas->size()} * 0.5f;

            mImageCanvas->scale(
                scaleAmount,
                {origin.x(), origin.y()}
            );
        }

        if (key == GLFW_KEY_E) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setExposure(exposure() - 0.5f);
            } else {
                setExposure(exposure() + 0.5f);
            }
        }

        if (key == GLFW_KEY_O) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setOffset(offset() - 0.1f);
            } else {
                setOffset(offset() + 0.1f);
            }
        }

        if (mGammaSlider->enabled()) {
            if (key == GLFW_KEY_G) {
                if (modifiers & GLFW_MOD_SHIFT) {
                    setGamma(gamma() - 0.1f);
                } else {
                    setGamma(gamma() + 0.1f);
                }
            }
        }

        if (key == GLFW_KEY_W && (modifiers & SYSTEM_COMMAND_MOD)) {
            if (modifiers & GLFW_MOD_SHIFT) {
                removeAllImages();
            } else {
                removeImage(mCurrentImage);
            }
        } else if (
            key == GLFW_KEY_UP || key == GLFW_KEY_W || key == GLFW_KEY_PAGE_UP || (
                key == GLFW_KEY_TAB && (modifiers & GLFW_MOD_CONTROL) && (modifiers & GLFW_MOD_SHIFT)
            )
        ) {
            if (key != GLFW_KEY_TAB && (modifiers & GLFW_MOD_SHIFT)) {
                selectReference(nextImage(mCurrentReference, Backward));
            } else {
                selectImage(nextImage(mCurrentImage, Backward));
            }
        } else if (
            key == GLFW_KEY_DOWN || key == GLFW_KEY_S || key == GLFW_KEY_PAGE_DOWN || (
                key == GLFW_KEY_TAB && (modifiers & GLFW_MOD_CONTROL) && !(modifiers & GLFW_MOD_SHIFT)
            )
        ) {
            if (key != GLFW_KEY_TAB && (modifiers & GLFW_MOD_SHIFT)) {
                selectReference(nextImage(mCurrentReference, Forward));
            } else {
                selectImage(nextImage(mCurrentImage, Forward));
            }
        }

        if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_D || key == GLFW_KEY_RIGHT_BRACKET) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>((tonemap() + 1) % NumTonemaps));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>((metric() + 1) % NumMetrics));
                }
            } else {
                selectGroup(nextGroup(mCurrentGroup, Forward));
            }
        } else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_A || key == GLFW_KEY_LEFT_BRACKET) {
            if (modifiers & GLFW_MOD_SHIFT) {
                setTonemap(static_cast<ETonemap>((tonemap() - 1 + NumTonemaps) % NumTonemaps));
            } else if (modifiers & GLFW_MOD_CONTROL) {
                if (mCurrentReference) {
                    setMetric(static_cast<EMetric>((metric() - 1 + NumMetrics) % NumMetrics));
                }
            } else {
                selectGroup(nextGroup(mCurrentGroup, Backward));
            }
        }
    }

    return false;
}

void ImageViewer::focusWindow() {
    glfwFocusWindow(m_glfw_window);
}

void ImageViewer::draw_contents() {
    // HACK HACK HACK: on Windows, when restoring a window from maximization,
    //                 the old window size is restored _several times_, necessitating
    //                 a repeated resize to the actually desired window size.
    if (mDidFitToImage < 3 && !isMaximized()) {
        resizeToFit(sizeToFitAllImages());
        ++mDidFitToImage;
    }

    clear();

    // If playing back, ensure correct frame pacing
    if (playingBack() && mTaskQueue.empty()) {
        auto fps = clamp(mFpsTextBox->value(), 1, 1000);
        auto seconds_per_frame = chrono::duration<float>{1.0f / fps};
        auto now = chrono::steady_clock::now();

        if (now - mLastPlaybackFrameTime > 500s) {
            // If lagging behind too far, drop the frames, but otherwise...
            mLastPlaybackFrameTime = now;
            selectImage(nextImage(mCurrentImage, Forward), false);
        } else {
            // ...advance by as many frames as the user-specified FPS would
            // demand, given the elapsed time since the last render.
            while (now - mLastPlaybackFrameTime >= seconds_per_frame) {
                mLastPlaybackFrameTime += chrono::duration_cast<chrono::steady_clock::duration>(seconds_per_frame);
                selectImage(nextImage(mCurrentImage, Forward), false);
            }
        }

        redraw();
    }

    // If watching files for changes, do so every 100ms
    if (watchFilesForChanges()) {
        auto now = chrono::steady_clock::now();
        if (now - mLastFileChangesCheckTime >= 100ms) {
            reloadImagesWhoseFileChanged();
            mImagesLoader->checkDirectoriesForNewFilesAndLoadThose();
            mLastFileChangesCheckTime = now;
        }
    }

    // In case any images got loaded in the background, they sit around in mImagesLoader. Here is the
    // place where we actually add them to the GUI. Focus the application in case one of the
    // new images is meant to override the current selection.
    bool newFocus = false;
    while (auto addition = mImagesLoader->tryPop()) {
        newFocus |= addition->shallSelect;

        bool first = true;
        for (auto& image : addition->images) {
            // If the loaded file consists of multiple images (such as multi-part EXRs),
            // select the first part if selection is desired.
            bool shallSelect = first ? addition->shallSelect : false;
            if (addition->toReplace) {
                replaceImage(addition->toReplace, image, shallSelect);
            } else {
                addImage(image, shallSelect);
            }
            first = false;
        }
    }

    if (newFocus) {
        focusWindow();
    }

    // mTaskQueue contains jobs that should be executed on the main thread. It is useful for handling
    // callbacks from background threads
    while (auto task = mTaskQueue.tryPop()) {
        (*task)();
    }

    for (auto it = begin(mToBump); it != end(mToBump); ) {
        auto& image = *it;
        bool isShown = image == mCurrentImage || image == mCurrentReference;

        // If the image is no longer shown, bump ID immediately. Otherwise, wait until canvas statistics were ready for over 200 ms.
        if (
            !isShown || std::chrono::steady_clock::now() - mImageCanvas->canvasStatistics()->becameReadyAt() > 200ms
        ) {
            image->bumpId();
            auto localIt = it;
            ++it;
            mToBump.erase(localIt);
        } else {
            ++it;
        }
    }

    if (mRequiresFilterUpdate) {
        updateFilter();
        mRequiresFilterUpdate = false;
    }

    bool anyImageVisible = mCurrentImage || mCurrentReference || std::any_of(
        begin(mImageButtonContainer->children()),
        end(mImageButtonContainer->children()),
        [](const auto& c) { return c->visible(); }
    );

    for (auto button : mAnyImageButtons) {
        button->set_enabled(anyImageVisible);
    }

    if (mRequiresLayoutUpdate) {
        nanogui::Vector2i oldDraggedImageButtonPos{0, 0};
        auto& buttons = mImageButtonContainer->children();
        if (mDragType == EMouseDragType::ImageButtonDrag) {
            oldDraggedImageButtonPos = dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId])->position();
        }

        updateLayout();
        mRequiresLayoutUpdate = false;

        if (mDragType == EMouseDragType::ImageButtonDrag) {
            dynamic_cast<ImageButton*>(buttons[mDraggedImageButtonId])->set_position(oldDraggedImageButtonPos);
        }
    }

    updateTitle();

    // Update histogram
    static const string histogramTooltipBase = "Histogram of color values. Adapts to the currently chosen channel group and error metric.";
    auto lazyCanvasStatistics = mImageCanvas->canvasStatistics();
    if (lazyCanvasStatistics) {
        if (lazyCanvasStatistics->isReady()) {
            auto statistics = lazyCanvasStatistics->get();
            mHistogram->setNChannels(statistics->nChannels);
            mHistogram->setValues(statistics->histogram);
            mHistogram->setMinimum(statistics->minimum);
            mHistogram->setMean(statistics->mean);
            mHistogram->setMaximum(statistics->maximum);
            mHistogram->setZero(statistics->histogramZero);
            mHistogram->set_tooltip(fmt::format(
                "{}\n\n"
                "Minimum: {:.6f}\n"
                "Mean: {:.6f}\n"
                "Maximum: {:.6f}\n"
                "Variance: {:.6f}",
                histogramTooltipBase,
                statistics->minimum,
                statistics->mean,
                statistics->maximum,
                statistics->variance)
            );
        }
    } else {
        mHistogram->setNChannels(1);
        mHistogram->setValues({0.0f});
        mHistogram->setMinimum(0);
        mHistogram->setMean(0);
        mHistogram->setMaximum(0);
        mHistogram->setZero(0);
        mHistogram->set_tooltip(
            fmt::format("{}", histogramTooltipBase)
        );
    }
}

void ImageViewer::insertImage(shared_ptr<Image> image, size_t index, bool shallSelect) {
    if (!image) {
        throw invalid_argument{"Image may not be null."};
    }

    if (mDragType == EMouseDragType::ImageButtonDrag && index <= mDraggedImageButtonId) {
        ++mDraggedImageButtonId;
    }

    auto button = new ImageButton{nullptr, image->name(), true};
    button->set_font_size(15);
    button->setId(index + 1);
    button->set_tooltip(image->toString());

    button->setSelectedCallback([this, image]() {
        selectImage(image);
    });

    button->setReferenceCallback([this, image](bool isReference) {
        if (!isReference) {
            selectReference(nullptr);
        } else {
            selectReference(image);
        }
    });

    button->setCaptionChangeCallback([this]() {
        mRequiresFilterUpdate = true;
    });

    mImageButtonContainer->add_child((int)index, button);
    mImages.insert(begin(mImages) + index, image);

    mShouldFooterBeVisible |= image->channelGroups().size() > 1;
    // The following call will make sure the footer becomes visible
    // if the previous line enabled it.
    setUiVisible(isUiVisible());

    // Ensure the new image button will have the correct visibility state.
    setFilter(mFilter->value());

    requestLayoutUpdate();

    // First image got added, let's select it.
    if ((index == 0 && mImages.size() == 1) || shallSelect) {
        selectImage(image);
        if (!isMaximized()) {
            resizeToFit(sizeToFitImage(image));
        }
    }
}

void ImageViewer::moveImageInList(size_t oldIndex, size_t newIndex) {
    if (oldIndex == newIndex) {
        return;
    }

    TEV_ASSERT(oldIndex < mImages.size(), "oldIndex must be smaller than the number of images.");
    TEV_ASSERT(newIndex < mImages.size(), "newIndex must be smaller than the number of images.");

    auto* button = dynamic_cast<ImageButton*>(mImageButtonContainer->child_at((int)oldIndex));
    TEV_ASSERT(button, "Image button must exist.");

    button->inc_ref();
    mImageButtonContainer->remove_child_at((int)oldIndex);
    mImageButtonContainer->add_child((int)newIndex, button);
    button->dec_ref();

    int change = newIndex > oldIndex ? 1 : -1;
    for (size_t i = oldIndex; i != newIndex; i += change) {
        auto* curButton = dynamic_cast<ImageButton*>(mImageButtonContainer->child_at((int)i));
        if (curButton->visible()) {
            curButton->setId(curButton->id() - change);
            button->setId(button->id() + change);
        }
    }

    auto img = mImages[oldIndex];
    mImages.erase(mImages.begin() + oldIndex);
    mImages.insert(mImages.begin() + newIndex, img);

    requestLayoutUpdate();
}

void ImageViewer::removeImage(shared_ptr<Image> image) {
    int id = imageId(image);
    if (id == -1) {
        return;
    }

    if (mDragType == EMouseDragType::ImageButtonDrag) {
        // If we're currently dragging the to-be-removed image, stop.
        if ((size_t)id == mDraggedImageButtonId) {
            requestLayoutUpdate();
            mDragType = EMouseDragType::None;
        } else if ((size_t)id < mDraggedImageButtonId) {
            --mDraggedImageButtonId;
        }
    }

    auto nextCandidate = nextImage(image, Forward);
    // If we rolled over, let's rather use the previous image.
    // We don't want to jumpt to the beginning when deleting the
    // last image in our list.
    if (imageId(nextCandidate) < id) {
        nextCandidate = nextImage(image, Backward);
    }

    // If `nextImage` produced the same image again, this means
    // that `image` is the only (visible) image and hence, after
    // removal, should be replaced by no selection at all.
    if (nextCandidate == image) {
        nextCandidate = nullptr;
    }

    // Reset all focus as a workaround a crash caused by nanogui.
    // TODO: Remove once a fix exists.
    request_focus();

    mImages.erase(begin(mImages) + id);
    mImageButtonContainer->remove_child_at(id);

    if (mImages.empty()) {
        selectImage(nullptr);
        selectReference(nullptr);
        return;
    }

    if (mCurrentImage == image) {
        selectImage(nextCandidate);
    }

    if (mCurrentReference == image) {
        selectReference(nextCandidate);
    }
}

void ImageViewer::removeAllImages() {
    if (mImages.empty()) {
        return;
    }

    // Reset all focus as a workaround a crash caused by nanogui.
    // TODO: Remove once a fix exists.
    request_focus();

    for (int i = (int)mImages.size() - 1; i >= 0; --i) {
        if (mImageButtonContainer->child_at(i)->visible()) {
            mImages.erase(begin(mImages) + i);
            mImageButtonContainer->remove_child_at(i);
        }
    }

    // No images left to select
    selectImage(nullptr);
    selectReference(nullptr);
}

void ImageViewer::replaceImage(shared_ptr<Image> image, shared_ptr<Image> replacement, bool shallSelect) {
    if (replacement == nullptr) {
        throw std::runtime_error{"Must not replace image with nullptr."};
    }

    int currentId = imageId(mCurrentImage);
    int id = imageId(image);
    if (id == -1) {
        addImage(replacement, shallSelect);
        return;
    }

    // Preserve image button caption when replacing an image
    ImageButton* ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children()[id]);
    std::string caption = ib->caption();

    // If we already have the image selected, we must re-select it
    // regardless of the `shallSelect` parameter.
    shallSelect |= currentId == id;

    int referenceId = imageId(mCurrentReference);

    removeImage(image);
    insertImage(replacement, id, shallSelect);

    ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children()[id]);
    ib->setCaption(caption);

    if (referenceId != -1) {
        selectReference(mImages[referenceId]);
    }
}

void ImageViewer::reloadImage(shared_ptr<Image> image, bool shallSelect) {
    int id = imageId(image);
    if (id == -1) {
        return;
    }

    mImagesLoader->enqueue(image->path(), image->channelSelector(), shallSelect, image);
}

void ImageViewer::reloadAllImages() {
    for (size_t i = 0; i < mImages.size(); ++i) {
        reloadImage(mImages[i]);
    }
}

void ImageViewer::reloadImagesWhoseFileChanged() {
    for (size_t i = 0; i < mImages.size(); ++i) {
        auto& image = mImages[i];
        if (!fs::exists(image->path())) {
            continue;
        }

        fs::file_time_type fileLastModified;

        // Unlikely, but the file could have been deleted, moved, or something
        // else could have happened to it that makes obtaining its last modified
        // time impossible. Ignore such errors.
        try {
            fileLastModified = fs::last_write_time(image->path());
        } catch (...) {
            continue;
        }

        if (fileLastModified != image->fileLastModified()) {
            // Updating the last-modified date prevents double-scheduled
            // reloads if the load take a lot of time or fails.
            image->setFileLastModified(fileLastModified);
            reloadImage(image);
        }
    }
}

void ImageViewer::updateImage(
    const string& imageName,
    bool shallSelect,
    const string& channel,
    int x, int y,
    int width, int height,
    const vector<float>& imageData
) {
    auto image = imageByName(imageName);
    if (!image) {
        tlog::warning() << "Image " << imageName << " could not be updated, because it does not exist.";
        return;
    }

    image->updateChannel(channel, x, y, width, height, imageData);
    if (shallSelect) {
        selectImage(image);
    }

    // This image needs newly computed statistics... so give it a new ID.
    // However, if the image is currently shown, we don't want to overwhelm
    // the CPU, so we only launch new statistics computations every so often.
    // These computations are scheduled from `drawContents` via the `mToBump` set.
    if (image != mCurrentImage && image != mCurrentReference) {
        image->bumpId();
    } else {
        mToBump.insert(image);
    }
}

void ImageViewer::updateImageVectorGraphics(
    const string& imageName,
    bool shallSelect,
    bool append,
    const vector<VgCommand>& commands
) {
    auto image = imageByName(imageName);
    if (!image) {
        tlog::warning() << "Vector graphics of image " << imageName << " could not be updated, because it does not exist.";
        return;
    }

    image->updateVectorGraphics(append, commands);
    if (shallSelect) {
        selectImage(image);
    }
}

void ImageViewer::selectImage(const shared_ptr<Image>& image, bool stopPlayback) {
    if (stopPlayback) {
        mPlayButton->set_pushed(false);
    }

    for (auto button : mCurrentImageButtons) {
        button->set_enabled(image != nullptr);
    }

    if (!image) {
        auto& buttons = mImageButtonContainer->children();
        for (size_t i = 0; i < buttons.size(); ++i) {
            dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(false);
        }

        mCurrentImage = nullptr;
        mImageCanvas->setImage(nullptr);

        // Clear group buttons
        while (mGroupButtonContainer->child_count() > 0) {
            mGroupButtonContainer->remove_child_at(mGroupButtonContainer->child_count() - 1);
        }

        requestLayoutUpdate();
        return;
    }

    size_t id = (size_t)max(0, imageId(image));

    // Don't do anything if the image that wants to be selected is not visible.
    if (!mImageButtonContainer->child_at((int)id)->visible()) {
        return;
    }

    auto& buttons = mImageButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(i == id);
    }

    mCurrentImage = image;
    mImageCanvas->setImage(mCurrentImage);

    // Clear group buttons
    while (mGroupButtonContainer->child_count() > 0) {
        mGroupButtonContainer->remove_child_at(mGroupButtonContainer->child_count() - 1);
    }

    size_t numGroups = mCurrentImage->channelGroups().size();
    for (size_t i = 0; i < numGroups; ++i) {
        auto group = groupName(i);
        auto button = new ImageButton{mGroupButtonContainer, group, false};
        button->set_font_size(15);
        button->setId(i + 1);

        button->setSelectedCallback([this, group]() {
            selectGroup(group);
        });
    }

    mShouldFooterBeVisible |= image->channelGroups().size() > 1;
    // The following call will make sure the footer becomes visible
    // if the previous line enabled it.
    setUiVisible(isUiVisible());

    // Setting the filter again makes sure, that groups are correctly filtered.
    setFilter(mFilter->value());
    updateLayout();

    // This will automatically fall back to the root group if the current
    // group isn't found.
    selectGroup(mCurrentGroup);

    // Ensure the currently active image button is always fully on-screen
    Widget* activeImageButton = nullptr;
    for (Widget* widget : mImageButtonContainer->children()) {
        if (dynamic_cast<ImageButton*>(widget)->isSelected()) {
            activeImageButton = widget;
            break;
        }
    }

    if (activeImageButton) {
        float divisor = mScrollContent->height() - mImageScrollContainer->height();
        if (divisor > 0) {
            mImageScrollContainer->set_scroll(clamp(
                mImageScrollContainer->scroll(),
                (activeImageButton->position().y() + activeImageButton->height() - mImageScrollContainer->height()) / divisor,
                activeImageButton->position().y() / divisor
            ));
        }
    }
}

void ImageViewer::selectGroup(string group) {
    // If the group does not exist, select the first group.
    size_t id = (size_t)max(0, groupId(group));

    auto& buttons = mGroupButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        dynamic_cast<ImageButton*>(buttons[i])->setIsSelected(i == id);
    }

    mCurrentGroup = groupName(id);
    mImageCanvas->setRequestedChannelGroup(mCurrentGroup);

    // Ensure the currently active group button is always fully on-screen
    Widget* activeGroupButton = nullptr;
    for (Widget* widget : mGroupButtonContainer->children()) {
        if (dynamic_cast<ImageButton*>(widget)->isSelected()) {
            activeGroupButton = widget;
            break;
        }
    }

    // Ensure the currently active group button is always fully on-screen
    if (activeGroupButton) {
        mGroupButtonContainer->set_position(nanogui::Vector2i{
            clamp(
                mGroupButtonContainer->position().x(),
                -activeGroupButton->position().x(),
                m_size.x() - activeGroupButton->position().x() - activeGroupButton->width()
            ),
            0
        });
    }
}

void ImageViewer::selectReference(const shared_ptr<Image>& image) {
    if (!image) {
        auto& buttons = mImageButtonContainer->children();
        for (size_t i = 0; i < buttons.size(); ++i) {
            dynamic_cast<ImageButton*>(buttons[i])->setIsReference(false);
        }

        auto& metricButtons = mMetricButtonContainer->children();
        for (size_t i = 0; i < metricButtons.size(); ++i) {
            dynamic_cast<Button*>(metricButtons[i])->set_enabled(false);
        }

        mCurrentReference = nullptr;
        mImageCanvas->setReference(nullptr);
        return;
    }

    size_t id = (size_t)max(0, imageId(image));

    auto& buttons = mImageButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        dynamic_cast<ImageButton*>(buttons[i])->setIsReference(i == id);
    }

    auto& metricButtons = mMetricButtonContainer->children();
    for (size_t i = 0; i < metricButtons.size(); ++i) {
        dynamic_cast<Button*>(metricButtons[i])->set_enabled(true);
    }

    mCurrentReference = image;
    mImageCanvas->setReference(mCurrentReference);

    // Ensure the currently active reference button is always fully on-screen
    Widget* activeReferenceButton = nullptr;
    for (Widget* widget : mImageButtonContainer->children()) {
        if (dynamic_cast<ImageButton*>(widget)->isReference()) {
            activeReferenceButton = widget;
            break;
        }
    }

    if (activeReferenceButton) {
        float divisor = mScrollContent->height() - mImageScrollContainer->height();
        if (divisor > 0) {
            mImageScrollContainer->set_scroll(clamp(
                mImageScrollContainer->scroll(),
                (activeReferenceButton->position().y() + activeReferenceButton->height() - mImageScrollContainer->height()) / divisor,
                activeReferenceButton->position().y() / divisor
            ));
        }
    }
}

void ImageViewer::setExposure(float value) {
    value = round(value, 1.0f);
    mExposureSlider->set_value(value);
    mExposureLabel->set_caption(fmt::format("Exposure: {:+.1f}", value));

    mImageCanvas->setExposure(value);
}

void ImageViewer::setOffset(float value) {
    value = round(value, 2.0f);
    mOffsetSlider->set_value(value);
    mOffsetLabel->set_caption(fmt::format("Offset: {:+.2f}", value));

    mImageCanvas->setOffset(value);
}

void ImageViewer::setGamma(float value) {
    value = round(value, 2.0f);
    mGammaSlider->set_value(value);
    mGammaLabel->set_caption(fmt::format("Gamma: {:+.2f}", value));

    mImageCanvas->setGamma(value);
}

void ImageViewer::normalizeExposureAndOffset() {
    if (!mCurrentImage) {
        return;
    }

    auto channels = mCurrentImage->channelsInGroup(mCurrentGroup);

    float minimum = numeric_limits<float>::max();
    float maximum = numeric_limits<float>::min();
    for (const auto& channelName : channels) {
        const auto& channel = mCurrentImage->channel(channelName);
        auto [cmin, cmax, cmean] = channel->minMaxMean();
        maximum = max(maximum, cmax);
        minimum = min(minimum, cmin);
    }

    float factor = 1.0f / (maximum - minimum);
    setExposure(log2(factor));
    setOffset(-minimum * factor);
}

void ImageViewer::resetImage() {
    setExposure(0);
    setOffset(0);
    setGamma(2.2f);
    mImageCanvas->resetTransform();
}

void ImageViewer::setTonemap(ETonemap tonemap) {
    mImageCanvas->setTonemap(tonemap);
    auto& buttons = mTonemapButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        Button* b = dynamic_cast<Button*>(buttons[i]);
        b->set_pushed((ETonemap)i == tonemap);
    }

    mGammaSlider->set_enabled(tonemap == ETonemap::Gamma);
    mGammaLabel->set_color(
        tonemap == ETonemap::Gamma ? mGammaLabel->theme()->m_text_color : Color{0.5f, 1.0f}
    );
}

void ImageViewer::setMetric(EMetric metric) {
    mImageCanvas->setMetric(metric);
    auto& buttons = mMetricButtonContainer->children();
    for (size_t i = 0; i < buttons.size(); ++i) {
        Button* b = dynamic_cast<Button*>(buttons[i]);
        b->set_pushed((EMetric)i == metric);
    }
}

nanogui::Vector2i ImageViewer::sizeToFitImage(const shared_ptr<Image>& image) {
    if (!image) {
        return m_size;
    }

    nanogui::Vector2i requiredSize{image->size().x(), image->size().y()};

    // Convert from image pixel coordinates to nanogui coordinates.
    requiredSize = nanogui::Vector2i{nanogui::Vector2f{requiredSize} / pixel_ratio()};

    // Take into account the size of the UI.
    if (mSidebar->visible()) {
        requiredSize.x() += mSidebar->fixed_width();
    }

    if (mFooter->visible()) {
        requiredSize.y() += mFooter->fixed_height();
    }

    return requiredSize;
}

nanogui::Vector2i ImageViewer::sizeToFitAllImages() {
    nanogui::Vector2i result = m_size;
    for (const auto& image : mImages) {
        result = max(result, sizeToFitImage(image));
    }
    return result;
}

void ImageViewer::resizeToFit(nanogui::Vector2i targetSize) {
    // Only increase our current size if we are larger than the current size of the window.
    targetSize = max(m_size, targetSize);
    if (targetSize == m_size) {
        return;
    }

    // For sanity, don't make us larger than 8192x8192 to ensure that we
    // don't break any texture size limitations of the user's GPU.
    targetSize = min(targetSize, mMaxSize);
    set_size(targetSize);
}

bool ImageViewer::playingBack() const {
    return mPlayButton->pushed();
}

void ImageViewer::setPlayingBack(bool value) {
    mPlayButton->set_pushed(value);
    mLastPlaybackFrameTime = chrono::steady_clock::now();
    redraw();
}

bool ImageViewer::setFilter(const string& filter) {
    mFilter->set_value(filter);
    mRequiresFilterUpdate = true;
    return true;
}

bool ImageViewer::useRegex() const {
    return mRegexButton->pushed();
}

void ImageViewer::setUseRegex(bool value) {
    mRegexButton->set_pushed(value);
    mRequiresFilterUpdate = true;
}

bool ImageViewer::watchFilesForChanges() const {
    return mWatchFilesForChangesButton->pushed();
}

void ImageViewer::setWatchFilesForChanges(bool value) {
    mWatchFilesForChangesButton->set_pushed(value);
}

void ImageViewer::maximize() {
    glfwMaximizeWindow(m_glfw_window);
}

bool ImageViewer::isMaximized() {
    return glfwGetWindowAttrib(m_glfw_window, GLFW_MAXIMIZED) != 0;
}

void ImageViewer::toggleMaximized() {
    if (isMaximized()) {
        glfwRestoreWindow(m_glfw_window);
    } else {
        maximize();
    }
}

void ImageViewer::setUiVisible(bool shouldBeVisible) {
    if (!shouldBeVisible && mDragType == EMouseDragType::SidebarDrag) {
        mDragType = EMouseDragType::None;
    }

    mSidebar->set_visible(shouldBeVisible);
    mFooter->set_visible(mShouldFooterBeVisible && shouldBeVisible);

    requestLayoutUpdate();
}

void ImageViewer::toggleHelpWindow() {
    if (mHelpWindow) {
        mHelpWindow->dispose();
        mHelpWindow = nullptr;
        mHelpButton->set_pushed(false);
    } else {
        mHelpWindow = new HelpWindow{this, mSupportsHdr, [this] { toggleHelpWindow(); }};
        mHelpWindow->center();
        mHelpWindow->request_focus();
        mHelpButton->set_pushed(true);
    }

    requestLayoutUpdate();
}

void ImageViewer::openImageDialog() {
    vector<string> paths = file_dialog(
    {
        // HDR formats
        {"exr",  "OpenEXR image"},
        {"hdr",  "HDR image"},
        {"pfm",  "Portable Float Map image"},
        // LDR formats
        {"bmp",  "Bitmap Image File"},
        {"gif",  "Graphics Interchange Format image"},
        {"jpg",  "JPEG image"},
        {"jpeg", "JPEG image"},
        {"pic",  "PIC image"},
        {"pgm",  "Portable GrayMap image"},
        {"png",  "Portable Network Graphics image"},
        {"pnm",  "Portable AnyMap image"},
        {"ppm",  "Portable PixMap image"},
        {"psd",  "PSD image"},
        {"qoi",  "Quite OK Image format"},
        {"tga",  "Truevision TGA image"},
    }, false, true);

    for (size_t i = 0; i < paths.size(); ++i) {
        bool shallSelect = i == paths.size() - 1;
        mImagesLoader->enqueue(toPath(paths[i]), "", shallSelect);
    }

    // Make sure we gain focus after seleting a file to be loaded.
    focusWindow();
}

void ImageViewer::saveImageDialog() {
    if (!mCurrentImage) {
        return;
    }

    fs::path path = toPath(
        file_dialog(
            {
                {"exr",  "OpenEXR image"},
                {"hdr",  "HDR image"},
                {"bmp",  "Bitmap Image File"},
                {"jpg",  "JPEG image"},
                {"jpeg", "JPEG image"},
                {"png",  "Portable Network Graphics image"},
                {"qoi",  "Quite OK Image format"},
                {"tga",  "Truevision TGA image"},
            },
            true
        )
    );

    if (path.empty()) {
        return;
    }

    try {
        mImageCanvas->saveImage(path);
    } catch (const invalid_argument& e) {
        new MessageDialog(
            this,
            MessageDialog::Type::Warning,
            "Error",
            fmt::format("Failed to save image: {}", e.what())
        );
    }

    // Make sure we gain focus after selecting a file to be loaded.
    focusWindow();
}

void ImageViewer::updateFilter() {
    string filter = mFilter->value();
    string imagePart = filter;
    string groupPart = "";

    auto colonPos = filter.find_last_of(':');
    if (colonPos != string::npos) {
        imagePart = filter.substr(0, colonPos);
        groupPart = filter.substr(colonPos + 1);
    }

    // Image filtering
    {
        // Checks whether an image matches the filter.
        // This is the case if the image name matches the image part
        // and at least one of the image's groups matches the group part.
        auto doesImageMatch = [&](const auto& name, const auto& channelGroups) {
            bool doesMatch = matchesFuzzyOrRegex(name, imagePart, useRegex());
            if (doesMatch) {
                bool anyGroupsMatch = false;
                for (const auto& group : channelGroups) {
                    if (matchesFuzzyOrRegex(group.name, groupPart, useRegex())) {
                        anyGroupsMatch = true;
                        break;
                    }
                }

                if (!anyGroupsMatch) {
                    doesMatch = false;
                }
            }

            return doesMatch;
        };

        vector<string> activeImageNames;
        size_t id = 1;
        for (size_t i = 0; i < mImages.size(); ++i) {
            ImageButton* ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children()[i]);
            ib->set_visible(doesImageMatch(ib->caption(), mImages[i]->channelGroups()));
            if (ib->visible()) {
                ib->setId(id++);
                activeImageNames.emplace_back(ib->caption());
            }
        }

        int beginOffset = 0, endOffset = 0;
        if (!activeImageNames.empty()) {
            string first = activeImageNames.front();
            int firstSize = (int)first.size();
            if (firstSize > 0) {
                bool allStartWithSameChar;
                do {
                    int len = codePointLength(first[beginOffset]);

                    allStartWithSameChar = all_of(
                        begin(activeImageNames),
                        end(activeImageNames),
                        [&first, beginOffset, len](const string& name) {
                            if (beginOffset + len > (int)name.size()) {
                                return false;
                            }
                            for (int i = beginOffset; i < beginOffset + len; ++i) {
                                if (name[i] != first[i]) {
                                    return false;
                                }
                            }
                            return true;
                        }
                    );

                    if (allStartWithSameChar) {
                        beginOffset += len;
                    }
                } while (allStartWithSameChar && beginOffset < firstSize);

                bool allEndWithSameChar;
                do {
                    char lastChar = first[firstSize - endOffset - 1];
                    allEndWithSameChar = all_of(
                        begin(activeImageNames),
                        end(activeImageNames),
                        [lastChar, endOffset](const string& name) {
                            int index = (int)name.size() - endOffset - 1;
                            return index >= 0 && name[index] == lastChar;
                        }
                    );

                    if (allEndWithSameChar) {
                        ++endOffset;
                    }
                } while (allEndWithSameChar && endOffset < firstSize);
            }
        }

        bool currentImageMatchesFilter = false;
        for (size_t i = 0; i < mImages.size(); ++i) {
            ImageButton* ib = dynamic_cast<ImageButton*>(mImageButtonContainer->children()[i]);
            if (ib->visible()) {
                currentImageMatchesFilter |= mImages[i] == mCurrentImage;
                ib->setHighlightRange(beginOffset, endOffset);
            }
        }

        if (!currentImageMatchesFilter) {
            selectImage(nthVisibleImage(0));
        }

        if (mCurrentReference && !matchesFuzzyOrRegex(mCurrentReference->name(), imagePart, useRegex())) {
            selectReference(nullptr);
        }
    }

    // Group filtering
    if (mCurrentImage) {
        size_t id = 1;
        const auto& buttons = mGroupButtonContainer->children();
        for (Widget* button : buttons) {
            ImageButton* ib = dynamic_cast<ImageButton*>(button);
            ib->set_visible(matchesFuzzyOrRegex(ib->caption(), groupPart, useRegex()));
            if (ib->visible()) {
                ib->setId(id++);
            }
        }

        if (!matchesFuzzyOrRegex(mCurrentGroup, groupPart, useRegex())) {
            selectGroup(nthVisibleGroup(0));
        }
    }

    requestLayoutUpdate();
}

void ImageViewer::updateLayout() {
    int sidebarWidth = visibleSidebarWidth();
    int footerHeight = visibleFooterHeight();
    mImageCanvas->set_fixed_size(m_size - nanogui::Vector2i{sidebarWidth - 1, footerHeight - 1});
    mSidebar->set_fixed_height(m_size.y() - footerHeight);

    mCropXminTextBox->set_value(std::to_string(mCurrCrop ? mCurrCrop->min.x() : 0));
    mCropYminTextBox->set_value(std::to_string(mCurrCrop ? mCurrCrop->min.y() : 0));
    mCropXmaxTextBox->set_value(std::to_string(mCurrCrop ? mCurrCrop->max.x() : 0));
    mCropYmaxTextBox->set_value(std::to_string(mCurrCrop ? mCurrCrop->max.y() : 0));

    mCropWidthTextBox->set_value(std::to_string(mCurrCrop ? mCurrCrop->max.x() - mCurrCrop->min.x() : 0));
    mCropHeightTextBox->set_value(std::to_string(mCurrCrop ? mCurrCrop->max.y() - mCurrCrop->min.y() : 0));

    mVerticalScreenSplit->set_fixed_size(m_size);
    mImageScrollContainer->set_fixed_height(
        m_size.y() - mImageScrollContainer->position().y() - footerHeight
    );

    if (mImageScrollContainer->fixed_height() < 100) {
        // Stop scrolling the image button container and instead scroll the entire sidebar
        mImageScrollContainer->set_fixed_height(0);
    }

    mSidebarLayout->parent()->set_height(mSidebarLayout->preferred_size(m_nvg_context).y());
    perform_layout();

    mSidebarLayout->set_fixed_width(mSidebarLayout->parent()->width());
    mHelpButton->set_position(nanogui::Vector2i{mSidebarLayout->fixed_width() - 38, 5});
    mFilter->set_fixed_width(mSidebarLayout->fixed_width() - 50);
    perform_layout();

    // With a changed layout the relative position of the mouse
    // within children changes and therefore should get updated.
    // nanogui does not handle this for us.
    double x, y;
    glfwGetCursorPos(m_glfw_window, &x, &y);
    cursor_pos_callback_event(x, y);

    int height = min(100, mCropListContainer->preferred_size(m_nvg_context).y());
    mCropListContainer->set_fixed_height(height);
    perform_layout();
}

void ImageViewer::updateTitle() {
    string caption = "tev";
    if (mCurrentImage) {
        auto channels = mCurrentImage->channelsInGroup(mCurrentGroup);
        // Remove duplicates
        channels.erase(unique(begin(channels), end(channels)), end(channels));

        auto channelTails = channels;
        transform(begin(channelTails), end(channelTails), begin(channelTails), Channel::tail);

        caption = fmt::format(
            "{} – {} – {}%",
            mCurrentImage->shortName(),
            mCurrentGroup,
            (int)std::round(mImageCanvas->scale() * 100)
        );

        auto rel = mouse_pos() - mImageCanvas->position();
        vector<float> values = mImageCanvas->getValuesAtNanoPos({rel.x(), rel.y()}, channels);
        nanogui::Vector2i imageCoords = mImageCanvas->getImageCoords(mCurrentImage.get(), {rel.x(), rel.y()});
        TEV_ASSERT(values.size() >= channelTails.size(), "Should obtain a value for every existing channel.");

        string valuesString;
        for (size_t i = 0; i < channelTails.size(); ++i) {
            valuesString += fmt::format("{:.2f},", values[i]);
        }
        valuesString.pop_back();
        valuesString += " / 0x";
        for (size_t i = 0; i < channelTails.size(); ++i) {
            float tonemappedValue = channelTails[i] == "A" ? values[i] : toSRGB(values[i]);
            unsigned char discretizedValue = (char)(tonemappedValue * 255 + 0.5f);
            valuesString += fmt::format("{:02X}", discretizedValue);
        }

        caption += fmt::format(
            " – @{},{} / {}x{}: {}",
            imageCoords.x(),
            imageCoords.y(),
            mCurrentImage->size().x(),
            mCurrentImage->size().y(),
            valuesString
        );
    }

    set_caption(caption);
}

string ImageViewer::groupName(size_t index) {
    if (!mCurrentImage) {
        return "";
    }

    return mCurrentImage->channelGroups().at(index).name;
}

int ImageViewer::groupId(const string& groupName) const {
    if (!mCurrentImage) {
        return 0;
    }

    const auto& groups = mCurrentImage->channelGroups();
    size_t pos = 0;
    for (; pos < groups.size(); ++pos) {
        if (groups[pos].name == groupName) {
            break;
        }
    }

    return pos >= groups.size() ? -1 : (int)pos;
}

int ImageViewer::imageId(const shared_ptr<Image>& image) const {
    auto pos = static_cast<size_t>(distance(begin(mImages), find(begin(mImages), end(mImages), image)));
    return pos >= mImages.size() ? -1 : (int)pos;
}

int ImageViewer::imageId(const string& imageName) const {
    auto pos = static_cast<size_t>(distance(
        begin(mImages),
        find_if(
            begin(mImages),
            end(mImages),
            [&](const shared_ptr<Image>& image) { return image->name() == imageName; }
        )
    ));
    return pos >= mImages.size() ? -1 : (int)pos;
}

string ImageViewer::nextGroup(const string& group, EDirection direction) {
    if (mGroupButtonContainer->child_count() == 0) {
        return mCurrentGroup;
    }

    int dir = direction == Forward ? 1 : -1;

    // If the group does not exist, start at index 0.
    int startId = max(0, groupId(group));

    int id = startId;
    do {
        id = (id + mGroupButtonContainer->child_count() + dir) % mGroupButtonContainer->child_count();
    } while (!mGroupButtonContainer->child_at(id)->visible() && id != startId);

    return groupName(id);
}

string ImageViewer::nthVisibleGroup(size_t n) {
    string lastVisible = mCurrentGroup;
    for (int i = 0; i < mGroupButtonContainer->child_count(); ++i) {
        if (mGroupButtonContainer->child_at(i)->visible()) {
            lastVisible = groupName(i);
            if (n == 0) {
                break;
            }
            --n;
        }
    }
    return lastVisible;
}

shared_ptr<Image> ImageViewer::nextImage(const shared_ptr<Image>& image, EDirection direction) {
    if (mImages.empty()) {
        return nullptr;
    }

    int dir = direction == Forward ? 1 : -1;

    // If the image does not exist, start at image 0.
    int startId = max(0, imageId(image));

    int id = startId;
    do {
        id = (id + mImageButtonContainer->child_count() + dir) % mImageButtonContainer->child_count();
    } while (!mImageButtonContainer->child_at(id)->visible() && id != startId);

    return mImages[id];
}

shared_ptr<Image> ImageViewer::nthVisibleImage(size_t n) {
    shared_ptr<Image> lastVisible = nullptr;
    for (size_t i = 0; i < mImages.size(); ++i) {
        if (mImageButtonContainer->children()[i]->visible()) {
            lastVisible = mImages[i];
            if (n == 0) {
                break;
            }
            --n;
        }
    }
    return lastVisible;
}

shared_ptr<Image> ImageViewer::imageByName(const string& imageName) {
    int id = imageId(imageName);
    if (id != -1) {
        return mImages[id];
    } else {
        return nullptr;
    }
}

template <typename T> std::vector<T> ImageViewer::resizeImageArray(const std::vector<T>& arr, const int inputWidth, const int inputHeight) {
    float resizeX = std::stof(mCopyResizeXTextBox->value());
    float resizeY = std::stof(mCopyResizeYTextBox->value());

    // Ensure valid ratios
    if (resizeX <= 0 || resizeY <= 0) {
        throw std::runtime_error("Resize ratio must be greater than zero.");
    } else if (resizeX == 1 && resizeY == 1) {
        return arr;
    }

    const int outWidth = static_cast<int>(std::round(inputWidth * resizeX));
    const int outHeight = static_cast<int>(std::round(inputHeight * resizeY));

    std::vector<T> out(outWidth * outHeight * 4 /*channel*/);

    if (mClipResizeMode == EClipResizeMode::Nearest) {
        tlog::info() << "Using nearest neighbor resize";
        for (int y = 0; y < outHeight; ++y) {
            for (int x = 0; x < outWidth; ++x) {
                const int inX = static_cast<int>(x / resizeX);
                const int inY = static_cast<int>(y / resizeY);

                const int inIndex = (inY * inputWidth + inX) * 4;
                const int outIndex = (y * outWidth + x) * 4;

                out[outIndex + 0] = arr[inIndex + 0];
                out[outIndex + 1] = arr[inIndex + 1];
                out[outIndex + 2] = arr[inIndex + 2];
                out[outIndex + 3] = arr[inIndex + 3];
            }
        }
    } else if (mClipResizeMode == EClipResizeMode::Bilinear) {
        tlog::info() << "Using bilinear resize";
        for (int y = 0; y < outHeight; ++y) {
            for (int x = 0; x < outWidth; ++x) {
                const float inX = static_cast<float>(x) / resizeX;
                const float inY = static_cast<float>(y) / resizeY;

                const int inX0 = static_cast<int>(std::floor(inX));
                const int inY0 = static_cast<int>(std::floor(inY));

                const int inX1 = std::min(inX0 + 1, inputWidth - 1);
                const int inY1 = std::min(inY0 + 1, inputHeight - 1);

                const float xRatio = inX - inX0;
                const float yRatio = inY - inY0;

                if (xRatio < 0 || xRatio > 1 || yRatio < 0 || yRatio > 1) {
                    tlog::warning() << "Invalid ratio: " << xRatio << ", " << yRatio;
                }

                const int inIndex00 = (inY0 * inputWidth + inX0) * 4;
                const int inIndex01 = (inY0 * inputWidth + inX1) * 4;
                const int inIndex10 = (inY1 * inputWidth + inX0) * 4;
                const int inIndex11 = (inY1 * inputWidth + inX1) * 4;

                const int outIndex = (y * outWidth + x) * 4;

                for (int c = 0; c < 4; ++c) {
                    float val00 = (static_cast<float>(arr[inIndex00 + c]));
                    float val01 = (static_cast<float>(arr[inIndex01 + c]));
                    float val10 = (static_cast<float>(arr[inIndex10 + c]));
                    float val11 = (static_cast<float>(arr[inIndex11 + c]));

                    // Interpolating horizontally first
                    float val0 = val00 * (1 - xRatio) + val01 * xRatio;
                    float val1 = val10 * (1 - xRatio) + val11 * xRatio;

                    // Then interpolating vertically
                    float val = val0 * (1 - yRatio) + val1 * yRatio;

                    // Clamp the value between 0 and 255 before casting
                    val = std::max(0.0f, std::min(1.0f, val));

                    out[outIndex + c] = static_cast<T>((val));
                }
            }
        }
    }

    return out;
}

void ImageViewer::focusPixel(const nanogui::Vector2i& pixelPos) {
    if (!mCurrentImage) return;

    Vector2i imageSize = mCurrentImage->size();
    Vector2i center = imageSize / 2;
    Vector2f offset = center - pixelPos;
    offset -= Vector2f(0.5f);

    // Scale the transform to maintain the current zoom level
    float currentScale = mImageCanvas->scale();

    Matrix3f newTransform;
    newTransform = Matrix3f::scale(Vector2f{currentScale});
    newTransform = Matrix3f::translate(offset / pixel_ratio() * currentScale) * newTransform;

    mImageCanvas->setTransform(newTransform);
}
} // namespace tev
