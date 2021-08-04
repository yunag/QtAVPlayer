/*********************************************************
 * Copyright (C) 2020, Val Doroshchuk <valbok@gmail.com> *
 *                                                       *
 * This file is part of QtAVPlayer.                      *
 * Free Qt Media Player based on FFmpeg.                 *
 *********************************************************/

#include "qavplayer.h"
#include "qavdemuxer_p.h"
#include "qavvideocodec_p.h"
#include "qavaudiocodec_p.h"
#include "qavvideoframe.h"
#include "qavaudioframe.h"
#include "qavpacketqueue_p.h"
#include <QtConcurrent/qtconcurrentrun.h>
#include <QLoggingCategory>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
}

QT_BEGIN_NAMESPACE

Q_LOGGING_CATEGORY(lcAVPlayer, "qt.QtAVPlayer")

class QAVPlayerPrivate
{
    Q_DECLARE_PUBLIC(QAVPlayer)
public:
    QAVPlayerPrivate(QAVPlayer *q)
        : q_ptr(q)
    {
        threadPool.setMaxThreadCount(4);
    }

    void setMediaStatus(QAVPlayer::MediaStatus status);
    void event(std::function<bool(bool)> fn = std::function<bool(bool)>());
    void processEvents(bool tick);
    bool setState(QAVPlayer::State s);
    void setSeekable(bool seekable);
    void setError(QAVPlayer::Error err, const QString &str);
    void setDuration(double d);
    bool isSeeking() const;
    void setVideoFrameRate(double v);

    void terminate();

    void doWait();
    void wait(bool v);
    void doLoad(const QUrl &url);
    void doDemux();
    void doPlayVideo();
    void doPlayAudio();

    template <class T>
    void dispatch(T fn);

    QAVPlayer *q_ptr = nullptr;
    QUrl url;
    QAVPlayer::MediaStatus mediaStatus = QAVPlayer::NoMedia;
    QAVPlayer::State state = QAVPlayer::StoppedState;
    mutable QMutex stateMutex;
    QMutex eventsMutex;
    QList<std::function<bool(bool)>> events;

    bool seekable = false;
    qreal speed = 1.0;
    mutable QMutex speedMutex;
    double videoFrameRate = 0.0;

    QAVPlayer::Error error = QAVPlayer::NoError;
    QString errorString;

    double duration = 0;
    double pendingPosition = -1;
    mutable QMutex positionMutex;

    QAVDemuxer demuxer;

    QThreadPool threadPool;
    QFuture<void> loaderFuture;
    QFuture<void> demuxerFuture;

    QFuture<void> videoPlayFuture;
    QAVPacketQueue videoQueue;

    QFuture<void> audioPlayFuture;
    QAVPacketQueue audioQueue;
    double audioPts = 0;

    bool quit = 0;
    bool isWaiting = false;
    mutable QMutex waitMutex;
    QWaitCondition waitCond;
};

static QString err_str(int err)
{
    char errbuf[128];
    const char *errbuf_ptr = errbuf;
    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));

    return QString::fromUtf8(errbuf_ptr);
}

void QAVPlayerPrivate::setMediaStatus(QAVPlayer::MediaStatus status)
{
    {
        QMutexLocker locker(&stateMutex);
        if (mediaStatus == status)
            return;

        qCDebug(lcAVPlayer) << __FUNCTION__ << ":" << mediaStatus << "->" << status;
        mediaStatus = status;
    }

    emit q_ptr->mediaStatusChanged(status);
}

void QAVPlayerPrivate::event(std::function<bool(bool)> fn)
{
    QMutexLocker locker(&eventsMutex);
    if (fn)
        events.append(fn);
}

bool QAVPlayerPrivate::setState(QAVPlayer::State s)
{
    Q_Q(QAVPlayer);
    bool result = false;
    {
        QMutexLocker locker(&stateMutex);
        if (state == s)
            return result;

        qCDebug(lcAVPlayer) << __FUNCTION__ << ":" << state << "->" << s;
        state = s;
        result = true;
    }

    emit q->stateChanged(s);
    return result;
}

void QAVPlayerPrivate::setSeekable(bool s)
{
    Q_Q(QAVPlayer);
    if (seekable == s)
        return;

    qCDebug(lcAVPlayer) << __FUNCTION__ << ":" << seekable << "->" << s;
    seekable = s;
    emit q->seekableChanged(seekable);
}

