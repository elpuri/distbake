QT += core gui svg

TARGET = distbake
CONFIG += console c++11
CONFIG -= app_bundle
TEMPLATE = app

SOURCES += main.cpp

DISTFILES += \
    tester.qml

