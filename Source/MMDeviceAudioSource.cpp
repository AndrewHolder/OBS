/********************************************************************************
 Copyright (C) 2012 Hugh Bailey <obs.jim@gmail.com>

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
********************************************************************************/


#include "Main.h"

#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <propsys.h>
#include <Functiondiscoverykeys_devpkey.h>

#include "../libsamplerate/samplerate.h"

inline QWORD GetQWDif(QWORD val1, QWORD val2)
{
    return (val1 > val2) ? (val1-val2) : (val2-val1);
}


class MMDeviceAudioSource : public AudioSource
{
    bool bResample;
    SRC_STATE *resampler;
    double resampleRatio;

    //-----------------------------------------

    IMMDeviceEnumerator *mmEnumerator;

    IMMDevice           *mmDevice;
    IAudioClient        *mmClient;
    IAudioCaptureClient *mmCapture;
    IAudioClock         *mmClock;

    UINT inputChannels;
    UINT inputSamplesPerSec;
    UINT inputBitsPerSample;
    UINT inputBlockSize;
    DWORD inputChannelMask;

    List<AudioSegment> audioSegments;

    bool bFirstFrameReceived;
    QWORD lastUsedTimestamp, lastSentTimestamp;
    bool bBrokenTimestamp;

    //-----------------------------------------

    List<float> storageBuffer;

    //-----------------------------------------

    List<float> outputBuffer;
    List<float> tempBuffer;
    List<float> tempResampleBuffer;

    String GetDeviceName();

public:
    bool Initialize(bool bMic, CTSTR lpID);

    ~MMDeviceAudioSource()
    {
        StopCapture();

        SafeRelease(mmCapture);
        SafeRelease(mmClient);
        SafeRelease(mmDevice);
        SafeRelease(mmEnumerator);
        SafeRelease(mmClock);

        if(bResample)
            src_delete(resampler);

        for(UINT i=0; i<audioSegments.Num(); i++)
            audioSegments[i].ClearData();
    }

    virtual void StartCapture();
    virtual void StopCapture();

    virtual UINT GetNextBuffer(float curVolume);

    virtual bool GetEarliestTimestamp(QWORD &timestamp);
    virtual bool GetBuffer(float **buffer, UINT *numFrames, QWORD targetTimestamp);

    virtual QWORD GetBufferedTime()
    {
        if(audioSegments.Num())
            return audioSegments.Last().timestamp - audioSegments[0].timestamp;

        return 0;
    }

    virtual bool GetNewestFrame(float **buffer, UINT *numFrames)
    {
        if(buffer && numFrames)
        {
            if(audioSegments.Num())
            {
                List<float> &data = audioSegments.Last().audioData;
                *buffer = data.Array();
                *numFrames = data.Num()/2;
                return true;
            }
        }

        return false;
    }
};

AudioSource* CreateAudioSource(bool bMic, CTSTR lpID)
{
    MMDeviceAudioSource *source = new MMDeviceAudioSource;
    if(source->Initialize(bMic, lpID))
        return source;
    else
    {
        delete source;
        return NULL;
    }
}

//==============================================================================================================================

#define KSAUDIO_SPEAKER_4POINT1     (KSAUDIO_SPEAKER_QUAD|SPEAKER_LOW_FREQUENCY)
#define KSAUDIO_SPEAKER_2POINT1     (KSAUDIO_SPEAKER_STEREO|SPEAKER_LOW_FREQUENCY)

