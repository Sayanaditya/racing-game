#pragma once
#include <cstdint>
#include "track.h"

struct TrackRecord {
    uint32_t highScore     = 10000;
    float    bestLapSec    = 0.0f;   // 0.0 means no lap record yet
    float    maxSpeedKmh   = 200.0f;
    int      mostOvertakes = 5;
};

class HighScoreManager {
public:
    void init(const char* filePath = "highscores.dat");
    void resetDefaults();
    void load();
    void save() const;

    const TrackRecord& record(int trackIdx) const;
    bool submitResult(int trackIdx, uint32_t score, float bestLapSec, float maxSpeedKmh, int overtakes,
                      bool& outNewHighScore, bool& outNewBestLap);

private:
    char        filePath_[256] = "highscores.dat";
    TrackRecord records_[NUM_TRACK_PRESETS];
};
