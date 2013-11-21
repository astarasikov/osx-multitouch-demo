#include "mainwindow.h"
#include <QApplication>

#include "touch_shared.h"

static MainWindow *g_Window = 0;

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    MainWindow w;
    g_Window = &w;
    w.show();
    startTouchLoop();

    return a.exec();
}

extern "C" {
    void submitTouch(struct TouchEvent ev) {
        if (g_Window) {
            g_Window->submitEvent(ev);
        }
    }
}
