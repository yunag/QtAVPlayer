TARGET = tst_qavdemuxer

QT += multimedia-private testlib QtAVPlayer-private

INCLUDEPATH += .
CONFIG += testcase console
RESOURCES += files.qrc

SOURCES += \
    tst_qavdemuxer.cpp

