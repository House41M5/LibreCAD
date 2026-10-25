# "Open DWG..." for LibreCAD 2.2.1.6 - converts through LibreDWG (rake dwg in ~/dxf-tools). See dwgopen.cpp.
QT       += gui widgets
TEMPLATE = lib
CONFIG += plugin
VERSION = 0.1.0
TARGET = $$qtLibraryTarget(dwgopen)

GENERATED_DIR = ../../generated/plugin/dwgopen
# Use common project definitions.
include(../../common.pri)

# For plugins
INCLUDEPATH    += ../../librecad/src/plugins
win32-msvc: QMAKE_CXXFLAGS += /utf-8

SOURCES += dwgopen.cpp

HEADERS += dwgopen.h

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