void QAVPlayerPrivate::setDuration(double d)
{
    Q_Q(QAVPlayer);
    if (qFuzzyCompare(duration, d))
        return;

    qCDebug(lcAVPlayer) << __FUNCTION__ << ":" << duration << "->" << d;
    duration = d;
    emit q->durationChanged(q->duration());
}

bool QAVPlayerPrivate::isSeeking() const
{
    QMutexLocker locker(&positionMutex);
    return pendingPosition >= 0;
}

void QAVPlayerPrivate::setVideoFrameRate(double v)
{
    Q_Q(QAVPlayer);
    if (qFuzzyCompare(videoFrameRate, v))
        return;

    qCDebug(lcAVPlayer) << __FUNCTION__ << ":" << videoFrameRate << "->" << v;
    videoFrameRate = v;
    emit q->videoFrameRateChanged(v);
}

template <class T>
void QAVPlayerPrivate::dispatch(T fn)
{
    QMetaObject::invokeMethod(q_ptr, fn, nullptr);
}

void QAVPlayerPrivate::setError(QAVPlayer::Error err, const QString &str)
{
    Q_Q(QAVPlayer);
    if (error == err)
        return;

    qWarning() << "Error:" << url << ":"<< str;
    error = err;
    errorString = str;
    emit q->errorOccurred(err, str);
    setMediaStatus(QAVPlayer::InvalidMedia);
}

void QAVPlayerPrivate::terminate()
{
    qCDebug(lcAVPlayer) << __FUNCTION__;
    setState(QAVPlayer::StoppedState);
    setMediaStatus(QAVPlayer::NoMedia);
    demuxer.abort();
    quit = true;
    wait(false);
    videoFrameRate = 0.0;
    videoQueue.clear();
    videoQueue.abort();
    audioQueue.clear();
    audioQueue.abort();
    loaderFuture.waitForFinished();
    demuxerFuture.waitForFinished();
    videoPlayFuture.waitForFinished();
    audioPlayFuture.waitForFinished();
    pendingPosition = -1;
}

void QAVPlayerPrivate::processEvents(bool tick)
{
    QMutexLocker locker(&eventsMutex);
    if (events.isEmpty() || isSeeking())
        return;

    while (!events.isEmpty()) {
        auto event = events.first();
        locker.unlock();
        if (!event(tick))
            break;
        locker.relock();
        events.removeFirst();
    }
}

void QAVPlayerPrivate::doWait()
{
    QMutexLocker lock(&waitMutex);
    if (isWaiting)
        waitCond.wait(&waitMutex);
}

void QAVPlayerPrivate::wait(bool v)
{
    {
        QMutexLocker locker(&waitMutex);
        isWaiting = v;
    }

    if (!v)
        waitCond.wakeAll();
    videoQueue.wakeAll();
    audioQueue.wakeAll();
}

void QAVPlayerPrivate::doLoad(const QUrl &url)
{
    demuxer.abort(false);
    demuxer.unload();
    int ret = demuxer.load(url);
    if (ret < 0) {
        dispatch([this, ret] { setError(QAVPlayer::ResourceError, err_str(ret)); });
        return;
    }

    if (demuxer.videoStream() < 0 && demuxer.audioStream() < 0) {
        dispatch([this] { setError(QAVPlayer::ResourceError, QLatin1String("No codecs found")); });
        return;
    }

    double d = demuxer.duration();
    bool seekable = demuxer.seekable();
    double frameRate = demuxer.frameRate();
    dispatch([this, d, seekable, frameRate] {
        qCDebug(lcAVPlayer) << "[" << this->url << "]: Loaded, seekable:" << seekable << ", duration:" << d;
        setSeekable(seekable);
        setDuration(d);
        setVideoFrameRate(frameRate);
        setMediaStatus(QAVPlayer::LoadedMedia);
        qCDebug(lcAVPlayer) << "Process all state events";
        processEvents(true);
    });

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    demuxerFuture = QtConcurrent::run(&threadPool, this, &QAVPlayerPrivate::doDemux);
    if (q_ptr->hasVideo())
        videoPlayFuture = QtConcurrent::run(&threadPool, this, &QAVPlayerPrivate::doPlayVideo);
    if (q_ptr->hasAudio())
        audioPlayFuture = QtConcurrent::run(&threadPool, this, &QAVPlayerPrivate::doPlayAudio);
#else
    demuxerFuture = QtConcurrent::run(&threadPool, &QAVPlayerPrivate::doDemux, this);
    if (q_ptr->hasVideo())
        videoPlayFuture = QtConcurrent::run(&threadPool, &QAVPlayerPrivate::doPlayVideo, this);
    if (q_ptr->hasAudio())
        audioPlayFuture = QtConcurrent::run(&threadPool, &QAVPlayerPrivate::doPlayAudio, this);
#endif
}

