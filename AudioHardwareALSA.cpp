/* AudioHardwareALSA.cpp
**
** Copyright 2008 Wind River Systems
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

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#define LOG_TAG "AudioHardwareALSA"
#include <utils/Log.h>
#include <utils/String8.h>

#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware/power.h>

#include <alsa/asoundlib.h>
#include "AudioHardwareALSA.h"

#define SND_MIXER_VOL_RANGE_MIN  (0)
#define SND_MIXER_VOL_RANGE_MAX  (1000)

extern "C" {

extern int ffs(int i);

//
// Make sure this prototype is consistent with what's in
// external/libasound/alsa-lib-1.0.16/src/pcm/pcm_null.c!
//
extern int snd_pcm_null_open(snd_pcm_t **pcmp,
                             const char *name,
                             snd_pcm_stream_t stream,
                             int mode);

//
// Function for dlsym() to look up for creating a new AudioHardwareInterface.
//
android::AudioHardwareInterface *createAudioHardware(void)
{
    return new android::AudioHardwareALSA();
}

} // extern "C"


namespace android {

// ----------------------------------------------------------------------------

static const char _nullALSADeviceName[] = "NULL_Device";

static void ALSAErrorHandler(const char *file,
                             int line,
                             const char *function,
                             int err,
                             const char *fmt,
                             ...)
{
    char buf[BUFSIZ];
    va_list arg;
    int l;

    va_start(arg, fmt);
    l = snprintf(buf, BUFSIZ, "%s:%i:(%s) ", file, line, function);
    vsnprintf(buf + l, BUFSIZ - l, fmt, arg);
    buf[BUFSIZ-1] = '\0';
    LOG(LOG_ERROR, "ALSALib", buf);
    va_end(arg);
}

// ----------------------------------------------------------------------------

struct alsa_properties_t {
	const char *propName;
	const char *propDefault;
};

static const alsa_properties_t masterPlaybackProp = {
	"alsa.mixer.playback.master", "PCM"
};

static const alsa_properties_t masterCaptureProp = {
	"alsa.mixer.capture.master", "Capture"
};

/* The following table(s) need to match in order of the route bits
 */
static const char *deviceSuffix[] = {
	/* ROUTE_EARPIECE  */ "_Earpiece",
    /* ROUTE_SPEAKER   */ "_Speaker",
    /* ROUTE_BLUETOOTH */ "_Bluetooth",
    /* ROUTE_HEADSET   */ "_Headset",
};

static const int deviceSuffixLen = (sizeof(deviceSuffix) / sizeof(char *));

static const alsa_properties_t
	mixerMasterProp[SND_PCM_STREAM_LAST+1] =
{
	{ "alsa.mixer.playback.master",  "PCM" },
	{ "alsa.mixer.capture.master",   "Capture" }
};

static const alsa_properties_t
	mixerProp[SND_PCM_STREAM_LAST+1][ALSAMixer::MIXER_LAST+1] =
{
    {
    	{"alsa.mixer.playback.earpiece",  "Earpiece"},
		{"alsa.mixer.playback.speaker",   "Speaker"},
		{"alsa.mixer.playback.bluetooth", "Bluetooth"},
		{"alsa.mixer.playback.headset",   "Headphone"}
	},
	{
		{"alsa.mixer.capture.earpiece",  "Capture"},
		{"alsa.mixer.capture.speaker",   ""},
		{"alsa.mixer.capture.bluetooth", "Bluetooth Capture"},
		{"alsa.mixer.capture.headset",   "Capture"}
	}
};

// ----------------------------------------------------------------------------

AudioHardwareALSA::AudioHardwareALSA() :
    mOutput(0),
    mInput(0)
{
    snd_lib_error_set_handler(&ALSAErrorHandler);
    mMixer = new ALSAMixer;
}

AudioHardwareALSA::~AudioHardwareALSA()
{
    if (mOutput) delete mOutput;
    if (mInput) delete mInput;
    if (mMixer) delete mMixer;
}

status_t AudioHardwareALSA::initCheck()
{
	if (mMixer && mMixer->isValid())
		return NO_ERROR;
	else
		return NO_INIT;
}

status_t AudioHardwareALSA::standby()
{
	if (mOutput)
		return mOutput->standby();

    return NO_ERROR;
}

status_t AudioHardwareALSA::setVoiceVolume(float volume)
{
    // The voice volume is used by the VOICE_CALL audio stream.
	if (mMixer)
		return mMixer->setVolume(ALSAMixer::MIXER_EARPIECE, volume);
	else
		return INVALID_OPERATION;
}

status_t AudioHardwareALSA::setMasterVolume(float volume)
{
	if (mMixer)
		return mMixer->setMasterVolume(volume);
	else
		return INVALID_OPERATION;
}

