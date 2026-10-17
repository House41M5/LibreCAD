#-------------------------------------------------
# Kai Bridge — ให้ Claude (Kai) สั่งงานแบบที่เปิดอยู่ใน LibreCAD ผ่าน named pipe
# สร้าง 2026-10-16 (Claude-Win)
#-------------------------------------------------

QT       += gui widgets
TEMPLATE = lib
CONFIG += plugin
VERSION = 0.2.4
TARGET = $$qtLibraryTarget(kaibridge)

GENERATED_DIR = ../../generated/plugin/kaibridge
include(../../common.pri)

INCLUDEPATH    += ../../librecad/src/plugins

SOURCES += kaibridge.cpp
HEADERS += kaibridge.h

win32 {
        DESTDIR = ../../windows/resources/plugins
}
