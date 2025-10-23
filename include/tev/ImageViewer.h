/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <tev/HelpWindow.h>
#include <tev/Image.h>
#include <tev/ImageButton.h>
#include <tev/ImageCanvas.h>
#include <tev/ImageInfoWindow.h>
#include <tev/Ipc.h>
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
    ImageViewer(
        const nanogui::Vector2i& size,
        const std::shared_ptr<BackgroundImagesLoader>& imagesLoader,
        const std::shared_ptr<Ipc>& ipc,
        bool maximize,
        bool showUi,
        bool floatBuffer
    );

    bool resize_event(const nanogui::Vector2i& size) override;

    bool mouse_button_event(const nanogui::Vector2i& p, int button, bool down, int modifiers) override;
    bool mouse_motion_event_f(const nanogui::Vector2f& p, const nanogui::Vector2f& rel, int button, int modifiers) override;

    bool drop_event(const std::vector<std::string>& filenames) override;

    bool keyboard_event(int key, int scancode, int action, int modifiers) override;

    void focusWindow();

    void draw_contents() override;

    void insertImage(std::shared_ptr<Image> image, size_t index, bool shallSelect = false);
    void moveImageInList(size_t oldIndex, size_t newIndex);

    bool hasImageWithName(std::string_view imageName) { return !!imageByName(imageName); }

    void addImage(std::shared_ptr<Image> image, bool shallSelect = false) { insertImage(image, mImages.size(), shallSelect); }

    void removeImage(std::shared_ptr<Image> image);
    void removeImage(std::string_view imageName) { removeImage(imageByName(imageName)); }
    void removeAllImages();

    void replaceImage(std::shared_ptr<Image> image, std::shared_ptr<Image> replacement, bool shallSelect);
    void replaceImage(std::string_view imageName, std::shared_ptr<Image> replacement, bool shallSelect) {
        replaceImage(imageByName(imageName), replacement, shallSelect);
    }

    void reloadImage(std::shared_ptr<Image> image, bool shallSelect = false);
    void reloadImage(std::string_view imageName, bool shallSelect = false) { reloadImage(imageByName(imageName), shallSelect); }
    void reloadAllImages();
    void reloadImagesWhoseFileChanged();

    void updateImage(
        std::string_view imageName, bool shallSelect, std::string_view channel, int x, int y, int width, int height, std::span<const float> imageData
    );

    void updateImageVectorGraphics(std::string_view imageName, bool shallSelect, bool append, std::span<const VgCommand> commands);

    void selectImage(const std::shared_ptr<Image>& image, bool stopPlayback = true);

    void selectGroup(std::string name);

    void selectReference(const std::shared_ptr<Image>& image);

    float exposure() const { return mExposureSlider->value(); }

    void setExposure(float value);

    float offset() const { return mOffsetSlider->value(); }

    void setOffset(float value);

    float gamma() const { return mGammaSlider->value(); }

    void setGamma(float value);

    void normalizeExposureAndOffset();

    void resetImage();

    EInterpolationMode minFilter() const { return mImageCanvas->minFilter(); }
    void setMinFilter(EInterpolationMode value) { mImageCanvas->setMinFilter(value); }

    EInterpolationMode magFilter() const { return mImageCanvas->magFilter(); }
    void setMagFilter(EInterpolationMode value) { mImageCanvas->setMagFilter(value); }

    ETonemap tonemap() const { return mImageCanvas->tonemap(); }

    void setTonemap(ETonemap tonemap);

    EMetric metric() const { return mImageCanvas->metric(); }

    void setMetric(EMetric metric);

    nanogui::Vector2i sizeToFitImage(const std::shared_ptr<Image>& image);
    nanogui::Vector2i sizeToFitAllImages();
    void resizeToFit(nanogui::Vector2i size);

    bool playingBack() const;
    void setPlayingBack(bool value);

    bool setFilter(std::string_view filter);

    void setFps(int value);

    bool useRegex() const;
    void setUseRegex(bool value);

    bool watchFilesForChanges() const;
    void setWatchFilesForChanges(bool value);

    bool autoFitToScreen() const;
    void setAutoFitToScreen(bool value);

    void maximize();
    bool isMaximized();
    void toggleMaximized();

    bool isUiVisible() { return mSidebar->visible(); }
    void setUiVisible(bool shouldBeVisible);

    void toggleHelpWindow();

    void toggleImageInfoWindow();
    void updateImageInfoWindow();

    void openImageDialog();
    void saveImageDialog();

    void requestLayoutUpdate() { mRequiresLayoutUpdate = true; }

    template <typename T> void scheduleToUiThread(const T& fun) {
        mTaskQueue.push(fun);
        redraw();
    }

    BackgroundImagesLoader& imagesLoader() const { return *mImagesLoader; }
    Ipc& ipc() const { return *mIpc; }

    void copyImageCanvasToClipboard() const;
    void copyImageNameToClipboard() const;
    void pasteImagesFromClipboard();

    void showErrorDialog(std::string_view message);
    void focusPixel(const nanogui::Vector2i& pixelPos);

    void setSyncExposure(bool sync);