AudioStreamOut *AudioHardwareALSA::openOutputStream(int      format,
                                                    int      channelCount,
                                                    uint32_t sampleRate)
{
    AutoMutex lock(mLock);

    // only one output stream allowed
    if (mOutput)
        return 0;

    AudioStreamOutALSA *out = new AudioStreamOutALSA(this);

    if (out->set(format, channelCount, sampleRate) == NO_ERROR) {
        mOutput = out;
        // Some information is expected to be available immediately after
        // the device is open.
	    uint32_t routes = mRoutes[mMode];
        mOutput->setDevice(mMode, routes);
    } else {
        delete out;
    }

    return mOutput;
}

AudioStreamIn *AudioHardwareALSA::openInputStream(int      format,
                                                  int      channelCount,
                                                  uint32_t sampleRate)
{
    AutoMutex lock(mLock);

    // only one input stream allowed
    if (mInput)
        return 0;

    AudioStreamInALSA *in = new AudioStreamInALSA(this);

    if (in->set(format, channelCount, sampleRate) == NO_ERROR) {
        mInput = in;
        // Now, actually open the device. Only 1 route used
        mInput->setDevice(0, 0);
    } else {
        delete in;
    }
    return mInput;
}

status_t AudioHardwareALSA::doRouting()
{
    uint32_t routes;

    AutoMutex lock(mLock);

    if (mOutput) {
        routes = mRoutes[mMode];
        return mOutput->setDevice(mMode, routes);
    }
    return NO_INIT;
}

status_t AudioHardwareALSA::setMicMute(bool state)
{
	ALSAMixer::mixer_types mixer_type =
		static_cast<ALSAMixer::mixer_types>(ffs(AudioSystem::ROUTE_EARPIECE) - 1);

    if (mMixer)
        return mMixer->setCaptureMuteState(mixer_type, state);

    return NO_INIT;
}

status_t AudioHardwareALSA::getMicMute(bool *state)
{
	ALSAMixer::mixer_types mixer_type =
		static_cast<ALSAMixer::mixer_types>(ffs(AudioSystem::ROUTE_EARPIECE) - 1);

    if (mMixer)
        return mMixer->getCaptureMuteState(mixer_type, state);

    return NO_ERROR;
}

status_t AudioHardwareALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

// ----------------------------------------------------------------------------

ALSAStreamOps::ALSAStreamOps() :
    mHandle(0),
    mHardwareParams(0),
    mSoftwareParams(0),
    mMode(-1),
    mDevice(-1)
{
    if (snd_pcm_hw_params_malloc(&mHardwareParams) < 0) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA hardware parameters!");
    }

    if (snd_pcm_sw_params_malloc(&mSoftwareParams) < 0) {
        LOG_ALWAYS_FATAL("Failed to allocate ALSA software parameters!");
    }
}

ALSAStreamOps::~ALSAStreamOps()
{
	AutoMutex lock(mLock);

    close();

    if (mHardwareParams)
        snd_pcm_hw_params_free(mHardwareParams);

    if (mSoftwareParams)
        snd_pcm_sw_params_free(mSoftwareParams);
}

status_t ALSAStreamOps::set(int      format,
                            int      channels,
                            uint32_t rate)
{
    if (channels != 0)
        mDefaults->channels = channels;

    if (rate != 0)
        mDefaults->sampleRate = rate;

    switch(format) {
    case AudioSystem::DEFAULT:  // format == 0
        break;

    case AudioSystem::PCM_16_BIT:
        mDefaults->format = SND_PCM_FORMAT_S16_LE;
        break;

    case AudioSystem::PCM_8_BIT:
        mDefaults->format = SND_PCM_FORMAT_S8;
        break;

    default:
        LOGE("Unknown PCM format %i. Forcing default", format);
        break;
    }

	return NO_ERROR;
}

uint32_t ALSAStreamOps::sampleRate() const
{
    unsigned int rate;
    int err;

    if (! mHandle)
        return NO_INIT;

    return snd_pcm_hw_params_get_rate(mHardwareParams, &rate, 0) < 0
           ? 0 : static_cast<uint32_t>(rate);
}

status_t ALSAStreamOps::sampleRate(uint32_t rate)
{
    const char *stream;
    unsigned int requestedRate;
    int err;

    if (!mHandle)
        return NO_INIT;

    stream = streamName();
    requestedRate = rate;
    err = snd_pcm_hw_params_set_rate_near(mHandle,
                                          mHardwareParams,
                                          &requestedRate,
                                          0);

    if (err < 0) {
        LOGE("Unable to set %s sample rate to %u: %s",
             stream, rate, snd_strerror(err));
        return BAD_VALUE;
    }
    if (requestedRate != rate) {
        // Some devices have a fixed sample rate, and can not be changed.
        // This may cause resampling problems; i.e. PCM playback will be too
        // slow or fast.
        LOGW("Requested rate (%u HZ) does not match actual rate (%u HZ)",
             rate, requestedRate);
    } else {
        LOGD("Set %s sample rate to %u HZ", stream, requestedRate);
    }
    return NO_ERROR;
}

