#include "audio.h"
#include "mathutil.h"
#include <cmath>
#include <cstring>
#include <algorithm>

#if !defined(RACER_NO_QT)
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QAudioDevice>
#endif

static_assert(std::atomic<float>::is_always_lock_free, "atomic<float> must be lock-free");

namespace {

inline float whiteNoise(uint32_t& rng) {
    return (static_cast<float>(mathutil::xorshift32(rng) & 0x00FFFFFFu) / static_cast<float>(0x00800000u)) - 1.0f;
}

inline float softClip(float x) {
    float u = mathutil::clamp(x, -3.0f, 3.0f);
    float u2 = u * u;
    return (u * (27.0f + u2)) / (27.0f + 9.0f * u2);
}

// Lead melody (MIDI note per 16th step, 0 = rest) from Section 7.6
const uint8_t LEAD_MELODY[64] = {
    // bar 1 (Am)
    76, 0, 0, 72,  0, 74, 0, 76,  0, 0, 79, 0,  76, 0, 0, 0,
    // bar 2 (F)
    77, 0, 0, 74,  0, 72, 0, 74,  0, 0, 77, 0,  81, 0, 0, 0,
    // bar 3 (C)
    79, 0, 0, 76,  0, 72, 0, 76,  0, 0, 79, 0,  84, 0, 0, 0,
    // bar 4 (G)
    83, 0, 0, 79,  0, 74, 0, 79,  0, 0, 83, 0,  79, 74, 71, 0
};

// Chord roots: Am (45), F (41), C (48), G (43)
const int CHORD_ROOTS[4] = { 45, 41, 48, 43 };
// Chord third offset (minor=3 for Am, major=4 for F, C, G)
const int CHORD_THIRDS[4] = { 3, 4, 4, 4 };

} // namespace

float ChamberlinSVF::processBandPass(float in, float fc, float sr, float qInv) {
    fc = mathutil::clamp(fc, 40.0f, 0.42f * sr);
    float F = 2.0f * std::sin(mathutil::PI * fc / sr);
    low  += F * band;
    high  = in - low - qInv * band;
    band += F * high;
    // Guard against numerical blow-up
    low  = mathutil::clamp(low,  -4.0f, 4.0f);
    band = mathutil::clamp(band, -4.0f, 4.0f);
    return band;
}

SynthDevice::SynthDevice() {
    for (int m = 0; m < 128; ++m) {
        midiFreqLut_[m] = 440.0f * std::pow(2.0f, static_cast<float>(m - 69) / 12.0f);
    }

    // Build 12-harmonic band-limited engine table (Section 7.4)
    float maxAbs = 0.0f;
    for (int i = 0; i < 2048; ++i) {
        float sum = 0.0f;
        for (int k = 1; k <= 12; ++k) {
            float fk   = static_cast<float>(k);
            float amp  = 1.0f / std::pow(fk, 0.9f);
            float phiK = 0.7f * fk * fk;
            float arg  = mathutil::TWO_PI * fk * (static_cast<float>(i) / 2048.0f) + phiK;
            sum += amp * std::sin(arg);
        }
        engineTable_[i] = sum;
        maxAbs = std::max(maxAbs, std::fabs(sum));
    }
    float invN = (maxAbs > 0.0f) ? (1.0f / maxAbs) : 1.0f;
    for (int i = 0; i < 2048; ++i) {
        engineTable_[i] *= invN;
    }
    engineTable_[2048] = engineTable_[0]; // guard sample for linear interpolation

    resetState();
}

SynthDevice::~SynthDevice() = default;

void SynthDevice::resetState() {
    engPhase_  = 0.0f;
    engFreqSm_ = sh_.rpm.load(std::memory_order_relaxed) / 30.0f;
    engGainSm_ = 0.16f;
    intakeLp_  = 0.0f;
    rng_       = 0xA341316Cu;
    skidBp_.reset();
    skidWobblePhase_ = 0.0f;
    gravelLp_ = 0.0f;
    crashActive_  = false;
    crashT_       = 0.0f;
    crashBp_.reset();
    scrapeActive_ = false;
    scrapeT_      = 0.0f;
    scrapeBp_.reset();
    shiftActive_  = false;
    shiftT_       = 0.0f;
    seenCrash_  = sh_.crashCount.load(std::memory_order_relaxed);
    seenScrape_ = sh_.scrapeCount.load(std::memory_order_relaxed);
    seenShift_  = sh_.shiftCount.load(std::memory_order_relaxed);
    duck_ = 0.0f;

    stepAcc_        = 0.0;
    musicStep_      = 0;
    musicStepTotal_ = 0;
    bassPhase_ = arpPhase_ = leadPhase_ = 0.0f;
    leadEnv_   = 0.0f;
    leadFreq_  = midiFreqLut_[76];
    kickPhase_ = 0.0f;
    kickT_     = 1.0f;
    clapEnv_   = 0.0f;
    clapBp_.reset();
    hatEnv_    = 0.0f;
    hatPrev_   = 0.0f;
    advanceMusicStep();
    musicStepTotal_ = 0; // start step counter at 0 before rendering frames
}

