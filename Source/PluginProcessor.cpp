/*
  ==============================================================================

    Ensoniq SD-1 MAME VST Emulation
    Open Source GPLv2/v3
    https://www.sojusrecords.com

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <iostream>
#include <chrono>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <stdlib.h>
#include <new>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
#endif

// ==============================================================================
// MANDATORY MAME MACROS - MUST BE DEFINED BEFORE ANY MAME INCLUDES!
// ==============================================================================
#define PTR64 1
#define LSB_FIRST 1
#define NDEBUG 1
#define __STDC_LIMIT_MACROS 1
#define __STDC_FORMAT_MACROS 1
#define __STDC_CONSTANT_MACROS 1

// --- MAME Core Includes ---
#include "emu.h"
#include "frontend/mame/ui/ui.h"
#include "osd/modules/lib/osdobj_common.h"
#include "uiinput.h"
#include "inputdev.h"
#include "emuopts.h"
#include "render.h"
#include "osdepend.h"
#include "frontend/mame/mame.h"
#include "frontend/mame/clifront.h"
#include "frontend/mame/mameopts.h"
#include "drivenum.h"
#include "diimage.h"

#include <fstream>

// Global lock to prevent multiple MAME instances from running in the same process
static std::atomic<bool> globalMameLock { false };

// --- AU Highlander Instance Manager ---
static std::mutex instanceMutex;
static EnsoniqSD1AudioProcessor* activeMameProcessor = nullptr;

// MAME Versioning stubs required by the linker
extern const char bare_build_version[] = "0.286";
extern const char bare_vcs_revision[] = "";
extern const char build_version[] = "0.286";

const char * emulator_info::get_appname() { return "mame"; }
const char * emulator_info::get_appname_lower() { return "mame"; }
const char * emulator_info::get_configname() { return "mame"; }
const char * emulator_info::get_copyright() { return "Copyright"; }
const char * emulator_info::get_copyright_info() { return "Copyright"; }

// ==============================================================================
// AUDIO STREAMING & THROTTLING
// ==============================================================================
void EnsoniqSD1AudioProcessor::pushAudioFromMame(const int16_t* pcmBuffer, int numSamples) {
    if (!isMameRunningFlag()) return;

    // AUDIO-DRIVEN THROTTLING:
    // MAME runs on a separate thread and can generate audio much faster than real-time.
    // We throttle the MAME thread here by putting it to sleep if our ring buffer has 
    // more unread samples than the defined threshold. This prevents buffer overflows.
    
        while (isMameRunningFlag()) {
            
                        // --- DEADLOCK BREAKER ---

                        if (requestMameSave.load(std::memory_order_acquire) || requestMameLoad.load(std::memory_order_acquire)) {
                            break;
                        }
                        
                        // --- ANCHOR DEADLOCK BREAKER ---

                        if (needAnchorSync.load(std::memory_order_acquire)) {
                            break;
                        }
            
            uint64_t writePos = getTotalWritten();
            uint64_t readPos = getTotalRead();
            int64_t available = writePos - readPos;

            int maxAllowedBuffer = mameBufferThreshold.load(std::memory_order_relaxed);
            
            if (isNonRealtime()) {
                maxAllowedBuffer = maxOfflineBuffer.load(std::memory_order_relaxed);
            }
                               
            if (available < maxAllowedBuffer) {
                break; // There is enough room, write
            }

            // Buffer is full. Sleep the MAME thread until processBlock consumes some data.
            
// --- OPTIMIZATION START ---
#ifdef _WIN32
        // Default Windows scheduler resolution is ~15.6ms. Waiting for 5ms might 
        // put the thread to sleep for 16ms, causing severe buffer underruns in MAME.
        // We force a much tighter wake-up interval on Windows.
            mameThrottleEvent.wait(1);
#else
        // Original macOS behavior preserved
            mameThrottleEvent.wait(5);
#endif
            // --- OPTIMIZATION END ---
        }
        

    uint64_t currentWritePos = totalWritten.load(std::memory_order_relaxed);
    
    if (needAnchorSync.load(std::memory_order_acquire) && mameMachine != nullptr) {
            anchorMameTime.store(mameMachine->time().as_double(), std::memory_order_relaxed);
            anchorDawSample.store(currentWritePos, std::memory_order_relaxed);
            needAnchorSync.store(false, std::memory_order_release);
        }

    // MAME outputs interleaved audio. STRIDE = 5 (Main L, Main R, Aux L, Aux R, Floppy)
    for (int i = 0; i < numSamples; ++i) {

        // --- OPTIMIZATION ---
        // Replace slow modulo (%) with ultra-fast bitwise AND (&).
        // This relies on RING_BUFFER_SIZE being a strict power of 2!
        int index = currentWritePos & (RING_BUFFER_SIZE - 1);

        ringBufferL[index] = pcmBuffer[i * 5 + 0] / 32768.0f;
        ringBufferR[index] = pcmBuffer[i * 5 + 1] / 32768.0f;

        ringBufferAuxL[index] = pcmBuffer[i * 5 + 2] / 32768.0f;
        ringBufferAuxR[index] = pcmBuffer[i * 5 + 3] / 32768.0f;

        currentWritePos++;
    }
    
    totalWritten.store(currentWritePos, std::memory_order_release);
}
    
// ==============================================================================
// VST MIDI PORT IMPLEMENTATION (TIMESTAMPED)
// ==============================================================================
class VstMidiInputPort : public osd::midi_input_port {
private:
    EnsoniqSD1AudioProcessor* processor;
public:
    VstMidiInputPort(EnsoniqSD1AudioProcessor* p) : processor(p) {}
    virtual ~VstMidiInputPort() {}
    
    virtual bool poll() override {
        return processor->pollMidiData();
    }
    
    virtual int read(uint8_t *pOut) override {
        if (processor->pollMidiData()) {
            *pOut = static_cast<uint8_t>(processor->readMidiByte());
            return 1;
        }
        return 0;
    }
};

// ==============================================================================
// TIMESTAMPED MIDI QUEUE LOGIC (PURE ANCHOR)
// ==============================================================================
void EnsoniqSD1AudioProcessor::pushMidiByte(uint8_t data, double targetMameTime) {
    int currentWrite = midiWritePos.load(std::memory_order_relaxed);

    // --- OPTIMIZATION ---
    // Fast wrap-around for MIDI ring buffer
    int nextWrite = (currentWrite + 1) & (MIDI_BUFFER_SIZE - 1);

    if (nextWrite != midiReadPos.load(std::memory_order_acquire)) {
        midiBuffer[currentWrite].data = data;
        midiBuffer[currentWrite].targetMameTime = targetMameTime;
        midiWritePos.store(nextWrite, std::memory_order_release);
    }
}

bool EnsoniqSD1AudioProcessor::pollMidiData() {
    if (mameMachine == nullptr) return false;
    
    int currentRead = midiReadPos.load(std::memory_order_relaxed);
    if (currentRead == midiWritePos.load(std::memory_order_acquire)) return false;
    
    // The MAME processor accurately waits for the calculated microsecond!
    return mameMachine->time().as_double() >= midiBuffer[currentRead].targetMameTime;
}

int EnsoniqSD1AudioProcessor::readMidiByte() {
    if (mameMachine == nullptr) return 0;

    int currentRead = midiReadPos.load(std::memory_order_relaxed);
    if (currentRead == midiWritePos.load(std::memory_order_acquire)) return 0;

    if (mameMachine->time().as_double() >= midiBuffer[currentRead].targetMameTime) {
        uint8_t data = midiBuffer[currentRead].data;

        // --- OPTIMIZATION ---
        // Fast wrap-around using bitwise AND
        midiReadPos.store((currentRead + 1) & (MIDI_BUFFER_SIZE - 1), std::memory_order_release);

        return data;
    }
    return 0;
}

// ==============================================================================
// HEADLESS OSD (Operating System Dependent) INTERFACE
// This class acts as the bridge between MAME's core and the JUCE environment.
// ==============================================================================
class VstOsdInterface : public osd_common_t
{
private:
    EnsoniqSD1AudioProcessor* processor;
    running_machine* mame_machine = nullptr;
    uint32_t lastMouseButtons = 0;
    
    render_target* main_target = nullptr;

    int saveFrameDelay = 0;
    int loadFrameDelay = 0;
    int frameSkipCounter = 0;
    
public:
    
    virtual void process_events() override {}
    virtual bool has_focus() const override { return true; }
    
    VstOsdInterface(EnsoniqSD1AudioProcessor* p, osd_options &options)
    : osd_common_t(options), processor(p) {}
    
    virtual ~VstOsdInterface() {}
    
    virtual void init(running_machine &machine) override {
        
        // REQUIRED: Initializes MAME's core sound, mouse, and keyboard modules
        osd_common_t::init(machine);
                
        mame_machine = &machine;
        processor->mameMachine = &machine;
        
        int targetViewIdx = processor->requestedViewIndex.load(std::memory_order_acquire);
        int startW = processor->mameInternalWidth.load(std::memory_order_acquire);
        int startH = processor->mameInternalHeight.load(std::memory_order_acquire);

        // Map the atomic index to the MAME view string
        const char* targetViewName = "Compact";
        if (targetViewIdx == 1) targetViewName = "Full";
        else if (targetViewIdx == 2) targetViewName = "Panel";
        else if (targetViewIdx == 3) targetViewName = "Tablet";

        // SINGLE ALLOCATION with exact dimensions to prevent memory leaks on exit
        main_target = machine.render().target_alloc();
        main_target->set_bounds(startW, startH);

        // Find and set the requested layout view
        for (int i = 0; ; i++) {
            const char* vname = main_target->view_name(i);
            if (vname == nullptr) break;

            std::string nameStr(vname);
            if (nameStr.find(targetViewName) != std::string::npos) {
                main_target->set_view(i);
                break;
            }
        }
    };
    
    virtual void osd_exit() override {
        if (mame_machine != nullptr && main_target != nullptr) {
            // Gracefully return the render target to MAME to avoid crashes during cleanup
            mame_machine->render().target_free(main_target);
            main_target = nullptr;
        }
        osd_common_t::osd_exit();
    }
    
    virtual void update(bool skip_redraw) override {
        if (skip_redraw || mame_machine == nullptr) return;
        
        // --- DYNAMIC VIEW SWITCHING ---
        if (processor->requestViewChange.exchange(false, std::memory_order_acquire)) {

            // Read atomic values lock-free
            int targetViewIdx = processor->requestedViewIndex.load(std::memory_order_acquire);
            int newW = processor->mameInternalWidth.load(std::memory_order_acquire);
            int newH = processor->mameInternalHeight.load(std::memory_order_acquire);

            const char* targetViewName = "Compact";
            if (targetViewIdx == 1) targetViewName = "Full";
            else if (targetViewIdx == 2) targetViewName = "Panel";
            else if (targetViewIdx == 3) targetViewName = "Tablet";

            for (int i = 0; ; i++) {
                const char* vname = main_target->view_name(i);
                if (vname == nullptr) break;

                std::string nameStr(vname);
                if (nameStr.find(targetViewName) != std::string::npos) {
                    main_target->set_view(i);
                    break;
                }
            }
            main_target->set_bounds(newW, newH); 
            
            // Clear buffers to black to prevent ghosting from the previous layout
            for(int i=0; i<2; i++) {
                juce::Graphics g2(processor->screenBuffers[i]);
                g2.fillAll(juce::Colours::black);
            }
        }
        
        // --- SYNCHRONIZED MAME STATE SAVING ---
                if (processor->requestMameSave.load(std::memory_order_acquire)) {
                    if (saveFrameDelay == 0) { // Only call it the first time
                        mame_machine->schedule_save("vst_temp");
                        saveFrameDelay = 3;
                    }
                }

                if (saveFrameDelay > 0) {
                    saveFrameDelay--;
                    if (saveFrameDelay == 0) {
                        processor->requestMameSave.store(false, std::memory_order_release); // Clear the flag here!
                        processor->mameStateIsReady.store(true, std::memory_order_release);
                        processor->mameStateEvent.signal();
                    }
                }

                // --- SYNCHRONIZED MAME STATE LOADING ---
                if (processor->requestMameLoad.load(std::memory_order_acquire)) {
                    if (loadFrameDelay == 0) { // Only call it the first time
                        mame_machine->schedule_load("vst_temp");
                        loadFrameDelay = 3;
                    }
                }

                if (loadFrameDelay > 0) {
                    loadFrameDelay--;
                    if (loadFrameDelay == 0) {
                        processor->requestMameLoad.store(false, std::memory_order_release); // Clear the flag here!
                        processor->mameStateIsReady.store(true, std::memory_order_release);
                        processor->mameStateEvent.signal();
                    }
                }
        
        // --- FLOPPY MOUNTING ---
        if (processor->requestFloppyLoad.exchange(false, std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(processor->mediaMutex);
            
            for (device_image_interface &image : image_interface_enumerator(mame_machine->root_device())) {
                if (image.brief_instance_name() == "flop" || image.brief_instance_name() == "floppydisk") {
                    image.load(processor->pendingFloppyPath);
                    break;
                }
            }
        }

        // --- CARTRIDGE MOUNTING ---
        if (processor->requestCartLoad.exchange(false, std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(processor->mediaMutex);
            
            for (device_image_interface &image : image_interface_enumerator(mame_machine->root_device())) {
                if (image.brief_instance_name() == "cart" || image.brief_instance_name() == "cartridge") {
                    image.load(processor->pendingCartPath);
                    break;
                }
            }
        }
        
                // Disable on-screen popups (e.g., "State loaded")
                mame_machine->ui().popup_time(0, " ");
                
                // --- FRAME SKIPPING
                frameSkipCounter++;
                if (frameSkipCounter % 2 != 0) {
                    return;
                }

                render_target *target = mame_machine->render().first_target();
                if (target == nullptr) return;
                
                render_primitive_list &prims = target->get_primitives();
                prims.acquire_lock();
        
        // --- DOUBLE BUFFERED RENDERING ---
        // Draw into the buffer that is currently NOT being read by the JUCE GUI thread
        int writeIndex = 1 - processor->readyBufferIndex.load(std::memory_order_acquire);
        juce::Graphics g(processor->screenBuffers[writeIndex]);
        g.fillAll(juce::Colours::black);
        
        for (render_primitive *prim = prims.first(); prim != nullptr; prim = prim->next()) {
            juce::Rectangle<float> rect(
                                        prim->bounds.x0, prim->bounds.y0,
                                        prim->bounds.x1 - prim->bounds.x0,
                                        prim->bounds.y1 - prim->bounds.y0
                                        );
            
            if (prim->type == render_primitive::QUAD) {
                if (prim->texture.base != nullptr) {
                    if (prim->texture.width > processor->cachedTexture.getWidth() ||
                        prim->texture.height > processor->cachedTexture.getHeight()) {
                        continue; // Failsafe to prevent out-of-bounds rendering
                    }
                    
                    uint32_t format = PRIMFLAG_GET_TEXFORMAT(prim->flags);
                    
                    // Pre-calculate fixed color multipliers for fast pixel processing
                    const uint32_t rT = (uint32_t)(prim->color.r * 255.0f);
                    const uint32_t gT = (uint32_t)(prim->color.g * 255.0f);
                    const uint32_t bT = (uint32_t)(prim->color.b * 255.0f);
                    const uint32_t aT = (uint32_t)(prim->color.a * 255.0f);
                    
                    const int width = prim->texture.width;
                    const int height = prim->texture.height;
                    const uint32_t srcPitch = prim->texture.rowpixels; 
                    
                    {
                        juce::Image::BitmapData texData(processor->cachedTexture, juce::Image::BitmapData::writeOnly);
                        
                        // 1. ARGB32 MODE (Each pixel has its own Alpha channel)
                        if (format == TEXFORMAT_ARGB32) {
                            for (int y = 0; y < height; ++y) {

                                const uint32_t* __restrict srcRow = static_cast<const uint32_t*>(prim->texture.base) + (y * srcPitch);
                                uint32_t* __restrict dstRow = reinterpret_cast<uint32_t*>(texData.getLinePointer(y));

                                for (int x = 0; x < width; ++x) {
                                    uint32_t p = srcRow[x];
                                    uint32_t a = (p >> 24);
                                    uint32_t r = (p >> 16) & 0xff;
                                    uint32_t g = (p >> 8) & 0xff;
                                    uint32_t b = p & 0xff;

                                    a = (a * aT) >> 8;
                                    r = (((r * rT) >> 8) * a) >> 8;
                                    g = (((g * gT) >> 8) * a) >> 8;
                                    b = (((b * bT) >> 8) * a) >> 8;

                                    dstRow[x] = (a << 24) | (r << 16) | (g << 8) | b;
                                }
                            }
                        }
                        // 2. RGB32 MODE (Alpha is fixed to 255 - Massive optimization!)
                        else if (format == TEXFORMAT_RGB32) {
                            const uint32_t finalA = (255 * aT) >> 8;
                            const uint32_t rMult = (rT * finalA) >> 8;
                            const uint32_t gMult = (gT * finalA) >> 8;
                            const uint32_t bMult = (bT * finalA) >> 8;
                            
                            for (int y = 0; y < height; ++y) {
                                const uint32_t* srcRow = static_cast<const uint32_t*>(prim->texture.base) + (y * srcPitch);
                                uint32_t* dstRow = reinterpret_cast<uint32_t*>(texData.getLinePointer(y));
                                
                                for (int x = 0; x < width; ++x) {
                                    uint32_t p = srcRow[x];
                                    uint32_t r = (p >> 16) & 0xff;
                                    uint32_t g = (p >> 8) & 0xff;
                                    uint32_t b = p & 0xff;
                                    
                                    r = (r * rMult) >> 8;
                                    g = (g * gMult) >> 8;
                                    b = (b * bMult) >> 8;
                                    
                                    dstRow[x] = (finalA << 24) | (r << 16) | (g << 8) | b;
                                }
                            }
                        }
                        // 3. PALETTE16 MODE (MAME's internal palette uses rgb_t, which is 32-bit ARGB)
                        else if (format == TEXFORMAT_PALETTE16) { 
                            const rgb_t* palette = prim->texture.palette;
                            
                            for (int y = 0; y < height; ++y) {
                                const uint16_t* srcRow = static_cast<const uint16_t*>(prim->texture.base) + (y * srcPitch);
                                uint32_t* dstRow = reinterpret_cast<uint32_t*>(texData.getLinePointer(y));
                                
                                for (int x = 0; x < width; ++x) {
                                    uint32_t p = palette[srcRow[x]];
                                    
                                    uint32_t a = 255;
                                    uint32_t r = (p >> 16) & 0xff;
                                    uint32_t g = (p >> 8) & 0xff;
                                    uint32_t b = p & 0xff;
                                    
                                    a = (a * aT) >> 8;
                                    r = (((r * rT) >> 8) * a) >> 8;
                                    g = (((g * gT) >> 8) * a) >> 8;
                                    b = (((b * bT) >> 8) * a) >> 8;
                                    
                                    dstRow[x] = (a << 24) | (r << 16) | (g << 8) | b;
                                }
                            }
                        }
                    } // Scoped BitmapData write lock released here
                    
                    g.drawImage(processor->cachedTexture,
                                static_cast<int>(rect.getX()), static_cast<int>(rect.getY()),
                                static_cast<int>(rect.getWidth()), static_cast<int>(rect.getHeight()),
                                0, 0, width, height, false);
                    
                } else {
                    juce::Colour color((uint8_t)(prim->color.r * 255.0f), (uint8_t)(prim->color.g * 255.0f),
                                       (uint8_t)(prim->color.b * 255.0f), (uint8_t)(prim->color.a * 255.0f));
                    g.setColour(color);
                    g.fillRect(rect);
                }
                
            } else if (prim->type == render_primitive::LINE) {
                juce::Colour color((uint8_t)(prim->color.r * 255.0f), (uint8_t)(prim->color.g * 255.0f),
                                   (uint8_t)(prim->color.b * 255.0f), (uint8_t)(prim->color.a * 255.0f));
                g.setColour(color);
                
                g.drawLine(prim->bounds.x0, prim->bounds.y0, prim->bounds.x1, prim->bounds.y1, prim->width);
            }
        }
        
        prims.release_lock();
                    
        // Swap the ready buffer index. The JUCE GUI will now pick up the fresh frame.
        processor->readyBufferIndex.store(writeIndex, std::memory_order_release);
        processor->getFrameFlag().store(true, std::memory_order_release);
    }
        
    virtual void input_update(bool relative_reset) override {
        if (mame_machine == nullptr) return;
        render_target* target = mame_machine->render().first_target();
        
        if (target != nullptr) {
            int x = processor->mouseX.load(std::memory_order_relaxed);
            int y = processor->mouseY.load(std::memory_order_relaxed);
            uint32_t currentBtns = processor->mouseButtons.load(std::memory_order_relaxed);
            
            // Edge detection for mouse clicks
            int32_t pressed =  ((currentBtns & 1) && !(lastMouseButtons & 1)) ? 1 : 0; 
            int32_t released = (!(currentBtns & 1) && (lastMouseButtons & 1)) ? 1 : 0; 
            int32_t clicks = pressed; 
            
            mame_machine->ui_input().push_pointer_update(
                target, ui_input_manager::pointer::MOUSE, 0, 0,
                x, y, currentBtns, pressed, released, clicks
            );
            
            lastMouseButtons = currentBtns;
        }
    }
    
    virtual void check_osd_inputs() override {};
    virtual void set_verbose(bool print_verbose) override {};

    virtual void init_debugger() override {};
    virtual void wait_for_debugger(device_t &device, bool firststop) override {};

    virtual bool no_sound() override { return false; };
    virtual bool sound_external_per_channel_volume() override { return false; };
    virtual bool sound_split_streams_per_source() override { return false; };
            
    virtual osd::audio_info sound_get_information() override {
        osd::audio_info info;
        osd::audio_info::node_info node;
        
        node.m_name = "vst_audio";
        node.m_display_name = "VST Audio Output";
        node.m_id = 1;
        
        // Force MAME to generate audio at the exact sample rate required by the DAW host.
        node.m_rate = { static_cast<uint32_t>(processor->getHostSampleRate()) };
        node.m_sinks = 1;
        node.m_sources = 0;
        
        info.m_nodes.push_back(node);
        info.m_default_sink = 1;
        info.m_generation = 1;
        
        return info;
    };
    
    virtual uint32_t sound_stream_sink_open(uint32_t node, std::string name, uint32_t rate) override { return 1; };
    virtual void sound_stream_close(uint32_t id) override {};
    
    virtual void add_audio_to_recording(const int16_t *buffer, int samples_this_frame) override {
        if (processor != nullptr) {
            processor->pushAudioFromMame(buffer, samples_this_frame);
        }
    };
    
    virtual uint32_t sound_stream_source_open(uint32_t node, std::string name, uint32_t rate) override { return 0; };
    virtual uint32_t sound_get_generation() override { return 1; };
    
    virtual void sound_stream_source_update(uint32_t id, int16_t *buffer, int samples_this_frame) override {};
    virtual void sound_stream_set_volumes(uint32_t id, const std::vector<float> &db) override {};
    virtual void sound_begin_update() override {};
    virtual void sound_end_update() override {};
    virtual void sound_stream_sink_update(uint32_t id, const int16_t *buffer, int samples_this_frame) override {};

    virtual void customize_input_type_list(std::vector<input_type_entry> &typelist) override { typelist.clear(); };
    virtual std::vector<ui::menu_item> get_slider_list() override { return {}; };

    virtual osd_font::ptr font_alloc() override { return nullptr; };
    virtual bool get_font_families(std::string const &font_path, std::vector<std::pair<std::string, std::string> > &result) override { return false; };
    virtual bool execute_command(const char *command) override { return false; };

    virtual std::unique_ptr<osd::midi_input_port> create_midi_input(std::string_view name) override {
        return std::make_unique<VstMidiInputPort>(processor);
    };
    
    virtual std::unique_ptr<osd::midi_output_port> create_midi_output(std::string_view name) override { return {}; };

    virtual std::vector<osd::midi_port_info> list_midi_ports() override {
        std::vector<osd::midi_port_info> ports;
        osd::midi_port_info info;
        
        info.name = "VST MIDI"; 
        info.input = true;
        info.output = false;
        info.default_input = true;
        info.default_output = false;
        
        ports.push_back(info);
        return ports;
    };

    virtual std::unique_ptr<osd::network_device> open_network_device(int id, osd::network_handler &handler) override { return {}; };
    virtual std::vector<osd::network_device_info> list_network_devices() override { return {}; };
};

// ==============================================================================
// VST AUTOMATION (APVTS) DEFINITIONS
// ==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout EnsoniqSD1AudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Automated parameters
    params.push_back(std::make_unique<juce::AudioParameterFloat>("volume", "Volume", 0.0f, 1.0f, 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("data_entry", "Data Entry", 0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("pitch_bend", "Pitch Bend", 0.0f, 1.0f, 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("mod_wheel", "Mod Wheel", 0.0f, 1.0f, 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>("sustain_pedal", "Sustain Pedal", 0.0f, 1.0f, 0.0f));
    
    // --- No automation of settings ---
    auto nonAutomatable = juce::AudioParameterChoiceAttributes().withAutomatable(false);

    juce::StringArray bufferSizes = { "256", "512", "1024", "2048", "4096", "8192" };
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID("buffer_size", 1), "Internal Buffer", bufferSizes, 2, nonAutomatable));

    // Dynamic Panel Layout Selector
    juce::StringArray views = { "Compact (Default)", "Full Keyboard", "Rack Panel", "Tablet View" };
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID("layout_view", 1), "Panel Layout", views, 0, nonAutomatable));

    return { params.begin(), params.end() };
}

void EnsoniqSD1AudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
    // --- 1. SETUP ---
    if (parameterID == "buffer_size") {
            auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("buffer_size"));
            if (choiceParam != nullptr) {
                int sizes[] = { 256, 512, 1024, 2048, 4096, 8192 };
                int newThreshold = sizes[choiceParam->getIndex()];
                mameBufferThreshold.store(newThreshold, std::memory_order_relaxed);
                
                if ((juce::MessageManager::getInstanceWithoutCreating() != nullptr && juce::MessageManager::getInstanceWithoutCreating()->isThisTheMessageThread())) {
                    setLatencySamples(newThreshold + getInternalHardwareLatencySamples());
                }
            }
        }
                
    else if (parameterID == "layout_view") {
        auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("layout_view"));
        int idx = choiceParam != nullptr ? choiceParam->getIndex() : 0;
        
        std::string mameViewName = "Compact";
        int newW = 2048; int newH = 921;
        
        if (idx == 0) { mameViewName = "Compact"; newW = 2048; newH = 921; }
        else if (idx == 1) { mameViewName = "Full"; newW = 2048; newH = 671; }
        else if (idx == 2) { mameViewName = "Panel"; newW = 2048; newH = 379; }
        else if (idx == 3) { mameViewName = "Tablet"; newW = 2048; newH = 1476; }
        
        requestedViewIndex.store(idx, std::memory_order_release);
        mameInternalWidth.store(newW, std::memory_order_release);
        mameInternalHeight.store(newH, std::memory_order_release);
        requestViewChange.store(true, std::memory_order_release);
        
    }
    
    // --- 2. AUTOMATION ---
        else {
            uint64_t targetSample = totalRead.load(std::memory_order_acquire) + mameBufferThreshold.load(std::memory_order_relaxed);
            double sr = hostSampleRate.load(std::memory_order_relaxed);
            double t_anchor = anchorMameTime.load(std::memory_order_relaxed);
            uint64_t s_anchor = anchorDawSample.load(std::memory_order_relaxed);
            
            // Same clean math as in processBlock
            double targetMameTime = t_anchor + static_cast<double>(targetSample - s_anchor) / sr;

            if (parameterID == "volume") {
                uint8_t val = static_cast<uint8_t>(newValue * 127.0f);
                pushMidiByte(0xB0, targetMameTime);
                pushMidiByte(0x07, targetMameTime);
                pushMidiByte(val, targetMameTime);
            }
        }
}
// ==============================================================================

EnsoniqSD1AudioProcessor::EnsoniqSD1AudioProcessor()
     : AudioProcessor (BusesProperties()
                       .withOutput ("Main Out", juce::AudioChannelSet::stereo(), true)
                       .withOutput ("Aux Out",  juce::AudioChannelSet::stereo(), true) 
                       ),
       apvts(*this, nullptr, "Parameters", createParameterLayout())
{
    apvts.addParameterListener("volume", this);
    apvts.addParameterListener("data_entry", this);
    apvts.addParameterListener("pitch_bend", this);
    apvts.addParameterListener("mod_wheel", this);
    apvts.addParameterListener("buffer_size", this);
    apvts.addParameterListener("layout_view", this);
    
    // --- SINGLE INSTANCE LOCKING ---
        isMasterInstance = false;
        isBlockedByAnotherInstance.store(false, std::memory_order_release);

#ifdef _WIN32
        // Request 1ms timer resolution from the Windows OS scheduler.
        // By default, Windows thread sleeping (e.g., wait(1)) can overshoot up to 15.6ms.
        // This strict 1ms resolution ensures the MAME background thread wakes up precisely,
        // preventing audio dropouts during low-latency real-time playback, while keeping
        // offline rendering (bounce) mathematically intact and fully synchronized.
        timeBeginPeriod(1);
#endif
}

EnsoniqSD1AudioProcessor::~EnsoniqSD1AudioProcessor()
{
    // AU Highlander cleanup
        {
            std::lock_guard<std::mutex> lock(instanceMutex);
            if (activeMameProcessor == this) {
                activeMameProcessor = nullptr;
            }
        }
        
        // safely shut down MAME engine
        shutdownMame();

#ifdef _WIN32
        // Release the high-resolution timer request gracefully when the plugin is destroyed
        // or removed from the DAW track, returning Windows to its default scheduler resolution.
        timeEndPeriod(1);
#endif
}

void EnsoniqSD1AudioProcessor::shutdownMame()
{

    if (isMasterInstance) {
        isMameRunning.store(false, std::memory_order_release);
        
        if (mameMachine != nullptr) {
            mameMachine->schedule_exit();
        }
        
        mameThrottleEvent.signal();
        mameStateEvent.signal();
        
        if (mameThread.joinable()) {
            mameThread.join();
        }
        
        isMasterInstance = false;
        mameHasStarted.store(false, std::memory_order_release);
        isBlockedByAnotherInstance.store(true, std::memory_order_release);
        
        // No lock for VST3
        globalMameLock.store(false);
    }
}

//==============================================================================
const juce::String EnsoniqSD1AudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool EnsoniqSD1AudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool EnsoniqSD1AudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool EnsoniqSD1AudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double EnsoniqSD1AudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int EnsoniqSD1AudioProcessor::getNumPrograms()
{
    return 1;
}

int EnsoniqSD1AudioProcessor::getCurrentProgram()
{
    return 0;
}

void EnsoniqSD1AudioProcessor::setCurrentProgram (int index)
{
}

const juce::String EnsoniqSD1AudioProcessor::getProgramName (int index)
{
    return {};
}

void EnsoniqSD1AudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

//==============================================================================
void EnsoniqSD1AudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{

    // SINGLE INSTANCE LOCK (Format-specific handling)

        if (!isMasterInstance) {
            bool acquired = false;

            if (wrapperType == juce::AudioProcessor::wrapperType_AudioUnit) {
                // AU Branch: Forced takeover (Highlander)
                std::lock_guard<std::mutex> lock(instanceMutex);
                
                if (activeMameProcessor != this) {
                    // If there's already another instance of AU in memory, we'll kill your MAME right away!
                    if (activeMameProcessor != nullptr) {
                        activeMameProcessor->shutdownMame();
                    }
                    
                    // Now we (the new version) are the Masters
                    activeMameProcessor = this;
                    acquired = true;
                }
            } else {
                // VST3 BRANCH: Original blocking (synchronous) wait
                // VST3 waits properly until the host program terminates the old instance.
                int retries = 60;
                while (retries > 0) {
                    bool expected = false;
                    if (globalMameLock.compare_exchange_strong(expected, true)) {
                        acquired = true;
                        break; // We successfully obtained the lock!
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    retries--;
                }
            }

            // --- CONTINUE IF SUCCESSFUL ---
            if (acquired) {
                isMasterInstance = true;
                isBlockedByAnotherInstance.store(false, std::memory_order_release);
                
                juce::File docsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
                juce::File ensoniqDir = docsDir.getChildFile("EnsoniqSD1");
                if (!ensoniqDir.exists()) ensoniqDir.createDirectory();
            } else {
                // If the 3-second wait has elapsed on the VST3 thread
                isBlockedByAnotherInstance.store(true, std::memory_order_release);
                return;
            }
        }
    
    hostSampleRate.store(sampleRate);
    
    auto* choiceParam = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter("buffer_size"));
        if (choiceParam != nullptr) {
            int sizes[] = { 256, 512, 1024, 2048, 4096, 8192 };
            mameBufferThreshold.store(sizes[choiceParam->getIndex()], std::memory_order_relaxed);
        }

    // Report buffer for Latency Compensation (PDC)
        int currentThreshold = mameBufferThreshold.load(std::memory_order_relaxed);
        int hwLatency = getInternalHardwareLatencySamples();
        setLatencySamples(currentThreshold + hwLatency);
    
    // Only boot MAME the very first time play is prepared
        if (!mameHasStarted.exchange(true)) {
        
        initialSampleRate.store(sampleRate); 
        
        juce::File docsDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        juce::File ensoniqDir = docsDir.getChildFile("EnsoniqSD1");
        juce::File romFile = ensoniqDir.getChildFile("sd132.zip");

        if (!romFile.existsAsFile()) {
            isRomMissing.store(true, std::memory_order_release);
            isMameRunning = false;
        } else {
            isRomMissing.store(false, std::memory_order_release);
            isMameRunning = true;
            
            // Safety net against std::terminate if thread wasn't joined
            if (mameThread.joinable()) {
                mameThread.join();
            }
            mameThread = std::thread(&EnsoniqSD1AudioProcessor::runMameEngine, this);
        }
    } else {
        // MAME cannot change sample rates on the fly. If the DAW changes it mid-session, we must halt processing.
        if (sampleRate != initialSampleRate.load()) {
            sampleRateMismatch.store(true, std::memory_order_release); 
        } else {
            sampleRateMismatch.store(false, std::memory_order_release); 
        }
    }
    // --- Buffer reset & PDC Pre-fill ---
        totalRead.store(0, std::memory_order_release);
        
    // We shift the MAME write pointer forward by the specified PDC delay!
    // This ensures that the first 'currentThreshold' samples will be pure silence,
    // which the DAW's PDC will be able to trim precisely from the beginning of the file.

    // We shift the MAME write pointer forward by the specified PDC delay!
        totalWritten.store(currentThreshold, std::memory_order_release);
        
        // --- Reset interpolator ---
        needAnchorSync.store(true, std::memory_order_release);
            
        midiReadPos.store(0, std::memory_order_release);
        midiWritePos.store(0, std::memory_order_release);

        // We clear the entire ring buffer to ensure that the preloaded section is completely silent
        for (int i = 0; i < RING_BUFFER_SIZE; ++i) {
            ringBufferL[i] = 0.0f;
            ringBufferR[i] = 0.0f;
            ringBufferAuxL[i] = 0.0f;
            ringBufferAuxR[i] = 0.0f;
        }
}

void EnsoniqSD1AudioProcessor::releaseResources()
{
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool EnsoniqSD1AudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // 1. Main Out MUST be stereo
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // 2. Aux Out must also be stereo if enabled by the DAW host
    auto auxBus = layouts.getChannelSet(false, 1);
    if (auxBus != juce::AudioChannelSet::disabled() && auxBus != juce::AudioChannelSet::stereo())
        return false;

    return true;
}
#endif

void EnsoniqSD1AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());
        
    if (isBlockedByAnotherInstance.load(std::memory_order_acquire)) return;
    if (sampleRateMismatch.load(std::memory_order_acquire)) return;

    int numSamples = buffer.getNumSamples();
    uint64_t currentReadPos = totalRead.load(std::memory_order_acquire);
    int threshold = mameBufferThreshold.load(std::memory_order_relaxed);
    double sr = hostSampleRate.load(std::memory_order_relaxed);

    // security boot check ---
    if (mameMachine == nullptr) {
        totalRead.store(currentReadPos + numSamples, std::memory_order_release);
        return;
    }
    
    bool isOffline = isNonRealtime();
    
        // ========================================================
        // 0. WAIT FOR PERFECT ANCHOR (HYBRID MODEL)
        // ========================================================

    if (needAnchorSync.load(std::memory_order_acquire)) {
        mameThrottleEvent.signal(); // Wake up MAME!

        // We determine how long we are allowed to block the audio thread.
        // OFFLINE (Bounce): We can wait safely up to 2000ms. The DAW will pause and wait.
        // REALTIME (Live Play): We must NOT block for more than 2ms, otherwise the DAW 
        // will drop the audio buffer (resulting in severe CPU crackles/spikes).
        int timeoutMs = isNonRealtime() ? 2000 : 2;
        int waitMs = 0;

        // Wait loop: Check if MAME has established the exact audio anchor yet
        while (needAnchorSync.load(std::memory_order_acquire) && waitMs < timeoutMs) {
#ifdef _WIN32
            // --- WINDOWS ---
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
#else
            // --- macOS ---
            if (pthread_main_np() == 0) { // Only wait if we are NOT on the main thread
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            else {
                break; // Safety breakout for macOS main thread rendering
            }
#endif
            waitMs++;
        }

        // EMERGENCY FALLBACK (REALTIME ONLY):
        // If the short 2ms timeout expired, MAME is taking too long to generate the first audio block.
        // To prevent the DAW from dropping the audio thread AND to prevent losing the very first 
        // MIDI notes, we force an immediate anchor estimation.
        if (needAnchorSync.load(std::memory_order_acquire)) {
            if (!isNonRealtime() && mameMachine != nullptr) {

                // We fake the perfect audio callback by grabbing the current execution time of the 
                // MAME CPU and matching it to the current DAW write position.
                // This keeps the complex MIDI synchronization math perfectly valid and guarantees 
                // that no notes are dropped or slipped during live playback.
                anchorMameTime.store(mameMachine->time().as_double(), std::memory_order_relaxed);
                anchorDawSample.store(totalWritten.load(std::memory_order_acquire), std::memory_order_relaxed);

                needAnchorSync.store(false, std::memory_order_release);

            }
            else {
                // If we are offline and it timed out (or MAME crashed), we must bail out safely.
                // Outputting silence is the only way to avoid a complete DAW freeze.
                return;
            }
        }
    }

    double t_anchor = anchorMameTime.load(std::memory_order_relaxed);
    uint64_t s_anchor = anchorDawSample.load(std::memory_order_relaxed);

    // ========================================================
    // 1. TIMESTAMPED MIDI INJECTION
    // ========================================================
    for (const auto metadata : midiMessages) {
        int eventOffset = metadata.samplePosition;
        uint64_t targetSample = currentReadPos + eventOffset + threshold;
        
        // anchor time + time since anchor via DAW time
        double targetMameTime = t_anchor + static_cast<double>(targetSample - s_anchor) / sr;
        
        auto msg = metadata.getMessage();
        const uint8_t* rawData = msg.getRawData();
        for (int i = 0; i < msg.getRawDataSize(); ++i) {
            pushMidiByte(rawData[i], targetMameTime);
        }
    }
   
            // ========================================================
            // 2. AUDIO OUT & RING BUFFER CONSUMPTION
            // ========================================================
                    
            int timeoutMs = 0;

                if (isOffline) {
                    //
                    timeoutMs = 2000;
                    maxOfflineBuffer.store(numSamples + mameBufferThreshold.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }

                if (timeoutMs > 0) {
                int elapsedMs = 0;
                uint64_t targetWritePos = currentReadPos + numSamples;
                
                if (isOffline) {
                    targetWritePos += mameBufferThreshold.load(std::memory_order_relaxed);
                }

                while (isMameRunningFlag()) {
                    uint64_t writePos = totalWritten.load(std::memory_order_acquire);
                    if (writePos >= targetWritePos) break;
                    if (elapsedMs >= timeoutMs) break;

                    mameThrottleEvent.signal();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    elapsedMs++;
                }
            }

        uint64_t currentWritePos = totalWritten.load(std::memory_order_acquire);
        
        int64_t available = static_cast<int64_t>(currentWritePos) - static_cast<int64_t>(currentReadPos);

        // Calculate how many samples we can push
        int samplesToProcess = (available < numSamples) ? static_cast<int>(available) : numSamples;

        // Output pointers (0, 1 = Main L/R | 2, 3 = Aux L/R)
        auto* outL = buffer.getWritePointer(0);
        auto* outR = (totalNumOutputChannels > 1) ? buffer.getWritePointer(1) : nullptr;
        auto* outAuxL = (totalNumOutputChannels > 2) ? buffer.getWritePointer(2) : nullptr;
        auto* outAuxR = (totalNumOutputChannels > 3) ? buffer.getWritePointer(3) : nullptr;

        // Consume samples from the ring buffers
        for (int i = 0; i < samplesToProcess; ++i) {

            // --- OPTIMIZATION ---
            // Bitwise AND (&) wrap-around instead of modulo (%)
            uint64_t idx = currentReadPos & (RING_BUFFER_SIZE - 1);

            outL[i] = ringBufferL[idx];
            if (outR != nullptr) outR[i] = ringBufferR[idx];

            if (outAuxL != nullptr) outAuxL[i] = ringBufferAuxL[idx];
            if (outAuxR != nullptr) outAuxR[i] = ringBufferAuxR[idx];

            currentReadPos++;
        }

    // Underrun protection: pad remaining required samples with zeroes
        if (!isNonRealtime() && samplesToProcess < numSamples) {
            for (int i = samplesToProcess; i < numSamples; ++i) {
                outL[i] = 0.0f;
                if (outR != nullptr) outR[i] = 0.0f;
                if (outAuxL != nullptr) outAuxL[i] = 0.0f;
                if (outAuxR != nullptr) outAuxR[i] = 0.0f;
            }
        }

        totalRead.store(currentReadPos, std::memory_order_release);
        mameThrottleEvent.signal(); // Wake MAME for the next round
    
        // --- ANTI-SMART-DISABLE Protection
        static float antiDisable = 1e-8f;
        antiDisable = -antiDisable;
        for (int i = 0; i < numSamples; ++i) {
            outL[i] += antiDisable;
            if (outR != nullptr) outR[i] += antiDisable;
        }
}

//==============================================================================
bool EnsoniqSD1AudioProcessor::hasEditor() const
{
    return true; 
}

juce::AudioProcessorEditor* EnsoniqSD1AudioProcessor::createEditor()
{
    return new EnsoniqSD1AudioProcessorEditor (*this);
}

//==============================================================================
void EnsoniqSD1AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // 1. Save VST parameters to XML
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());

    // --- SAVE WINDOW SIZE TO XML ---
    xml->setAttribute("ui_width", savedWindowWidth);
    xml->setAttribute("ui_height", savedWindowHeight);

    if (isMameRunningFlag()) {
        // 2. Request MAME to save its RAM state and wait (up to 2 seconds) for it to finish.
        mameStateEvent.reset();
        mameStateIsReady.store(false, std::memory_order_release);
        requestMameSave.store(true, std::memory_order_release);
        
        // Wake up MAME in case it was throttled
        mameThrottleEvent.signal();
        
        if (mameStateEvent.wait(2000)) {
            // 3. Once MAME is done, read the temporary state file
            juce::File tempStateFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sd132").getChildFile("vst_temp.sta");
            
            if (tempStateFile.existsAsFile()) {
                juce::MemoryBlock mameData;
                tempStateFile.loadFileAsData(mameData);
                // Encode the entire MAME hardware state (RAM, CPU, etc.) as Base64 and embed it in the DAW's project file.
                xml->setAttribute("mame_state", mameData.toBase64Encoding());
            }
        }
    }
    
    copyXmlToBinary (*xml, destData);
}

void EnsoniqSD1AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr) {
        
        // 1. Restore VST Automation Parameters
        if (xmlState->hasTagName (apvts.state.getType())) {
            apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
        }

        // --- RESTORE WINDOW SIZE ---
        savedWindowWidth = xmlState->getIntAttribute("ui_width", 0);
        savedWindowHeight = xmlState->getIntAttribute("ui_height", 0);
        
        // 2. Restore complete MAME hardware state
        if (xmlState->hasAttribute("mame_state")) {
            juce::MemoryBlock mameData;
            mameData.fromBase64Encoding(xmlState->getStringAttribute("mame_state"));
            
            // Always save temp file (either MAME booted or not)
            juce::File tempStateDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sd132");
            tempStateDir.createDirectory();
            
            juce::File tempStateFile = tempStateDir.getChildFile("vst_temp.sta");
            tempStateFile.replaceWithData(mameData.getData(), mameData.getSize());
            
            // Load flag, MAME read it if booted
            requestMameLoad.store(true, std::memory_order_release);
            
            // Wait only for load when MAME is already run (e.g. preset change)

#ifdef _WIN32
    // --- WINDOWS no pthread ---
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread()) {
        mameStateEvent.wait(2000);
    }
#else
    // --- macOS ---
    if (pthread_main_np() == 0) {
        mameStateEvent.wait(2000);
    }
#endif
                                
                }
            }
                                    
}

// ==============================================================================

void EnsoniqSD1AudioProcessor::injectMouseMove(int x, int y) {
    mouseX.store(x, std::memory_order_relaxed);
    mouseY.store(y, std::memory_order_relaxed);
}

void EnsoniqSD1AudioProcessor::injectMouseDown(int x, int y) {
    mouseX.store(x, std::memory_order_relaxed);
    mouseY.store(y, std::memory_order_relaxed);
    mouseButtons.fetch_or(1, std::memory_order_relaxed); // Set Left Click Bit
}

void EnsoniqSD1AudioProcessor::injectMouseUp(int x, int y) {
    mouseX.store(x, std::memory_order_relaxed);
    mouseY.store(y, std::memory_order_relaxed);
    mouseButtons.fetch_and(~1, std::memory_order_relaxed); // Clear Left Click Bit
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new EnsoniqSD1AudioProcessor();
}

// ==============================================================================
// ENSONIQ MAME DRIVER LIST REGISTRATION
// ==============================================================================
extern const game_driver driver____empty;
extern const game_driver driver_eps;
extern const game_driver driver_eps16p;
extern const game_driver driver_ks32;
extern const game_driver driver_sd1;
extern const game_driver driver_sd132;
extern const game_driver driver_sq1;
extern const game_driver driver_sq2;
extern const game_driver driver_sqrack;
extern const game_driver driver_vfx;
extern const game_driver driver_vfxsd;

const game_driver * const driver_list::s_drivers_sorted[11] =
{
    &driver____empty,
    &driver_eps,
    &driver_eps16p,
    &driver_ks32,
    &driver_sd1,
    &driver_sd132,
    &driver_sq1,
    &driver_sq2,
    &driver_sqrack,
    &driver_vfx,
    &driver_vfxsd,
};

const std::size_t driver_list::s_driver_count = 11;
// ==============================================================================

void EnsoniqSD1AudioProcessor::runMameEngine()
{
#ifdef _WIN32
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);
    if (hTask != NULL) {
        AvSetMmThreadPriority(hTask, AVRT_PRIORITY_CRITICAL);
    }
    else {
        // Fallback if no MMCSS
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
#endif

    // Prevent MAME from hijacking the host OS audio/video drivers
#ifdef _WIN32
    _putenv("SDL_VIDEODRIVER=dummy");
    _putenv("SDL_AUDIODRIVER=dummy");
#else
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif  
    
    std::vector<std::string> args;
    args.push_back("mame");
    args.push_back("sd132");
    
    juce::File ensoniqDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("EnsoniqSD1");
    args.push_back("-rompath");

#ifdef _WIN32
    // Ensure strict UTF-8 string conversion on Windows to prevent MAME boot failures 
    // if the user's home directory contains non-ASCII characters (e.g., accents).
    args.push_back(ensoniqDir.getFullPathName().toUTF8().getAddress());
#else
    args.push_back(ensoniqDir.getFullPathName().toStdString());
#endif

    // Setup internal Layouts plugin path

#ifdef _WIN32

    juce::String finalPluginsPath;
    // Windows: Unzip plugins to Temp dir
    juce::File tempMameDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("EnsoniqSD1_MAME_Data");

    // Only at first run
    if (!tempMameDir.exists())
    {
        tempMameDir.createDirectory();

        // using binarydata
        juce::MemoryInputStream zipStream(BinaryData::mame_plugins_zip, BinaryData::mame_plugins_zipSize, false);
        juce::ZipFile zip(zipStream);
        zip.uncompressTo(tempMameDir);
    }

    // real Windows path
    finalPluginsPath = tempMameDir.getChildFile("plugins").getFullPathName();
    args.push_back("-pluginspath");
    args.push_back(finalPluginsPath.toStdString());

#else

    // original mac solution
    juce::File exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    juce::File pluginsDir = exeFile.getParentDirectory().getParentDirectory().getChildFile("Resources").getChildFile("plugins");
    
    args.push_back("-pluginspath");
    args.push_back(pluginsDir.getFullPathName().toStdString());

#endif

    args.push_back("-plugin");
    args.push_back("layout");

    // Use software rendering and our custom OSD sound module
    args.push_back("-video");
    args.push_back("soft");
    args.push_back("-sound");
    args.push_back("osd");
    args.push_back("-midiin");
    args.push_back("VST MIDI");
    
    // Disable MAME's internal pacing (we control this via audio throttle)
    args.push_back("-nothrottle");
    args.push_back("-nosleep");
    args.push_back("-nowaitvsync");
    args.push_back("-nowindow");
    args.push_back("-nobackground_input");

    args.push_back("-noreadconfig");
    args.push_back("-skip_gameinfo");
    args.push_back("-samplerate");
    args.push_back(std::to_string(static_cast<int>(hostSampleRate.load())));
    
    args.push_back("-keyboardprovider");
    args.push_back("none");
    args.push_back("-mouseprovider");
    args.push_back("none");
    args.push_back("-joystickprovider");
    args.push_back("none");
    
    args.push_back("-state_directory");
    args.push_back(juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName().toStdString());
       
    // Boot the headless CLI Frontend
    auto* mameOpts = new osd_options();
    auto* headlessOsd = new VstOsdInterface(this, *mameOpts);
    auto* frontend = new cli_frontend(*mameOpts, *headlessOsd);
    
    frontend->execute(args);
    
    delete frontend;
    delete headlessOsd;
    delete mameOpts;
}

