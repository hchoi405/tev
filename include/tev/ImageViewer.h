// This file was developed by Thomas Müller <contact@tom94.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/HelpWindow.h>
#include <tev/Image.h>
#include <tev/ImageButton.h>
#include <tev/ImageCanvas.h>
#include <tev/Lazy.h>
#include <tev/MultiGraph.h>
#include <tev/SharedQueue.h>
#include <tev/VectorGraphics.h>

#include <nanogui/opengl.h>
#include <nanogui/screen.h>
#include <nanogui/slider.h>
#include <nanogui/textbox.h>
#include <nanogui/checkbox.h>

#include <chrono>
#include <memory>
#include <set>
#include <vector>
#include <unordered_map>
#include <functional>

namespace tev {

enum class ETonemapComponent {
    Exposure,
    Offset,
    Gamma
};

class ImageViewer : public nanogui::Screen {
public:
    ImageViewer(const std::shared_ptr<BackgroundImagesLoader>& imagesLoader, bool fullscreen, bool floatBuffer, bool supportsHdr);

    bool mouse_button_event(const nanogui::Vector2i &p, int button, bool down, int modifiers) override;
    bool mouse_motion_event(const nanogui::Vector2i& p, const nanogui::Vector2i& rel, int button, int modifiers) override;

    bool drop_event(const std::vector<std::string>& filenames) override;

    bool keyboard_event(int key, int scancode, int action, int modifiers) override;

    void focusWindow();

    void draw_contents() override;

    void insertImage(std::shared_ptr<Image> image, size_t index, bool shallSelect = false);
    void moveImageInList(size_t oldIndex, size_t newIndex);

    bool hasImageWithName(const std::string& imageName) {
        return !!imageByName(imageName);
    }

    void addImage(std::shared_ptr<Image> image, bool shallSelect = false) {
        insertImage(image, mImages.size(), shallSelect);
    }

    void removeImage(std::shared_ptr<Image> image);
    void removeImage(const std::string& imageName) {
        removeImage(imageByName(imageName));
    }
    void removeAllImages();

    void replaceImage(std::shared_ptr<Image> image, std::shared_ptr<Image> replacement, bool shallSelect);
    void replaceImage(const std::string& imageName, std::shared_ptr<Image> replacement, bool shallSelect) {
        replaceImage(imageByName(imageName), replacement, shallSelect);
    }

    void reloadImage(std::shared_ptr<Image> image, bool shallSelect = false);
    void reloadImage(const std::string& imageName, bool shallSelect = false) {
        reloadImage(imageByName(imageName), shallSelect);
    }
    void reloadAllImages();
    void reloadImagesWhoseFileChanged();

    void updateImage(
        const std::string& imageName,
        bool shallSelect,
        const std::string& channel,
        int x, int y,
        int width, int height,
        const std::vector<float>& imageData
    );

    void updateImageVectorGraphics(const std::string& imageName, bool shallSelect, bool append, const std::vector<VgCommand>& commands);

    void selectImage(const std::shared_ptr<Image>& image, bool stopPlayback = true);

    void selectGroup(std::string name);

    void selectReference(const std::shared_ptr<Image>& image);

    float exposure() const {
        return mExposureSlider->value();
    }

    void setExposure(float value);

    float offset() const {
        return mOffsetSlider->value();
    }

    void setOffset(float value);

    float gamma() const {
        return mGammaSlider->value();
    }

    void setGamma(float value);

    void normalizeExposureAndOffset();

    void resetImage();

    ETonemap tonemap() const {
        return mImageCanvas->tonemap();
    }

    void setTonemap(ETonemap tonemap);

    EMetric metric() const {
        return mImageCanvas->metric();
    }

    void setMetric(EMetric metric);

    nanogui::Vector2i sizeToFitImage(const std::shared_ptr<Image>& image);
    nanogui::Vector2i sizeToFitAllImages();
    void resizeToFit(nanogui::Vector2i size);

    bool playingBack() const;
    void setPlayingBack(bool value);

    bool setFilter(const std::string& filter);

    bool useRegex() const;
    void setUseRegex(bool value);

    bool watchFilesForChanges() const;
    void setWatchFilesForChanges(bool value);

    void maximize();
    bool isMaximized();
    void toggleMaximized();

    bool isUiVisible() {
        return mSidebar->visible();
    }
    void setUiVisible(bool shouldBeVisible);

    void toggleHelpWindow();

    void openImageDialog();
    void saveImageDialog();

    void requestLayoutUpdate() {
        mRequiresLayoutUpdate = true;
    }

    template <typename T>
    void scheduleToUiThread(const T& fun) {
        mTaskQueue.push(fun);
    }

    BackgroundImagesLoader& imagesLoader() const {
        return *mImagesLoader;
    }

    void focusPixel(const nanogui::Vector2i& pixelPos);

    void setSyncExposure(bool sync);

private:
    void updateFilter();
    void updateLayout();
    void updateTitle();
    std::string groupName(size_t index);

    int groupId(const std::string& groupName) const;
    int imageId(const std::shared_ptr<Image>& image) const;
    int imageId(const std::string& imageName) const;

    std::string nextGroup(const std::string& groupName, EDirection direction);
    std::string nthVisibleGroup(size_t n);

    std::shared_ptr<Image> nextImage(const std::shared_ptr<Image>& image, EDirection direction);
    std::shared_ptr<Image> nthVisibleImage(size_t n);
    std::shared_ptr<Image> imageByName(const std::string& imageName);

