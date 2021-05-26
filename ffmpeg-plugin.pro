
###################################################################################
# Support Library Path.
#
# - This is where we find our supporting ffmpeg library / includes
# - And also maybe SDL2.dll or e.g. libSDL2.so
###################################################################################

win32: MYLIBDIR = c:/devel/libraries

win32: INCLUDEPATH += $$MYLIBDIR/win64/include/ffmpeg $$MYLIBDIR/win64/include
CONFIG(debug, debug|release) {
    win32: LIBS += -L$$MYLIBDIR/win64/libd
} else {
    win32: LIBS += -L$$MYLIBDIR/win64/lib
}

###################################################################################
# Link to the right libraries
###################################################################################

win32: LIBS += -lavcodec -lavformat -lavutil -lswscale -lswresample

###################################################################################
# The hard work
###################################################################################

QT += multimedia
qtHaveModule(widgets): QT += multimediawidgets

# or use CONFIG+=qt_plugin and add .qmake.conf with PLUGIN_TYPE PLUGIN_CLASS_NAME,
# but error "Could not find feature stack-protector-strong". can be fixed by `load(qt_build_config)`
QTDIR_build {
    # This is only for the Qt build. Do not use externally. We mean it.
    PLUGIN_TYPE = mediaservice
    PLUGIN_CLASS_NAME = FFmpegPlugin
    load(qt_plugin)
} else {
    TARGET = $$qtLibraryTarget(ffmpeg-plugin)
    TEMPLATE = lib
    load(qt_build_config) # see https://github.com/CrimsonAS/gtkplatform/issues/20#issuecomment-331722270
    CONFIG += plugin qt_plugin
    target.path = $$[QT_INSTALL_PLUGINS]/mediaservice
    INSTALLS += target
}

SOURCES += \
    mediaplayerservice.cpp \
    mediaplayercontrol.cpp \
    metadatareadercontrol.cpp \
    renderercontrol.cpp \
    ffmpeg-plugin.cpp

HEADERS += \
    mediaplayerservice.h \
    mediaplayercontrol.h \
    metadatareadercontrol.h \
    renderercontrol.h \
    ffmpeg-plugin.h

qtHaveModule(widgets) {
    SOURCES += videowidgetcontrol.cpp
    HEADERS += videowidgetcontrol.h
}

OTHER_FILES += ffmpeg-plugin.json

include(ffmpeg/ffmpeg.pri)

DISTFILES += \
    .qmake.conf \
    LICENSE \
    README.md