private:
    void updateFilter();
    void applyHistogramStatistics(const std::shared_ptr<CanvasStatistics>& statistics, std::string_view histogramTooltipBase);
    enum class EHistogramScale {
        Log,
        Linear,
    };
    void setHistogramScale(EHistogramScale scale);

    struct ChannelProcessContext {
        std::vector<std::string> channelNames;
        std::vector<const Channel*> channels;
        std::vector<const Channel*> referenceChannels;
        std::vector<bool> isAlpha;
        nanogui::Vector2i size;
        nanogui::Vector2i refOffset{0, 0};
        int minX = 0, maxX = 0, minY = 0, maxY = 0;
        bool hasReference = false;
    };

    bool buildChannelProcessContext(ChannelProcessContext& ctx) const;
    void forEachChannelPixelValue(const ChannelProcessContext& ctx, const std::function<void(int ci, int x, int y, float val)>& fn) const;
    void updateLayout();
    void updateTitle();
    void updatePixelLocatorHighlightState(bool forceRefresh = false);
    void clearPixelLocatorState(bool resetStatusLabel);
    std::string groupName(size_t index);

    int groupId(std::string_view groupName) const;
    int imageId(const std::shared_ptr<Image>& image) const;
    int imageId(std::string_view imageName) const;

    std::string nextGroup(std::string_view groupName, EDirection direction);
    std::string nthVisibleGroup(size_t n);

    std::shared_ptr<Image> nextImage(const std::shared_ptr<Image>& image, EDirection direction);
    std::shared_ptr<Image> nthVisibleImage(size_t n);
    std::shared_ptr<Image> imageByName(std::string_view imageName);

    bool canDragSidebarFrom(const nanogui::Vector2i& p) {
        return mSidebar->visible() && p.x() - mSidebar->fixed_width() < 10 && p.x() - mSidebar->fixed_width() > -5;
    }

    int visibleSidebarWidth() { return mSidebar->visible() ? mSidebar->fixed_width() : 0; }

    int visibleFooterHeight() { return mFooter->visible() ? mFooter->fixed_height() : 0; }

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
    std::shared_ptr<Ipc> mIpc;

    std::shared_ptr<Image> mCurrentImage;
    std::shared_ptr<Image> mCurrentReference;

    std::vector<std::shared_ptr<Image>> mImages;

    MultiGraph* mHistogram;
    std::set<std::shared_ptr<Image>> mToBump;

    nanogui::TextBox* mFilter;
    nanogui::Button* mRegexButton;

    nanogui::Button* mWatchFilesForChangesButton;
    std::chrono::steady_clock::time_point mLastFileChangesCheckTime = {};

    nanogui::Button* mAutoFitToScreenButton;

    // Buttons which require a current image to be meaningful.
    std::vector<nanogui::Button*> mCurrentImageButtons;
    nanogui::Button* mImageInfoButton;
    ImageInfoWindow* mImageInfoWindow = nullptr;

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

    nanogui::Button* mHistogramLogButton = nullptr;
    nanogui::Button* mHistogramLinearButton = nullptr;
    EHistogramScale mHistogramScale = EHistogramScale::Log;

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
    std::vector<T> resizeImageArray(const std::vector<T>& arr, const int inputWidth, const int inputHeight) const;

    int mDidFitToImage = 0;

    nanogui::Vector2i mMaxSize = {8192, 8192};
    bool mInitialized = false;

    std::unique_ptr<std::thread> mFileDialogThread;

    // Pixel locator
    nanogui::Button* mPixelLocatorShowHideButton = nullptr;
    nanogui::TextBox* mRangeMinTextBox = nullptr;
    nanogui::TextBox* mRangeMaxTextBox = nullptr;
    nanogui::Button* mFindRangeButton = nullptr;
    nanogui::Button* mFindNextRangeButton = nullptr;
    nanogui::Button* mResetPixelLocatorButton = nullptr;
    nanogui::Label* mStatusLabel = nullptr;
    std::vector<std::pair<nanogui::Vector2i, float>> mFoundPixels;
    int mCurrentFoundPixelIdx = -1;
    std::vector<nanogui::Vector2i> mPixelLocatorRangeHighlights;
    std::optional<nanogui::Vector2i> mPixelLocatorPrimaryHighlight;

    // Tonemapping
    std::unordered_map<std::shared_ptr<Image>, float> mImageExposures;
    std::unordered_map<std::shared_ptr<Image>, float> mImageOffsets;
    std::unordered_map<std::shared_ptr<Image>, float> mImageGammas;

    nanogui::CheckBox* mSyncTonemapping = nullptr;

    void setTonemappingValue(ETonemapComponent component, float value);
};

} // namespace tev
