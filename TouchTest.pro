#-------------------------------------------------
#
# Project created by QtCreator 2013-11-22T01:19:26
#
#-------------------------------------------------

QT       += core gui

QMAKE_LFLAGS += -F /System/Library/Frameworks -F /System/Library/Frameworks/IOKit.framework -F /System/Library/Frameworks/CoreFoundation.framework
LIBS += -framework IOKit -framework CoreFoundation

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = TouchTest
TEMPLATE = app


SOURCES += main.cpp\
        mainwindow.cpp \
    touch_osx.cpp

HEADERS  += mainwindow.h \
    touch_shared.h

FORMS    += mainwindow.ui