void SynthDevice::setParams(const AudioParams& p) {
    sh_.rpm.store(p.rpm, std::memory_order_relaxed);
    sh_.throttle.store(p.throttle, std::memory_order_relaxed);
    sh_.skid.store(p.skid, std::memory_order_relaxed);
    sh_.offroad.store(p.offroad, std::memory_order_relaxed);
    sh_.speedFrac.store(p.speedFrac, std::memory_order_relaxed);
    sh_.crashCount.store(p.crashCount, std::memory_order_relaxed);
    sh_.scrapeCount.store(p.scrapeCount, std::memory_order_relaxed);
    sh_.shiftCount.store(p.shiftCount, std::memory_order_relaxed);
}

void SynthDevice::setMasterGain(float g) {
    sh_.master.store(mathutil::clamp(g, 0.0f, 1.0f), std::memory_order_relaxed);
}

void SynthDevice::setMusicEnabled(bool en) {
    sh_.musicEnabled.store(en, std::memory_order_relaxed);
}

void SynthDevice::advanceMusicStep() {
    int s = musicStep_ & 63;
    int stepInBar = s & 15;

    uint8_t leadMidi = LEAD_MELODY[s];
    if (leadMidi > 0 && leadMidi < 128) {
        leadFreq_ = midiFreqLut_[leadMidi];
        leadEnv_  = 1.0f;
    }
    if (stepInBar == 0 || stepInBar == 4 || stepInBar == 8 || stepInBar == 12) {
        kickT_     = 0.0f;
        kickPhase_ = 0.0f;
    }
    if (stepInBar == 4 || stepInBar == 12) {
        clapEnv_ = 1.0f;
    }
    if (stepInBar == 2 || stepInBar == 6 || stepInBar == 10 || stepInBar == 14) {
        hatEnv_ = 1.0f;
    }

    musicStep_ = (musicStep_ + 1) & 63;
    musicStepTotal_++;
}

float SynthDevice::sampleMusic() {
    // 128 BPM, 16th-note step length = (60 / 128 / 4) * 44100 = 5167.96875 samples
    constexpr double STEP_LEN = (60.0 / 128.0 / 4.0) * static_cast<double>(SR);
    constexpr float  INV_SR   = 1.0f / static_cast<float>(SR);

    stepAcc_ += 1.0;
    if (stepAcc_ >= STEP_LEN) {
        stepAcc_ -= STEP_LEN;
        advanceMusicStep();
    }

    int curStep   = (musicStep_ + 63) & 63;
    int bar       = (curStep >> 4) & 3;
    int stepInBar = curStep & 15;
    int rootMidi  = CHORD_ROOTS[bar];

    // 1. Bass (triangle wave)
    float bassOut = 0.0f;
    if ((stepInBar & 1) == 0 || stepInBar == 15) {
        int note = rootMidi;
        if (stepInBar == 4 || stepInBar == 12) note += 12;
        else if (stepInBar == 15) note += 7;
        float bf = midiFreqLut_[note & 127];
        bassPhase_ += bf * INV_SR;
        bassPhase_ -= std::floor(bassPhase_);
        float tri = 4.0f * std::fabs(bassPhase_ - 0.5f) - 1.0f;
        bassOut = 0.22f * tri;
    }

    // 2. Arpeggio (pulse 25% duty, chord tones root, third, fifth, third, one octave above bass)
    const int arpOffsets[4] = { 0, CHORD_THIRDS[bar], 7, CHORD_THIRDS[bar] };
    int arpMidi = rootMidi + 12 + arpOffsets[stepInBar & 3];
    float af = midiFreqLut_[arpMidi & 127];
    arpPhase_ += af * INV_SR;
    arpPhase_ -= std::floor(arpPhase_);
    float arpOut = ((arpPhase_ < 0.25f) ? 1.0f : -1.0f) * 0.10f;

    // 3. Lead (pulse 50% duty, 90 ms decay per note, gain 0.12)
    float leadOut = 0.0f;
    if (leadEnv_ > 1e-4f) {
        leadPhase_ += leadFreq_ * INV_SR;
        leadPhase_ -= std::floor(leadPhase_);
        leadOut = ((leadPhase_ < 0.50f) ? 1.0f : -1.0f) * 0.12f * leadEnv_;
        leadEnv_ *= std::exp(-INV_SR / 0.090f);
    }

    // 4. Kick (sine sweep 120 to 45 Hz over 90 ms)
    float kickOut = 0.0f;
    if (kickT_ < 0.18f) {
        float kf = 45.0f + 75.0f * std::exp(-kickT_ / 0.035f);
        kickPhase_ += kf * INV_SR;
        kickPhase_ -= std::floor(kickPhase_);
        float kEnv = std::exp(-kickT_ / 0.090f);
        kickOut = 0.35f * kEnv * std::sin(mathutil::TWO_PI * kickPhase_);
        kickT_ += INV_SR;
    }

    // 5. Clap (band-passed noise at 1.8 kHz, 120 ms decay)
    float clapOut = 0.0f;
    if (clapEnv_ > 1e-4f) {
        float n = whiteNoise(rng_);
        clapOut = 0.20f * clapEnv_ * clapBp_.processBandPass(n, 1800.0f, static_cast<float>(SR));
        clapEnv_ *= std::exp(-INV_SR / 0.120f);
    }

    // 6. Hat (high-passed noise, 25 ms decay)
    float hatOut = 0.0f;
    if (hatEnv_ > 1e-4f) {
        float n = whiteNoise(rng_);
        float hp = n - hatPrev_;
        hatPrev_ = n;
        hatOut = 0.10f * hatEnv_ * hp;
        hatEnv_ *= std::exp(-INV_SR / 0.025f);
    }

    return bassOut + arpOut + leadOut + kickOut + clapOut + hatOut;
}