    bool canDragSidebarFrom(const nanogui::Vector2i& p) {
        return mSidebar->visible() && p.x() - mSidebar->fixed_width() < 10 && p.x() - mSidebar->fixed_width() > -5;
    }

    int visibleSidebarWidth() {
        return mSidebar->visible() ? mSidebar->fixed_width() : 0;
    }

    int visibleFooterHeight() {
        return mFooter->visible() ? mFooter->fixed_height() : 0;
    }

    SharedQueue<std::function<void(void)>> mTaskQueue;

    bool mRequiresFilterUpdate = true;
    bool mRequiresLayoutUpdate = true;

    nanogui::Widget* mVerticalScreenSplit;

    nanogui::Widget* mSidebar;
    nanogui::Button* mHelpButton;
    nanogui::Widget* mSidebarLayout;

    nanogui::Widget* mFooter;
    bool mShouldFooterBeVisible = false;

    nanogui::Label* mExposureLabel;
    nanogui::Slider* mExposureSlider;

    nanogui::Label* mOffsetLabel;
    nanogui::Slider* mOffsetSlider;

    nanogui::Label* mGammaLabel;
    nanogui::Slider* mGammaSlider;

    nanogui::Widget* mTonemapButtonContainer;
    nanogui::Widget* mMetricButtonContainer;

    // Crop
    nanogui::Button* mCropShowHideButton;
    nanogui::TextBox* mCropXminTextBox;
    nanogui::TextBox* mCropYminTextBox;
    nanogui::TextBox* mCropXmaxTextBox;
    nanogui::TextBox* mCropYmaxTextBox;
    nanogui::TextBox* mCropWidthTextBox;
    nanogui::TextBox* mCropHeightTextBox;
    std::string mCropListFilename = "cropList.txt";
    nanogui::TextBox* mCropListPathTextBox = nullptr;
    std::fstream mCropListFile;
    nanogui::VScrollPanel* mCropListContainer;

    // Flags to prevent update loops between the two sets of text boxes
    bool mUpdatingFromMinMax = false;
    bool mUpdatingFromSizeFields = false;

    std::shared_ptr<BackgroundImagesLoader> mImagesLoader;

    std::shared_ptr<Image> mCurrentImage;
    std::shared_ptr<Image> mCurrentReference;

    std::vector<std::shared_ptr<Image>> mImages;

    MultiGraph* mHistogram;
    std::set<std::shared_ptr<Image>> mToBump;

    nanogui::TextBox* mFilter;
    nanogui::Button* mRegexButton;

    nanogui::Button* mWatchFilesForChangesButton;
    std::chrono::steady_clock::time_point mLastFileChangesCheckTime = {};

    // Buttons which require a current image to be meaningful.
    std::vector<nanogui::Button*> mCurrentImageButtons;

    // Buttons which require at least one image to be meaningful
    std::vector<nanogui::Button*> mAnyImageButtons;

    nanogui::Button* mPlayButton;
    nanogui::IntBox<int>* mFpsTextBox;
    std::chrono::steady_clock::time_point mLastPlaybackFrameTime = {};

    nanogui::Widget* mImageButtonContainer;
    nanogui::Widget* mScrollContent;
    nanogui::VScrollPanel* mImageScrollContainer;

    ImageCanvas* mImageCanvas;

    nanogui::Widget* mGroupButtonContainer;
    std::string mCurrentGroup;

    HelpWindow* mHelpWindow = nullptr;

    std::optional<Box2i> mCurrCrop = Box2i{{0, 0}, {0, 0}};
    enum class EMouseDragType {
        None,
        ImageDrag,
        ImageCrop,
        ImageButtonDrag,
        SidebarDrag,
    };

    nanogui::Vector2i mDraggingStartPosition;
    EMouseDragType mDragType = EMouseDragType::None;
    size_t mDraggedImageButtonId;

    size_t mClipboardIndex = 0;

    bool mSupportsHdr = false;
    nanogui::Button* mClipToLdrButton;

    // Clipboard size modifier
    nanogui::Button* mCopyResizeShowHideButton;
    nanogui::TextBox* mCopyResizeXTextBox;
    nanogui::TextBox* mCopyResizeYTextBox;
    enum class EClipResizeMode {
        Nearest,
        Bilinear,
    };
    EClipResizeMode mClipResizeMode = EClipResizeMode::Nearest;
    template <typename T>
    std::vector<T> resizeImageArray(const std::vector<T>& arr, const int inputWidth, const int inputHeight);

    int mDidFitToImage = 0;

    nanogui::Vector2i mMaxSize = {8192, 8192};

    // Pixel locator
    nanogui::Button* mPixelLocatorShowHideButton = nullptr;
    nanogui::TextBox* mRangeMinTextBox = nullptr;
    nanogui::TextBox* mRangeMaxTextBox = nullptr;
    nanogui::Button* mFindRangeButton = nullptr;
    nanogui::Button* mFindNextRangeButton = nullptr;
    nanogui::Label* mStatusLabel = nullptr;
    std::vector<std::pair<nanogui::Vector2i, float>> mFoundPixels;
    int mCurrentFoundPixelIdx = -1;

    // Tonemapping
    std::unordered_map<std::shared_ptr<Image>, float> mImageExposures;
    std::unordered_map<std::shared_ptr<Image>, float> mImageOffsets;
    std::unordered_map<std::shared_ptr<Image>, float> mImageGammas;

    nanogui::CheckBox* mSyncTonemapping = nullptr;

    void setTonemappingValue(ETonemapComponent component, float value);
};

}
