#include "tests.h"
#include <cstring>

#if !defined(RACER_NO_QT)
#include <QApplication>
#include "gamewidget.h"
#endif

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--test") == 0 || std::strcmp(argv[i], "-t") == 0) {
            return runAllUnitTests();
        }
    }

#if !defined(RACER_NO_QT)
    QApplication app(argc, argv);
    static GameWidget widget;
    widget.show();
    return app.exec();
#else
    return runAllUnitTests();
#endif
}