void SynthDevice::render(int16_t* out, int frames) {
    if (!out || frames <= 0) return;

    const float rpm       = sh_.rpm.load(std::memory_order_relaxed);
    const float throttle  = sh_.throttle.load(std::memory_order_relaxed);
    const float skid      = sh_.skid.load(std::memory_order_relaxed);
    const float offroad   = sh_.offroad.load(std::memory_order_relaxed);
    const float speedFrac = sh_.speedFrac.load(std::memory_order_relaxed);
    const float master    = sh_.master.load(std::memory_order_relaxed);
    const bool  musOn     = sh_.musicEnabled.load(std::memory_order_relaxed);

    const uint32_t curCrash  = sh_.crashCount.load(std::memory_order_relaxed);
    const uint32_t curScrape = sh_.scrapeCount.load(std::memory_order_relaxed);
    const uint32_t curShift  = sh_.shiftCount.load(std::memory_order_relaxed);

    if (curCrash != seenCrash_) {
        seenCrash_   = curCrash;
        crashActive_ = true;
        crashT_      = 0.0f;
        crashBp_.reset();
        duck_        = 1.0f;
    }
    if (curScrape != seenScrape_) {
        seenScrape_   = curScrape;
        scrapeActive_ = true;
        scrapeT_      = 0.0f;
        scrapeBp_.reset();
    }
    if (curShift != seenShift_) {
        seenShift_   = curShift;
        shiftActive_ = true;
        shiftT_      = 0.0f;
    }

    constexpr float INV_SR   = 1.0f / static_cast<float>(SR);
    const float aFreq        = 1.0f - std::exp(-INV_SR / 0.040f);
    const float aGain        = 1.0f - std::exp(-INV_SR / 0.020f);
    const float aIntake      = 1.0f - std::exp(-mathutil::TWO_PI * 2000.0f * INV_SR);
    const float aGravel      = 1.0f - std::exp(-mathutil::TWO_PI * 220.0f  * INV_SR);
    const float duckDecay    = std::exp(-INV_SR / 1.50f);

    const float targetF0  = std::max(10.0f, rpm / 30.0f);
    const float rpmFrac   = mathutil::clamp((rpm - 900.0f) / 7600.0f, 0.0f, 1.0f);
    const float targetEngG = 0.16f + 0.20f * rpmFrac + 0.12f * throttle;
    const float skidGain   = 0.30f * skid;
    const float skidFc     = 1500.0f + 700.0f * skid;
    const float gravGain   = 0.35f * offroad * speedFrac;

    for (int i = 0; i < frames; ++i) {
        // 1. Engine voice (12-harmonic wavetable with linear interpolation + intake noise)
        engFreqSm_ += aFreq * (targetF0   - engFreqSm_);
        engGainSm_ += aGain * (targetEngG - engGainSm_);

        engPhase_ += engFreqSm_ * INV_SR;
        engPhase_ -= std::floor(engPhase_);

        float idxF = engPhase_ * 2048.0f;
        int   idx0 = static_cast<int>(idxF) & 2047;
        float frac = idxF - std::floor(idxF);
        float wave = mathutil::lerp(engineTable_[idx0], engineTable_[idx0 + 1], frac);

        float wn = whiteNoise(rng_);
        intakeLp_ += aIntake * (wn - intakeLp_);
        float E = wave + (0.05f * throttle) * intakeLp_;

        // 2. Skid screech voice
        float S = 0.0f;
        if (skidGain > 1e-4f) {
            skidWobblePhase_ += 7.0f * INV_SR;
            skidWobblePhase_ -= std::floor(skidWobblePhase_);
            float wobble = 1.0f + 0.15f * std::sin(mathutil::TWO_PI * skidWobblePhase_);
            S = skidBp_.processBandPass(wn, skidFc, static_cast<float>(SR)) * wobble;
        }

        // 3. Gravel rumble voice
        gravelLp_ += aGravel * (wn - gravelLp_);
        float G = gravelLp_;

        // 4. Crash voice (noise + thump + 3-partial clang)
        float C = 0.0f;
        if (crashActive_) {
            float t = crashT_;
            if (t < 1.5f) {
                float fc = 3000.0f * std::exp(-t / 0.35f) + 400.0f;
                float cNoise = std::exp(-t / 0.22f) * crashBp_.processBandPass(wn, fc, static_cast<float>(SR)) * 0.9f;
                float cThump = std::exp(-t / 0.15f) * std::sin(mathutil::TWO_PI * (35.0f + 40.0f * std::exp(-t / 0.08f)) * t) * 0.8f;
                float cDecay = 0.12f * std::exp(-t / 0.25f);
                float cClang = cDecay * (std::sin(mathutil::TWO_PI * 523.0f  * t)
                                       + std::sin(mathutil::TWO_PI * 1130.0f * t)
                                       + std::sin(mathutil::TWO_PI * 1790.0f * t));
                C = cNoise + cThump + cClang;
                crashT_ += INV_SR;
            } else {
                crashActive_ = false;
            }
        }

        // 5. Scrape & shift voices
        float P = 0.0f;
        if (scrapeActive_) {
            if (scrapeT_ < 0.150f) {
                float env = 1.0f - (scrapeT_ / 0.150f);
                P += 0.30f * env * scrapeBp_.processBandPass(wn, 2500.0f, static_cast<float>(SR));
                scrapeT_ += INV_SR;
            } else {
                scrapeActive_ = false;
            }
        }
        if (shiftActive_) {
            if (shiftT_ < 0.015f) {
                P += 0.12f * wn;
                shiftT_ += INV_SR;
            } else {
                shiftActive_ = false;
            }
        }

        // 6. Chiptune music sequencer
        float M = musOn ? sampleMusic() : 0.0f;

        // 7. Ducking & Master Mix
        float gEng = engGainSm_ * (1.0f - 0.70f * duck_);
        float gMus = (musOn ? 0.30f : 0.0f) * (1.0f - 0.40f * duck_);
        duck_ *= duckDecay;

        float mix = gEng * E + skidGain * S + gravGain * G + C + P + gMus * M;
        float clipped = softClip(master * mix);
        int32_t sample = static_cast<int32_t>(std::lround(32767.0f * clipped));
        out[i] = static_cast<int16_t>(mathutil::clamp(sample, -32767, 32767));
    }
}

#if !defined(RACER_NO_QT)
qint64 SynthDevice::readData(char* d, qint64 maxlen) {
    int frames = static_cast<int>(maxlen / sizeof(int16_t));
    if (frames <= 0) return 0;
    render(reinterpret_cast<int16_t*>(d), frames);
    return static_cast<qint64>(frames * sizeof(int16_t));
}

AudioEngine::AudioEngine(QObject* parent) : QObject(parent) {}

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::init() {
    dev_.open(QIODevice::ReadOnly);
    QAudioFormat fmt;
    fmt.setSampleRate(SynthDevice::SR);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice outDev = QMediaDevices::defaultAudioOutput();
    if (outDev.isNull()) {
        return false;
    }
    sink_ = new QAudioSink(outDev, fmt, this);
    sink_->setBufferSize(4096);
    start();
    return sink_->error() == QAudio::NoError;
}

void AudioEngine::start() {
    if (sink_) {
        sink_->start(&dev_);
    }
}

void AudioEngine::stop() {
    if (sink_) {
        sink_->stop();
    }
}

void AudioEngine::setMuted(bool muted) {
    muted_ = muted;
    dev_.setMasterGain(muted_ ? 0.0f : 0.8f);
}
#endif
