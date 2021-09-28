/*
 * ffmpeg-plugin - a Qt MultiMedia plugin for playback of video/audio using
 * the ffmpeg library for decoding.
 *
 * The internal ffmpeg interfacing / decoding part of the plugin.
 *
 * Copyright (C) 2021 Hans Dijkema, License: LGPLv3
 * https://github.com/hdijkema/qmultimedia-plugin-ffmpeg
 */

#include "ffmpegprovider.h"
#include "mediaplayercontrol.h"

//#define VIDEO_FORMAT AV_PIX_FMT_RGB24
#define VIDEO_FORMAT AV_PIX_FMT_RGB32

#define SEEK_BEGIN -98765
#define SEEK_CONTINUE -99223

#define AUDIO_THRESHOLD_EXTRA_MS 200
#define AUDIO_MAX_OFF_MS 300

#include <QDebug>
#include <QUrl>
#include <QThread>
#include <QMutex>
#include <QImage>
#include <QRegularExpression>
#include <QFile>
#include <QElapsedTimer>
#include <QQueue>
#include <QLibrary>
#include <QProcessEnvironment>
#include <QAudioOutput>
#include <QAbstractVideoBuffer>

#include <QPainter>
#include <QOpenGLPaintDevice>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

/*******************************************************************************
 * Dynamic use of SDL2
 *******************************************************************************/

#ifndef SDL_audio_h_
#define SDL_MIX_MAXVOLUME       128
#define SDL_INIT_AUDIO          0x00000010u

#ifndef SDL_BYTEORDER           /* Not defined in SDL_config.h? */
#ifdef __linux__
#include <endian.h>
#define SDL_BYTEORDER  __BYTE_ORDER
#elif defined(__OpenBSD__)
#include <endian.h>
#define SDL_BYTEORDER  BYTE_ORDER
#else
#if defined(__hppa__) || \
    defined(__m68k__) || defined(mc68000) || defined(_M_M68K) || \
    (defined(__MIPS__) && defined(__MIPSEB__)) || \
    defined(__ppc__) || defined(__POWERPC__) || defined(_M_PPC) || \
    defined(__sparc__)
#define SDL_BYTEORDER   SDL_BIG_ENDIAN
#else
#define SDL_BYTEORDER   SDL_LIL_ENDIAN
#endif
#endif /* __linux__ */
#endif /* !SDL_BYTEORDER */

#define AUDIO_S16LSB    0x8010  /**< Signed 16-bit samples */
#define AUDIO_S16MSB    0x9010  /**< As above, but big-endian byte order */

#if SDL_BYTEORDER == SDL_LIL_ENDIAN
#define AUDIO_S16SYS    AUDIO_S16LSB
#else
#define AUDIO_S16SYS    AUDIO_S16MSB
#endif

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;

typedef struct SDL_version
{
    Uint8 major;        /**< major version */
    Uint8 minor;        /**< minor version */
    Uint8 patch;        /**< update version */
} SDL_version;


typedef Uint16 SDL_AudioFormat;

typedef Uint32 SDL_AudioDeviceID;

typedef void (*SDL_AudioCallback) (void *userdata, Uint8 * stream, int len);

typedef struct SDL_AudioSpec
{
    int freq;                   /**< DSP frequency -- samples per second */
    SDL_AudioFormat format;     /**< Audio data format */
    Uint8 channels;             /**< Number of channels: 1 mono, 2 stereo */
    Uint8 silence;              /**< Audio buffer silence value (calculated) */
    Uint16 samples;             /**< Audio buffer size in sample FRAMES (total samples divided by channel count) */
    Uint16 padding;             /**< Necessary for some compile environments */
    Uint32 size;                /**< Audio buffer size in bytes (calculated) */
    SDL_AudioCallback callback; /**< Callback that feeds the audio device (NULL to use SDL_QueueAudio()). */
    void *userdata;             /**< Userdata passed to callback (ignored for NULL callbacks). */
} SDL_AudioSpec;
#endif

extern "C" {
    typedef struct {
        int (*SDL_Init)(Uint32 flags);
        const char* (*SDL_GetError)(void);
        SDL_AudioDeviceID (*SDL_OpenAudioDevice)(const char *device, int iscapture, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed_changes);
        void (*SDL_PauseAudioDevice)(SDL_AudioDeviceID dev, int pause_on);
        void *(*SDL_memset)(void *dst, int c, size_t len);
        void (*SDL_MixAudioFormat)(Uint8 * dst, const Uint8 * src, SDL_AudioFormat format, Uint32 len, int volume);
        void (*SDL_CloseAudioDevice)(SDL_AudioDeviceID dev);
        void (*SDL_GetVersion)(SDL_version * ver);
    } LibSdl;
}

static LibSdl *loadSdl();

/*******************************************************************************
 * Some General Defines
 *******************************************************************************/

#define LINE_DEBUG qDebug() << __FUNCTION__ << __LINE__
#define LINE_INFO  qInfo() << __FUNCTION__ << __LINE__
#define LINE_WARN  qWarning() << __FUNCTION__ << __LINE__

#define SIGNAL_ERROR(e, s) signalError(e, s, __FUNCTION__, __LINE__)
#define THREAD_ERROR(e, s) threadError(e, s, __FUNCTION__, __LINE__)

#define NOT_IMPLEMENTED     LINE_WARN << "Not implemented in version" << FFMPEG_PROVIDER_VERSION << " of " << FFMPEG_PROVIDER_NAME
#define unused(a) ((void)a)

#define MS(s)   static_cast<int>((static_cast<qreal>(s) / AV_TIME_BASE) * 1000)
#define FS(ms)  (ms * (AV_TIME_BASE / 1000))
#define AD(a)   if (_decoder != nullptr) a

static bool initialized = false;
static bool _can_render = false;
static LibSdl *lib_sdl = nullptr;

/*******************************************************************************
 * Initialization, Internal structures and types
 *******************************************************************************/

static void initffmpeg()
{
    if (!initialized) {
        initialized = true;
        _can_render = true;
        lib_sdl = loadSdl();
    }
}

typedef struct {
    QImage      image;
    int         position_in_ms;
} FFmpegImage;

typedef struct {
    QByteArray  audio;
    int         position_in_ms;
    bool        clear;
} FFmpegAudio;


class SdlBuf
{
public:
    SDL_AudioFormat format;
    QByteArray      audiobuf;
    int             volume_percent;
    bool            muted;
    QMutex          mutex;
};

