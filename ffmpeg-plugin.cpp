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

#include "ffmpeg-plugin.h"
#include "mediaplayerservice.h"

QMediaService* FFmpegPlugin::create(QString const& key)
{
    if (key == QLatin1String(Q_MEDIASERVICE_MEDIAPLAYER))
        return new MediaPlayerService();

    qWarning("FFmpegPlugin: unsupported key: %s", qPrintable(key));

    return nullptr;
}

void FFmpegPlugin::release(QMediaService *service)
{
    delete service;
}

QMediaServiceProviderHint::Features FFmpegPlugin::supportedFeatures(const QByteArray &service) const
{
    if (service == Q_MEDIASERVICE_MEDIAPLAYER)
        return QMediaServiceProviderHint::VideoSurface; //QMediaServiceProviderHint::StreamPlayback

    return QMediaServiceProviderHint::Features();
}
