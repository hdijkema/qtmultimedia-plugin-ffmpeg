/*
 * Copyright (C) 2021 Hans Dijkema, License: LGPLv2.1
 * https://github.com/hdijkema/qmultimedia-plugin-ffmpeg
 *
 * Derived from the work of Wang Bin
 * https://github.com/wang-bin/qtmultimedia-plugins-mdk
 */


#include "metadatareadercontrol.h"
#include "mediaplayercontrol.h"
#include "ffmpegprovider.h"
#include <QMediaMetaData>
#include <QSize>
#include <QDebug>

#define LINE_DEBUG qDebug() << __FUNCTION__ << __LINE__

MetaDataReaderControl::MetaDataReaderControl(MediaPlayerControl* mpc, QObject *parent)
    : QMetaDataReaderControl(parent)
    , _ffmpeg(mpc)
{
    LINE_DEBUG << "This module has not yet been fully implemented by the provider backend";

    connect(mpc, &QMediaPlayerControl::durationChanged,
            this, &MetaDataReaderControl::readMetaData, Qt::DirectConnection);
}

void MetaDataReaderControl::readMetaData()
{
    FFmpegProvider *provider = _ffmpeg->provider();

    using namespace QMediaMetaData;
    const auto& info = provider->mediaInfo();

    QVariantMap m;
    m[Size] = (qint64)info.size;
    m[Duration] = (qint64)info.duration;
    m[QMediaMetaData::MediaType] = !info.has_video ? "audio" : "video"; // FIXME: album cover image can be a video stream

    if (!info.metadata.empty()) { // TODO: metadata update
        struct {
            const char* key;
            QString qkey;
        } key_map[] = {
            {"title", Title},
            //{"Sub_Title", SubTitle},
            {"author", Author},
            {"comment", Comment},
            {"description", Description},//
            //{"category", Category}, // stringlist
            {"genre", Genre}, // stringlist
            {"year", Year}, // int
            {"date", Date}, // ISO 8601=>QDate
            //{"UserRating", UserRating},
            //{"Keywords", Keywords},
            {"language", Language},
            {"publisher", Publisher},
            {"copyright", Copyright},
            //{"ParentalRating", ParentalRating},
            //{"RatingOrganization", RatingOrganization},

            // movies
            {"performer", LeadPerformer},

            {"album", AlbumTitle},
            {"album_artist", AlbumArtist},
            //{"ContributingArtist", ContributingArtist},
            {"composer", Composer},
            //{"Conductor", Conductor},
            //{"Lyrics", Lyrics}, // AV_DISPOSITION_LYRICS
            //{"mood", Mood},
            {"track", TrackNumber},
            //{"CoverArtUrlSmall", CoverArtUrlSmall},
            //{"CoverArtUrlLarge", CoverArtUrlLarge},
            //{"AV_DISPOSITION_ATTACHED_PIC", CoverArtImage}, // qimage
            {"track", TrackNumber},
        };

        for (const auto& k : key_map) {
            if (info.metadata.contains(k.key)) {
                m[k.qkey] = info.metadata[k.key];
            }
        }
    }

    // AV_DISPOSITION_TIMED_THUMBNAILS => ThumbnailImage. qimage
    if (info.has_audio) {
        const auto& p = info.audio;
        m[AudioBitRate] = (int)p.bit_rate;
        m[AudioCodec] = p.codec;
        //m[AverageLevel]
        m[ChannelCount] = p.channels;
        m[SampleRate] = p.sample_rate;
    }

    if (info.has_video) {
        const auto& p = info.video;
        m[VideoFrameRate] = qreal(p.frame_rate);
        m[VideoBitRate] = (int)p.bit_rate;
        m[VideoCodec] = p.codec;
        m[Resolution] = QSize(p.width, p.height);
        // PixelAspectRatio
    }

    bool avail_change = _tags.empty() != m.empty();
    _tags = m;

    if (avail_change)
        emit metaDataAvailableChanged(!_tags.isEmpty());

    emit metaDataChanged();
}
