/* AudioHardwareALSA.h
**
** Copyright 2008, Wind River Systems
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#ifndef ANDROID_AUDIO_HARDWARE_ALSA_H
#define ANDROID_AUDIO_HARDWARE_ALSA_H

#include <stdint.h>
#include <sys/types.h>
#include <alsa/asoundlib.h>

#include <hardware/AudioHardwareInterface.h>

namespace android {

class AudioHardwareALSA;

// ----------------------------------------------------------------------------

class ALSAMixer
{
public:
    //
    // Keep this in sync with AudioSystem::audio_routes
    //
    enum mixer_types {
        MIXER_EARPIECE  = 0,
        MIXER_SPEAKER   = 1,
        MIXER_BLUETOOTH = 2,
        MIXER_HEADSET   = 3,
        MIXER_LAST      = MIXER_HEADSET
    };

                                         ALSAMixer();
    virtual                     ~ALSAMixer();

                bool                     isValid() { return !!mMixer[SND_PCM_STREAM_PLAYBACK]; }
                status_t                 setMasterVolume(float volume);
            status_t             setMasterGain(float gain);

        status_t                 setVolume(mixer_types mixer, float volume);
            status_t             setGain(mixer_types mixer, float gain);

            status_t             setCaptureMuteState(mixer_types mixer, bool state);
            status_t         getCaptureMuteState(mixer_types mixer, bool *state);
            status_t             setPlaybackMuteState(mixer_types mixer, bool state);
            status_t         getPlaybackMuteState(mixer_types mixer, bool *state);

private:
    snd_mixer_t             *mMixer[SND_PCM_STREAM_LAST+1];

    struct mixer_info_t;
    mixer_info_t *mMaster[SND_PCM_STREAM_LAST+1];
    mixer_info_t *mInfo[SND_PCM_STREAM_LAST+1][MIXER_LAST+1];
};

class ALSAStreamOps
{
public:
    struct StreamDefaults
    {
        const char *            deviceName;
        snd_pcm_stream_t        direction;  // playback or capture
        snd_pcm_format_t        format;
        int                                     channels;
        uint32_t                        sampleRate;
        unsigned int            bufferTime; // Ring buffer length in usec
        unsigned int            periodTime; // Period time in usec
    };

                             ALSAStreamOps();
    virtual                  ~ALSAStreamOps();

            status_t         set(int format,
                                 int channels,
                                 uint32_t rate);
    virtual uint32_t         sampleRate() const;
            status_t         sampleRate(uint32_t rate);
    virtual size_t           bufferSize() const;
    virtual int              format() const;
    virtual int              channelCount() const;
            status_t         channelCount(int channels);
            const char      *streamName();
    virtual status_t         setDevice(int mode, uint32_t device);

    virtual const char      *deviceName(int mode, int device) = 0;

protected:
    friend class AudioStreamOutALSA;
    friend class AudioStreamInALSA;

    status_t                 open(int mode, int device);
    void                     close();
    status_t                 setSoftwareParams();
    status_t                 setPCMFormat(snd_pcm_format_t format);
    status_t                 setHardwareResample(bool resample);

    void                     setStreamDefaults(StreamDefaults *dev)
    {
        mDefaults = dev;
    }

    Mutex                    mLock;

private:
    snd_pcm_t               *mHandle;
    snd_pcm_hw_params_t     *mHardwareParams;
    snd_pcm_sw_params_t     *mSoftwareParams;
    int                      mMode;
    int                      mDevice;

    StreamDefaults                      *mDefaults;
};

// ----------------------------------------------------------------------------

class AudioStreamOutALSA : public AudioStreamOut, public ALSAStreamOps
{
public:
                             AudioStreamOutALSA(AudioHardwareALSA *parent);
    virtual                  ~AudioStreamOutALSA();

    status_t                 set(int format          = 0,
                                 int channelCount    = 0,
                                 uint32_t sampleRate = 0)
    {
        return ALSAStreamOps::set(format, channelCount, sampleRate);
    }

    virtual uint32_t         sampleRate() const
    {
        return ALSAStreamOps::sampleRate();
    }

    virtual size_t           bufferSize() const
    {
        return ALSAStreamOps::bufferSize();
    }

    virtual int              channelCount() const;

    virtual int              format() const
    {
        return ALSAStreamOps::format();
    }

    virtual ssize_t          write(const void *buffer, size_t bytes);
    virtual status_t         dump(int fd, const Vector<String16>& args);
    virtual status_t         setDevice(int mode, uint32_t newDevice);

                        status_t         setVolume(float volume);

    virtual const char      *deviceName(int mode, int device);

    status_t                 standby();
    bool                     isStandby();

private:
        AudioHardwareALSA       *mParent;
        bool                     mPowerLock;
};


class AudioStreamInALSA : public AudioStreamIn, public ALSAStreamOps
{
public:
                             AudioStreamInALSA(AudioHardwareALSA *parent);
    virtual                  ~AudioStreamInALSA();

    status_t                 set(int      format       = 0,
                                 int      channelCount = 0,
                                 uint32_t sampleRate   = 0)
    {
        return ALSAStreamOps::set(format, channelCount, sampleRate);
    }

    virtual uint32_t         sampleRate()
    {
        return ALSAStreamOps::sampleRate();
    }

    virtual size_t           bufferSize() const
    {
        return ALSAStreamOps::bufferSize();
    }

    virtual int              channelCount() const
    {
        return ALSAStreamOps::channelCount();
    }

    virtual int              format() const
    {
        return ALSAStreamOps::format();
    }

    virtual ssize_t          read(void* buffer, ssize_t bytes);
    virtual status_t         dump(int fd, const Vector<String16>& args);
    virtual status_t         setDevice(int mode, uint32_t newDevice);

        virtual status_t         setGain(float gain);

    virtual const char      *deviceName(int mode, int device);

private:
        AudioHardwareALSA *mParent;
};


class AudioHardwareALSA : public AudioHardwareInterface
{
public:
                             AudioHardwareALSA();
    virtual                  ~AudioHardwareALSA();

    virtual status_t         initCheck();
    virtual status_t         standby();
    virtual status_t         setVoiceVolume(float volume);
    virtual status_t         setMasterVolume(float volume);

    virtual AudioStreamOut  *openOutputStream(int format          = 0,
                                              int channelCount    = 0,
                                              uint32_t sampleRate = 0);

    virtual AudioStreamIn   *openInputStream (int format          = 0,
                                              int channelCount    = 0,
                                              uint32_t sampleRate = 0);

    // Microphone mute
    virtual status_t         setMicMute(bool state);
    virtual status_t         getMicMute(bool *state);

protected:
    // audio routing
    virtual status_t         doRouting();
    virtual status_t         dump(int fd, const Vector<String16>& args);

    friend class AudioStreamOutALSA;
    friend class AudioStreamInALSA;

    ALSAMixer               *mMixer;
    AudioStreamOutALSA      *mOutput;
    AudioStreamInALSA       *mInput;

private:
    Mutex                    mLock;
};

// ----------------------------------------------------------------------------

}; // namespace android

#endif // ANDROID_AUDIO_HARDWARE_ALSA_H
