#include "gamewidget.h"

#if !defined(RACER_NO_QT)
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QFocusEvent>
#include <cmath>
#include <algorithm>

namespace {
inline constexpr qint64 STEP_NS    = 16'666'667LL; // 60.000 Hz in nanoseconds
inline constexpr qint64 MAX_ACC_NS = 100'000'000LL; // 100 ms spiral-of-death guard
} // namespace

GameWidget::GameWidget(QWidget* parent)
    : QWidget(parent),
      view_(reinterpret_cast<uchar*>(game_.displayFb.px),
            DisplayBuffer::W, DisplayBuffer::H,
            DisplayBuffer::STRIDE * 4,
            QImage::Format_RGB32) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(640, 480);
    setWindowTitle("Argon: Overdrive — Modern First-Person Software-Rasterized 3D Racer (Qt 6)");
    resize(1152, 768);

    game_.init();
    game_.enterLaunchMenu();
    audio_.init();
    updateTargetRect();

    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &GameWidget::onFrame);

    clock_.start();
    deadlineNs_ = lastNs_ = prevFrameStartNs_ = clock_.nsecsElapsed();
    timer_.start(0);
}

GameWidget::~GameWidget() {
    timer_.stop();
    audio_.stop();
    game_.shutdown();
}

void GameWidget::toggleFullscreenMode() {
    if (isFullScreen()) {
        showNormal();
        game_.isFullscreen = false;
    } else {
        showFullScreen();
        game_.isFullscreen = true;
    }
    updateTargetRect();
}

void GameWidget::updateTargetRect() {
    int w = std::max(1, width());
    int h = std::max(1, height());
    game_.isFullscreen = isFullScreen();

    if (game_.scaleMode == ScaleMode::FullStretch) {
        target_ = QRect(0, 0, w, h);
    } else if (game_.scaleMode == ScaleMode::AspectFit) {
        double sx = static_cast<double>(w) / static_cast<double>(Framebuffer::W);
        double sy = static_cast<double>(h) / static_cast<double>(Framebuffer::H);
        double s  = std::min(sx, sy);
        int tw = std::max(1, static_cast<int>(std::lround(Framebuffer::W * s)));
        int th = std::max(1, static_cast<int>(std::lround(Framebuffer::H * s)));
        target_ = QRect((w - tw) / 2, (h - th) / 2, tw, th);
    } else {
        double sx = static_cast<double>(w) / static_cast<double>(Framebuffer::W);
        double sy = static_cast<double>(h) / static_cast<double>(Framebuffer::H);
        double s  = std::min(sx, sy);
        if (s >= 1.0) s = std::floor(s);
        int tw = std::max(1, static_cast<int>(std::lround(Framebuffer::W * s)));
        int th = std::max(1, static_cast<int>(std::lround(Framebuffer::H * s)));
        target_ = QRect((w - tw) / 2, (h - th) / 2, tw, th);
    }
    barsDirty_ = true;
}

void GameWidget::mapMouseToDisplay(int wx, int wy, int& outX960, int& outY720) const {
    if (target_.width() <= 0 || target_.height() <= 0) {
        outX960 = 0;
        outY720 = 0;
        return;
    }
    outX960 = ((wx - target_.left()) * DisplayBuffer::W) / target_.width();
    outY720 = ((wy - target_.top())  * DisplayBuffer::H) / target_.height();
}

void GameWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateTargetRect();
}

void GameWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    if (barsDirty_ || target_ != rect()) {
        p.fillRect(rect(), Qt::black);
        barsDirty_ = false;
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(target_, view_);
}

void GameWidget::onFrame() {
    qint64 now = clock_.nsecsElapsed();
    accNs_ += (now - lastNs_);
    lastNs_ = now;
    if (accNs_ > MAX_ACC_NS) {
        accNs_ = MAX_ACC_NS;
    }

    ScaleMode prevScale = game_.scaleMode;
    while (accNs_ >= STEP_NS) {
        game_.tick(input_);
        audio_.push(game_.vehicle.audio);
        input_.pressed = 0;
        accNs_ -= STEP_NS;
    }
    if (game_.scaleMode != prevScale) {
        updateTargetRect();
    }

    game_.render();
    update();

    float frameMs = static_cast<float>(now - prevFrameStartNs_) / 1'000'000.0f;
    prevFrameStartNs_ = now;
    stats_.push(frameMs);

    deadlineNs_ += STEP_NS;
    qint64 afterRender = clock_.nsecsElapsed();
    if (deadlineNs_ < afterRender) {
        deadlineNs_ = afterRender;
    }
    int waitMs = static_cast<int>(std::max<qint64>(0, (deadlineNs_ - afterRender) / 1'000'000LL));
    timer_.start(waitMs);
}

uint8_t GameWidget::mapKeyToBit(int key) {
    switch (key) {
        case Qt::Key_Up:
        case Qt::Key_W:     return InputState::ACCEL;
        case Qt::Key_Down:
        case Qt::Key_S:     return InputState::BRAKE;
        case Qt::Key_Left:
        case Qt::Key_A:     return InputState::LEFT;
        case Qt::Key_Right:
        case Qt::Key_D:     return InputState::RIGHT;
        case Qt::Key_R:     return InputState::RESET;
        case Qt::Key_P:     return InputState::PAUSE;
        case Qt::Key_Space:
        case Qt::Key_Shift: return InputState::NITRO;
        case Qt::Key_X:     return InputState::WIPER;
        default:            return 0;
    }
}

