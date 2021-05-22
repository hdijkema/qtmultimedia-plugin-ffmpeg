/*
 * Copyright (C) 2021 Hans Dijkema, License: LGPLv3
 * https://github.com/hdijkema/qmultimedia-plugin-ffmpeg
 *
 * Derived from the work of Wang Bin
 * https://github.com/wang-bin/qtmultimedia-plugins-mdk
 */


#ifndef __MediaPlayerService_H
#define __MediaPlayerService_H

#include <QMediaService>

class MediaPlayerControl;
class VideoWindowControl;

class MediaPlayerService : public QMediaService {
    Q_OBJECT
private:
    MediaPlayerControl *_mpc;

public:
    explicit MediaPlayerService(QObject* parent = nullptr);
    QMediaControl* requestControl(const char* name) override;
    void releaseControl(QMediaControl* control) override;
private:
    MediaPlayerControl *ffmpeg = nullptr;
};

#endif
