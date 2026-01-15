#include <oboe/Oboe.h>
#include <mgba/core/core.h>
#include <mgba/core/thread.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>
#include <android/log.h>

#define TAG "OboeAudio"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

using namespace oboe;

// Context to hold audio state, similar to mSDLAudio in sdl-audio.c
struct OboeAudioContext {
    struct mAudioBuffer buffer;
    struct mAudioResampler resampler;
    struct mCore* core;
    struct mCoreSync* sync;
    unsigned samples;     // Buffer size in samples
    unsigned sampleRate;  // Output sample rate
};

static OboeAudioContext gContext = {0};
static std::shared_ptr<AudioStream> mStream;
static class OboeAudioStreamCallback* mCallback = nullptr;

class OboeAudioStreamCallback : public AudioStreamCallback {
public:
    DataCallbackResult onAudioReady(AudioStream *oboeStream, void *audioData, int32_t numFrames) override {
        if (!gContext.core) {
            memset(audioData, 0, numFrames * oboeStream->getChannelCount() * sizeof(int16_t));
             return DataCallbackResult::Continue;
        }

        struct mAudioBuffer* coreBuffer = NULL;
        unsigned coreSampleRate = 32768; // Default GBA rate

        if (gContext.core) {
            coreBuffer = gContext.core->getAudioBuffer(gContext.core);
            coreSampleRate = gContext.core->audioSampleRate(gContext.core);
        }

        double fauxClock = 1.0;
        if (gContext.sync) {
             // Try to calculate framerate ratio for synchronization
             if (gContext.sync->fpsTarget > 0 && gContext.core) {
                 fauxClock = mCoreCalculateFramerateRatio(gContext.core, gContext.sync->fpsTarget);
             }
             
             mCoreSyncLockAudio(gContext.sync);
             
             // Update high water mark for sync
             // Logic adapted from sdl-audio.c
             if (gContext.sampleRate > 0) {
                 gContext.sync->audioHighWater = gContext.samples + gContext.resampler.highWaterMark + gContext.resampler.lowWaterMark + (gContext.samples >> 6);
                 gContext.sync->audioHighWater *= coreSampleRate / (fauxClock * gContext.sampleRate);
             }
        }

        // Resample from core buffer to our local buffer
        mAudioResamplerSetSource(&gContext.resampler, coreBuffer, coreSampleRate / fauxClock, true);
        mAudioResamplerProcess(&gContext.resampler);

        if (gContext.sync) {
             mCoreSyncConsumeAudio(gContext.sync);
        }

        // Read from local buffer to Oboe output
        // Oboe requests 'numFrames' frames. Each frame is 2 samples (Stereo).
        // mAudioBufferRead 'samples' argument is number of frames if we consider the buffer is interleaved? 
        // Checking audio-buffer.c: mAudioBufferRead(..., samples) -> bytes = samples * buffer->channels * sizeof(int16_t)
        // So yes, 'samples' here means 'frames' for the buffer logic if channels=2.
        
        int available = mAudioBufferRead(&gContext.buffer, (int16_t*)audioData, numFrames);

        if (available < numFrames) {
             // Fill the rest with silence
             memset(((int16_t*)audioData) + available * 2, 0, (numFrames - available) * 2 * sizeof(int16_t));
        }

        return DataCallbackResult::Continue;
    }
};

extern "C" {

bool mOboeInit(struct mCoreThread* thread);
void mOboeDeinit();

bool mOboeInit(struct mCoreThread* thread) {
    LOGD("Initializing Oboe Audio");
    
    // Initialize Context
    memset(&gContext, 0, sizeof(gContext));
    gContext.samples = 2048; // Buffer size, power of 2
    gContext.sampleRate = 48000; // Standard Android rate
    
    if (thread) {
        gContext.core = thread->core;
        gContext.sync = &thread->impl->sync;
    }

    // Initialize mGBA audio components
    mAudioBufferInit(&gContext.buffer, gContext.samples, 2); // 2 channels
    mAudioResamplerInit(&gContext.resampler, mINTERPOLATOR_SINC);
    mAudioResamplerSetDestination(&gContext.resampler, &gContext.buffer, gContext.sampleRate);

    // Initialize Oboe
    AudioStreamBuilder builder;
    builder.setDirection(Direction::Output);
    builder.setPerformanceMode(PerformanceMode::LowLatency);
    builder.setSharingMode(SharingMode::Shared);
    builder.setFormat(AudioFormat::I16);
    builder.setChannelCount(ChannelCount::Stereo);
    builder.setSampleRate(gContext.sampleRate);
    
    mCallback = new OboeAudioStreamCallback();
    builder.setCallback(mCallback);
    
    Result result = builder.openStream(mStream);
    if (result != Result::OK) {
        LOGE("Failed to open Oboe stream: %s", convertToText(result));
        mAudioBufferDeinit(&gContext.buffer);
        mAudioResamplerDeinit(&gContext.resampler);
        return false;
    }
    
    // Update sample rate if Oboe chose something else
    gContext.sampleRate = mStream->getSampleRate();
    mAudioResamplerSetDestination(&gContext.resampler, &gContext.buffer, gContext.sampleRate);

    result = mStream->requestStart();
    if (result != Result::OK) {
        LOGE("Failed to start Oboe stream: %s", convertToText(result));
        mStream->close();
        mAudioBufferDeinit(&gContext.buffer);
        mAudioResamplerDeinit(&gContext.resampler);
        return false;
    }
    
    LOGD("Oboe Audio Started at %d Hz", gContext.sampleRate);
    return true;
}

void mOboeDeinit() {
    if (mStream) {
        mStream->close();
        mStream.reset();
    }
    if (mCallback) {
        delete mCallback;
        mCallback = nullptr;
    }
    
    mAudioBufferDeinit(&gContext.buffer);
    mAudioResamplerDeinit(&gContext.resampler);
    memset(&gContext, 0, sizeof(gContext));
    LOGD("Oboe Audio Deinitialized");
}

}
