/******************************************************************************
    AudioOutputOpenSL.cpp: description
    Copyright (C) 2012-2014 Wang Bin <wbsecg1@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "QtAV/AudioOutput.h"
#include "QtAV/private/AudioOutput_p.h"
#include <QtCore/QThread>
#include <SLES/OpenSLES.h>
#include "prepost.h"

namespace QtAV {

class AudioOutputOpenSLPrivate;
class AudioOutputOpenSL : public AudioOutput
{
    DPTR_DECLARE_PRIVATE(AudioOutputOpenSL)
public:
    AudioOutputOpenSL();
    ~AudioOutputOpenSL();

    virtual bool isSupported(const AudioFormat& format) const;
    virtual bool isSupported(AudioFormat::SampleFormat sampleFormat) const;
    virtual bool isSupported(AudioFormat::ChannelLayout channelLayout) const;
    virtual AudioFormat::SampleFormat preferredSampleFormat() const;
    virtual AudioFormat::ChannelLayout preferredChannelLayout() const;

    virtual bool open();
    virtual bool close();

    QString name() const;
    void waitForNextBuffer();

protected:
    virtual bool write();
};

extern AudioOutputId AudioOutputId_OpenSL;
FACTORY_REGISTER_ID_AUTO(AudioOutput, OpenSL, "OpenSL")

void RegisterAudioOutputOpenSL_Man()
{
    FACTORY_REGISTER_ID_MAN(AudioOutput, OpenSL, "OpenSL")
}

#define SL_RUN_CHECK_RETURN(FUNC, RET) \
    do { \
        SLresult ret = FUNC; \
        if (ret != SL_RESULT_SUCCESS) { \
            qWarning("AudioOutputOpenSL Error>>> " #FUNC " (%lu)", ret); \
            return RET; \
        } \
    } while(0)
#define SL_RUN_CHECK(FUNC) SL_RUN_CHECK_RETURN(FUNC,)
#define SL_RUN_CHECK_FALSE(FUNC) SL_RUN_CHECK_RETURN(FUNC, false)

static SLDataFormat_PCM audioFormatToSL(const AudioFormat &format)
{
    SLDataFormat_PCM format_pcm;
    format_pcm.formatType = SL_DATAFORMAT_PCM;
    format_pcm.numChannels = format.channels();
    format_pcm.samplesPerSec = format.sampleRate() * 1000;
    format_pcm.bitsPerSample = format.bytesPerSample()*8;
    format_pcm.containerSize = format.bytesPerSample()*8;
    format_pcm.channelMask = (format.channels() == 1 ?
                                  SL_SPEAKER_FRONT_CENTER :
                                  SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT);
    format_pcm.endianness = SL_BYTEORDER_LITTLEENDIAN; //FIXME
    return format_pcm;
}

class  AudioOutputOpenSLPrivate : public AudioOutputPrivate
{
public:
    AudioOutputOpenSLPrivate()
        : AudioOutputPrivate()
        //, format(AL_FORMAT_STEREO16)
        , m_outputMixObject(0)
        , m_playerObject(0)
        , m_playItf(0)
        , m_volumeItf(0)
        , m_bufferQueueItf(0)
        , m_notifyInterval(1000)
        , buffers_queued(0)
        , init_buffers(true)
        , callback_mode(true)
    {
        SL_RUN_CHECK(slCreateEngine(&engineObject, 0, 0, 0, 0, 0));
        SL_RUN_CHECK((*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE));
        SL_RUN_CHECK((*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engine));
        available = false;
    }
    ~AudioOutputOpenSLPrivate() {
    }
    static void bufferQueueCallback(SLBufferQueueItf bufferQueue, void *context)
    {
        SLBufferQueueState state;
        (*bufferQueue)->GetState(bufferQueue, &state);
        qDebug(">>>>>>>>>>>>>>bufferQueueCallback state.count=%lu .playIndex=%lu", state.count, state.playIndex);
        AudioOutputOpenSLPrivate *priv = reinterpret_cast<AudioOutputOpenSLPrivate*>(context);
        if (priv->callback_mode) {
            priv->cond.wakeAll();
        }
    }
    static void playCallback(SLPlayItf player, void *ctx, SLuint32 event)
    {
        Q_UNUSED(player);
        Q_UNUSED(ctx);
        qDebug("---------%s  event=%lu", __FUNCTION__, event);
    }

    SLObjectItf engineObject;
    SLEngineItf engine;
    SLObjectItf m_outputMixObject;
    SLObjectItf m_playerObject;
    SLPlayItf m_playItf;
    SLVolumeItf m_volumeItf;
    SLBufferQueueItf m_bufferQueueItf;
    int m_notifyInterval;
    quint32 buffers_queued;
    bool init_buffers;
    bool callback_mode;
};

AudioOutputOpenSL::AudioOutputOpenSL()
    :AudioOutput(*new AudioOutputOpenSLPrivate())
{
}

AudioOutputOpenSL::~AudioOutputOpenSL()
{
}

bool AudioOutputOpenSL::isSupported(const AudioFormat& format) const
{
    return isSupported(format.sampleFormat()) && isSupported(format.channelLayout());
}

bool AudioOutputOpenSL::isSupported(AudioFormat::SampleFormat sampleFormat) const
{
    return sampleFormat == AudioFormat::SampleFormat_Unsigned8 || sampleFormat == AudioFormat::SampleFormat_Signed16;
}

bool AudioOutputOpenSL::isSupported(AudioFormat::ChannelLayout channelLayout) const
{
    return channelLayout == AudioFormat::ChannelLayout_Mono || channelLayout == AudioFormat::ChannelLayout_Stero;
}

AudioFormat::SampleFormat AudioOutputOpenSL::preferredSampleFormat() const
{
    return AudioFormat::SampleFormat_Signed16;
}

AudioFormat::ChannelLayout AudioOutputOpenSL::preferredChannelLayout() const
{
    return AudioFormat::ChannelLayout_Stero;
}

bool AudioOutputOpenSL::open()
{
    DPTR_D(AudioOutputOpenSL);
    d.init_buffers = true;
    d.available = false;
    SLDataLocator_BufferQueue bufferQueueLocator = { SL_DATALOCATOR_BUFFERQUEUE, (SLuint32)d.nb_buffers };
    SLDataFormat_PCM pcmFormat = audioFormatToSL(audioFormat());
    SLDataSource audioSrc = { &bufferQueueLocator, &pcmFormat };
    // OutputMix
    SL_RUN_CHECK_FALSE((*d.engine)->CreateOutputMix(d.engine, &d.m_outputMixObject, 0, NULL, NULL));
    SL_RUN_CHECK_FALSE((*d.m_outputMixObject)->Realize(d.m_outputMixObject, SL_BOOLEAN_FALSE));
    SLDataLocator_OutputMix outputMixLocator = { SL_DATALOCATOR_OUTPUTMIX, d.m_outputMixObject };
    SLDataSink audioSink = { &outputMixLocator, NULL };

    const int iids = 1;//2;
    const SLInterfaceID ids[iids] = { SL_IID_BUFFERQUEUE};//, SL_IID_VOLUME };
    const SLboolean req[iids] = { SL_BOOLEAN_TRUE};//, SL_BOOLEAN_TRUE };
    // AudioPlayer
    SL_RUN_CHECK_FALSE((*d.engine)->CreateAudioPlayer(d.engine, &d.m_playerObject, &audioSrc, &audioSink, iids, ids, req));
    SL_RUN_CHECK_FALSE((*d.m_playerObject)->Realize(d.m_playerObject, SL_BOOLEAN_FALSE));
    // Buffer interface
    SL_RUN_CHECK_FALSE((*d.m_playerObject)->GetInterface(d.m_playerObject, SL_IID_BUFFERQUEUE, &d.m_bufferQueueItf));
    SL_RUN_CHECK_FALSE((*d.m_bufferQueueItf)->RegisterCallback(d.m_bufferQueueItf, AudioOutputOpenSLPrivate::bufferQueueCallback, &d));
    // Play interface
    SL_RUN_CHECK_FALSE((*d.m_playerObject)->GetInterface(d.m_playerObject, SL_IID_PLAY, &d.m_playItf));
    // call when SL_PLAYSTATE_STOPPED
    SL_RUN_CHECK_FALSE((*d.m_playItf)->RegisterCallback(d.m_playItf, AudioOutputOpenSLPrivate::playCallback, this));

    SLuint32 mask = SL_PLAYEVENT_HEADATEND;
    // TODO: what does this do?
    SL_RUN_CHECK_FALSE((*d.m_playItf)->SetPositionUpdatePeriod(d.m_playItf, 100));
    SL_RUN_CHECK_FALSE((*d.m_playItf)->SetCallbackEventsMask(d.m_playItf, mask));
    // Volume interface
    //SL_RUN_CHECK_FALSE((*d.m_playerObject)->GetInterface(d.m_playerObject, SL_IID_VOLUME, &d.m_volumeItf));

    d.available = true;
    return true;
}

bool AudioOutputOpenSL::close()
{
    DPTR_D(AudioOutputOpenSL);
    d.available = false;
    d.init_buffers = true;
    if (d.m_playItf)
        (*d.m_playItf)->SetPlayState(d.m_playItf, SL_PLAYSTATE_STOPPED);

    if (d.m_bufferQueueItf && SL_RESULT_SUCCESS != (*d.m_bufferQueueItf)->Clear(d.m_bufferQueueItf))
        qWarning("Unable to clear buffer");

    if (d.m_playerObject) {
        (*d.m_playerObject)->Destroy(d.m_playerObject);
        d.m_playerObject = NULL;
    }
    if (d.m_outputMixObject) {
        (*d.m_outputMixObject)->Destroy(d.m_outputMixObject);
        d.m_outputMixObject = NULL;
    }

    d.m_playItf = NULL;
    d.m_volumeItf = NULL;
    d.m_bufferQueueItf = NULL;
    return true;
}

bool AudioOutputOpenSL::write()
{
    DPTR_D(AudioOutputOpenSL);
    if (d.init_buffers) {
        d.init_buffers = false;
        for (quint32 i = 0; i < d.nb_buffers; ++i) {
            SL_RUN_CHECK_FALSE((*d.m_bufferQueueItf)->Enqueue(d.m_bufferQueueItf, d.data.constData(), d.data.size()));
            d.buffers_queued++;
        }
        if (SL_RESULT_SUCCESS != (*d.m_playItf)->SetPlayState(d.m_playItf, SL_PLAYSTATE_PLAYING)) {
            //destroyPlayer();
        }
        return true;
    }
    if (SL_RESULT_SUCCESS != (*d.m_bufferQueueItf)->Enqueue(d.m_bufferQueueItf, d.data.constData(), d.data.size())) {
        qWarning("failed to enqueue");
        return false;
    }
    d.buffers_queued++;
    return true;
}

void AudioOutputOpenSL::waitForNextBuffer()
{
    DPTR_D(AudioOutputOpenSL);
    if (!d.canRemoveBuffer()) {
        return;
    }
    SLBufferQueueState state;
    (*d.m_bufferQueueItf)->GetState(d.m_bufferQueueItf, &state);
    qDebug(">>>>>>>>>>>>>>bufferQueueCallback state.count=%lu .playIndex=%lu", state.count, state.playIndex);
    // number of buffers in queue
    if (state.count <= 0) {
        return;
    }
    if (d.callback_mode) {
        QMutexLocker lock(&d.mutex);
        Q_UNUSED(lock);
        d.cond.wait(&d.mutex);
        d.bufferRemoved();
        return;
    }
    int processed = d.buffers_queued;
    while (state.count >= d.buffers_queued) {
        unsigned long duration = d.format.durationForBytes(d.nextDequeueInfo().data_size)/1000LL;
        QMutexLocker lock(&d.mutex);
        Q_UNUSED(lock);
        d.cond.wait(&d.mutex, duration);
        (*d.m_bufferQueueItf)->GetState(d.m_bufferQueueItf, &state);
    }
    d.buffers_queued = state.count;
    processed -= state.count;
    while (processed--) {
        d.bufferRemoved();
    }
}

} //namespace QtAV