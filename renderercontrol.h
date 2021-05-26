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

#ifndef __RenderControl_H
#define __RenderControl_H

#include <QVideoRendererControl>


class MediaPlayerControl;
class QOpenGLFramebufferObject;

typedef int MediaStatus;

class RendererControl : public QVideoRendererControl
{
    Q_OBJECT
public:
    RendererControl(MediaPlayerControl* player, QObject *parent = nullptr);
    QAbstractVideoSurface *surface() const override;
    void setSurface(QAbstractVideoSurface *surface) override;

    void setSource();

public slots:
    void onFrameAvailable();

private:
    QAbstractVideoSurface    *_surface = nullptr;
    MediaPlayerControl       *_ffmpeg = nullptr;
    QOpenGLFramebufferObject *_fbo = nullptr;

    // video_w/h_ is from MediaInfo, which may be incorrect.
    // A better value is from VideoFrame but it's not a public class now.

    int video_w_ = 0;
    int video_h_ = 0;
    MediaStatus _status;
};

#endif
