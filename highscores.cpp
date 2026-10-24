#include "highscores.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

void HighScoreManager::init(const char* filePath) {
    if (filePath && filePath[0] != '\0') {
        std::snprintf(filePath_, sizeof(filePath_), "%s", filePath);
    }
    resetDefaults();
    load();
}

void HighScoreManager::resetDefaults() {
    for (int i = 0; i < NUM_TRACK_PRESETS; ++i) {
        records_[i].highScore     = 12000u + static_cast<uint32_t>(i) * 3000u;
        records_[i].bestLapSec    = 0.0f;
        records_[i].maxSpeedKmh   = 240.0f;
        records_[i].mostOvertakes = 8;
    }
}

void HighScoreManager::load() {
    FILE* f = std::fopen(filePath_, "r");
    if (!f) return;
    for (int i = 0; i < NUM_TRACK_PRESETS; ++i) {
        unsigned int hs = 0;
        float lap = 0.0f, spd = 0.0f;
        int ov = 0;
        if (std::fscanf(f, "%u %f %f %d", &hs, &lap, &spd, &ov) == 4) {
            records_[i].highScore     = hs;
            records_[i].bestLapSec    = lap;
            records_[i].maxSpeedKmh   = spd;
            records_[i].mostOvertakes = ov;
        }
    }
    std::fclose(f);
}

void HighScoreManager::save() const {
    FILE* f = std::fopen(filePath_, "w");
    if (!f) return;
    for (int i = 0; i < NUM_TRACK_PRESETS; ++i) {
        std::fprintf(f, "%u %.3f %.2f %d\n",
                     records_[i].highScore,
                     records_[i].bestLapSec,
                     records_[i].maxSpeedKmh,
                     records_[i].mostOvertakes);
    }
    std::fclose(f);
}

const TrackRecord& HighScoreManager::record(int trackIdx) const {
    int idx = ((trackIdx % NUM_TRACK_PRESETS) + NUM_TRACK_PRESETS) % NUM_TRACK_PRESETS;
    return records_[idx];
}

bool HighScoreManager::submitResult(int trackIdx, uint32_t score, float bestLapSec, float maxSpeedKmh, int overtakes,
                                    bool& outNewHighScore, bool& outNewBestLap) {
    int idx = ((trackIdx % NUM_TRACK_PRESETS) + NUM_TRACK_PRESETS) % NUM_TRACK_PRESETS;
    TrackRecord& rec = records_[idx];
    outNewHighScore = false;
    outNewBestLap   = false;
    bool changed    = false;

    if (score > rec.highScore) {
        rec.highScore   = score;
        outNewHighScore = true;
        changed         = true;
    }
    if (bestLapSec > 5.0f && (rec.bestLapSec <= 0.0f || bestLapSec < rec.bestLapSec)) {
        rec.bestLapSec = bestLapSec;
        outNewBestLap  = true;
        changed        = true;
    }
    if (maxSpeedKmh > rec.maxSpeedKmh) {
        rec.maxSpeedKmh = maxSpeedKmh;
        changed         = true;
    }
    if (overtakes > rec.mostOvertakes) {
        rec.mostOvertakes = overtakes;
        changed           = true;
    }
    if (changed) {
        save();
    }
    return changed;
}
