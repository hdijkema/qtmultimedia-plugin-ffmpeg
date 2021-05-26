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

#ifndef __FFmpegPlugin_H
#define __FFmpegPlugin_H

#include <QMediaServiceProviderPlugin>

// TODO: formats
class FFmpegPlugin : public QMediaServiceProviderPlugin, public QMediaServiceFeaturesInterface
{
  Q_OBJECT
  Q_INTERFACES(QMediaServiceFeaturesInterface)
  Q_PLUGIN_METADATA(IID "org.qt-project.qt.mediaserviceproviderfactory/5.0" FILE
                        "ffmpeg-plugin.json")
public:
  QMediaService *create(QString const &key) Q_DECL_OVERRIDE;
  void release(QMediaService *service) Q_DECL_OVERRIDE;

  QMediaServiceProviderHint::Features
  supportedFeatures(const QByteArray &service) const Q_DECL_OVERRIDE;
};

#endif