//
// Return the number of bytes (not frames)
//
size_t ALSAStreamOps::bufferSize() const
{
    snd_pcm_uframes_t periodSize;
    int err;

    if (!mHandle)
        return -1;

    err = snd_pcm_hw_params_get_period_size(mHardwareParams,
                                            &periodSize,
                                            0);
    if (err < 0)
        return -1;

    return static_cast<size_t>(snd_pcm_frames_to_bytes(mHandle, periodSize));
}

int ALSAStreamOps::format() const
{
    snd_pcm_format_t ALSAFormat;
    int pcmFormatBitWidth;
    int audioSystemFormat;

    if (!mHandle)
        return -1;

    if (snd_pcm_hw_params_get_format(mHardwareParams, &ALSAFormat) < 0) {
        return -1;
    }

    pcmFormatBitWidth = snd_pcm_format_physical_width(ALSAFormat);
    audioSystemFormat = AudioSystem::DEFAULT;
    switch(pcmFormatBitWidth)
    {
    case 8:
        audioSystemFormat = AudioSystem::PCM_8_BIT;
        break;

    case 16:
        audioSystemFormat = AudioSystem::PCM_16_BIT;
        break;

    default:
        LOG_FATAL("Unknown AudioSystem bit width %i!", pcmFormatBitWidth);
    }

    return audioSystemFormat;
}

int ALSAStreamOps::channelCount() const
{
    unsigned int val;
    int err;

    if (!mHandle)
        return -1;

    err = snd_pcm_hw_params_get_channels(mHardwareParams, &val);
    if (err < 0) {
        LOGE("Unable to get device channel count: %s",
             snd_strerror(err));
        return -1;
    }

    return val;
}

status_t ALSAStreamOps::channelCount(int channels)
{
    int err;

    if (!mHandle)
        return NO_INIT;

    err = snd_pcm_hw_params_set_channels(mHandle, mHardwareParams, channels);
    if (err < 0) {
        LOGE("Unable to set channel count to %i: %s",
             channels, snd_strerror(err));
        return BAD_VALUE;
    }

    LOGD("Using %i %s for %s.",
         channels, channels == 1 ? "channel" : "channels", streamName());

    return NO_ERROR;
}

status_t ALSAStreamOps::open(int mode, int device)
{
    const char *stream = streamName();
    const char *devName = deviceName(mode, device);

    int         err;

    // The PCM stream is opened in blocking mode, per ALSA defaults.  The
    // AudioFlinger seems to assume blocking mode too, so asynchronous mode
    // should not be used.
    if ((err = snd_pcm_open(&mHandle, devName, mDefaults->direction, 0)) < 0) {

        // Try without the mode.
        devName  = deviceName(AudioSystem::MODE_INVALID, device);

        err = snd_pcm_open(&mHandle, devName, mDefaults->direction, 0);
        if (err < 0) {

 	       // Try without mode or device.
 	       devName  = deviceName(AudioSystem::MODE_INVALID, -1);

 	       err = snd_pcm_open(&mHandle, devName, mDefaults->direction, 0);
 	       if (err < 0) {

		        err = snd_pcm_open(&mHandle, "hw:00,0", mDefaults->direction, 0);

		        if (err < 0) {
		            LOGE("Unable to open fallback %s device: %s",
		                 stream, snd_strerror(err));

		            // Last resort is the NULL device (i.e. the bit bucket).
				    err = snd_pcm_null_open(&mHandle, _nullALSADeviceName,
											mDefaults->direction, 0);
				    if (err < 0) {
				        LOG_FATAL("Unable to open NULL ALSA device: %s",
				                  snd_strerror(err));
				    }
				    LOGD("Opened NULL %s device.", streamName());
				    return err;
		        }
	        }
        }
    }

	mMode   = mode;
	mDevice = device;

    LOGI("Initialized ALSA %s device %s", stream, devName);
    return err;
}

void ALSAStreamOps::close()
{
	snd_pcm_t *handle = mHandle;
    mHandle = NULL;

    if (handle) {
        snd_pcm_close(handle);
	    mMode   = -1;
	    mDevice = -1;
    }
}

