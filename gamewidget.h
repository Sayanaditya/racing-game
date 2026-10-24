#pragma once
#include "config.h"
#include "framebuffer.h"
#include "input.h"
#include "game.h"
#include "audio.h"

#if !defined(RACER_NO_QT)
#include <QWidget>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>
#include <QRect>

class GameWidget : public QWidget {
    Q_OBJECT
public:
    explicit GameWidget(QWidget* parent = nullptr);
    ~GameWidget() override;

    Game& game() { return game_; }
    void toggleFullscreenMode();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private slots:
    void onFrame();

private:
    static uint8_t mapKeyToBit(int key);
    void updateTargetRect();
    void mapMouseToDisplay(int wx, int wy, int& outX960, int& outY720) const;

    alignas(64) Game game_;
    AudioEngine      audio_;
    QImage           view_;        // wraps game_.displayFb.px (960x720), zero copy
    QTimer           timer_;       // single-shot, re-armed every frame
    QElapsedTimer    clock_;
    qint64           accNs_      = 0;
    qint64           lastNs_     = 0;
    qint64           deadlineNs_ = 0;
    qint64           prevFrameStartNs_ = 0;
    InputState       input_;
    QRect            target_;      // destination rect (fills full window/fullscreen by default)
    FrameStats       stats_;       // ring buffer, zero allocation
    bool             barsDirty_  = true;
};
#endif