void GameWidget::keyPressEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;

    // Global hotkeys available in any screen state
    switch (e->key()) {
        case Qt::Key_F:
        case Qt::Key_F11:
            toggleFullscreenMode();
            return;
        case Qt::Key_L:
            game_.cycleScaleMode();
            updateTargetRect();
            return;
        case Qt::Key_G:
            game_.toggleSteerAssist();
            return;
        case Qt::Key_T:
            game_.selectTrafficDensity(game_.selectedDensity + 1);
            return;
        case Qt::Key_1: game_.setPixelSizePreset(0); return;
        case Qt::Key_2: game_.setPixelSizePreset(1); return;
        case Qt::Key_3: game_.setPixelSizePreset(2); return;
        case Qt::Key_4: game_.setPixelSizePreset(3); return;
        case Qt::Key_5: game_.setPixelSizePreset(4); return;
        case Qt::Key_BracketLeft:  game_.cyclePixelSize(-1); return;
        case Qt::Key_BracketRight: game_.cyclePixelSize(+1); return;
        case Qt::Key_M:
            audio_.setMuted(!audio_.isMuted());
            return;
        case Qt::Key_N:
            audio_.setMusicEnabled(!audio_.isMusicEnabled());
            return;
        case Qt::Key_F3:
            game_.toggleDebugHud();
            return;
        default:
            break;
    }

    // Wrecked state (Car Health == 0%): [R] or [H] repairs on the spot, [Enter] restarts from grid
    if (game_.screenState == ScreenState::Wrecked) {
        switch (e->key()) {
            case Qt::Key_R:
            case Qt::Key_H:
            case Qt::Key_Space:
                game_.repairAndContinue();
                return;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                game_.startNewRace();
                return;
            case Qt::Key_Escape:
                game_.enterLaunchMenu();
                return;
            default:
                return;
        }
    }

    // Menu navigation when in LaunchMenu, Paused, or RaceOver
    if (game_.screenState == ScreenState::LaunchMenu ||
        game_.screenState == ScreenState::Paused ||
        game_.screenState == ScreenState::RaceOver) {
        bool doFs = false, doQuit = false;
        switch (e->key()) {
            case Qt::Key_Up:
            case Qt::Key_W:
                game_.menuMove(-1);
                return;
            case Qt::Key_Down:
            case Qt::Key_S:
                game_.menuMove(+1);
                return;
            case Qt::Key_Left:
            case Qt::Key_A:
                game_.menuAdjust(-1, doFs, doQuit);
                updateTargetRect();
                if (doFs) toggleFullscreenMode();
                if (doQuit) close();
                return;
            case Qt::Key_Right:
            case Qt::Key_D:
                game_.menuAdjust(+1, doFs, doQuit);
                updateTargetRect();
                if (doFs) toggleFullscreenMode();
                if (doQuit) close();
                return;
            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Space:
                game_.menuActivate(doFs, doQuit);
                updateTargetRect();
                if (doFs) toggleFullscreenMode();
                if (doQuit) close();
                return;
            case Qt::Key_R:
                if (game_.screenState == ScreenState::RaceOver) {
                    game_.startNewRace();
                    return;
                }
                break;
            case Qt::Key_P:
                if (game_.screenState == ScreenState::Paused) {
                    game_.togglePause();
                    return;
                }
                break;
            case Qt::Key_Escape:
                if (game_.screenState == ScreenState::Paused || game_.screenState == ScreenState::RaceOver) {
                    game_.enterLaunchMenu();
                } else {
                    close();
                }
                return;
            default:
                break;
        }
        return;
    }

    // In-race hotkeys
    switch (e->key()) {
        case Qt::Key_H:
            game_.fieldRepair();
            return;
        case Qt::Key_E:
            game_.toggle3DPipeline();
            return;
        case Qt::Key_Q:
            game_.toggleGapGuide();
            return;
        case Qt::Key_B:
            game_.cycleTheme();
            return;
        case Qt::Key_V:
            game_.toggleMirror();
            return;
        case Qt::Key_C:
            game_.toggleWeather();
            return;
        case Qt::Key_Tab:
            game_.toggleMinimap();
            return;
        case Qt::Key_Escape:
            game_.togglePause();
            return;
        default:
            break;
    }

    uint8_t bit = mapKeyToBit(e->key());
    if (bit != 0) {
        if ((input_.down & bit) == 0) {
            input_.pressed |= bit;
        }
        input_.down |= bit;
    }
}

void GameWidget::keyReleaseEvent(QKeyEvent* e) {
    if (e->isAutoRepeat()) return;
    uint8_t bit = mapKeyToBit(e->key());
    if (bit != 0) {
        input_.down &= static_cast<uint8_t>(~bit);
    }
}

void GameWidget::mouseMoveEvent(QMouseEvent* e) {
    int nx = 0, ny = 0;
    mapMouseToDisplay(e->position().x(), e->position().y(), nx, ny);
    game_.handleMouseMove(nx, ny);
}

void GameWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    int nx = 0, ny = 0;
    mapMouseToDisplay(e->position().x(), e->position().y(), nx, ny);
    bool doFs = false, doQuit = false;
    game_.handleMouseClick(nx, ny, doFs, doQuit);
    updateTargetRect();
    if (doFs) toggleFullscreenMode();
    if (doQuit) close();
}

void GameWidget::focusOutEvent(QFocusEvent* e) {
    QWidget::focusOutEvent(e);
    input_.down = 0;
}
#endif
