# lctest - round-trip test plugin: resaves the drawing through LibreCAD's own DXF writer when LC_TEST_RESAVE is set
# (~/dxf-tools/rtsuite). Not installed for the user. See lctest.cpp.
QT       += gui widgets
TEMPLATE = lib
CONFIG += plugin
VERSION = 0.1.0
TARGET = $$qtLibraryTarget(lctest)

GENERATED_DIR = ../../generated/plugin/lctest
# Use common project definitions.
include(../../common.pri)

# For plugins
INCLUDEPATH    += ../../librecad/src/plugins
win32-msvc: QMAKE_CXXFLAGS += /utf-8

SOURCES += lctest.cpp

HEADERS += lctest.h

# Installation Directory
win32 {
        DESTDIR = ../../windows/resources/plugins
}
unix {
    macx {
        DESTDIR = ../../LibreCAD.app/Contents/Resources/plugins
    }
    else {
        DESTDIR = ../../unix/resources/plugins
    }
}