status_t ALSAStreamOps::setSoftwareParams()
{
    if (!mHandle)
        return NO_INIT;

    int err;

    // Get the current software parameters
    err = snd_pcm_sw_params_current(mHandle, mSoftwareParams);
    if (err < 0) {
        LOGE("Unable to get software parameters: %s", snd_strerror(err));
        return NO_INIT;
    }

    snd_pcm_uframes_t bufferSize = 0;
    snd_pcm_uframes_t periodSize = 0;
    snd_pcm_uframes_t startThreshold;

    // Configure ALSA to start the transfer when the buffer is almost full.
    snd_pcm_get_params(mHandle, &bufferSize, &periodSize);

    if (mDefaults->direction == SND_PCM_STREAM_PLAYBACK) {
        // For playback, configure ALSA to start the transfer when the
        // buffer is almost full.
        startThreshold = (bufferSize / periodSize) * periodSize;
    } else {
        // For recording, configure ALSA to start the transfer on the
        // first frame.
        startThreshold = 1;
    }

    err = snd_pcm_sw_params_set_start_threshold(mHandle,
                                                mSoftwareParams,
                                                startThreshold);
    if (err < 0) {
        LOGE("Unable to set start threshold to %lu frames: %s",
             startThreshold, snd_strerror(err));
        return NO_INIT;
    }

    // Stop the transfer when the buffer is full.
    err = snd_pcm_sw_params_set_stop_threshold(mHandle,
                                               mSoftwareParams,
                                               bufferSize);
    if (err < 0) {
        LOGE("Unable to set stop threshold to %lu frames: %s",
             bufferSize, snd_strerror(err));
        return NO_INIT;
    }

    // Allow the transfer to start when at least periodSize samples can be
    // processed.
    err = snd_pcm_sw_params_set_avail_min(mHandle,
                                          mSoftwareParams,
                                          periodSize);
    if (err < 0) {
        LOGE("Unable to configure available minimum to %lu: %s",
             periodSize, snd_strerror(err));
        return NO_INIT;
    }

    // Commit the software parameters back to the device.
    err = snd_pcm_sw_params(mHandle, mSoftwareParams);
    if (err < 0) {
        LOGE("Unable to configure software parameters: %s",
             snd_strerror(err));
        return NO_INIT;
    }

    return NO_ERROR;
}

status_t ALSAStreamOps::setPCMFormat(snd_pcm_format_t format)
{
    const char *formatDesc;
    const char *formatName;
    bool validFormat;
    int err;

    // snd_pcm_format_description() and snd_pcm_format_name() do not perform
    // proper bounds checking.
    validFormat = (static_cast<int>(format) > SND_PCM_FORMAT_UNKNOWN) &&
        (static_cast<int>(format) <= SND_PCM_FORMAT_LAST);
    formatDesc = validFormat ?
        snd_pcm_format_description(format) : "Invalid Format";
    formatName = validFormat ?
        snd_pcm_format_name(format) : "UNKNOWN";

    err = snd_pcm_hw_params_set_format(mHandle, mHardwareParams, format);
    if (err < 0) {
        LOGE("Unable to configure PCM format %s (%s): %s",
             formatName, formatDesc, snd_strerror(err));
        return NO_INIT;
    }

    LOGD("Set %s PCM format to %s (%s)", streamName(), formatName, formatDesc);
    return NO_ERROR;
}

status_t ALSAStreamOps::setHardwareResample(bool resample)
{
    int err;

    err = snd_pcm_hw_params_set_rate_resample(mHandle,
                                              mHardwareParams,
                                              static_cast<int>(resample));
    if (err < 0) {
        LOGE("Unable to %s hardware resampling: %s",
             resample ? "enable" : "disable",
             snd_strerror(err));
        return NO_INIT;
    }
    return NO_ERROR;
}

const char *ALSAStreamOps::streamName()
{
    // Don't use snd_pcm_stream(mHandle), as the PCM stream may not be
    // opened yet.  In such case, snd_pcm_stream() will abort().
    return snd_pcm_stream_name(mDefaults->direction);
}

