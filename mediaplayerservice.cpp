/*
 * Copyright (C) 2021 Hans Dijkema, License: LGPLv2.1
 * https://github.com/hdijkema/qmultimedia-plugin-ffmpeg
 *
 * Derived from the work of Wang Bin
 * https://github.com/wang-bin/qtmultimedia-plugins-mdk
 */

//#include "iodevice.h"
#include "mediaplayerservice.h"
#include "mediaplayercontrol.h"
#include "metadatareadercontrol.h"
#include "renderercontrol.h"

#ifdef QT_MULTIMEDIAWIDGETS_LIB
#include "videowidgetcontrol.h"
#endif //QT_MULTIMEDIAWIDGETS_LIB

MediaPlayerService::MediaPlayerService(QObject* parent)
    : QMediaService(parent)
    , _mpc(new MediaPlayerControl(parent))
{
    qInfo("create service...");
}

QMediaControl* MediaPlayerService::requestControl(const char* name)
{
    qInfo("requestControl %s", name);

    if (qstrcmp(name, QMetaDataReaderControl_iid) == 0) // requested when constructing QMediaObject(QMediaPlayer)
        return new MetaDataReaderControl(_mpc);
    if (qstrcmp(name, QMediaPlayerControl_iid) == 0)
        return _mpc;
    if (qstrcmp(name, QVideoRendererControl_iid) == 0)
        return new RendererControl(_mpc, this);
#ifdef QT_MULTIMEDIAWIDGETS_LIB
    if (qstrcmp(name, QVideoWidgetControl_iid) == 0)
        return new VideoWidgetControl(_mpc, this);
#endif //QT_MULTIMEDIAWIDGETS_LIB
    qWarning("MediaPlayerService: unsupported control: %s", qPrintable(name));
    return nullptr;
}

void MediaPlayerService::releaseControl(QMediaControl* control)
{
    qInfo() << "release control " << control;
    delete control;
    if (control == _mpc)
        _mpc = nullptr;
}
