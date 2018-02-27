TEMPLATE = app

QT       += core concurrent gui

greaterThan(QT_MAJOR_VERSION, 4): {
    QT += widgets
    CONFIG += c++11
}

TARGET = myffmplay

QMAKE_CXXFLAGS += -Werror=return-type -Wenum-compare

unix:LIBS += -Wl,--start-group \
    -lswresample \
    -lavdevice \
    -lavformat \
    -lavcodec \
    -lavutil \
    -lswscale \
    -lavfilter \
    -Wl,--end-group

unix:LIBS += -lvdpau -lva -lX11 -lva-drm -lva-x11 -lSDL2

unix:LIBS += -ldl -pthread -lm

unix:LIBS += -lboost_system -lboost_thread -lboost_filesystem -lboost_regex -lboost_chrono -lboost_date_time -lboost_atomic

unix:LIBS += -lglog

SOURCES += \
#    main.c \
    cmdutils.c \
    main.cpp \
    imageshow.cpp

HEADERS += \
    cmdutils.h \
    imageshow.h
    cmdutils_common_opts.h