bool MMDeviceAudioSource::Initialize(bool bMic, CTSTR lpID)
{
    const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
    const IID IID_IMMDeviceEnumerator    = __uuidof(IMMDeviceEnumerator);
    const IID IID_IAudioClient           = __uuidof(IAudioClient);
    const IID IID_IAudioCaptureClient    = __uuidof(IAudioCaptureClient);

    HRESULT err;
    err = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&mmEnumerator);
    if(FAILED(err))
    {
        AppWarning(TEXT("MMDeviceAudioSource::Initialize(%d): Could not create IMMDeviceEnumerator = %08lX"), (BOOL)bMic, err);
        return false;
    }

    if(bMic)
        err = mmEnumerator->GetDevice(lpID, &mmDevice);
    else
        err = mmEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &mmDevice);

    if(FAILED(err))
    {
        AppWarning(TEXT("MMDeviceAudioSource::Initialize(%d): Could not create IMMDevice = %08lX"), (BOOL)bMic, err);
        return false;
    }

    err = mmDevice->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&mmClient);
    if(FAILED(err))
    {
        AppWarning(TEXT("MMDeviceAudioSource::Initialize(%d): Could not create IAudioClient = %08lX"), (BOOL)bMic, err);
        return false;
    }

    WAVEFORMATEX *pwfx;
    err = mmClient->GetMixFormat(&pwfx);
    if(FAILED(err))
    {
        AppWarning(TEXT("MMDeviceAudioSource::Initialize(%d): Could not get mix format from audio client = %08lX"), (BOOL)bMic, err);
        return false;
    }

    String strName = GetDeviceName();
    if(bMic)
    {
        Log(TEXT("------------------------------------------"));
        Log(TEXT("Using auxilary audio input: %s"), strName.Array());
    }

    //the internal audio engine should always use floats (or so I read), but I suppose just to be safe better check
    if(pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        WAVEFORMATEXTENSIBLE *wfext = (WAVEFORMATEXTENSIBLE*)pwfx;
        inputChannelMask = wfext->dwChannelMask;

        if(wfext->SubFormat != KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
        {
            AppWarning(TEXT("MMDeviceAudioSource::Initialize(%d): Unsupported wave format"), (BOOL)bMic);
            return false;
        }
    }
    else if(pwfx->wFormatTag != WAVE_FORMAT_IEEE_FLOAT)
    {
        AppWarning(TEXT("MMDeviceAudioSource::Initialize(%d): Unsupported wave format"), (BOOL)bMic);
        return false;
    }

    inputChannels      = pwfx->nChannels;
    inputBitsPerSample = 32;
    inputBlockSize     = pwfx->nBlockAlign;
    inputSamplesPerSec = pwfx->nSamplesPerSec;

    DWORD flags = bMic ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;
    err = mmClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, ConvertMSTo100NanoSec(5000), 0, pwfx, NULL);
    if(FAILED(err))
    {
        AppWarning(TEXT("MMDeviceAudioSource::Initialize(%d): Could not initialize audio client, result = %08lX"), (BOOL)bMic, err);
        return false;
    }

    err = mmClient->GetService(IID_IAudioCaptureClient, (void**)&mmCapture);
    if(FAILED(err))
    {
        AppWarning(TEXT("MMDeviceAudioSource::Initialize(%d): Could not get audio capture client, result = %08lX"), (BOOL)bMic, err);
        return false;
    }

    err = mmClient->GetService(__uuidof(IAudioClock), (void**)&mmClock);
    if(FAILED(err))
    {
        AppWarning(TEXT("MMDeviceAudioSource::Initialize(%d): Could not get audio capture clock, result = %08lX"), (BOOL)bMic, err);
        return false;
    }

    CoTaskMemFree(pwfx);

    //-------------------------------------------------------------------------

    if(inputSamplesPerSec != 44100)
    {
        int errVal;

        int converterType = AppConfig->GetInt(TEXT("Audio"), TEXT("UseHighQualityResampling"), FALSE) ? SRC_SINC_FASTEST : SRC_LINEAR;
        resampler = src_new(converterType, 2, &errVal);//SRC_SINC_FASTEST//SRC_ZERO_ORDER_HOLD
        if(!resampler)
        {
            CrashError(TEXT("MMDeviceAudioSource::Initialize(%d): Could not initiate resampler"), (BOOL)bMic);
            return false;
        }

        resampleRatio = 44100.0 / double(inputSamplesPerSec);
        bResample = true;

        //----------------------------------------------------
        // hack to get rid of that weird first quirky resampled packet size
        // (always returns a non-441 sized packet on the first resample)

        SRC_DATA data;
        data.src_ratio = resampleRatio;

        List<float> blankBuffer;
        blankBuffer.SetSize(inputSamplesPerSec/100*2);

        data.data_in = blankBuffer.Array();
        data.input_frames = inputSamplesPerSec/100;

        UINT frameAdjust = UINT((double(data.input_frames) * resampleRatio) + 1.0);
        UINT newFrameSize = frameAdjust*2;

        tempResampleBuffer.SetSize(newFrameSize);

        data.data_out = tempResampleBuffer.Array();
        data.output_frames = frameAdjust;

        data.end_of_input = 0;

        int err = src_process(resampler, &data);

        nop();
    }

    //-------------------------------------------------------------------------

    if(inputChannels > 2)
    {
        if(inputChannelMask == 0)
        {
            switch(inputChannels)
            {
                case 3: inputChannelMask = KSAUDIO_SPEAKER_2POINT1; break;
                case 4: inputChannelMask = KSAUDIO_SPEAKER_QUAD;    break;
                case 5: inputChannelMask = KSAUDIO_SPEAKER_4POINT1; break;
                case 6: inputChannelMask = KSAUDIO_SPEAKER_5POINT1; break;
                case 8: inputChannelMask = KSAUDIO_SPEAKER_7POINT1; break;
            }
        }

        switch(inputChannelMask)
        {
            case KSAUDIO_SPEAKER_QUAD:              Log(TEXT("Using quad speaker setup"));                          break; //ocd anyone?
            case KSAUDIO_SPEAKER_2POINT1:           Log(TEXT("Using 2.1 speaker setup"));                           break;
            case KSAUDIO_SPEAKER_4POINT1:           Log(TEXT("Using 4.1 speaker setup"));                           break;
            case KSAUDIO_SPEAKER_SURROUND:          Log(TEXT("Using basic surround speaker setup"));                break;
            case KSAUDIO_SPEAKER_5POINT1:           Log(TEXT("Using 5.1 speaker setup"));                           break;
            case KSAUDIO_SPEAKER_5POINT1_SURROUND:  Log(TEXT("Using 5.1 surround speaker setup"));                  break;
            case KSAUDIO_SPEAKER_7POINT1:           Log(TEXT("Using 7.1 speaker setup (experimental)"));            break;
            case KSAUDIO_SPEAKER_7POINT1_SURROUND:  Log(TEXT("Using 7.1 surround speaker setup (experimental)"));   break;

            default:
                Log(TEXT("Using unknown speaker setup: 0x%lX"), inputChannelMask);
                CrashError(TEXT("Speaker setup not yet implemented -- dear god of all the audio APIs, the one I -have- to use doesn't support resampling or downmixing.  fabulous."));
                break;
        }
    }

    return true;
}

