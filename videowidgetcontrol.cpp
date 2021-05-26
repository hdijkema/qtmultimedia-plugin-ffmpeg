/*
 * ffmpeg-plugin - a Qt MultiMedia plugin for playback of video/audio using
 * the ffmpeg library for decoding.
 *
 * Copyright (C) 2021 Hans Dijkema, License: LGPLv3
 * https://github.com/hdijkema/qmultimedia-plugin-ffmpeg
 *
 * Derived from the work of Wang Bin
 * https://github.com/wang-bin/qtmultimedia-plugins-mdk
 */

#include "videowidgetcontrol.h"
#include "mediaplayercontrol.h"

#include <QOpenGLWidget>
#include <QCoreApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QDebug>

#define LINE_DEBUG qDebug() << __FUNCTION__ << __LINE__;

class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
private:
    FFmpegProvider *_provider = nullptr;
    bool            _first_time;

public:
    VideoWidget(QWidget *parent = nullptr) : QOpenGLWidget(parent) {}

    void setSource(FFmpegProvider *provider) {
        _provider = provider;
        _first_time = true;
    }

protected:
    void initializeGL() override {
        initializeOpenGLFunctions();
        auto ctx = context();
        connect(context(), &QOpenGLContext::aboutToBeDestroyed, [ctx]{
            QOffscreenSurface s;
            s.create();
            ctx->makeCurrent(&s);
            FFmpegProvider::foreignGLContextDestroyed();
            ctx->doneCurrent();
        });
        _first_time = true;
    }

    void resizeGL(int w, int h) override {
        if (!_provider)
            return;
        _provider->setVideoSurfaceSize(w, h);
    }

    void paintGL() override {
        if (!_provider) return;
        if (_first_time) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        QPainter p(this);
        _provider->renderVideo(&p);
    }
};

VideoWidgetControl::VideoWidgetControl(MediaPlayerControl* player, QObject* parent)
    : QVideoWidgetControl(parent)
    , _video_widget(new VideoWidget())
    , _ffmpeg(player)
{
    _video_widget->setSource(_ffmpeg->provider());
    connect(_ffmpeg, SIGNAL(frameAvailable()),
            _video_widget, SLOT(update()),
            Qt::QueuedConnection);
}

VideoWidgetControl::~VideoWidgetControl()
{
    delete _video_widget;
}

bool VideoWidgetControl::isFullScreen() const
{
    return _fs;
}

void VideoWidgetControl::setFullScreen(bool fullScreen)
{
    _fs = fullScreen;
}

Qt::AspectRatioMode VideoWidgetControl::aspectRatioMode() const
{
    return _am;
}

static FFmpegProvider::Ratio fromQt(Qt::AspectRatioMode value)
{
    switch (value) {
    case Qt::IgnoreAspectRatio: return FFmpegProvider::IgnoreAspectRatio;
    case Qt::KeepAspectRatioByExpanding: return FFmpegProvider::KeepAspectRatioCrop;
    case Qt::KeepAspectRatio: return FFmpegProvider::KeepAspectRatio;
    default: return FFmpegProvider::KeepAspectRatio;
    }
}

void VideoWidgetControl::setAspectRatioMode(Qt::AspectRatioMode mode)
{
    _am = mode;
    _ffmpeg->provider()->setAspectRatio(fromQt(mode));
}

void VideoWidgetControl::setBrightness(int brightness)
{
    _ffmpeg->provider()->setBrightness(brightness);
}

void VideoWidgetControl::setContrast(int contrast)
{
    _ffmpeg->provider()->setContrast(contrast);
}

void VideoWidgetControl::setHue(int hue)
{
    _ffmpeg->provider()->setHue(hue);
}

void VideoWidgetControl::setSaturation(int saturation)
{
    _ffmpeg->provider()->setSaturation(saturation);
}

QWidget* VideoWidgetControl::videoWidget()
{
    return _video_widget;
}
