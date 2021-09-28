/*
 * ffmpeg-plugin - a Qt MultiMedia plugin for playback of video/audio using
 * the ffmpeg library for decoding.
 *
 * The internal ffmpeg interfacing / decoding part of the plugin.
 *
 * Copyright (C) 2021 Hans Dijkema, License: LGPLv3
 * https://github.com/hdijkema/qmultimedia-plugin-ffmpeg
 */

#ifndef FFMPEGPROVIDER_H
#define FFMPEGPROVIDER_H

#define FFMPEG_PROVIDER_VERSION "v0.1"
#define FFMPEG_PROVIDER_LICENSE "v0.1 (c) 2021 Hans Dijkema, License: LGPLv3"
#define FFMPEG_PROVIDER_NAME    "QMultimedia Plugin ffmpeg-plugin"

#include <QObject>
#include <QHash>
#include <QList>
#include <QSize>
#include <QAudio>

class MediaPlayerControl;
class FFmpeg;
class DecoderThread;
class QPainter;
class QAudioOutput;

class FFmpegProvider : public QObject
{
    Q_OBJECT
public:
    enum Error {
        NoError         = 0,
        UrlNotSupported = 1,
        CannotOpenVideo = 2,
        CannotFindStreamInfo = 3,
        CantAlloc = 4,
        UnsupportedCodec = 5,
        Internal = 6
    };

    enum State {
        Stopped = 0,
        Playing = 1,
        Paused = 2
    };

    enum MediaState {
        NoMedia = 0x0000,
        Invalid = 0x8000,
        Loading = 0x0001,
        Stalled = 0x0002,
        Buffering = 0x0004,
        Buffered = 0x0008,
        End = 0x0100,
        Loaded = 0x0010
    };

    enum MediaKind {
        Audio = 1,
        Video = 2,
        Other = 3
    };

    enum Ratio {
        IgnoreAspectRatio = 1,
        KeepAspectRatioCrop = 2,
        KeepAspectRatio = 3
    };

    class MediaEvent
    {
    public:
        int error = 0;
        MediaKind kind = Other;
    };

    struct Audio
    {
        int     bit_rate;
        QString codec;
        int     channels;
        int     sample_rate;
    };

    struct Video
    {
        qreal   frame_rate;
        int     bit_rate;
        QString codec;
        int     width;
        int     height;
    };

    class Info
    {
    public:
        qint64 size;
        qint64 duration;
        bool   has_audio;
        bool   has_video;
        QHash<QString, QString> metadata;
        struct Audio audio;
        struct Video video;
    };

private:
    DecoderThread      *_decoder;
    FFmpeg             *_ffmpeg;
    Info                _info;
    State               _play_state;
    MediaState          _media_state;
    QSize               _surface_size;

    QStringList         _video_decoders;

    MediaPlayerControl *_control;

    QString             _current_url;

    QList<std::function<void (State s)>> state_cbs;
    QList<std::function<void (MediaState s)>> mediastate_cbs;
    QList<std::function<void (const MediaEvent &e)>> mediaevent_cbs;
    std::function<void (void *context)> _render_cb;

public:
    FFmpegProvider(MediaPlayerControl *parent = nullptr);
   ~FFmpegProvider();

public:
    void setVideoDecoders(const QStringList &dec);

    void onStateChanged(std::function<void (State s)> f);
    void onMediaStateChanged(std::function<void (MediaState s)> f);
    void onEvent(std::function<void (const MediaEvent &e)> f);
    void setRenderCallback(std::function<void (void *context)> f);

public:
    void setState(State s);
    State state() const;

    MediaState mediaState() const;
    void setMediaState(MediaState s);

public:
    void setVolume(int percentage);
    void setMuted(bool yes);

    qreal playbackRate() const;
    void setPlaybackRate(qreal rate);

    void seek(qint64 pos_in_ms);
    qint64 position() const;

    void setHue(int hue);
    void setSaturation(int sat);
    void setContrast(int contr);
    void setBrightness(int brightness);

public:
    bool setMedia(const QString &url);

public:
    void waitFor(State s);
    void prepare(qint64 seek, std::function<void (qint64 pos, bool *ok)> cb);

    const Info &mediaInfo() const;

public:
    void setVideoSurfaceSize(int w, int h);
    QSize getVideoSurfaceSize() const;
    void scale(qreal x, qreal y);
    void renderVideo(QPainter *p);
    QImage *getImage(bool &gotIt);
    void popImage();

public:
    void setAspectRatio(float ar);

public:
    static void foreignGLContextDestroyed();

public:
    void signalError(Error e, const QString &msg, const char *func, int line);
    void threadError(Error e, const QString &msg, const char *func, int line);

public:
    void signalImageAvailable();
    void signalPcmAvailable();
    void signalClearAudioBuffer();
    void signalClearVideoBuffer();
    void signalSetState(State s);

private:
    void resetProvider();
    void stopThreads();
    void startThreads();
    bool allocBuffers();

private:
    int audioThresholdMs();
    void audiobClearBuf();
    int audiobBufSizeInMs();
    void audiobPutAudio(const QByteArray &samples);

signals:
    void imageAvailable();
    void pcmAvailable();
    void setStateSig(State s);

private slots:
    void handleImageAvailable();
    void handleAudioAvailable();
    void handleSetState(State s);
};

#endif // FFMPEGPROVIDER_H