void QAVPlayerPrivate::doDemux()
{
    const int maxQueueBytes = 15 * 1024 * 1024;
    QMutex waiterMutex;
    QWaitCondition waiter;

    while (!quit) {
        doWait();
        if (videoQueue.bytes() + audioQueue.bytes() > maxQueueBytes
            || (videoQueue.enough() && audioQueue.enough()))
        {
            QMutexLocker locker(&waiterMutex);
            waiter.wait(&waiterMutex, 10);
            continue;
        }

        {
            QMutexLocker locker(&positionMutex);
            if (pendingPosition >= 0) {
                const double pos = pendingPosition;
                locker.unlock();
                qCDebug(lcAVPlayer) << "Seeking to pos:" << pos * 1000;
                const int ret = demuxer.seek(pos);
                if (ret >= 0) {
                    videoQueue.clear();
                    audioQueue.clear();
                    qCDebug(lcAVPlayer) << "Waiting video thread finished processing packets";
                    videoQueue.waitForEmpty();
                    qCDebug(lcAVPlayer) << "Waiting audio thread finished processing packets";
                    audioQueue.waitForEmpty();
                    qCDebug(lcAVPlayer) << "Start reading packets from" << pos * 1000;
                } else {
                    qWarning() << "Could not seek:" << err_str(ret);
                }
                locker.relock();
                if (qFuzzyCompare(pendingPosition, pos))
                    pendingPosition = -1;
            }
        }

        auto packet = demuxer.read();
        if (!packet) {
            if (demuxer.eof() && videoQueue.isEmpty() && audioQueue.isEmpty() && !videoQueue.finished() && !audioQueue.finished()) {
                if (q_ptr->hasVideo())
                    videoQueue.finish();
                if (q_ptr->hasAudio())
                    audioQueue.finish();
                dispatch([this] {
                    qCDebug(lcAVPlayer) << "EndOfMedia";
                    setMediaStatus(QAVPlayer::EndOfMedia);
                    q_ptr->stop();
                });
            }

            QMutexLocker locker(&waiterMutex);
            waiter.wait(&waiterMutex, 10);
            continue;
        }

        if (packet.streamIndex() == demuxer.videoStream())
            videoQueue.enqueue(packet);
        else if (packet.streamIndex() == demuxer.audioStream())
            audioQueue.enqueue(packet);
    }
}

void QAVPlayerPrivate::doPlayVideo()
{
    videoQueue.setFrameRate(demuxer.frameRate());

    while (!quit) {
        doWait();
        QAVVideoFrame frame = videoQueue.sync(q_ptr->speed(), audioQueue.pts());
        if (frame) {
            emit q_ptr->videoFrame(frame);
            videoQueue.pop();
        }
        processEvents(frame);
    }

    emit q_ptr->videoFrame({});
    videoQueue.clear();
}

void QAVPlayerPrivate::doPlayAudio()
{
    const bool hasVideo = q_ptr->hasVideo();

    while (!quit) {
        doWait();
        const qreal currSpeed = q_ptr->speed();
        QAVAudioFrame frame = audioQueue.sync(currSpeed);
        if (frame) {
            frame.frame()->sample_rate *= currSpeed;
            emit q_ptr->audioFrame(frame);
            audioQueue.pop();
        }

        if (!hasVideo)
            processEvents(frame);
    }

    audioQueue.clear();
}

QAVPlayer::QAVPlayer(QObject *parent)
    : QObject(parent)
    , d_ptr(new QAVPlayerPrivate(this))
{
}

QAVPlayer::~QAVPlayer()
{
    Q_D(QAVPlayer);
    d->terminate();
}