void MMDeviceAudioSource::StartCapture()
{
    if(mmClient)
        mmClient->Start();
}

void MMDeviceAudioSource::StopCapture()
{
    if(mmClient)
        mmClient->Stop();
}

String MMDeviceAudioSource::GetDeviceName()
{
    IPropertyStore *store;
    if(SUCCEEDED(mmDevice->OpenPropertyStore(STGM_READ, &store)))
    {
        PROPVARIANT varName;

        PropVariantInit(&varName);
        if(SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &varName)))
        {
            CWSTR wstrName = varName.pwszVal;

            String strName = wstrName;
            return strName;
        }

        store->Release();
    }

    return String(TEXT("(could not query name of device)"));
}

const float dbMinus3    = 0.7071067811865476f;
const float dbMinus6    = 0.5f;
const float dbMinus9    = 0.3535533905932738f;

//not entirely sure if these are the correct coefficients for downmixing,
//I'm fairly new to the whole multi speaker thing
const float surroundMix = dbMinus3;
const float centerMix   = dbMinus3;
const float lowFreqMix  = 3.16227766f*dbMinus3;

inline void MultiplyAudioBuffer(float *buffer, int totalFloats, float mulVal)
{
    float sum = 0.0f;
    int totalFloatsStore = totalFloats;

    if(App->SSE2Available() && (UPARAM(buffer) & 0xF) == 0)
    {
        UINT alignedFloats = totalFloats & 0xFFFFFFFC;
        __m128 sseMulVal = _mm_set_ps1(mulVal);

        for(UINT i=0; i<alignedFloats; i += 4)
        {
            __m128 sseScaledVals = _mm_mul_ps(_mm_load_ps(buffer+i), sseMulVal);
            _mm_store_ps(buffer+i, sseScaledVals);
        }

        buffer      += alignedFloats;
        totalFloats -= alignedFloats;
    }

    for(int i=0; i<totalFloats; i++)
        buffer[i] *= mulVal;
}