//
// Set playback or capture PCM device.  It's possible to support audio output
// or input from multiple devices by using the ALSA plugins, but this is
// not supported for simplicity.
//
// The AudioHardwareALSA API does not allow one to set the input routing.
//
// If the "routes" value does not map to a valid device, the default playback
// device is used.
//
status_t ALSAStreamOps::setDevice(int mode, uint32_t device)
{
    // Close off previously opened device.
    // It would be nice to determine if the underlying device actually
    // changes, but we might be manipulating mixer settings (see asound.conf).
    //
    close();

    const char *stream = streamName();

    status_t    status = open (mode, device);
    int			err;

    if (status != NO_ERROR)
        return status;

    err = snd_pcm_hw_params_any(mHandle, mHardwareParams);
    if (err < 0) {
        LOGE("Unable to configure hardware: %s", snd_strerror(err));
        return NO_INIT;
    }

    // Set the interleaved read and write format.
    err = snd_pcm_hw_params_set_access(mHandle, mHardwareParams,
                                       SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        LOGE("Unable to configure PCM read/write format: %s",
             snd_strerror(err));
        return NO_INIT;
    }

    status = setPCMFormat(mDefaults->format);

    //
    // Some devices do not have the default two channels.  Force an error to
    // prevent AudioMixer from crashing and taking the whole system down.
    //
    // Note that some devices will return an -EINVAL if the channel count
    // is queried before it has been set.  i.e. calling channelCount()
    // before channelCount(channels) may return -EINVAL.
    //
    status = channelCount(mDefaults->channels);
    if (status != NO_ERROR)
        return status;

    // Don't check for failure; some devices do not support the default
    // 44100 Hz rate.
    sampleRate(mDefaults->sampleRate);

    // Disable hardware resampling.
    status = setHardwareResample(false);
    if (status != NO_ERROR)
        return status;

    unsigned int     bufferTime;
    unsigned int     periodTime;

    // Set the buffer time.
    bufferTime = mDefaults->bufferTime;
    err = snd_pcm_hw_params_set_buffer_time_near(mHandle,
                                                 mHardwareParams,
                                                 &bufferTime,
                                                 0);
    if (err < 0) {
        LOGE("Unable to set buffer time to %u usec: %s",
             bufferTime, snd_strerror(err));
        return NO_INIT;
    }

    // Set the period time (i.e. the number of frames)
    periodTime = mDefaults->periodTime;
    err = snd_pcm_hw_params_set_period_time_near(mHandle,
                                                 mHardwareParams,
                                                 &periodTime,
                                                 0);
    if (err < 0) {
        LOGE("Unable to set period time to %u usec: %s",
             periodTime, snd_strerror(err));
        return NO_INIT;
    }

    // Commit the hardware parameters back to the device.
    err = snd_pcm_hw_params(mHandle, mHardwareParams);
    if (err < 0) {
        LOGE("Unable to set hardware parameters: %s", snd_strerror(err));
        return NO_INIT;
    }

    status = setSoftwareParams();

    return status;
}

// ----------------------------------------------------------------------------

AudioStreamOutALSA::AudioStreamOutALSA(AudioHardwareALSA *parent) :
	mParent(parent),
	mPowerLock(false)
{
    static StreamDefaults _defaults =
    {
        deviceName     : "AndroidPlayback",
        direction      : SND_PCM_STREAM_PLAYBACK,
        format         : SND_PCM_FORMAT_S16_LE, // AudioSystem::PCM_16_BIT
        channels       : 2,
        sampleRate     : 44100,
        bufferTime     : 500000, // Ring buffer length in usec, 1/2 second
        periodTime     : 100000, // Period time in usec
    };

    setStreamDefaults(&_defaults);
}

AudioStreamOutALSA::~AudioStreamOutALSA()
{
	standby();
	mParent->mOutput = NULL;
}

int AudioStreamOutALSA::channelCount() const
{
    int c;

    c = ALSAStreamOps::channelCount();

    // AudioMixer will seg fault if it doesn't have two channels.
    LOGW_IF(c != 2,
            "AudioMixer expects two channels, but only %i found!", c);
    return c;
}

status_t AudioStreamOutALSA::setVolume(float volume)
{
	if (! mParent->mMixer || mDevice < 0)
		return NO_INIT;

	ALSAMixer::mixer_types mixer_type = static_cast<ALSAMixer::mixer_types>(mDevice);

	return mParent->mMixer->setVolume (mixer_type, volume);
}

ssize_t AudioStreamOutALSA::write(const void *buffer, size_t bytes)
{
    snd_pcm_sframes_t n;
    status_t          err;

    AutoMutex lock(mLock);

    if (isStandby())
    	return 0;

    if (!mPowerLock) {
        acquire_wake_lock (PARTIAL_WAKE_LOCK, "AudioLock");
        ALSAStreamOps::setDevice(mMode, mDevice);
        mPowerLock = true;
    }

    n = snd_pcm_writei(mHandle,
                       buffer,
                       snd_pcm_bytes_to_frames(mHandle, bytes));
    if (n < 0 && mHandle) {
        // snd_pcm_recover() will return 0 if successful in recovering from
        // an error, or -errno if the error was unrecoverable.
        n = snd_pcm_recover(mHandle, n, 0);
    }

    return static_cast<ssize_t>(n);
}

