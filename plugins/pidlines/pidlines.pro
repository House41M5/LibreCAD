#-------------------------------------------------
# pidlines - P&ID signal line decorator (Kai build)
#-------------------------------------------------

QT       += gui widgets
TEMPLATE = lib
CONFIG += plugin
VERSION = 0.1.0
TARGET = $$qtLibraryTarget(pidlines)

GENERATED_DIR = ../../generated/plugin/pidlines
# Use common project definitions.
include(../../common.pri)

# For plugins
INCLUDEPATH    += ../../librecad/src/plugins

# Thai UI strings in the source
win32-msvc: QMAKE_CXXFLAGS += /utf-8

SOURCES += pidlines.cpp

HEADERS += pidlines.h pidgeom.h

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
