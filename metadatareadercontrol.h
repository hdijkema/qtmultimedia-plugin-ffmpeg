/*
 * Copyright (C) 2021 Hans Dijkema, License: LGPLv2.1
 * https://github.com/hdijkema/qmultimedia-plugin-ffmpeg
 *
 * Derived from the work of Wang Bin
 * https://github.com/wang-bin/qtmultimedia-plugins-mdk
 */


#ifndef __MetaDataReaderControl_H
#define __MetaDataReaderControl_H

#include <QMetaDataReaderControl>

class MediaPlayerControl;
class MetaDataReaderControl final: public QMetaDataReaderControl
{
public:
    MetaDataReaderControl(MediaPlayerControl* mpc, QObject *parent = nullptr);

    bool isMetaDataAvailable() const override {return !_tags.isEmpty();}
    QVariant metaData(const QString &key) const override {return _tags.value(key);}
    QStringList availableMetaData() const override {return _tags.keys();}

private:
    void readMetaData();

private:
    MediaPlayerControl* _ffmpeg = nullptr;
    QVariantMap         _tags;
};

#endif