status_t AudioStreamOutALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioStreamOutALSA::setDevice(int mode, uint32_t newDevice)
{
    uint32_t dev;

    //
    // Output to only one device.  The new device is the first selected bit
    // in newDevice (per IAudioFlinger::ROUTE_*).
    //
    // It's possible to not output to any device (i.e. newDevice is 0).
    //
    dev = newDevice ? (ffs(static_cast<int>(newDevice)) - 1) : -1;

    AutoMutex lock(mLock);

    return ALSAStreamOps::setDevice(mode, dev);
}

const char *AudioStreamOutALSA::deviceName(int mode, int device)
{
    static char devString[PROPERTY_VALUE_MAX];
	int hasDevExt = 0;

	strcpy (devString, mDefaults->deviceName);

    if (device >= 0 && device < deviceSuffixLen) {
        strcat (devString, deviceSuffix[device]);
        hasDevExt = 1;
    }

	if (hasDevExt)
	    switch (mode) {
			case AudioSystem::MODE_NORMAL:
		        strcat (devString, "_normal");
				break;
	        case AudioSystem::MODE_RINGTONE:
		        strcat (devString, "_ringtone");
				break;
	        case AudioSystem::MODE_IN_CALL:
		        strcat (devString, "_incall");
				break;
	    };

	return devString;
}

status_t AudioStreamOutALSA::standby()
{
    AutoMutex lock(mLock);

    if (mHandle)
        snd_pcm_drain (mHandle);

    if (mPowerLock) {
        release_wake_lock ("AudioLock");
        mPowerLock = false;
    }

    return NO_ERROR;
}

bool AudioStreamOutALSA::isStandby()
{
    return (!mHandle);
}

// ----------------------------------------------------------------------------

AudioStreamInALSA::AudioStreamInALSA(AudioHardwareALSA *parent) :
	mParent(parent)
{
    static StreamDefaults _defaults =
    {
        deviceName     : "AndroidRecord",
        direction      : SND_PCM_STREAM_CAPTURE,
        format         : SND_PCM_FORMAT_S16_LE, // AudioSystem::PCM_16_BIT
        channels       : 1,
        sampleRate     : AudioRecord::DEFAULT_SAMPLE_RATE,
        bufferTime     : 500000, // Ring buffer length in usec, 1/2 second
        periodTime     : 100000, // Period time in usec
    };

    setStreamDefaults(&_defaults);
}

AudioStreamInALSA::~AudioStreamInALSA()
{
	mParent->mInput = NULL;
}

status_t AudioStreamInALSA::setGain(float gain)
{
	if (mParent->mMixer)
		return mParent->mMixer->setMasterGain (gain);
	else
		return NO_INIT;
}

ssize_t AudioStreamInALSA::read(void *buffer, ssize_t bytes)
{
    snd_pcm_sframes_t n;
    status_t          err;

    AutoMutex lock(mLock);

    n = snd_pcm_readi(mHandle,
                      buffer,
                      snd_pcm_bytes_to_frames(mHandle, bytes));
    if (n < 0 && mHandle) {
        n = snd_pcm_recover(mHandle, n, 0);
    }

    return static_cast<ssize_t>(n);
}

status_t AudioStreamInALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

status_t AudioStreamInALSA::setDevice(int mode, uint32_t newDevice)
{
    AutoMutex lock(mLock);

    // The AudioHardwareALSA API does not allow one to set the input routing.
    // Only one input device (the microphone) is currently supported.
    //
    return ALSAStreamOps::setDevice(mode, AudioRecord::MIC_INPUT);
}

const char *AudioStreamInALSA::deviceName(int mode, int device)
{
    static char devString[PROPERTY_VALUE_MAX];

	strcpy (devString, mDefaults->deviceName);
    strcat (devString, "_Microphone");

    return devString;
}

// ----------------------------------------------------------------------------

struct ALSAMixer::mixer_info_t {
	mixer_info_t() :
	    elem(0), min(0), max(100), mute(false)
	{
	}
	snd_mixer_elem_t	*elem;
	long			 min;
	long			 max;
	long			 volume;
	bool			 mute;
	char			 name[PROPERTY_VALUE_MAX];
};

static int initMixer (snd_mixer_t **mixer, const char *name)
{
	int err;

    if ((err = snd_mixer_open(mixer, 0)) < 0) {
        LOGE("Unable to open mixer: %s", snd_strerror(err));
        return err;
    }

    if ((err = snd_mixer_attach(*mixer, name)) < 0) {
        LOGE("Unable to attach mixer to device %s: %s",
             name, snd_strerror(err));

	    if ((err = snd_mixer_attach(*mixer, "hw:00")) < 0) {
	        LOGE("Unable to attach mixer to device default: %s",
		             snd_strerror(err));

			snd_mixer_close (*mixer);
			*mixer = NULL;
			return err;
	    }
    }

    if ((err = snd_mixer_selem_register(*mixer, NULL, NULL)) < 0) {
        LOGE("Unable to register mixer elements: %s", snd_strerror(err));
		snd_mixer_close (*mixer);
		*mixer = NULL;
		return err;
    }

    // Get the mixer controls from the kernel
    if ((err = snd_mixer_load(*mixer)) < 0) {
        LOGE("Unable to load mixer elements: %s", snd_strerror(err));
		snd_mixer_close (*mixer);
		*mixer = NULL;
		return err;
    }

	return 0;
}

