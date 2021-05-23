/*
 * Copyright (C) 2021 Hans Dijkema, License: LGPLv2.1
 * https://github.com/hdijkema/qmultimedia-plugin-ffmpeg
 *
 * Derived from the work of Wang Bin
 * https://github.com/wang-bin/qtmultimedia-plugins-mdk
 */

// GLTextureVideoBuffer render to fbo texture, texture as frame
// map to host: store tls ui context in map(GLTextureArray), create tls context shared with ui ctx, download
// move to mdk public NativeVideoBuffer::fromTexture()

#include "renderercontrol.h"
#include "mediaplayercontrol.h"

#include <QAbstractVideoBuffer>
#include <QAbstractVideoSurface>
#include <QVideoFrame>
#include <QVideoSurfaceFormat>
#include <QImage>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLContext>
#include <QDebug>
#include <QOpenGLPaintDevice>
#include <QPainter>

#define LINE_DEBUG qDebug() << __FUNCTION__ << __LINE__

#define flags_added(a, b, fl) (((b - a)&fl) != 0)

// qtmultimedia only support packed rgb gltexture handle, so offscreen rendering may be required
class FBOVideoBuffer final : public QAbstractVideoBuffer
{
private:
    MapMode                       _mode = NotMapped;
    int                           _width;
    int                           _height;
    FFmpegProvider               *_provider;
    QOpenGLFramebufferObject    **_fbo = nullptr;
    QImage                        _img;

public:
    FBOVideoBuffer(FFmpegProvider *player, QOpenGLFramebufferObject** fbo, int width, int height)
        : QAbstractVideoBuffer(GLTextureHandle)
        , _width(width), _height(height)
        , _provider(player)
        , _fbo(fbo)
    {}

    MapMode mapMode() const override { return _mode; }

    uchar *map(MapMode mode, int *numBytes, int *bytesPerLine) override
    {
        if (_mode == mode)
            return _img.bits();

        if (mode & WriteOnly)
            return nullptr;

        _mode = mode;
        renderToFbo();
        _img = (*_fbo)->toImage(false);

        if (numBytes)
            *numBytes = _img.sizeInBytes();

        if (bytesPerLine)
            *bytesPerLine = _img.bytesPerLine();

        return _img.bits();
    }

    void unmap() override
    {
        _mode = NotMapped;
    }

    // current context is not null!
    QVariant handle() const override
    {
        auto that = const_cast<FBOVideoBuffer*>(this);
        that->renderToFbo();
        return (*_fbo)->texture();
    }

private:
    void renderToFbo()
    {
        auto f = QOpenGLContext::currentContext()->functions();
        GLint prevFbo = 0;
        f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

        auto fbo = *_fbo;

        if (!fbo || fbo->size() != QSize(_width, _height)) {
            _provider->scale(1.0f, -1.0f); // flip y in fbo
            _provider->setVideoSurfaceSize(_width, _height);
            delete fbo;
            fbo = new QOpenGLFramebufferObject(_width, _height);
            *_fbo = fbo;
        }

        fbo->bind();

        QOpenGLPaintDevice dev;
        QPainter p(&dev);
        _provider->renderVideo(&p);

        f->glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    }

};

RendererControl::RendererControl(MediaPlayerControl* player, QObject* parent)
    : QVideoRendererControl(parent)
    , _ffmpeg(player)
{
    _ffmpeg = player;
    connect(_ffmpeg, &MediaPlayerControl::frameAvailable,
            this, &RendererControl::onFrameAvailable);
}

QAbstractVideoSurface* RendererControl::surface() const
{
    return _surface;
}

void RendererControl::setSurface(QAbstractVideoSurface* surface)
{
    FFmpegProvider *provider = _ffmpeg->provider();

    if (_surface && _surface->isActive())
        _surface->stop();

    _surface = surface;

    if (!surface) {
        provider->setRenderCallback(nullptr); // surfcace is set to null before destroy, avoid invokeMethod() on invalid this
        return;
    }

    //const QSize r = surface->nativeResolution(); // may be (-1, -1)
    // mdk player needs a vo. add before delivering a video frame
    provider->setVideoSurfaceSize(1, 1);//r.width(), r.height());

    if (provider->mediaInfo().has_video) {
        auto &c = provider->mediaInfo().video;
        video_w_ = c.width;
        video_h_ = c.height;
    }

    provider->onMediaStateChanged([this](FFmpegProvider::MediaState s){
        FFmpegProvider *provider = this->_ffmpeg->provider();
        if (flags_added(_status, s, FFmpegProvider::Loaded)) {
            if (provider->mediaInfo().has_video) {
                auto &c = provider->mediaInfo().video;
                video_w_ = c.width;
                video_h_ = c.height;
            }
        }
        _status = s;
    });
}

void RendererControl::onFrameAvailable()
{
    if (!_surface)
        return;

    if (video_w_ <= 0 || video_h_ <= 0)
        return; // not playing, e.g. when stop() is called there is also a frameAvailable
                // signal to update renderer which is required by mdk internally.
                // if create fbo with an invalid size anyway, qt gl rendering will be broken forever

    QVideoFrame frame(new FBOVideoBuffer(_ffmpeg->provider(), &_fbo, video_w_, video_h_),
                                         QSize(video_w_, video_h_),
                                         QVideoFrame::Format_BGR32
                      ); // RGB32 for qimage

    if (!_surface->isActive()) { // || surfaceFormat()!=
        QVideoSurfaceFormat format(frame.size(), frame.pixelFormat(), frame.handleType());
        _surface->start(format);
    }

    _surface->present(frame); // main thread
}
