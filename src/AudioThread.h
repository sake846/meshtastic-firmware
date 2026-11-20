#pragma once
#include "PowerFSM.h"
#include "SPILock.h"
#include "concurrency/OSThread.h"
#include "configuration.h"
#include "main.h"
#include "sleep.h"
#include <memory>

#ifdef HAS_I2S
#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorRTTTL.h>
#include <AudioOutputI2S.h>
#include <ESP8266SAM.h>

// A board with an I2S amplifier opts in by defining AUDIO_AMP_ENABLE(on) in its variant.h to power the
// amp on/off around playback (e.g. an enable pin on an I/O expander). The includes below expose the
// expander instances (io / mcpIoExpander) those macros typically reference.
#ifdef USE_PCA95X5
#include PCA95X5_INC
extern PCA95X5_CLS io;
#endif

#ifdef USE_MCP23017
#include "platform/esp32/ExtensionIOMCP23017.h"
#endif

#define AUDIO_THREAD_INTERVAL_MS 100

class AudioThread : public concurrency::OSThread
{
  public:
    AudioThread() : OSThread("Audio") { initOutput(); }

    void beginRttl(const void *data, uint32_t len)
    {
        ampEnable(true);
        setCPUFast(true);
        audioOut->SetGain(rtttlGain(data, len));
        rtttlFile = std::unique_ptr<AudioFileSourcePROGMEM>(new AudioFileSourcePROGMEM(data, len));
        i2sRtttl = std::unique_ptr<AudioGeneratorRTTTL>(new AudioGeneratorRTTTL());
        i2sRtttl->begin(rtttlFile.get(), audioOut.get());
    }

    // Also handles actually playing the RTTTL, needs to be called in loop
    bool isPlaying()
    {
        if (i2sRtttl != nullptr) {
            return i2sRtttl->isRunning() && i2sRtttl->loop();
        }
        return false;
    }

    void stop()
    {
        if (i2sRtttl != nullptr) {
            i2sRtttl->stop();
            i2sRtttl = nullptr;
        }

        rtttlFile = nullptr;

        setCPUFast(false);
        ampEnable(false);
    }

    void readAloud(const char *text)
    {
        if (i2sRtttl != nullptr) {
            i2sRtttl->stop();
            i2sRtttl = nullptr;
        }

        ampEnable(true);
        auto sam = std::unique_ptr<ESP8266SAM>(new ESP8266SAM);
        sam->Say(audioOut.get(), text);
        setCPUFast(false);
        audioOut->stop();
        ampEnable(false);
    }

  protected:
    int32_t runOnce() override
    {
        canSleep = true; // Assume we should not keep the board awake

        // if (i2sRtttl != nullptr && i2sRtttl->isRunning()) {
        //     i2sRtttl->loop();
        // }
        return AUDIO_THREAD_INTERVAL_MS;
    }

  private:
    static float rtttlGain(const void *data, uint32_t len)
    {
        const auto *rtttl = static_cast<const char *>(data);
        uint32_t volume = 20;
        for (uint32_t i = 0; i + 3 < len && rtttl[i] != ':'; ++i) {
            if (rtttl[i] != '~' || rtttl[i + 1] != 'v' || rtttl[i + 2] != '=')
                continue;

            uint32_t parsed = 0;
            uint32_t j = i + 3;
            while (j < len && rtttl[j] >= '0' && rtttl[j] <= '9') {
                parsed = parsed * 10 + static_cast<uint32_t>(rtttl[j++] - '0');
                if (parsed > 100)
                    parsed = 100;
            }
            if (j > i + 3)
                volume = parsed;
            break;
        }
        return volume / 100.0f;
    }

    // Amps like the NS4150 need time to leave shutdown, longer when the enable is an I/O expander write.
    // Without a variant's AUDIO_AMP_SETTLE_MS the short system tones are over before any audio gets out.
    static void ampEnable(bool on)
    {
#ifdef AUDIO_AMP_ENABLE
        AUDIO_AMP_ENABLE(on);
#ifdef AUDIO_AMP_SETTLE_MS
        if (on)
            delay(AUDIO_AMP_SETTLE_MS);
#endif
#else
        (void)on;
#endif
    }

    void initOutput()
    {
        audioOut = std::unique_ptr<AudioOutputI2S>(new AudioOutputI2S(1, AudioOutputI2S::EXTERNAL_I2S));
        audioOut->SetPinout(DAC_I2S_BCK, DAC_I2S_WS, DAC_I2S_DOUT, DAC_I2S_MCLK);
        audioOut->SetGain(0.2);
    };

    std::unique_ptr<AudioGeneratorRTTTL> i2sRtttl = nullptr;
    std::unique_ptr<AudioOutputI2S> audioOut = nullptr;

    std::unique_ptr<AudioFileSourcePROGMEM> rtttlFile = nullptr;
};

#endif