typedef int (*hasVolume_t)(snd_mixer_elem_t*);

static hasVolume_t hasVolume[] =
{
	snd_mixer_selem_has_playback_volume,
	snd_mixer_selem_has_capture_volume
};

typedef int (*getVolumeRange_t)(snd_mixer_elem_t*, long int*, long int*);

static getVolumeRange_t getVolumeRange[] =
{
	snd_mixer_selem_get_playback_volume_range,
	snd_mixer_selem_get_capture_volume_range
};

typedef int (*setVolume_t)(snd_mixer_elem_t*, long int);

static setVolume_t setVol[] =
{
	snd_mixer_selem_set_playback_volume_all,
	snd_mixer_selem_set_capture_volume_all
};

ALSAMixer::ALSAMixer()
{
    int err;

	initMixer (&mMixer[SND_PCM_STREAM_PLAYBACK], "AndroidPlayback");
	initMixer (&mMixer[SND_PCM_STREAM_CAPTURE], "AndroidRecord");

    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);

	for (int i = 0; i <= SND_PCM_STREAM_LAST; i++) {

		mMaster[i] = new mixer_info_t;

	    property_get (mixerMasterProp[i].propName,
					  mMaster[i]->name,
					  mixerMasterProp[i].propDefault);

		for (snd_mixer_elem_t *elem = snd_mixer_first_elem(mMixer[i]);
			 elem;
	         elem = snd_mixer_elem_next(elem)) {

			if (!snd_mixer_selem_is_active(elem))
				continue;

	        snd_mixer_selem_get_id(elem, sid);

	        // Find PCM playback volume control element.
	        const char *elementName = snd_mixer_selem_id_get_name(sid);

			if (mMaster[i]->elem == NULL &&
			    strcmp(elementName, mMaster[i]->name) == 0 &&
			    hasVolume[i] (elem)) {

				mMaster[i]->elem = elem;
				getVolumeRange[i] (elem, &mMaster[i]->min, &mMaster[i]->max);
				mMaster[i]->volume = mMaster[i]->max;
			    setVol[i] (elem, mMaster[i]->volume);
			    if (i == SND_PCM_STREAM_PLAYBACK &&
			    	snd_mixer_selem_has_playback_switch (elem))
						snd_mixer_selem_set_playback_switch_all (elem, 1);
			    break;
			}
        }

		for (int j = 0; j <= MIXER_LAST; j++) {

			mInfo[i][j] = new mixer_info_t;

		    property_get (mixerProp[i][j].propName,
						  mInfo[i][j]->name,
						  mixerProp[i][j].propDefault);

		    for (snd_mixer_elem_t *elem = snd_mixer_first_elem(mMixer[i]);
				 elem;
		         elem = snd_mixer_elem_next(elem)) {

				if (!snd_mixer_selem_is_active(elem))
					continue;

		        snd_mixer_selem_get_id(elem, sid);

		        // Find PCM playback volume control element.
		        const char *elementName = snd_mixer_selem_id_get_name(sid);

				if (mInfo[i][j]->elem == NULL &&
				    strcmp(elementName, mInfo[i][j]->name) == 0 &&
				    hasVolume[i] (elem)) {

					mInfo[i][j]->elem = elem;
					getVolumeRange[i] (elem, &mInfo[i][j]->min, &mInfo[i][j]->max);
					mInfo[i][j]->volume = mInfo[i][j]->max;
				    setVol[i] (elem, mInfo[i][j]->volume);
				    if (i == SND_PCM_STREAM_PLAYBACK &&
				    	snd_mixer_selem_has_playback_switch (elem))
							snd_mixer_selem_set_playback_switch_all (elem, 1);
				    break;
				}
			}
		}
	}
	LOGD("mixer initialized.");
}

ALSAMixer::~ALSAMixer()
{
	for (int i = 0; i <= SND_PCM_STREAM_LAST; i++) {
	    if (mMixer[i]) snd_mixer_close (mMixer[i]);
	    if (mMaster[i]) delete mMaster[i];
		for (int j = 0; j <= MIXER_LAST; j++) {
		    if (mInfo[i][j]) delete mInfo[i][j];
		}
	}
    LOGD("mixer destroyed.");
}

