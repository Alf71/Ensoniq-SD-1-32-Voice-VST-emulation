/*
  ==============================================================================

    Ensoniq SD-1 MAME VST Emulation
    Open Source GPLv2/v3
    https://www.sojusrecords.com

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

EnsoniqSD1AudioProcessorEditor::EnsoniqSD1AudioProcessorEditor (EnsoniqSD1AudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{

#ifdef _WIN32
    // Attach OpenGL context to offload high-resolution image resampling to the GPU.
    // This dramatically reduces CPU usage in Windows DAWs.
    openGLContext.attachTo(*this);
#endif

    // Start the UI polling timer (30 Hz is enough for smooth UI and MAME frame fetching)
    startTimerHz(30);
    
    this->setWantsKeyboardFocus(false);
    this->setMouseClickGrabsKeyboardFocus(false);
    
    // --- WINDOW SETTINGS & RESIZER ---
        lastW = audioProcessor.mameInternalWidth.load();
        lastH = audioProcessor.mameInternalHeight.load();
        float aspect = (float)lastW / (float)lastH;

        int startW = audioProcessor.savedWindowWidth >= 900 ? audioProcessor.savedWindowWidth : 1200;
        int startH = juce::roundToInt(startW / aspect);

        setResizable(true, true);
        getConstrainer()->setFixedAspectRatio(aspect);
        setResizeLimits(900, juce::roundToInt(900.0f / aspect), 2400, juce::roundToInt(2400.0f / aspect));
        setSize(startW, startH);
    
    // --- BUTTON INITIALIZATION ---
    addAndMakeVisible(loadMediaButton);
    loadMediaButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333333));
    loadMediaButton.setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
    
    loadMediaButton.setWantsKeyboardFocus(false);
    loadMediaButton.setMouseClickGrabsKeyboardFocus(false);
        
    loadMediaButton.onClick = [this] { loadMediaButtonClicked(); };
    
    if (audioProcessor.isRomMissing.load(std::memory_order_acquire) ||
        audioProcessor.isBlockedByAnotherInstance.load(std::memory_order_acquire)) {
        loadMediaButton.setVisible(false);
    }
    
    // --- SETTINGS BUTTON & PANEL INITIALIZATION ---
    addAndMakeVisible(settingsButton);
    
    settingsButton.setWantsKeyboardFocus(false);
    settingsButton.setMouseClickGrabsKeyboardFocus(false);
    
    settingsButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333333));
    settingsButton.setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
    settingsButton.onClick = [this] { toggleSettings(); };

    // GroupComponent and its children are hidden by default (addChildComponent instead of addAndMakeVisible)
    addChildComponent(settingsGroup);
        
    settingsGroup.setColour(juce::GroupComponent::textColourId, juce::Colours::orange);
    settingsGroup.setColour(juce::GroupComponent::outlineColourId, juce::Colours::grey);

    addChildComponent(bufferLabel);
    bufferLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

    addChildComponent(bufferCombo);
    bufferCombo.addItemList({ "256", "512", "1024", "2048", "4096", "8192" }, 1);
    bufferAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(audioProcessor.apvts, "buffer_size", bufferCombo);

    addChildComponent(viewLabel);
    viewLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    viewLabel.setJustificationType(juce::Justification::centredRight);

    addChildComponent(viewCombo);
    viewCombo.addItemList({ "Compact (Default)", "Full Keyboard", "Rack Panel", "Tablet View" }, 1);
    viewAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(audioProcessor.apvts, "layout_view", viewCombo);
        
    addChildComponent(aboutLabel);
    aboutLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    aboutLabel.setJustificationType(juce::Justification::centred);

    addChildComponent(webLink);

    addChildComponent(closeSettingsButton);
    
    closeSettingsButton.setWantsKeyboardFocus(false);
    closeSettingsButton.setMouseClickGrabsKeyboardFocus(false);
    
    closeSettingsButton.onClick = [this] { toggleSettings(); };
    
    // Hide UI elements if the plugin is in a disabled state
    if (audioProcessor.isRomMissing.load(std::memory_order_acquire) ||
        audioProcessor.isBlockedByAnotherInstance.load(std::memory_order_acquire)) {
        loadMediaButton.setVisible(false);
        settingsButton.setVisible(false);
    }
}

EnsoniqSD1AudioProcessorEditor::~EnsoniqSD1AudioProcessorEditor()
{
#ifdef _WIN32
    // Detach the context safely before the component is destroyed to prevent VST3 crashes on close
    openGLContext.detach();
#endif
    stopTimer();
}

void EnsoniqSD1AudioProcessorEditor::timerCallback()
{
        // SStandard offline protection
        if (audioProcessor.isNonRealtime()) {
            audioProcessor.getFrameFlag().store(false, std::memory_order_relaxed);
            return;
        }

        // Protection against drawing on the dead window
        if (!isShowing() || getWidth() <= 0 || getHeight() <= 0 || getTopLevelComponent()->getPeer() == nullptr) {
            audioProcessor.getFrameFlag().store(false, std::memory_order_relaxed);
            return;
        }
    
    // Dynamic Window Resize Monitor
    int currentMameW = audioProcessor.mameInternalWidth.load(std::memory_order_acquire);
    int currentMameH = audioProcessor.mameInternalHeight.load(std::memory_order_acquire);
    
    if (currentMameW != lastW || currentMameH != lastH) {
        lastW = currentMameW;
        lastH = currentMameH;
        
        // Always resize safely
        updateWindowSize();
    }

    // Frame Update
    if (audioProcessor.getFrameFlag().exchange(false, std::memory_order_acquire)) {
        repaint();
    }
}

void EnsoniqSD1AudioProcessorEditor::paint (juce::Graphics& g)
{
    // --- INSTANCE LIMIT WARNING MESSAGE ---
    if (audioProcessor.isBlockedByAnotherInstance.load(std::memory_order_acquire)) {
        g.fillAll(juce::Colour(0xff222222));
        g.setColour(juce::Colours::red);
        g.setFont(24.0f);
        
        juce::String errorMsg = "Ensoniq(R) SD-1/32 MAME(R) Emulation\n\n";
        errorMsg += "Instance Limit Reached!\n\n";
        errorMsg += "Due to MAME engine limitations, only ONE instance\n";
        errorMsg += "of this plugin can run at a time in the host.\n\n";
        errorMsg += "This instance has been disabled to prevent crashes.";
        
        g.drawFittedText(errorMsg, getLocalBounds().reduced(20), juce::Justification::centred, 10);
        return;
    }
    
    // --- SAMPLE RATE MISMATCH WARNING MESSAGE ---
    if (audioProcessor.sampleRateMismatch.load(std::memory_order_acquire)) {
        g.fillAll(juce::Colour(0xff222222));
        g.setColour(juce::Colours::orange);
        g.setFont(24.0f);
        
        juce::String errorMsg = "Ensoniq(R) SD-1/32 MAME(R) Emulation\n\n";
        errorMsg += "Sample Rate Changed!\n\n";
        errorMsg += "The host sample rate has changed since the plugin was loaded.\n";
        errorMsg += "To prevent audio glitches, please reload the plugin or restart your DAW.";
        
        g.drawFittedText(errorMsg, getLocalBounds().reduced(20), juce::Justification::centred, 10);
        return;
    }
    
    // --- MISSING ROM WARNING MESSAGE ---
    if (audioProcessor.isRomMissing.load(std::memory_order_acquire)) {
        g.fillAll(juce::Colour(0xff222222));
        g.setColour(juce::Colours::red);
        g.setFont(24.0f);
        
        juce::String errorMsg = "Ensoniq(R) SD-1/32 MAME(R) Emulation\n\n";
        errorMsg += "Error! Roms not found!\n\n";
        errorMsg += "Please copy the 'sd132.zip' file into the following folder:\n\n";
        errorMsg += juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsoniqSD1").getFullPathName();
        errorMsg += "\n\nThen restart the plugin!";
        
        g.drawFittedText(errorMsg, getLocalBounds().reduced(20), juce::Justification::centred, 10);
        return;
    }

    // --- MAME RENDER ROUTINE ---
    int readIndex = audioProcessor.readyBufferIndex.load(std::memory_order_acquire);
    int internalW = audioProcessor.mameInternalWidth.load();
    int internalH = audioProcessor.mameInternalHeight.load();
    
    // Enable High Quality Resampling to ensure vector-like sharpness when MAME's
    // internal 2048px canvas is scaled down to the VST window size.
    g.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);
        
    // Using JUCE's high-performance integer-based drawImage to stretch
    // the internal MAME canvas accurately to the VST window bounds.
    g.drawImage(audioProcessor.screenBuffers[readIndex],
                0, 0, getWidth(), getHeight(),
                0, 0, internalW, internalH,
                false);
               
    // --- SETTINGS OVERLAY ---
    if (isSettingsVisible) {
            // 1. Darken the background slightly to bring focus to the settings panel
            g.fillAll(juce::Colour(0x4d000000));
            
            // 2. resized() tells the dimensions
            auto panelRect = settingsGroup.getBounds().toFloat();
            
            panelRect.removeFromTop(8.0f);
            
            // 3. Draw the solid dark background for the settings box
            g.setColour(juce::Colour(0xcc000000));
            g.fillRoundedRectangle(panelRect, 5.0f);
        }
        
}

void EnsoniqSD1AudioProcessorEditor::resized()
{
   
    if (getWidth() <= 0 || getHeight() <= 0) return;
    int baseW = audioProcessor.mameInternalWidth.load();
    int baseH = audioProcessor.mameInternalHeight.load();
    
    float targetAspect = (float)baseW / (float)baseH;
    float currentAspect = (float)getWidth() / (float)getHeight();

    // SAFETY NET: Only save the new window dimensions to memory if the DAW (or user)
    // hasn't corrupted the aspect ratio with an invalid frame constraint.
    if (std::abs(currentAspect - targetAspect) < 0.05f) {
        audioProcessor.savedWindowWidth = getWidth();
        audioProcessor.savedWindowHeight = getHeight();
    }

    float scaleX = getWidth() / (float)baseW;
    float scaleY = getHeight() / (float)baseH;

    // LOCK-FREE READ: Safely get the current view index from the processor
    int viewIdx = audioProcessor.requestedViewIndex.load(std::memory_order_acquire);

    // --- DYNAMIC BUTTON POSITIONING (Mapped to 2048px internal MAME space) ---
    int btnX = 55;
    int btnY = 403; // Default position for Compact layout (2048x921)

    if (viewIdx == 1) {
        btnX = 82; btnY = 171;  // Full Keyboard layout (2048x671)
    }
    else if (viewIdx == 2) {
        btnX = 85; btnY = 265;  // Rack Panel layout (2048x379)
    }
    else if (viewIdx == 3) {
        btnX = 95; btnY = 1250; // Tablet layout (2048x1476)
    }

    // Apply scaled coordinates to buttons
    int btnWidth = 192;
    int btnHeight = 30;
    
    loadMediaButton.setBounds(juce::roundToInt(btnX * scaleX), juce::roundToInt(btnY * scaleY),
                              juce::roundToInt(btnWidth * scaleX), juce::roundToInt(btnHeight * scaleY));
                              
    settingsButton.setBounds(juce::roundToInt(btnX * scaleX), juce::roundToInt((btnY + 40) * scaleY),
                             juce::roundToInt(btnWidth * scaleX), juce::roundToInt(btnHeight * scaleY));

    // --- FLEXIBLE SETTINGS PANEL ---
    auto bounds = getLocalBounds();
    
    auto panelRect = bounds.withSizeKeepingCentre(juce::jmin(550, getWidth() - 20), juce::jmin(320, getHeight() - 20));
    settingsGroup.setBounds(panelRect);

    // Initial Y offset to clear the GroupComponent's top border text
    int y = panelRect.getY() + 35;
    int startX = panelRect.getCentreX() - 170;
    int rowHeight = 25;
    
    // Reduce spacing dynamically if the window is extremely flat (e.g. Rack Panel view)
    int spacing = (panelRect.getHeight() > 220) ? 15 : 5;

    bufferLabel.setBounds(startX, y, 180, rowHeight);
    bufferCombo.setBounds(startX + 190, y, 150, rowHeight);
    y += rowHeight + spacing;

    viewLabel.setBounds(startX, y, 180, rowHeight);
    viewCombo.setBounds(startX + 190, y, 150, rowHeight);
    y += rowHeight + spacing;

    // SMART TEXT: Hide the 'About' text block entirely if there's no physical space for it
    if (panelRect.getHeight() > 220) {
        if (isSettingsVisible) aboutLabel.setVisible(true);
        aboutLabel.setBounds(panelRect.getX() + 10, y, panelRect.getWidth() - 20, 85);
        y += 85 + spacing;
    } else {
        aboutLabel.setVisible(false);
    }

    webLink.setBounds(panelRect.getX() + 10, y, panelRect.getWidth() - 20, rowHeight);
    y += rowHeight + spacing;

    closeSettingsButton.setBounds(panelRect.getCentreX() - 50, y, 100, rowHeight);
    
}

// ==============================================================================
// MOUSE EVENT INJECTION (Scaled to MAME coordinates)
// ==============================================================================
void EnsoniqSD1AudioProcessorEditor::mouseDown(const juce::MouseEvent& e) {
    if (getWidth() <= 0 || getHeight() <= 0) return;
    int w = audioProcessor.mameInternalWidth.load();
    int h = audioProcessor.mameInternalHeight.load();
    audioProcessor.injectMouseDown(juce::roundToInt((e.x / (float)getWidth()) * w), juce::roundToInt((e.y / (float)getHeight()) * h));
}

void EnsoniqSD1AudioProcessorEditor::mouseUp(const juce::MouseEvent& e) {
    if (getWidth() <= 0 || getHeight() <= 0) return;
    int w = audioProcessor.mameInternalWidth.load();
    int h = audioProcessor.mameInternalHeight.load();
    audioProcessor.injectMouseUp(juce::roundToInt((e.x / (float)getWidth()) * w), juce::roundToInt((e.y / (float)getHeight()) * h));
}

void EnsoniqSD1AudioProcessorEditor::mouseDrag(const juce::MouseEvent& e) {
    if (getWidth() <= 0 || getHeight() <= 0) return;
    int w = audioProcessor.mameInternalWidth.load();
    int h = audioProcessor.mameInternalHeight.load();
    audioProcessor.injectMouseMove(juce::roundToInt((e.x / (float)getWidth()) * w), juce::roundToInt((e.y / (float)getHeight()) * h));
}

void EnsoniqSD1AudioProcessorEditor::mouseMove(const juce::MouseEvent& e) {
    if (getWidth() <= 0 || getHeight() <= 0) return;
    int w = audioProcessor.mameInternalWidth.load();
    int h = audioProcessor.mameInternalHeight.load();
    audioProcessor.injectMouseMove(juce::roundToInt((e.x / (float)getWidth()) * w), juce::roundToInt((e.y / (float)getHeight()) * h));
}

void EnsoniqSD1AudioProcessorEditor::loadMediaButtonClicked()
{
    // Launch asynchronous file chooser
    fileChooser = std::make_unique<juce::FileChooser>("Select Floppy Image or cartridge",
        juce::File::getSpecialLocation(juce::File::userHomeDirectory),
        "*.img;*.hfe;*.dsk;*.eda");

    auto folderChooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync(folderChooserFlags, [this](const juce::FileChooser& fc) {
        auto file = fc.getResult();
        if (file.existsAsFile()) {
            std::lock_guard<std::mutex> lock(audioProcessor.mediaMutex);

#ifdef _WIN32
            // Force strict UTF-8 conversion for MAME to handle Windows user paths with accents
            audioProcessor.pendingFloppyPath = file.getFullPathName().toUTF8().getAddress();
#else
            audioProcessor.pendingFloppyPath = file.getFullPathName().toStdString();
#endif

            audioProcessor.requestFloppyLoad.store(true, std::memory_order_release);
        }
        });
}

void EnsoniqSD1AudioProcessorEditor::toggleSettings()
{
    isSettingsVisible = !isSettingsVisible;
    
    settingsGroup.setVisible(isSettingsVisible);
    bufferLabel.setVisible(isSettingsVisible);
    bufferCombo.setVisible(isSettingsVisible);
    viewLabel.setVisible(isSettingsVisible);
    viewCombo.setVisible(isSettingsVisible);
    aboutLabel.setVisible(isSettingsVisible);
    webLink.setVisible(isSettingsVisible);
    closeSettingsButton.setVisible(isSettingsVisible);
    
    repaint();
}

void EnsoniqSD1AudioProcessorEditor::updateWindowSize()
{
    int w = audioProcessor.mameInternalWidth.load();
    int h = audioProcessor.mameInternalHeight.load();
    float aspect = (float)w / (float)h;
    
    int currentW = getWidth();
    if (currentW < 900) {
        currentW = 1200;
    }
    int winH = juce::roundToInt(currentW / aspect);
    
    if (getWidth() == currentW && getHeight() == winH) {
            return;
        }
    
    // Apply constraint logic and resize
    getConstrainer()->setFixedAspectRatio(aspect);
    getConstrainer()->setSizeLimits(900, juce::roundToInt(900.0f / aspect), 2400, juce::roundToInt(2400.0f / aspect));

    juce::MessageManager::callAsync([this, currentW, winH]() {
        setSize(currentW, winH);
        });

}