UINT MMDeviceAudioSource::GetNextBuffer(float curVolume)
{
    UINT captureSize = 0;
    HRESULT err = mmCapture->GetNextPacketSize(&captureSize);
    if(FAILED(err))
    {
        RUNONCE AppWarning(TEXT("MMDeviceAudioSource::GetBuffer: GetNextPacketSize failed"));
        return NoAudioAvailable;
    }

    float *outputBuffer = NULL;

    if(captureSize)
    {
        LPBYTE captureBuffer;
        DWORD dwFlags = 0;
        UINT numAudioFrames = 0;

        UINT64 devPosition;
        UINT64 qpcTimestamp;
        err = mmCapture->GetBuffer(&captureBuffer, &numAudioFrames, &dwFlags, &devPosition, &qpcTimestamp);
        if(FAILED(err))
        {
            RUNONCE AppWarning(TEXT("MMDeviceAudioSource::GetBuffer: GetBuffer failed"));
            return NoAudioAvailable;
        }

        QWORD newTimestamp;

        if(dwFlags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
        {
            RUNONCE AppWarning(TEXT("MMDeviceAudioSource::GetBuffer: woa woa woa, getting timestamp errors from the audio subsystem.  device = %s"), GetDeviceName().Array());
            if(!bBrokenTimestamp)
                newTimestamp = lastUsedTimestamp + numAudioFrames*1000/inputSamplesPerSec;
        }
        else
        {
            if(!bBrokenTimestamp)
                newTimestamp = qpcTimestamp/10000;

            /*UINT64 freq;
            mmClock->GetFrequency(&freq);
            Log(TEXT("position: %llu, numAudioFrames: %u, freq: %llu, newTimestamp: %llu, test: %llu"), devPosition, numAudioFrames, freq, newTimestamp, devPosition*8000/freq);*/
        }

        //have to do this crap to account for broken devices or device drivers.  absolutely unbelievable.
        if(!bFirstFrameReceived)
        {
            LARGE_INTEGER clockFreq;
            QueryPerformanceFrequency(&clockFreq);
            QWORD curTime = GetQPCTimeMS(clockFreq.QuadPart);

            if(newTimestamp < (curTime-1000) || newTimestamp > (curTime+1000))
            {
                bBrokenTimestamp = true;

                Log(TEXT("MMDeviceAudioSource::GetNextBuffer: Got bad audio timestamp offset %lld from device: '%s', timestamps for this device will be calculated.  curTime: %llu, newTimestamp: %llu"), (LONGLONG)(newTimestamp - curTime), GetDeviceName().Array(), curTime, newTimestamp);
                lastUsedTimestamp = newTimestamp = curTime;
            }
            else
                lastUsedTimestamp = newTimestamp;

            bFirstFrameReceived = true;
        }

        if(tempBuffer.Num() < numAudioFrames*2)
            tempBuffer.SetSize(numAudioFrames*2);

        outputBuffer = tempBuffer.Array();
        float *tempOut = outputBuffer;

        //------------------------------------------------------------
        // channel upmix/downmix

        if(inputChannels == 1)
        {
            UINT  numFloats   = numAudioFrames;
            float *inputTemp  = (float*)captureBuffer;
            float *outputTemp = outputBuffer;

            if(App->SSE2Available() && (UPARAM(inputTemp) & 0xF) == 0 && (UPARAM(outputTemp) & 0xF) == 0)
            {
                UINT alignedFloats = numFloats & 0xFFFFFFFC;
                for(UINT i=0; i<alignedFloats; i += 4)
                {
                    __m128 inVal   = _mm_load_ps(inputTemp+i);

                    __m128 outVal1 = _mm_unpacklo_ps(inVal, inVal);
                    __m128 outVal2 = _mm_unpackhi_ps(inVal, inVal);

                    _mm_store_ps(outputTemp+(i*2),   outVal1);
                    _mm_store_ps(outputTemp+(i*2)+4, outVal2);
                }

                numFloats  -= alignedFloats;
                inputTemp  += alignedFloats;
                outputTemp += alignedFloats*2;
            }

            while(numFloats--)
            {
                float inputVal = *inputTemp;
                *(outputTemp++) = inputVal;
                *(outputTemp++) = inputVal;

                inputTemp++;
            }
        }
        else if(inputChannels == 2) //straight up copy
        {
            if(App->SSE2Available())
                SSECopy(outputBuffer, captureBuffer, numAudioFrames*2*sizeof(float));
            else
                mcpy(outputBuffer, captureBuffer, numAudioFrames*2*sizeof(float));
        }
        else
        {
            //todo: downmix optimization, also support for other speaker configurations than ones I can merely "think" of.  ugh.
            float *inputTemp  = (float*)captureBuffer;
            float *outputTemp = outputBuffer;

            if(inputChannelMask == KSAUDIO_SPEAKER_QUAD)
            {
                UINT numFloats = numAudioFrames*4;
                float *endTemp = inputTemp+numFloats;

                while(inputTemp < endTemp)
                {
                    float left      = inputTemp[0];
                    float right     = inputTemp[1];
                    float rear      = (inputTemp[2]+inputTemp[3])*surroundMix;

                    *(outputTemp++) = left  - rear;
                    *(outputTemp++) = right + rear;

                    inputTemp  += 4;
                }
            }
            else if(inputChannelMask == KSAUDIO_SPEAKER_2POINT1)
            {
                UINT numFloats = numAudioFrames*3;
                float *endTemp = inputTemp+numFloats;

                while(inputTemp < endTemp)
                {
                    float left      = inputTemp[0];
                    float right     = inputTemp[1];
                    float lfe       = inputTemp[2]*lowFreqMix;

                    *(outputTemp++) = left  + lfe;
                    *(outputTemp++) = right + lfe;

                    inputTemp  += 3;
                }
            }
            else if(inputChannelMask == KSAUDIO_SPEAKER_4POINT1)
            {
                UINT numFloats = numAudioFrames*5;
                float *endTemp = inputTemp+numFloats;

                while(inputTemp < endTemp)
                {
                    float left      = inputTemp[0];
                    float right     = inputTemp[1];
                    float lfe       = inputTemp[2]*lowFreqMix;
                    float rear      = (inputTemp[3]+inputTemp[4])*surroundMix;

                    *(outputTemp++) = left  + lfe - rear;
                    *(outputTemp++) = right + lfe + rear;

                    inputTemp  += 5;
                }
            }
            else if(inputChannelMask == KSAUDIO_SPEAKER_SURROUND)
            {
                UINT numFloats = numAudioFrames*4;
                float *endTemp = inputTemp+numFloats;

                while(inputTemp < endTemp)
                {
                    float left      = inputTemp[0];
                    float right     = inputTemp[1];
                    float center    = inputTemp[2]*centerMix;
                    float rear      = inputTemp[3]*(surroundMix*dbMinus3);

                    *(outputTemp++) = left  + center - rear;
                    *(outputTemp++) = right + center + rear;

                    inputTemp  += 4;
                }
            }
            //don't think this will work for both
            else if(inputChannelMask == KSAUDIO_SPEAKER_5POINT1)
            {
                UINT numFloats = numAudioFrames*6;
                float *endTemp = inputTemp+numFloats;

                while(inputTemp < endTemp)
                {
                    float left      = inputTemp[0];
                    float right     = inputTemp[1];
                    float center    = inputTemp[2]*centerMix;
                    float lowFreq   = inputTemp[3]*lowFreqMix;
                    float rear      = (inputTemp[4]+inputTemp[5])*surroundMix;

                    *(outputTemp++) = left  + center + lowFreq - rear;
                    *(outputTemp++) = right + center + lowFreq + rear;

                    inputTemp  += 6;
                }
            }
            //todo ------------------
            //not sure if my 5.1/7.1 downmixes are correct
            else if(inputChannelMask == KSAUDIO_SPEAKER_5POINT1_SURROUND)
            {
                UINT numFloats = numAudioFrames*6;
                float *endTemp = inputTemp+numFloats;

                while(inputTemp < endTemp)
                {
                    float left      = inputTemp[0];
                    float right     = inputTemp[1];
                    float center    = inputTemp[2]*centerMix;
                    float lowFreq   = inputTemp[3]*lowFreqMix;
                    float sideLeft  = inputTemp[4]*dbMinus3;
                    float sideRight = inputTemp[5]*dbMinus3;

                    *(outputTemp++) = left  + center + sideLeft  + lowFreq;
                    *(outputTemp++) = right + center + sideRight + lowFreq;

                    inputTemp  += 6;
                }
            }
            else if(inputChannelMask == KSAUDIO_SPEAKER_7POINT1)
            {
                UINT numFloats = numAudioFrames*8;
                float *endTemp = inputTemp+numFloats;

                while(inputTemp < endTemp)
                {
                    float left          = inputTemp[0];
                    float right         = inputTemp[1];
                    float center        = inputTemp[2]*(centerMix*dbMinus3);
                    float lowFreq       = inputTemp[3]*lowFreqMix;
                    float rear          = (inputTemp[4]+inputTemp[5])*surroundMix;
                    float centerLeft    = inputTemp[6]*dbMinus6;
                    float centerRight   = inputTemp[7]*dbMinus6;

                    *(outputTemp++) = left  + centerLeft  + center + lowFreq - rear;
                    *(outputTemp++) = right + centerRight + center + lowFreq + rear;

                    inputTemp  += 8;
                }
            }
            else if(inputChannelMask == KSAUDIO_SPEAKER_7POINT1_SURROUND)
            {
                UINT numFloats = numAudioFrames*8;
                float *endTemp = inputTemp+numFloats;

                while(inputTemp < endTemp)
                {
                    float left      = inputTemp[0];
                    float right     = inputTemp[1];
                    float center    = inputTemp[2]*centerMix;
                    float lowFreq   = inputTemp[3]*lowFreqMix;
                    float rear      = (inputTemp[4]+inputTemp[5])*(surroundMix*dbMinus3);
                    float sideLeft  = inputTemp[6]*dbMinus6;
                    float sideRight = inputTemp[7]*dbMinus6;

                    *(outputTemp++) = left  + sideLeft + center + lowFreq - rear;
                    *(outputTemp++) = right + sideLeft + center + lowFreq + rear;

                    inputTemp  += 8;
                }
            }
        }

        mmCapture->ReleaseBuffer(numAudioFrames);

        //------------------------------------------------------------
        // resample

        if(bResample)
        {
            UINT frameAdjust = UINT((double(numAudioFrames) * resampleRatio) + 1.0);
            UINT newFrameSize = frameAdjust*2;

            if(tempResampleBuffer.Num() < newFrameSize)
                tempResampleBuffer.SetSize(newFrameSize);

            SRC_DATA data;
            data.src_ratio = resampleRatio;

            data.data_in = tempBuffer.Array();
            data.input_frames = numAudioFrames;

            data.data_out = tempResampleBuffer.Array();
            data.output_frames = frameAdjust;

            data.end_of_input = 0;

            int err = src_process(resampler, &data);
            if(err)
            {
                RUNONCE AppWarning(TEXT("Was unable to resample audio"));
                return NoAudioAvailable;
            }

            if(data.input_frames_used != numAudioFrames)
            {
                RUNONCE AppWarning(TEXT("Failed to downsample buffer completely, which shouldn't actually happen because it should be using 10ms of samples"));
                return NoAudioAvailable;
            }

            numAudioFrames = data.output_frames_gen;
        }

        //-----------------------------------------------------------------------------
        // sort all audio frames into 10 millisecond increments (done because not all devices output in 10ms increments)
        // NOTE: 0.457+ - instead of using the timestamps from windows, just compare and make sure it stays within a 100ms of their timestamps

        float *newBuffer = (bResample) ? tempResampleBuffer.Array() : tempBuffer.Array();

        if(storageBuffer.Num() == 0 && numAudioFrames == 441)
        {
            lastUsedTimestamp += 10;
            if(!bBrokenTimestamp) 
            {
                QWORD difVal = GetQWDif(newTimestamp, lastUsedTimestamp);
                if(difVal > 70)
                    lastUsedTimestamp = newTimestamp;
            }

            if(lastUsedTimestamp > lastSentTimestamp)
            {
                QWORD adjustVal = (lastUsedTimestamp-lastSentTimestamp);
                if(adjustVal < 10)
                    lastUsedTimestamp += 10-adjustVal;

                AudioSegment &newSegment = *audioSegments.CreateNew();
                newSegment.audioData.CopyArray(newBuffer, numAudioFrames*2);
                newSegment.timestamp = lastUsedTimestamp;
                MultiplyAudioBuffer(newSegment.audioData.Array(), numAudioFrames*2, curVolume);

                lastSentTimestamp = lastUsedTimestamp;
            }
        }
        else
        {
            UINT storedFrames = storageBuffer.Num();

            storageBuffer.AppendArray(newBuffer, numAudioFrames*2);
            if(storageBuffer.Num() >= (441*2))
            {
                lastUsedTimestamp += 10;
                if(!bBrokenTimestamp)
                {
                    QWORD difVal = GetQWDif(newTimestamp, lastUsedTimestamp);
                    if(difVal > 70)
                        lastUsedTimestamp = newTimestamp - (QWORD(storedFrames)/2*1000/44100);
                }

                //------------------------
                // add new data

                if(lastUsedTimestamp > lastSentTimestamp)
                {
                    QWORD adjustVal = (lastUsedTimestamp-lastSentTimestamp);
                    if(adjustVal < 10)
                        lastUsedTimestamp += 10-adjustVal;

                    AudioSegment &newSegment = *audioSegments.CreateNew();
                    newSegment.audioData.CopyArray(storageBuffer.Array(), (441*2));
                    newSegment.timestamp = lastUsedTimestamp;
                    MultiplyAudioBuffer(newSegment.audioData.Array(), 441*2, curVolume);

                    storageBuffer.RemoveRange(0, (441*2));
                }

                //------------------------
                // if still data pending (can happen)

                while(storageBuffer.Num() >= (441*2))
                {
                    lastUsedTimestamp += 10;

                    if(lastUsedTimestamp > lastSentTimestamp)
                    {
                        QWORD adjustVal = (lastUsedTimestamp-lastSentTimestamp);
                        if(adjustVal < 10)
                            lastUsedTimestamp += 10-adjustVal;

                        AudioSegment &newSegment = *audioSegments.CreateNew();
                        newSegment.audioData.CopyArray(storageBuffer.Array(), (441*2));
                        storageBuffer.RemoveRange(0, (441*2));
                        MultiplyAudioBuffer(newSegment.audioData.Array(), 441*2, curVolume);

                        newSegment.timestamp = lastUsedTimestamp;

                        lastSentTimestamp = lastUsedTimestamp;
                    }
                }
            }
        }

        //-----------------------------------------------------------------------------

        return ContinueAudioRequest;
    }

    return NoAudioAvailable;
}

bool MMDeviceAudioSource::GetEarliestTimestamp(QWORD &timestamp)
{
    if(audioSegments.Num())
    {
        timestamp = audioSegments[0].timestamp;
        return true;
    }

    return false;
}

bool MMDeviceAudioSource::GetBuffer(float **buffer, UINT *numFrames, QWORD targetTimestamp)
{
    bool bSuccess = false;
    outputBuffer.Clear();

    if(!bBrokenTimestamp)
    {
        while(audioSegments.Num())
        {
            if(audioSegments[0].timestamp < targetTimestamp)
            {
                audioSegments[0].audioData.Clear();
                audioSegments.Remove(0);
            }
            else
                break;
        }
    }

    if(audioSegments.Num())
    {
        bool bUseSegment = false;

        AudioSegment &segment = audioSegments[0];

        QWORD difference = (segment.timestamp-targetTimestamp);
        if(bBrokenTimestamp || difference <= 10)
        {
            //Log(TEXT("segment.timestamp: %llu, targetTimestamp: %llu"), segment.timestamp, targetTimestamp);
            outputBuffer.TransferFrom(segment.audioData);
            audioSegments.Remove(0);

            bSuccess = true;
        }
    }

    outputBuffer.SetSize(441*2);

    *buffer = outputBuffer.Array();
    *numFrames = outputBuffer.Num()/2;

    return bSuccess;
}