status_t ALSAMixer::setMasterVolume(float volume)
{
	mixer_info_t *info = mMaster[SND_PCM_STREAM_PLAYBACK];
    if (!info || !info->elem) return INVALID_OPERATION;

    long minVol = info->min;
    long maxVol = info->max;

    // Make sure volume is between bounds.
    long vol = minVol + volume * (maxVol - minVol);
    if (vol > maxVol) vol = maxVol;
    if (vol < minVol) vol = minVol;

    info->volume = vol;
    snd_mixer_selem_set_playback_volume_all (info->elem, vol);

    return NO_ERROR;
}

status_t ALSAMixer::setMasterGain(float gain)
{
	mixer_info_t *info = mMaster[SND_PCM_STREAM_CAPTURE];
    if (!info || !info->elem) return INVALID_OPERATION;

    long minVol = info->min;
    long maxVol = info->max;

    // Make sure volume is between bounds.
    long vol = minVol + gain * (maxVol - minVol);
    if (vol > maxVol) vol = maxVol;
    if (vol < minVol) vol = minVol;

    info->volume = vol;
    snd_mixer_selem_set_capture_volume_all (info->elem, vol);

    return NO_ERROR;
}

status_t ALSAMixer::setVolume(mixer_types mixer, float volume)
{
	mixer_info_t *info = mInfo[mixer][SND_PCM_STREAM_PLAYBACK];
    if (!info || !info->elem) return INVALID_OPERATION;

    long minVol = info->min;
    long maxVol = info->max;

    // Make sure volume is between bounds.
    long vol = minVol + volume * (maxVol - minVol);
    if (vol > maxVol) vol = maxVol;
    if (vol < minVol) vol = minVol;

    info->volume = vol;
    snd_mixer_selem_set_playback_volume_all (info->elem, vol);

    return NO_ERROR;
}

status_t ALSAMixer::setGain(mixer_types mixer, float gain)
{
	mixer_info_t *info = mInfo[mixer][SND_PCM_STREAM_CAPTURE];
    if (!info || !info->elem) return INVALID_OPERATION;

    long minVol = info->min;
    long maxVol = info->max;

    // Make sure volume is between bounds.
    long vol = minVol + gain * (maxVol - minVol);
    if (vol > maxVol) vol = maxVol;
    if (vol < minVol) vol = minVol;

    info->volume = vol;
    snd_mixer_selem_set_capture_volume_all (info->elem, vol);

    return NO_ERROR;
}

status_t ALSAMixer::setCaptureMuteState(mixer_types mixer, bool state)
{
	mixer_info_t *info = mInfo[mixer][SND_PCM_STREAM_CAPTURE];
    if (!info || !info->elem) return INVALID_OPERATION;

    if (info->mute == state) return NO_ERROR;

    if (snd_mixer_selem_has_capture_switch (info->elem)) {

	int err = snd_mixer_selem_set_capture_switch_all (info->elem, static_cast<int>(!state));
	if (err < 0) {
	    LOGE("Unable to %s capture mixer switch %s",
		 state ? "enable" : "disable", info->name);
	    return INVALID_OPERATION;
	}
    }

    info->mute = state;
    return NO_ERROR;
}

status_t ALSAMixer::getCaptureMuteState(mixer_types mixer, bool *state)
{
	mixer_info_t *info = mInfo[mixer][SND_PCM_STREAM_CAPTURE];
    if (!info || !info->elem) return INVALID_OPERATION;

    if (! state) return BAD_VALUE;

    *state = info->mute;

    return NO_ERROR;
}

status_t ALSAMixer::setPlaybackMuteState(mixer_types mixer, bool state)
{
	mixer_info_t *info = mInfo[mixer][SND_PCM_STREAM_PLAYBACK];
    if (!info || !info->elem) return INVALID_OPERATION;

    if (snd_mixer_selem_has_playback_switch (info->elem)) {

	int err = snd_mixer_selem_set_playback_switch_all (info->elem, static_cast<int>(!state));
	if (err < 0) {
	    LOGE("Unable to %s playback mixer switch %s",
		 state ? "enable" : "disable", info->name);
	    return INVALID_OPERATION;
	}
    }

    info->mute = state;
    return NO_ERROR;
}

status_t ALSAMixer::getPlaybackMuteState(mixer_types mixer, bool *state)
{
	mixer_info_t *info = mInfo[SND_PCM_STREAM_PLAYBACK][mixer];
    if (!info || !info->elem) return INVALID_OPERATION;

    if (! state) return BAD_VALUE;

    *state = info->mute;

    return NO_ERROR;
}

// ----------------------------------------------------------------------------

}; // namespace android