class FFmpeg
{
public:
    AVFormatContext     *pFormatCtx;
    AVCodec             *pAudioCodec;
    AVCodec             *pVideoCodec;
    AVCodecContext      *pVideoCtx;
    AVCodecContext      *pAudioCtx;
    QList<AVPacket *>    packetQueue;
    AVFrame             *pFrame;
    AVFrame             *pFrameRGB;
    uint8_t             *buffer;
    QMutex               mutex;
    int                  audio_stream_index;
    int                  video_stream_index;
    int                  duration_in_ms;
    int                  position_in_ms;
    int                  pos_offset_in_ms;
    QElapsedTimer        elapsed;
    QQueue<FFmpegImage>  image_queue;
    QQueue<FFmpegAudio>  audio_queue;
    qint64               seek_frame;
    int                  volume_percent;
    bool                 muted;
    bool                 sdl;
    SDL_AudioDeviceID    sdl_id;
    SDL_AudioFormat      sdl_format;
    SdlBuf              *sdl_buf;
    QAudioOutput        *audio_out;
    QIODevice           *audio_io;
public:
    FFmpeg();
};


class DecoderThread : public QThread
{
public:
    enum PlayState { Stopped, Paused, Playing, Ended };

private:
    FFmpegProvider      *_provider;
    FFmpeg               *_ffmpeg;
    QMutex              *_mutex;
    bool                 _run;
    PlayState            _request;
    PlayState            _current;

public:
    DecoderThread(FFmpegProvider *p, FFmpeg *ffmpeg, QMutex *mutex);

public:
    static DecoderThread::PlayState toDecoderState(FFmpegProvider::State s);
    static FFmpegProvider::State toFFmpegState(PlayState s);

public:
    void endDecoder();
    void requestPlayState(PlayState s);
    void waitForRequest();
    void waitForState(PlayState s);

    // QThread interface
protected:
    virtual void run() override;
};

/*******************************************************************************
 * FFmpegProvider methods
 *******************************************************************************/

FFmpegProvider::FFmpegProvider(MediaPlayerControl *parent)
    : QObject(parent)
{
    LINE_INFO << FFMPEG_PROVIDER_NAME << FFMPEG_PROVIDER_VERSION << FFMPEG_PROVIDER_LICENSE;
    LINE_INFO << "FFmpegProvider backend instantiating.";

    initffmpeg();

    _ffmpeg = new FFmpeg();
    _decoder = nullptr;

    quint64 ptr = reinterpret_cast<quint64>(this);
    setObjectName(QString::asprintf("FFmpegProvider_%llx", ptr));

    _control = parent;

    connect(this, &FFmpegProvider::imageAvailable, this, &FFmpegProvider::handleImageAvailable, Qt::QueuedConnection);
    connect(this, &FFmpegProvider::pcmAvailable, this, &FFmpegProvider::handleAudioAvailable, Qt::QueuedConnection);
    connect(this, &FFmpegProvider::setStateSig, this, &FFmpegProvider::handleSetState, Qt::QueuedConnection);
}

FFmpegProvider::~FFmpegProvider()
{
    if (_decoder != nullptr) {
        stopThreads();
    }
    resetProvider();
    delete _ffmpeg;
}

void FFmpegProvider::setVideoDecoders(const QStringList &dec)
{
    // We set them, but we don't use them.
    _video_decoders = dec;
}

void FFmpegProvider::onStateChanged(std::function<void (FFmpegProvider::State)> f)
{
    state_cbs.append(f);
}

void FFmpegProvider::onMediaStateChanged(std::function<void (FFmpegProvider::MediaState)> f)
{
    mediastate_cbs.append(f);
}

void FFmpegProvider::onEvent(std::function<void (const FFmpegProvider::MediaEvent &)> f)
{
    mediaevent_cbs.append(f);
}

void FFmpegProvider::setRenderCallback(std::function<void (void *)> f)
{
    _render_cb = f;
}

void FFmpegProvider::setState(FFmpegProvider::State s)
{
    if (_play_state != s) {
        _play_state = s;

        AD(_decoder->requestPlayState(DecoderThread::toDecoderState(s)));

        int i, N;
        for(i = 0, N = state_cbs.size(); i < N; i++) {
            state_cbs[i](s);
        }
    }
}

FFmpegProvider::State FFmpegProvider::state() const
{
    return _play_state;
}

FFmpegProvider::MediaState FFmpegProvider::mediaState() const
{
    return _media_state;
}

void FFmpegProvider::setMediaState(FFmpegProvider::MediaState s)
{
    if (_media_state != s) {
        _media_state = s;
        int i, N;
        for(i = 0, N = mediastate_cbs.size(); i < N; i++) {
            mediastate_cbs[i](s);
        }
    }
}

void FFmpegProvider::setVolume(int percentage)
{
    _ffmpeg->mutex.lock();
    _ffmpeg->volume_percent = percentage;

    if (_ffmpeg->sdl) {
        SdlBuf *buf = _ffmpeg->sdl_buf;
        if (buf) {
            buf->mutex.lock();
            buf->volume_percent = percentage;
            buf->mutex.unlock();
        }
    } else { // Qt backend
        int vol = (_ffmpeg->muted) ? 0 : _ffmpeg->volume_percent;
        if (_ffmpeg->audio_out) {
            qreal linearVolume = QAudio::convertVolume(vol / qreal(100.0),
                                                       QAudio::LogarithmicVolumeScale,
                                                       QAudio::LinearVolumeScale);
            _ffmpeg->audio_out->setVolume(linearVolume);
        }
    }

    _ffmpeg->mutex.unlock();
}

void FFmpegProvider::setMuted(bool yes)
{
    _ffmpeg->mutex.lock();
    _ffmpeg->muted = yes;

    if (_ffmpeg->sdl) {
        SdlBuf *buf = _ffmpeg->sdl_buf;
        if (buf) {
            buf->mutex.lock();
            buf->muted = yes;
            buf->mutex.unlock();
        }
    } else { // Qt
        int vol = (_ffmpeg->muted) ? 0 : _ffmpeg->volume_percent;
        if (_ffmpeg->audio_out) {
            _ffmpeg->audio_out->setVolume(vol / 100.0);
        }
    }

    _ffmpeg->mutex.unlock();
}

qreal FFmpegProvider::playbackRate() const
{
    return 1.0;
}

void FFmpegProvider::setPlaybackRate(qreal /*rate*/)
{
    LINE_DEBUG << "setPlaybackRate not implemented";
}

void FFmpegProvider::seek(qint64 pos_in_ms)
{
    _ffmpeg->mutex.lock();
    if (pos_in_ms == SEEK_BEGIN) {
        _ffmpeg->seek_frame = SEEK_BEGIN;
    } else {
        _ffmpeg->seek_frame = FS(pos_in_ms);
    }
    _ffmpeg->mutex.unlock();
}

qint64 FFmpegProvider::position() const
{
    qint64 p;
    _ffmpeg->mutex.lock();
    p = _ffmpeg->position_in_ms;
    _ffmpeg->mutex.unlock();
    return p;
}

void FFmpegProvider::setAspectRatio(float ar)
{
    unused(ar);
    NOT_IMPLEMENTED;
}

void FFmpegProvider::scale(qreal x, qreal y)
{
    unused(x);unused(y);
    NOT_IMPLEMENTED;
}