void QAVPlayer::setSource(const QUrl &url)
{
    Q_D(QAVPlayer);
    if (d->url == url)
        return;

    qCDebug(lcAVPlayer) << __FUNCTION__ << ":" << url;
    d->terminate();
    d->url = url;
    emit sourceChanged(url);
    if (d->url.isEmpty()) {
        d->setMediaStatus(QAVPlayer::NoMedia);
        d->setDuration(0);
        return;
    }

    d->wait(true);
    d->quit = false;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    d->loaderFuture = QtConcurrent::run(&d->threadPool, d, &QAVPlayerPrivate::doLoad, d->url);
#else
    d->loaderFuture = QtConcurrent::run(&d->threadPool, &QAVPlayerPrivate::doLoad, d, d->url);
#endif
}

QUrl QAVPlayer::source() const
{
    return d_func()->url;
}

bool QAVPlayer::hasAudio() const
{
    return d_func()->demuxer.audioStream() >= 0;
}

bool QAVPlayer::hasVideo() const
{
    return d_func()->demuxer.videoStream() >= 0;
}

QAVPlayer::State QAVPlayer::state() const
{
    Q_D(const QAVPlayer);
    QMutexLocker locker(&d->stateMutex);
    return d->state;
}

QAVPlayer::MediaStatus QAVPlayer::mediaStatus() const
{
    Q_D(const QAVPlayer);
    QMutexLocker locker(&d->stateMutex);
    return d->mediaStatus;
}

void QAVPlayer::play()
{
    Q_D(QAVPlayer);
    if (d->url.isEmpty() || mediaStatus() == QAVPlayer::InvalidMedia)
        return;

    qCDebug(lcAVPlayer) << __FUNCTION__;
    auto status = mediaStatus();
    if (status == QAVPlayer::LoadedMedia || status == QAVPlayer::EndOfMedia) {
        if (d->setState(QAVPlayer::PlayingState)) {
            if (status == QAVPlayer::EndOfMedia) {
                qCDebug(lcAVPlayer) << "Playing from beginning";
                seek(0);
            }

            d->event([this, d](bool tick) {
                d->wait(false);
                if (!tick && mediaStatus() != EndOfMedia)
                    return false;
                qCDebug(lcAVPlayer) << "Played from pos:" << position();
                emit played(position());
                return true;
            });
        }
        d->wait(false);
    } else {
        qCDebug(lcAVPlayer) << status << ": not loaded yet, postponing playing until loaded";
        d->event([this](bool) {
            qCDebug(lcAVPlayer) << "Starting pending playing";
            play();
            return true;
        });
    }
}

void QAVPlayer::pause()
{
    Q_D(QAVPlayer);
    qCDebug(lcAVPlayer) << __FUNCTION__;
    auto status = mediaStatus();
    if (status == QAVPlayer::LoadedMedia || status == QAVPlayer::EndOfMedia) {
        if (status == QAVPlayer::EndOfMedia) {
            qCDebug(lcAVPlayer) << "Pausing from beginning";
            seek(0);
        }
        if (d->setState(QAVPlayer::PausedState)) {
            d->wait(false);
            d->event([this, d](bool tick) {
                if (!tick && mediaStatus() != EndOfMedia)
                    return false;
                qCDebug(lcAVPlayer) << "Paused to pos:" << position();
                emit paused(position());
                d->wait(true);
                return true;
            });
        } else {
            d->wait(true);
        }
    } else {
        qCDebug(lcAVPlayer) << mediaStatus() << ": not loaded yet, postponing pausing until loaded";
        d->event([this](bool) {
            qCDebug(lcAVPlayer) << "Starting pending pause";
            pause();
            return true;
        });
    }
}

void QAVPlayer::stop()
{
    Q_D(QAVPlayer);
    qCDebug(lcAVPlayer) << __FUNCTION__;
    auto status = mediaStatus();
    if (status == QAVPlayer::LoadedMedia || status == QAVPlayer::EndOfMedia) {
        if (d->setState(QAVPlayer::StoppedState)) {
            d->wait(false);
            d->event([this, d](bool) {
                qCDebug(lcAVPlayer) << "Stopped to pos:" << position();
                emit stopped(position());
                if (hasVideo()) {
                    qCDebug(lcAVPlayer) << "Flushing empty video frame";
                    emit videoFrame({});
                }
                d->wait(true);
                return true;
            });
        } else {
            d->wait(true);
        }
    }
}

