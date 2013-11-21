#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QPainter>

#define NUM_TOUCHES 10

static const QImage::Format ImageFormat = QImage::Format_RGB32;

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    _image(new QImage(size(), ImageFormat)),
    _mutex(new QMutex()),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
}

MainWindow::~MainWindow()
{
    delete _image;
    delete _mutex;
    delete ui;
}

void MainWindow::paintEvent(QPaintEvent *)
{
    QMutexLocker lock(_mutex);
    QPainter painter(this);
    int dx = -(this->pos().x());
    int dy = -(this->pos().y());
    int w = _image->width();
    int h = _image->height();
    uchar* bits = _image->bits();
    size_t bpl = _image->bytesPerLine();
    foreach(TouchEvent ev, _events) {
        int x = (ev.x + dx) % w;
        int y = ((ev.y + dy) % h);
        int color = 0xff * !!(ev.idx & 1)
                + 0xff00 * !!(ev.idx & 2)
                + 0xff0000 * !!(ev.idx & 4);

        uchar *line = bits + bpl * y;
        ((unsigned*)line)[x] = color;
    }

    painter.drawImage(0, 0, *_image);
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);

    QMutexLocker lock(_mutex);
    delete _image;
    _image = new QImage(event->size(), ImageFormat);
    _image->fill(Qt::gray);
}

void MainWindow::submitEvent(struct TouchEvent ev) {
    static int last_x[NUM_TOUCHES] = {

    };
    static int last_y[NUM_TOUCHES] = {

    };

    if (ev.x > 0) {
        if (last_y[ev.idx] > 0) {
            ev.y = last_y[ev.idx];
            _events.append(ev);
            last_y[ev.idx] = 0;
        }
        else {
            last_x[ev.idx] = ev.x;
        }
    }
    if (ev.y > 0) {
        if (last_x[ev.idx] > 0) {
            ev.x = last_x[ev.idx];
            _events.append(ev);
            last_y[ev.idx] = 0;
        }
        else {
            last_y[ev.idx] = ev.y;
        }
    }
    repaint();
}
