#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QList>
#include <QImage>

#include <QMutex>
#include <QMutexLocker>
#include <QResizeEvent>

#include "touch_shared.h"

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();

    void paintEvent(QPaintEvent *);
    void submitEvent(struct TouchEvent ev);
    void resizeEvent(QResizeEvent *);

private:
    QList<TouchEvent> _events;
    QImage* _image;
    QMutex* _mutex;
    Ui::MainWindow *ui;
};

#endif // MAINWINDOW_H