bool QAVPlayer::isSeekable() const
{
    return d_func()->seekable;
}

void QAVPlayer::seek(qint64 pos)
{
    Q_D(QAVPlayer);
    if (pos < 0 || (duration() > 0 && pos > duration()))
        return;

    qCDebug(lcAVPlayer) << __FUNCTION__ << ":" << "pos:" << pos;
    auto status = mediaStatus();
    if (status == QAVPlayer::LoadedMedia || status == QAVPlayer::EndOfMedia) {
        {
            QMutexLocker locker(&d->positionMutex);
            d->pendingPosition = pos / 1000.0;
        }

        if (status == QAVPlayer::EndOfMedia)
            d->setMediaStatus(QAVPlayer::LoadedMedia);
        d->event([this, d](bool tick) {
            if (!tick || d->isSeeking())
                return false;
            qCDebug(lcAVPlayer) << "Seeked to pos:" << position();
            emit seeked(position());
            QAVPlayer::State currState = state();
            if (currState == QAVPlayer::PausedState || currState == QAVPlayer::StoppedState)
                d->wait(true);
            return true;
        });
        d->wait(false);
    } else {
        qCDebug(lcAVPlayer) << mediaStatus() << ": not loaded yet, postponing seeking until loaded";
        d->event([this, pos](bool) {
            qCDebug(lcAVPlayer) << "Starting pending seek" << pos;
            seek(pos);
            return true;
        });
    }
}

qint64 QAVPlayer::duration() const
{
    return d_func()->duration * 1000;
}

qint64 QAVPlayer::position() const
{
    Q_D(const QAVPlayer);

    if (mediaStatus() == QAVPlayer::EndOfMedia)
        return duration();

    QMutexLocker locker(&d->positionMutex);
    if (d->pendingPosition >= 0)
        return d->pendingPosition * 1000;

    double pts = hasVideo() ? d->videoQueue.pts() : d->audioQueue.pts();
    return pts * 1000;
}

void QAVPlayer::setSpeed(qreal r)
{
    Q_D(QAVPlayer);

    {
        QMutexLocker locker(&d->speedMutex);
        if (qFuzzyCompare(d->speed, r))
            return;

        qCDebug(lcAVPlayer) << __FUNCTION__ << ":" << d->speed << "->" << r;
        d->speed = r;
    }
    emit speedChanged(r);
}

qreal QAVPlayer::speed() const
{
    Q_D(const QAVPlayer);

    QMutexLocker locker(&d->speedMutex);
    return d->speed;
}

double QAVPlayer::videoFrameRate() const
{
    return d_func()->videoFrameRate;
}

QAVPlayer::Error QAVPlayer::error() const
{
    return d_func()->error;
}

QString QAVPlayer::errorString() const
{
    return d_func()->errorString;
}

#ifndef QT_NO_DEBUG_STREAM
QDebug operator<<(QDebug dbg, QAVPlayer::State state)
{
    QDebugStateSaver saver(dbg);
    dbg.nospace();
    switch (state) {
        case QAVPlayer::StoppedState:
            return dbg << "StoppedState";
        case QAVPlayer::PlayingState:
            return dbg << "PlayingState";
        case QAVPlayer::PausedState:
            return dbg << "PausedState";
        default:
            return dbg << QString(QLatin1String("UserType(%1)" )).arg(int(state)).toLatin1().constData();
    }
}

QDebug operator<<(QDebug dbg, QAVPlayer::MediaStatus status)
{
    QDebugStateSaver saver(dbg);
    dbg.nospace();
    switch (status) {
        case QAVPlayer::NoMedia:
            return dbg << "NoMedia";
        case QAVPlayer::LoadedMedia:
            return dbg << "LoadedMedia";
        case QAVPlayer::EndOfMedia:
            return dbg << "EndOfMedia";
        case QAVPlayer::InvalidMedia:
            return dbg << "InvalidMedia";
        default:
            return dbg << QString(QLatin1String("UserType(%1)" )).arg(int(status)).toLatin1().constData();
    }
}
#endif

QT_END_NAMESPACE
