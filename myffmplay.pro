TEMPLATE = app
CONFIG += console c++11
CONFIG -= app_bundle
CONFIG -= qt

TARGET = myffmplay_cpy

#CONFIG += c++11

QMAKE_CXXFLAGS += -Werror=return-type

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
    main.c \
    cmdutils.c

HEADERS += \
    cmdutils.h \
    cmdutils_common_opts.h
