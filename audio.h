#pragma once
#include <cstdint>
#include <atomic>
#include "config.h"
#include "vehicle.h"

#if !defined(RACER_NO_QT)
#include <QIODevice>
#include <QObject>
class QAudioSink;
#endif

struct ChamberlinSVF {
    float low  = 0.0f;
    float band = 0.0f;
    float high = 0.0f;

    void reset() { low = band = high = 0.0f; }
    float processBandPass(float in, float fc, float sr, float qInv = (1.0f / 3.0f));
};

#if !defined(RACER_NO_QT)
class SynthDevice : public QIODevice {
    Q_OBJECT
#else
class SynthDevice {
#endif
public:
    static constexpr int SR = 44100;
    SynthDevice();
    ~SynthDevice();

    void resetState();
    void setParams(const AudioParams& p);             // game thread, once per tick, lock-free
    void render(int16_t* out, int frames);            // audio thread; also called by offline tests (mono)
    void setMasterGain(float g);
    void setMusicEnabled(bool en);
    bool musicEnabled() const { return sh_.musicEnabled.load(std::memory_order_relaxed); }
    uint64_t musicStepsAdvanced() const { return musicStepTotal_; }
    float currentDuck() const { return duck_; }

#if !defined(RACER_NO_QT)
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return (1 << 20) + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char* d, qint64 maxlen) override;
    qint64 writeData(const char*, qint64) override { return -1; }
#endif

private:
    struct Shared {
        std::atomic<float>    rpm{900.0f};
        std::atomic<float>    throttle{0.0f};
        std::atomic<float>    skid{0.0f};
        std::atomic<float>    offroad{0.0f};
        std::atomic<float>    speedFrac{0.0f};
        std::atomic<float>    master{0.8f};
        std::atomic<uint32_t> crashCount{0};
        std::atomic<uint32_t> scrapeCount{0};
        std::atomic<uint32_t> shiftCount{0};
        std::atomic<bool>     musicEnabled{true};
    } sh_;

    // Engine oscillator state
    float engPhase_  = 0.0f;
    float engFreqSm_ = 30.0f;
    float engGainSm_ = 0.16f;
    float intakeLp_  = 0.0f;

    // Noise & FX state
    uint32_t      rng_ = 0xA341316Cu;
    ChamberlinSVF skidBp_;
    float         skidWobblePhase_ = 0.0f;
    float         gravelLp_ = 0.0f;

    // One-shot event state
    bool          crashActive_ = false;
    float         crashT_      = 0.0f;
    ChamberlinSVF crashBp_;

    bool          scrapeActive_ = false;
    float         scrapeT_      = 0.0f;
    ChamberlinSVF scrapeBp_;

    bool          shiftActive_  = false;
    float         shiftT_       = 0.0f;

    uint32_t seenCrash_  = 0;
    uint32_t seenScrape_ = 0;
    uint32_t seenShift_  = 0;
    float    duck_       = 0.0f;

    // Music sequencer state (128 BPM, 4 bars of 16 steps in A minor)
    double        stepAcc_   = 0.0;
    int           musicStep_ = 0;
    uint64_t      musicStepTotal_ = 0;
    float         bassPhase_ = 0.0f;
    float         arpPhase_  = 0.0f;
    float         leadPhase_ = 0.0f;
    float         leadEnv_   = 0.0f;
    float         leadFreq_  = 0.0f;
    float         kickPhase_ = 0.0f;
    float         kickT_     = 1.0f;
    float         clapEnv_   = 0.0f;
    ChamberlinSVF clapBp_;
    float         hatEnv_    = 0.0f;
    float         hatPrev_   = 0.0f;

    float midiFreqLut_[128];
    alignas(64) float engineTable_[2048 + 1];

    void advanceMusicStep();
    float sampleMusic();
};

#if !defined(RACER_NO_QT)
class AudioEngine : public QObject {
    Q_OBJECT
public:
    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    bool init();
    void start();
    void stop();
    void setMuted(bool muted);
    bool isMuted() const { return muted_; }
    void setMusicEnabled(bool en) { dev_.setMusicEnabled(en); }
    bool isMusicEnabled() const { return dev_.musicEnabled(); }
    void push(const AudioParams& p) { dev_.setParams(p); }
    SynthDevice& device() { return dev_; }

private:
    SynthDevice dev_;
    QAudioSink* sink_  = nullptr;
    bool        muted_ = false;
};
#endif