void FFmpegProvider::setHue(int hue)
{
    unused(hue);
    NOT_IMPLEMENTED;
}

void FFmpegProvider::setSaturation(int sat)
{
    unused(sat);
    NOT_IMPLEMENTED;
}

void FFmpegProvider::setContrast(int contr)
{
    unused(contr);
    NOT_IMPLEMENTED;
}

void FFmpegProvider::setBrightness(int brightness)
{
    unused(brightness);
    NOT_IMPLEMENTED;
}

static void sdl_audio_callback(void *user_data, uint8_t *stream, int len);

bool FFmpegProvider::setMedia(const QString &_url)
{
    LINE_INFO << "Trying to load media from" << _url;

    _current_url = _url;

    stopThreads();

    setState(Stopped);
    resetProvider();
    setMediaState(NoMedia);

    QString url = _url;
    {
        QFile f(url);
        if (f.exists()) {
            if (!url.startsWith("file:")) { url = "file:" + url; }
        }
    }

    QUrl u(url);

    if (u.scheme() == "file" || u.isLocalFile() || u.scheme() == "http" || u.scheme() == "https") {
        setMediaState(Loading);

        QString file;
        if (u.scheme() == "file" || u.isLocalFile()) {
            file = u.toLocalFile();
        } else {
            file = u.toString();
        }

        _ffmpeg->pFormatCtx = avformat_alloc_context();
        if (_ffmpeg->pFormatCtx == nullptr) {
            SIGNAL_ERROR(CantAlloc, tr("Not enough memory"));
            setMediaState(Invalid);
            return false;
        }

        if (avformat_open_input(&_ffmpeg->pFormatCtx, file.toLocal8Bit().constData(), nullptr, nullptr) != 0) {
            SIGNAL_ERROR(CannotOpenVideo, tr("Cannot open the Url %1").arg(url));
            setMediaState(Invalid);
            return false;
        }

        if (avformat_find_stream_info(_ffmpeg->pFormatCtx, nullptr) != 0) {
            SIGNAL_ERROR(CannotFindStreamInfo, tr("Cannot determine the stream information for %1").arg(url));
            setMediaState(Invalid);
            return false;
        }


        int videoStream = -1;
        int audioStream = -1;

        audioStream= av_find_best_stream(_ffmpeg->pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        videoStream = av_find_best_stream(_ffmpeg->pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

        //LINE_DEBUG;
        // Find the video and audio stream
        {
            for (unsigned int i = 0; i < _ffmpeg->pFormatCtx->nb_streams; i++) {
                // look for the video stream
                if (_ffmpeg->pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && videoStream < 0)
                {
                    videoStream = static_cast<int>(i);
                }

                // look for the audio stream
                if (_ffmpeg->pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audioStream < 0)
                {
                    audioStream = static_cast<int>(i);
                }
            }
        }

        _ffmpeg->audio_stream_index = audioStream;
        _ffmpeg->video_stream_index = videoStream;

        //LINE_DEBUG << audioStream << videoStream;

        if (_ffmpeg->audio_stream_index >= 0) {
            _info.has_audio = true;
            auto codec_par = _ffmpeg->pFormatCtx->streams[_ffmpeg->audio_stream_index]->codecpar;
            _ffmpeg->pAudioCodec = const_cast<AVCodec *>(avcodec_find_decoder(codec_par->codec_id));
            if (_ffmpeg->pAudioCodec == nullptr) {
                SIGNAL_ERROR(CannotOpenVideo, tr("Cannot open found audiostream for %1").arg(url));
                setMediaState(Invalid);
                return false;
            } else {
                _ffmpeg->pAudioCtx = avcodec_alloc_context3(_ffmpeg->pAudioCodec);
                if (_ffmpeg->pAudioCtx == nullptr) {
                    SIGNAL_ERROR(CantAlloc, tr("Cannot allocate audiostream context for %1").arg(url));
                    setMediaState(Invalid);
                    return false;
                } else {
                    int res = avcodec_parameters_to_context(_ffmpeg->pAudioCtx, codec_par);
                    if (res < 0) {
                        SIGNAL_ERROR(CannotOpenVideo, tr("Failed to transfer audio parameters to context"));
                        setMediaState(Invalid);
                        return false;
                    } else {
                        res = avcodec_open2(_ffmpeg->pAudioCtx, _ffmpeg->pAudioCodec, NULL);
                        if (res < 0) {
                            SIGNAL_ERROR(CannotOpenVideo, tr("Failed to open audiocodec"));
                            setMediaState(Invalid);
                            return false;
                        }
                    }
                }
            }
        } else {
            _info.has_audio = false;
        }

        //LINE_DEBUG;

        if (_ffmpeg->video_stream_index >= 0) {
            _info.has_video = true;
            auto codec_par = _ffmpeg->pFormatCtx->streams[_ffmpeg->video_stream_index]->codecpar;
            _ffmpeg->pVideoCodec = const_cast<AVCodec *>(avcodec_find_decoder(codec_par->codec_id));
            if (_ffmpeg->pVideoCodec == nullptr) {
                SIGNAL_ERROR(CannotOpenVideo, tr("Cannot open found videostream for %1").arg(url));
                setMediaState(Invalid);
                return false;
            } else {
                _ffmpeg->pVideoCtx = avcodec_alloc_context3(_ffmpeg->pVideoCodec);
                if (_ffmpeg->pVideoCtx == nullptr) {
                    SIGNAL_ERROR(CantAlloc, tr("Cannot allocate videostream context for %1").arg(url));
                    setMediaState(Invalid);
                    return false;
                } else {
                    int res = avcodec_parameters_to_context(_ffmpeg->pVideoCtx, codec_par);
                    if (res < 0) {
                        SIGNAL_ERROR(CannotOpenVideo, tr("Failed to transfer video parameters to context"));
                        setMediaState(Invalid);
                        return false;
                    } else {
                        res = avcodec_open2(_ffmpeg->pVideoCtx, _ffmpeg->pVideoCodec, NULL);
                        if (res < 0) {
                            SIGNAL_ERROR(CannotOpenVideo, tr("Failed to open videocodec"));
                            setMediaState(Invalid);
                            return false;
                        }
                    }
                }
            }
        }

        //LINE_DEBUG;

        _info.duration = MS(_ffmpeg->pFormatCtx->duration);
        _ffmpeg->duration_in_ms = static_cast<int>(_info.duration);

        //LINE_DEBUG;

        if (_ffmpeg->audio_stream_index >= 0) {
            auto ctx = _ffmpeg->pAudioCtx;
            _info.audio.bit_rate = ctx->bit_rate;
            _info.audio.channels = ctx->channels;
            _info.audio.sample_rate = ctx->sample_rate;
            _info.audio.codec = QString::fromUtf8(ctx->codec_descriptor->name);
        }

        //LINE_DEBUG;

        if (_ffmpeg->video_stream_index >= 0) {
            auto ctx = _ffmpeg->pVideoCtx;
            _info.video.bit_rate = ctx->bit_rate;
            _info.video.frame_rate = av_q2d(ctx->framerate);
            _info.video.height = ctx->height;
            _info.video.width = ctx->width;
            _info.video.codec = QString::fromUtf8(ctx->codec_descriptor->name);
        }

        //LINE_DEBUG;

        LINE_INFO << "Video information:";
        LINE_INFO << "Width:" << _info.video.width << "Height:" << _info.video.height;
        LINE_INFO << "Framrate:" << _info.video.frame_rate << "Bitrate:" << _info.video.bit_rate;
        LINE_INFO << "Codec:" << _info.video.codec;
        LINE_INFO << "Duration:" << _info.duration;
        LINE_INFO << "Audio information:";
        LINE_INFO << "Bitrate:" << _info.audio.bit_rate;
        LINE_INFO << "Channels:" << _info.audio.channels;
        LINE_INFO << "Sample Rate:" << _info.audio.sample_rate;
        LINE_INFO << "Codec:" << _info.audio.codec;

        //LINE_DEBUG;

        if (!allocBuffers()) {
            setMediaState(Invalid);
            return false;
        }

        //LINE_DEBUG;

        bool try_qt_audio = false;

        if (_ffmpeg->sdl) {
            LINE_INFO << "Using SDL Backend for audio";
            if(lib_sdl->SDL_Init(SDL_INIT_AUDIO)) {
                SIGNAL_ERROR(Internal, QString("Could not initialize SDL - %1").arg(lib_sdl->SDL_GetError()));
                try_qt_audio = true;
                _ffmpeg->sdl = false;
            } else {
                SdlBuf *sdl_buf = new SdlBuf();

                SDL_AudioSpec wanted_spec;
                wanted_spec.freq = 44100;
                wanted_spec.format = AUDIO_S16SYS;
                wanted_spec.channels = 2;
                wanted_spec.silence = 0;
                wanted_spec.samples = 1024;
                wanted_spec.callback = sdl_audio_callback;
                wanted_spec.userdata = sdl_buf;

                SDL_AudioSpec got_spec;

                _ffmpeg->sdl_id = lib_sdl->SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &got_spec, 0);
                _ffmpeg->sdl_format = got_spec.format;
                _ffmpeg->sdl_buf = sdl_buf;
                sdl_buf->format = got_spec.format;
                sdl_buf->volume_percent = _ffmpeg->volume_percent;

                if (_ffmpeg->sdl_id > 0) {
                    LINE_DEBUG << "Got audio device" << _ffmpeg->sdl_id;
                } else {
                    SIGNAL_ERROR(Internal, lib_sdl->SDL_GetError());
                }
            }
        } else {
            try_qt_audio = true;
        }

        if (try_qt_audio) {
            LINE_INFO << "Qt QAudioOutput Backend";

            _ffmpeg->sdl = false;

            QAudioFormat audioFormat;
            audioFormat.setSampleRate(44100);
            audioFormat.setChannelCount(2);
            audioFormat.setSampleSize(16);
            audioFormat.setSampleType(QAudioFormat::SignedInt);
            audioFormat.setCodec("audio/pcm");

            _ffmpeg->audio_out = new QAudioOutput(audioFormat, this);
            qreal audio_out_vol = (_ffmpeg->muted) ? 0.0 : (_ffmpeg->volume_percent / 100.0);
            _ffmpeg->audio_out->setVolume(audio_out_vol);
            _ffmpeg->audio_io = nullptr;
        }

        //LINE_DEBUG;
        startThreads();

        //LINE_DEBUG;
        seek(SEEK_BEGIN);

        //LINE_DEBUG;
        setMediaState(Loaded);

        return true;
    } else {
        SIGNAL_ERROR(UrlNotSupported, tr("The Url scheme for Url %1 is not supported").arg(url));
        setMediaState(Invalid);
        return false;
    }
}

bool FFmpegProvider::allocBuffers()
{
    _ffmpeg->pFrame = av_frame_alloc();
    if (_ffmpeg->pFrame == nullptr) {
        SIGNAL_ERROR(CantAlloc, tr("Cannot allocate frame memory"));
        return false;
    }
    _ffmpeg->pFrameRGB = av_frame_alloc();
    if (_ffmpeg->pFrameRGB == nullptr) {
        SIGNAL_ERROR(CantAlloc, tr("Cannot allocate rgb frame memory"));
        return false;
    }

    int size = av_image_get_buffer_size(VIDEO_FORMAT, _ffmpeg->pVideoCtx->width, _ffmpeg->pVideoCtx->height, 1);
    _ffmpeg->buffer = (uint8_t *) av_malloc(size * sizeof(uint8_t));

    int res = av_image_fill_arrays(_ffmpeg->pFrameRGB->data, _ffmpeg->pFrameRGB->linesize,
                                   _ffmpeg->buffer, VIDEO_FORMAT,
                                   _ffmpeg->pVideoCtx->width, _ffmpeg->pVideoCtx->height,
                                   1);

    if (res < 0) {
        SIGNAL_ERROR(Internal, tr("Cannot fill image arrays"));
        return false;
    }

    return true;
}

// This is called exclusively from the readerthread
void FFmpegProvider::signalImageAvailable()
{
    // Check if we need to render an image in the queue
    // If so, we emit the signal.
    if (_ffmpeg->image_queue.size() > 0) {
        FFmpegImage &img = _ffmpeg->image_queue.first();

        int pos_in_ms = img.position_in_ms;
        int current_time_ms = _ffmpeg->pos_offset_in_ms + _ffmpeg->elapsed.elapsed();
        if (current_time_ms >= pos_in_ms) {
            emit imageAvailable();
        }
    }
}

void FFmpegProvider::handleImageAvailable()
{
    _render_cb(this);
}

int FFmpegProvider::audioThresholdMs()
{
    int current_time_ms = _ffmpeg->pos_offset_in_ms + _ffmpeg->elapsed.elapsed();
    int extra_time_ms = AUDIO_THRESHOLD_EXTRA_MS;
    int threshold_ms = (current_time_ms + extra_time_ms);
    return threshold_ms;
}

void FFmpegProvider::signalPcmAvailable()
{
    // Check if we need to fillup more in the audiobuffer
    // if so, we emit the signal
    if (_ffmpeg->audio_queue.size() > 0) {
        int i, N;
        for(i = 1, N = _ffmpeg->audio_queue.size(); i < N && _ffmpeg->audio_queue[i].clear; i++);

        if (i == N) {
            // do nothing
        } else {
            FFmpegAudio &audio = _ffmpeg->audio_queue[i];

            int pos_in_ms = audio.position_in_ms;
            int threshold_ms = audioThresholdMs();

            if (threshold_ms >= pos_in_ms || audio.clear) {
                emit pcmAvailable();
            }
        }
    }
}

void FFmpegProvider::signalClearAudioBuffer()
{
    _ffmpeg->audio_queue.clear();

    FFmpegAudio au;
    au.position_in_ms = -1;
    au.clear = true;

    _ffmpeg->audio_queue.enqueue(au);
}

void FFmpegProvider::signalClearVideoBuffer()
{
    _ffmpeg->image_queue.clear();
}

void FFmpegProvider::signalSetState(FFmpegProvider::State s)
{
    emit setState(s);
}

void FFmpegProvider::audiobClearBuf()
{
    if (_ffmpeg->sdl) {
        SdlBuf *buf = _ffmpeg->sdl_buf;
        if (buf) {
            buf->mutex.lock();
            buf->audiobuf.clear();
            buf->mutex.unlock();
        }
    } else { // Qt
        // _ffmpeg->audio_out->reset();
    }
}

int FFmpegProvider::audiobBufSizeInMs()
{
    int size;
    if (_ffmpeg->sdl) {
        SdlBuf *buf = _ffmpeg->sdl_buf;
        size = 0;
        if (buf) {
            buf->mutex.lock();
            size = buf->audiobuf.size();
            buf->mutex.unlock();
        }
    } else { // Qt
        size = 0;
        if (_ffmpeg->audio_out) {
            size = _ffmpeg->audio_out->bytesFree();
        }
    }

    int samples_in_buffer = size / 2 / 2;  // 16bit, 2 channels
    int ms_in_buffer = (samples_in_buffer / (44100 / 1000)); // 44100 sample rate
    return ms_in_buffer;
}

void FFmpegProvider::audiobPutAudio(const QByteArray &samples)
{
    if (_ffmpeg->sdl) {
        SdlBuf *buf = _ffmpeg->sdl_buf;
        if (buf) {
            buf->mutex.lock();
            buf->audiobuf.append(samples);
            buf->mutex.unlock();
        }

        lib_sdl->SDL_PauseAudioDevice(_ffmpeg->sdl_id, 0);
    } else { // Qt
        if (_ffmpeg->audio_io == nullptr) {
            if (_ffmpeg->audio_out) {
                _ffmpeg->audio_io = _ffmpeg->audio_out->start();
            }
        }
        if (_ffmpeg->audio_io) {
            _ffmpeg->audio_io->write(samples);
        }
    }
}

void FFmpegProvider::handleAudioAvailable()
{
    _ffmpeg->mutex.lock();

    if (_ffmpeg->audio_queue.size() > 0) {

        int threshold_ms = audioThresholdMs();
        bool buffer_off_checked = false;
        bool prev_was_clear = false;

        while(_ffmpeg->audio_queue.size() > 0 &&
              (((_ffmpeg->audio_queue.first().position_in_ms) <= threshold_ms) || _ffmpeg->audio_queue.first().clear)
             ) {

            //LINE_DEBUG << _ffmpeg->audio_queue.first().position_in_ms;

            FFmpegAudio &au = _ffmpeg->audio_queue.first();

            if (au.clear) {
                if (!prev_was_clear) {
                    audiobClearBuf();
                    prev_was_clear = true;
                }
            } else if (!buffer_off_checked) {
                prev_was_clear = false;
                int ms_in_buffer = audiobBufSizeInMs();
                int max_ms_off = AUDIO_MAX_OFF_MS;
                if (ms_in_buffer > max_ms_off) {
                    audiobClearBuf();
                }
                buffer_off_checked = true;
            }

            if (au.audio.size() > 0) {
                audiobPutAudio(au.audio);
            }

            _ffmpeg->audio_queue.dequeue();
        }
    }

    _ffmpeg->mutex.unlock();
}

void FFmpegProvider::handleSetState(FFmpegProvider::State s)
{
    setState(s);
}

void sdl_audio_callback(void *user_data, uint8_t *stream, int len)
{
    lib_sdl->SDL_memset(stream, 0, len);

    SdlBuf *buf = reinterpret_cast<SdlBuf *>(user_data);

    if (buf) {
        buf->mutex.lock();

        int vol_p = buf->volume_percent;
        int mixlen = (len < buf->audiobuf.size()) ? len : buf->audiobuf.size();
        SDL_AudioFormat fmt = buf->format;

        // make vol act logarithmic
        double pow2 = log2(SDL_MIX_MAXVOLUME);
        double div = 100 / pow2;
        double exp_vol = pow(2, vol_p / div);   // min = 1, max = 128
        int v = static_cast<int>(round(exp_vol));
        if (exp_vol < 1.01) { v = 0; }
        int vol = (buf->muted) ? 0 : v;

        QByteArray b(buf->audiobuf.left(mixlen));
        buf->audiobuf = buf->audiobuf.mid(mixlen);

        //int remain = buf->audiobuf.size();

        buf->mutex.unlock();

        //LINE_DEBUG << vol << mixlen << b.size() << remain << len;

        lib_sdl->SDL_MixAudioFormat(stream, reinterpret_cast<uint8_t *>(b.data()), fmt, mixlen, vol);
    }
}

void FFmpegProvider::waitFor(FFmpegProvider::State s)
{
    AD(_decoder->waitForState(DecoderThread::toDecoderState(s)));
}

void FFmpegProvider::prepare(qint64 seek, std::function<void (qint64, bool *)> cb)
{
    cb(seek, nullptr);
}

const FFmpegProvider::Info &FFmpegProvider::mediaInfo() const
{
    return _info;
}

void FFmpegProvider::setVideoSurfaceSize(int w, int h)
{
    _surface_size = QSize(w, h);
}

QSize FFmpegProvider::getVideoSurfaceSize() const
{
    return _surface_size;
}

QImage *FFmpegProvider::getImage(bool &gotIt)
{
    if (_can_render) {
        _ffmpeg->mutex.lock();

        if (_ffmpeg->image_queue.size() > 0) {
            FFmpegImage &fimg = _ffmpeg->image_queue.first();
            _ffmpeg->mutex.unlock();
            gotIt = true;
            return &fimg.image;
        }

        _ffmpeg->mutex.unlock();
    }

    gotIt = false;
    return nullptr;
}

void FFmpegProvider::popImage()
{
    _ffmpeg->mutex.lock();

    if (_ffmpeg->image_queue.size() > 0) {
        _ffmpeg->image_queue.dequeue();
    }

    _ffmpeg->mutex.unlock();
}


void FFmpegProvider::renderVideo(QPainter *p)
{
    if (_can_render) {
        _ffmpeg->mutex.lock();

        if (_ffmpeg->image_queue.size() > 0) {
            FFmpegImage &fimg = _ffmpeg->image_queue.first();

            QSize img_s(fimg.image.size());
            QSize img_p_s(img_s.scaled(_surface_size, Qt::KeepAspectRatio));

            int top = (_surface_size.height() - img_p_s.height()) / 2;
            int left = (_surface_size.width() - img_p_s.width()) / 2;

            QRect img_r(QPoint(left, top), img_p_s);
            p->drawImage(img_r, fimg.image, fimg.image.rect());

            _ffmpeg->image_queue.dequeue();
        }

        _ffmpeg->mutex.unlock();
    }
}



void FFmpegProvider::foreignGLContextDestroyed()
{
    //_can_render = false;
}

void FFmpegProvider::signalError(Error e, const QString &msg, const char *func, int line)
{
    qWarning() << func << line << e << msg;
}

void FFmpegProvider::threadError(Error e, const QString &msg, const char *func, int line)
{
    _ffmpeg->mutex.lock();
    signalError(e, msg, func, line);
    _ffmpeg->mutex.unlock();
}

void FFmpegProvider::stopThreads()
{
    AD(_decoder->endDecoder());
    AD(_decoder->wait());
    AD(delete _decoder);
    _decoder = nullptr;
}

void FFmpegProvider::startThreads()
{
    _decoder = new DecoderThread(this, _ffmpeg, &_ffmpeg->mutex);
    AD(_decoder->start());
}

void FFmpegProvider::resetProvider()
{
    _info.size = 0;
    _info.duration = 0;
    _info.has_audio = false;
    _info.has_video = false;
    _info.metadata.clear();
    // maybe we need to cleanup stuff here...

    _info.audio.bit_rate = 0;
    _info.audio.channels = 0;
    _info.audio.sample_rate = 0;
    _info.audio.codec = "none";

    _info.video.bit_rate = 0;
    _info.video.frame_rate = 0;
    _info.video.height = 0;
    _info.video.width = 0;
    _info.video.codec = "none";

    // _ffmpeg stuff
    _ffmpeg->mutex.lock();

    _ffmpeg->seek_frame = -1;

    if (_ffmpeg->pAudioCtx != nullptr) {
        avcodec_free_context(&_ffmpeg->pAudioCtx);
    }
    if (_ffmpeg->pVideoCtx != nullptr) {
        avcodec_free_context(&_ffmpeg->pVideoCtx);
    }
    if (_ffmpeg->pFormatCtx != nullptr) {
        avformat_close_input(&_ffmpeg->pFormatCtx);
        _ffmpeg->pFormatCtx = nullptr;
    }
    if (_ffmpeg->pFrame != nullptr) {
        av_free(_ffmpeg->pFrame);
        _ffmpeg->pFrame = nullptr;
    }
    if (_ffmpeg->pFrameRGB != nullptr) {
        av_free(_ffmpeg->pFrameRGB);
        _ffmpeg->pFrameRGB = nullptr;
    }
    if (_ffmpeg->buffer != nullptr) {
        av_free(_ffmpeg->buffer);
        _ffmpeg->buffer = nullptr;
    }
    _ffmpeg->pVideoCodec = nullptr;
    _ffmpeg->pAudioCodec = nullptr;

    _ffmpeg->position_in_ms = 0;
    _ffmpeg->image_queue.clear();
    _ffmpeg->audio_queue.clear();
    _ffmpeg->pos_offset_in_ms = 0;
    _ffmpeg->elapsed.invalidate();

    if (_ffmpeg->sdl) {
        if (_ffmpeg->sdl_id != 0) {
            lib_sdl->SDL_CloseAudioDevice(_ffmpeg->sdl_id);
            _ffmpeg->sdl_id = 0;
            delete _ffmpeg->sdl_buf;
            _ffmpeg->sdl_buf = nullptr;
        }
    } else { // Qt
        if (_ffmpeg->audio_out != nullptr) {
            _ffmpeg->audio_out->stop();
            _ffmpeg->audio_out->deleteLater();
            _ffmpeg->audio_out = nullptr;
            _ffmpeg->audio_io = nullptr;
        }
    }

    _ffmpeg->mutex.unlock();
}

/*******************************************************************************
 * Our internal FFmpeg class for keeping track of all needed data
 *******************************************************************************/

FFmpeg::FFmpeg()
{
    pFormatCtx = nullptr;
    pVideoCodec = nullptr;
    pAudioCodec = nullptr;
    pVideoCtx = nullptr;
    pAudioCtx = nullptr;
    pFrame = nullptr;
    pFrameRGB = nullptr;
    audio_stream_index = -1;
    video_stream_index = -1;
    buffer = nullptr;
    position_in_ms = 0;
    pos_offset_in_ms = 0;
    seek_frame = -1;
    sdl = (lib_sdl != nullptr);
    sdl_id = 0;
    sdl_buf = nullptr;
    audio_out = nullptr;
    audio_io = nullptr;
    volume_percent = 100;
    muted = false;
}

/*******************************************************************************
 * Our internal DecoderThread to use ffmpeg to decode our input stream
 *******************************************************************************/

DecoderThread::DecoderThread(FFmpegProvider *p, FFmpeg *ffmpeg, QMutex *mutex)
{
    _provider = p;
    _ffmpeg = ffmpeg;
    _mutex = mutex;
    _run = true;
    _request = Stopped;
    _current = Stopped;
}

DecoderThread::PlayState DecoderThread::toDecoderState(FFmpegProvider::State s)
{
    DecoderThread::PlayState r;
    switch(s) {
    case FFmpegProvider::Stopped: r = DecoderThread::Stopped;
        break;
    case FFmpegProvider::Playing: r = DecoderThread::Playing;
        break;
    case FFmpegProvider::Paused: r = DecoderThread::Paused;
        break;
    }
    return r;
}

FFmpegProvider::State DecoderThread::toFFmpegState(PlayState s)
{
    switch(s) {
    case Stopped: return FFmpegProvider::Stopped;
    case Playing: return FFmpegProvider::Playing;
    case Paused: return FFmpegProvider::Paused;
    case Ended: return FFmpegProvider::Stopped;
    default: return FFmpegProvider::Stopped;
    }
}

static void setup_array(uint8_t* out[], AVFrame* in_frame, enum AVSampleFormat format, int /*samples*/)
{
    if (av_sample_fmt_is_planar(format)) {
        int i;
        for (i = 0; i < in_frame->channels; i++) {
            out[i] = in_frame->data[i];
        }
    } else {
        out[0] = in_frame->data[0];
    }
}

#define CH_MAX 128

#define ERR(a, b) _provider->threadError(a, b, __FUNCTION__, __LINE__)

void DecoderThread::run()
{
    AVPacket *pkt = av_packet_alloc();

    SwsContext *sws = nullptr;
    SwrContext *swr_ctx = nullptr;
    uint8_t **dst_data = nullptr;

    int max_queue_depth = 20;  // memory usage!
    int min_queue_depth = 10;

    int max_n_samples = -1;
    int dst_linesize;
    QElapsedTimer el;
    int ms_count;

    auto audio_ctx = _ffmpeg->pAudioCtx;
    auto video_ctx = _ffmpeg->pVideoCtx;
    auto format_ctx = _ffmpeg->pFormatCtx;

    if (audio_ctx != nullptr) { // only when there is audio
        swr_ctx = swr_alloc();

        av_opt_set_int(swr_ctx, "in_channel_layout", audio_ctx->channel_layout, 0);
        av_opt_set_int(swr_ctx, "in_sample_rate", audio_ctx->sample_rate, 0);
        av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", audio_ctx->sample_fmt, 0);

        av_opt_set_int(swr_ctx, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
        av_opt_set_int(swr_ctx, "out_sample_rate", 44100, 0);
        av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

        swr_init(swr_ctx);
    }

    auto at_end = [this](int ms) {
        return ms > (_ffmpeg->duration_in_ms - 200);    // Don't finalize till the end, keep 0,2s of lag
    };

    QByteArray tmp_audio_buf;

    int pause_offset_ms = -1;
    bool dont_decode = false;

    while(_run) {

        _mutex->lock();

        if (_request != _current) {
            if (_current == Paused) {
                _ffmpeg->seek_frame = SEEK_CONTINUE;
            }

            if (_request == Paused) {
                if (pause_offset_ms < 0) {
                    pause_offset_ms = _ffmpeg->elapsed.elapsed() + _ffmpeg->pos_offset_in_ms;
                }
            }

            _current = _request;
        }

        if (_ffmpeg->seek_frame >= 0 || _ffmpeg->seek_frame == SEEK_BEGIN || _ffmpeg->seek_frame == SEEK_CONTINUE) {
            bool s_begin = (_ffmpeg->seek_frame == SEEK_BEGIN);
            bool s_continue = (_ffmpeg->seek_frame == SEEK_CONTINUE);

            if (!s_begin && !s_continue) {
                if (_current == Paused) {
                    pause_offset_ms = MS(_ffmpeg->seek_frame);
                } else {
                    _ffmpeg->pos_offset_in_ms = MS(_ffmpeg->seek_frame);
                }
                av_seek_frame(format_ctx, -1, _ffmpeg->seek_frame, AVSEEK_FLAG_FRAME);
            } else if (s_begin) {
                _ffmpeg->pos_offset_in_ms = MS(0);
            } else if (s_continue) {
                _ffmpeg->pos_offset_in_ms = pause_offset_ms;
                pause_offset_ms = -1;
            }

            _ffmpeg->elapsed.start();
            _ffmpeg->seek_frame = -1;

            if (!s_continue) {
                if (video_ctx != nullptr) avcodec_flush_buffers(video_ctx);
                if (audio_ctx != nullptr) avcodec_flush_buffers(audio_ctx);
                _provider->signalClearAudioBuffer();
                _provider->signalClearVideoBuffer();
            }
        }

        _mutex->unlock();

        if (_current == Ended) {
            if (_ffmpeg->image_queue.size() > 0) {
                _mutex->lock();
                _provider->signalImageAvailable();  // make sure we're trying to handle our video images
                _provider->signalPcmAvailable();
                _mutex->unlock();
                msleep(1);
            } else {
                msleep(10);
                _mutex->lock();
                _ffmpeg->seek_frame = 0;
                _mutex->unlock();
                _request = Playing;
                el.start();
                ms_count = 200;     // Play for ms_count ms
            }
        } else if (_current == Paused) {
            msleep(100);
        } else if (_current == Stopped) {
            msleep(100);
        } else { // Playing
            if (el.isValid()) {
                if (el.elapsed() >= ms_count) {
                    _request = Stopped;
                    el.invalidate();
                    ms_count = -1;
                    _provider->signalSetState(toFFmpegState(Stopped));
                }
            }

            // Check if the queue > max_queue_depth
            _mutex->lock();
            int queue_depth = _ffmpeg->image_queue.size();
            _mutex->unlock();

            if (dont_decode) {
                if (queue_depth <= min_queue_depth) {
                    dont_decode = false;
                }
            } else {
                if (queue_depth >= max_queue_depth) {
                    dont_decode = true;
                }
            }

            if (dont_decode) {
                _mutex->lock();
                _provider->signalImageAvailable();  // make sure we're trying to handle our video images
                _provider->signalPcmAvailable();
                _mutex->unlock();
                msleep(3);  // frequency = 333Hz max
            } else {
                _mutex->lock();

                // Read from ffmpeg
                int ret = av_read_frame(format_ctx, pkt);


                if (ret == 0) {
                    if (pkt->stream_index == _ffmpeg->audio_stream_index) {

                        auto frame = _ffmpeg->pFrame;

                        int res = avcodec_send_packet(audio_ctx, pkt);
                        if (res < 0) {
                            ERR(FFmpegProvider::Internal, tr("Cannot send packet to audio controller"));
                            _request = Ended;
                        } else {
                            AVRational millisecondbase = { 1, 1000 };
                            int audio_position_in_ms = av_rescale_q(pkt->dts, format_ctx->streams[_ffmpeg->audio_stream_index]->time_base, millisecondbase);

                            if (at_end(audio_position_in_ms)) {
                                _request = Ended;
                            }

                            while(res >= 0) {
                                res = avcodec_receive_frame(audio_ctx, frame); // decodes to RAW PCM?

                                if (res >= 0) {
                                    int n_channels = av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO);

                                    int n_samples;
                                    if (max_n_samples == -1) {
                                        n_samples = av_rescale_rnd(frame->nb_samples, 44100, audio_ctx->sample_rate, AV_ROUND_UP);
                                        max_n_samples = n_samples;
                                        res = av_samples_alloc_array_and_samples(&dst_data, &dst_linesize, n_channels, n_samples, AV_SAMPLE_FMT_S16, 0);
                                        if (res < 0) {
                                            ERR(FFmpegProvider::Internal, tr("Cannot allocate dst_data"));
                                        }
                                    } else {
                                        n_samples = av_rescale_rnd(swr_get_delay(swr_ctx, audio_ctx->sample_rate) + frame->nb_samples,
                                                                   44100, audio_ctx->sample_rate, AV_ROUND_UP
                                                                   );
                                        if (n_samples > max_n_samples) {
                                            av_freep(&dst_data[0]);
                                            res = av_samples_alloc(dst_data, &dst_linesize, n_channels, n_samples, AV_SAMPLE_FMT_S16, 1);
                                            if (res < 0) {
                                                ERR(FFmpegProvider::Internal, tr("Cannot allocate dst_data again"));
                                            }
                                            max_n_samples = n_samples;
                                        }
                                    }

                                    uint8_t *tmp_in[CH_MAX];
                                    setup_array(reinterpret_cast<uint8_t **>(tmp_in), frame, audio_ctx->sample_fmt, frame->nb_samples);
                                    int r = swr_convert(swr_ctx, dst_data, n_samples, const_cast<const uint8_t **>(reinterpret_cast<uint8_t **>(tmp_in)), frame->nb_samples);
                                    if (r < 0) {
                                        ERR(FFmpegProvider::Internal, tr("Conversion error"));
                                    } else {
                                        char *out = reinterpret_cast<char *>(dst_data[0]);
                                        int bufsize = av_samples_get_buffer_size(&dst_linesize, n_channels, r, AV_SAMPLE_FMT_S16, 1);

                                        tmp_audio_buf.append(out, bufsize);
                                        while((r = swr_convert(swr_ctx, dst_data, n_samples, NULL, 0)) > 0) {
                                            bufsize = av_samples_get_buffer_size(&dst_linesize, n_channels, r, AV_SAMPLE_FMT_S16, 1);
                                            tmp_audio_buf.append(out, bufsize);
                                        }
                                    }
                                }
                            }

                            FFmpegAudio au;
                            au.audio = tmp_audio_buf;
                            au.position_in_ms = audio_position_in_ms;
                            au.clear = false;
                            _ffmpeg->audio_queue.enqueue(au);
                            _provider->signalPcmAvailable();

                            tmp_audio_buf.clear();
                        }
                    } else if (pkt->stream_index == _ffmpeg->video_stream_index) {
                        int res = avcodec_send_packet(video_ctx, pkt);
                        if (res < 0) {
                            ERR(FFmpegProvider::Internal, tr("Cannot send packet to video controller"));
                            _request = Ended;
                        } else {
                            res = avcodec_receive_frame(video_ctx, _ffmpeg->pFrame);
                            if (res == 0) {

                                AVRational millisecondbase = { 1, 1000 };
                                _ffmpeg->position_in_ms = av_rescale_q(pkt->dts, format_ctx->streams[_ffmpeg->video_stream_index]->time_base, millisecondbase);
                                if (at_end(_ffmpeg->position_in_ms)) {
                                    _request = Ended;
                                }


                                AVCodecContext *ctx = video_ctx;
                                int w = ctx->width;
                                int h = ctx->height;

                                int flags = SWS_BILINEAR; // SWS_POINT;  // SWS_FAST_BILINEAR;      // SWS_BILINEAR

                                sws = sws_getCachedContext(sws, w, h, ctx->pix_fmt, w, h, VIDEO_FORMAT, flags, NULL, NULL, NULL);

                                FFmpegImage fimg;
                                fimg.image = QImage(w, h, QImage::Format_RGB32);

                                if (sws == nullptr) {
                                    ERR(FFmpegProvider::Internal, tr("Cannot initialize conversion context"));
                                    _request = Ended;
                                } else {
                                    unsigned char *img[8] = { fimg.image.bits() };
                                    int rgb_linesize[8] = { 0 };
                                    rgb_linesize[0] = w * 4;
                                    sws_scale(sws, _ffmpeg->pFrame->data, _ffmpeg->pFrame->linesize, 0, h, img, rgb_linesize);
                                }

                                fimg.position_in_ms = _ffmpeg->position_in_ms;
                                _ffmpeg->image_queue.enqueue(fimg);

                                _provider->signalImageAvailable();
                            }
                        }
                    }
                } else {
                    if (ret == AVERROR_EOF) {
                        ERR(FFmpegProvider::Internal, tr("End of stream."));
                        _request = Ended;
                    } else {
                        ERR(FFmpegProvider::Internal, tr("Unclear %1").arg(ret));
                        _request = Ended;
                    }
                }

                _mutex->unlock();
            }
        }
    }

    if (dst_data) {
        av_freep(&dst_data[0]);
        av_freep(&dst_data);
    }
    av_packet_unref(pkt);
    if (swr_ctx) {
        swr_free(&swr_ctx);
    }
}

void DecoderThread::endDecoder()
{
    _mutex->lock();
    _run = false;
    _mutex->unlock();
}

void DecoderThread::requestPlayState(PlayState s)
{
    _mutex->lock();
    _request = s;
    _mutex->unlock();
}

void DecoderThread::waitForRequest()
{
    while(true) {
        bool reached = false;
        _mutex->lock();
        if (_current == _request) { reached = true; }
        _mutex->unlock();
        if (reached) { return; }
        QThread::msleep(10);
    }
}

void DecoderThread::waitForState(PlayState s)
{
    while(true) {
        bool reached = false;
        _mutex->lock();
        if (_current == s) { reached = true; }
        _mutex->unlock();
        if (reached) { return; }
        QThread::msleep(10);
    }
}

/*****************************************************************
 * SDL Dynamic loading
 *****************************************************************/

static void sdl_set(void **a, void *b) { *a = b; }

#define LDRS(a, c) \
    sdl_set(reinterpret_cast<void **>(&sdl.a), reinterpret_cast<void *>(lib.resolve(#a))); \
    if (!c || sdl.a == nullptr) c = false; \
    LINE_INFO << "Loading SDL function" << #a << " result: " << c;

static LibSdl *loadSdl()
{
    static LibSdl sdl;

    //QString sdl_path = QProcessEnvironment::systemEnvironment().value("SDL_LIB_PATH", "");

    QStringList libs = QStringList() << "SDL2" << "libsdl2" << "libSDL2" <<
                                         "SDL" << "libsdl" << "libSDL";
    QStringList exts = QStringList() << ".dll" << ".so" << ".dylib" << ".bundle" << ".a" << ".sl";

    QString the_lib = "";
    int i, N;
    for(i = 0, N = libs.size(); i < N && the_lib == ""; i++) {
        int j, M;
        for(j = 0, M = exts.size(); j < M && the_lib == ""; j++) {
            QString ll = libs[i] + exts[j];
            QLibrary l(ll);
            LINE_INFO << "Checking for SDL using:" << ll;
            if (l.load()) {
                LINE_INFO << "This library can be loaded";
                the_lib = ll;
            }
        }
    }

    if (the_lib != "") {
        QLibrary lib(the_lib);
        bool l = true;
        LDRS(SDL_Init, l);
        LDRS(SDL_GetError, l);
        LDRS(SDL_OpenAudioDevice, l);
        LDRS(SDL_PauseAudioDevice, l);
        LDRS(SDL_memset, l);
        LDRS(SDL_MixAudioFormat, l);
        LDRS(SDL_CloseAudioDevice, l);
        LDRS(SDL_GetVersion, l);

        if (!l) {
            LINE_INFO << "SDL Library found, but cannot load all functions";
            return nullptr;
        }

        if (sdl.SDL_GetVersion) {
            SDL_version v;
            sdl.SDL_GetVersion(&v);
            LINE_INFO << "Loaded SDL Version: " << v.major << "." << v.minor << "." << v.patch;
            if (v.major >= 2) {
                LINE_INFO << "Valid SDL version as far as we can see.";
                return &sdl;
            }
        }

        return nullptr;
    } else {
        LINE_INFO << "No SDL backend to be dynamically loaded found";
        return nullptr;
    }
}

