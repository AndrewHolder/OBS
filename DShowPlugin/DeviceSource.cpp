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


#include "DShowPlugin.h"

DWORD STDCALL PackPlanarThread(ConvertData *data);


bool DeviceSource::Init(XElement *data)
{
    HRESULT err;
    err = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC, (REFIID)IID_IFilterGraph, (void**)&graph);
    if(FAILED(err))
    {
        AppWarning(TEXT("DShowPlugin: Failed to build IGraphBuilder, result = %08lX"), err);
        return false;
    }

    err = CoCreateInstance(CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC, (REFIID)IID_ICaptureGraphBuilder2, (void**)&capture);
    if(FAILED(err))
    {
        AppWarning(TEXT("DShowPlugin: Failed to build ICaptureGraphBuilder2, result = %08lX"), err);
        return false;
    }

    hSampleMutex = OSCreateMutex();
    if(!hSampleMutex)
    {
        AppWarning(TEXT("DShowPlugin: could not create sasmple mutex"));
        return false;
    }

    capture->SetFiltergraph(graph);

    int numThreads = MAX(OSGetTotalCores()-2, 1);
    hConvertThreads = (HANDLE*)Allocate(sizeof(HANDLE)*numThreads);
    convertData = (ConvertData*)Allocate(sizeof(ConvertData)*numThreads);

    zero(hConvertThreads, sizeof(HANDLE)*numThreads);
    zero(convertData, sizeof(ConvertData)*numThreads);

    this->data = data;
    UpdateSettings();

    //if(!bFiltersLoaded)
    //    return false;

    Log(TEXT("Using directshow input"));

    return true;
}

DeviceSource::~DeviceSource()
{
    Stop();
    UnloadFilters();

    SafeRelease(capture);
    SafeRelease(graph);

    if(hConvertThreads)
        Free(hConvertThreads);

    if(convertData)
        Free(convertData);

    if(hSampleMutex)
        OSCloseMutex(hSampleMutex);
}

String DeviceSource::ChooseShader()
{
    if(colorType == DeviceOutputType_RGB && !bUseChromaKey)
        return String();

    String strShader;
    strShader << TEXT("plugins/DShowPlugin/shaders/");

    if(bUseChromaKey)
        strShader << TEXT("ChromaKey_");

    if(colorType == DeviceOutputType_I420)
        strShader << TEXT("YUVToRGB.pShader");
    else if(colorType == DeviceOutputType_YV12)
        strShader << TEXT("YVUToRGB.pShader");
    else if(colorType == DeviceOutputType_YVYU)
        strShader << TEXT("YVXUToRGB.pShader");
    else if(colorType == DeviceOutputType_YUY2)
        strShader << TEXT("YUXVToRGB.pShader");
    else if(colorType == DeviceOutputType_UYVY)
        strShader << TEXT("UYVToRGB.pShader");
    else if(colorType == DeviceOutputType_HDYC)
        strShader << TEXT("HDYCToRGB.pShader");
    else
        strShader << TEXT("RGB.pShader");

    return strShader;
}

const float yuv709Mat[16] = { 0.2126f,  0.7152f,  0.0722f, 0.0625f,
                             -0.1150f, -0.3850f,  0.5000f, 0.50f,
                              0.5000f, -0.4540f, -0.0460f, 0.50f,
                              0.0f,     0.0f,     0.0f,    1.0f};

const float yuvMat[16] = { 0.257f,  0.504f,  0.098f, 0.0625f,
                          -0.148f, -0.291f,  0.439f, 0.50f,
                           0.439f, -0.368f, -0.071f, 0.50f,
                           0.0f,    0.0f,    0.0f,   1.0f};

bool DeviceSource::LoadFilters()
{
    if(bCapturing || bFiltersLoaded)
        return false;

    bool bSucceeded = false;

    List<MediaOutputInfo> outputList;
    IAMStreamConfig *config = NULL;
    bool bAddedCapture = false, bAddedDevice = false;
    GUID expectedMediaType;
    IPin *devicePin = NULL;
    HRESULT err;
    String strShader;

    bUseThreadedConversion = API->UseMultithreadedOptimizations() && (OSGetTotalCores() > 1);

    //------------------------------------------------

    bUseCustomResolution = data->GetInt(TEXT("customResolution"));
    strDevice = data->GetString(TEXT("device"));
    strDeviceName = data->GetString(TEXT("deviceName"));
    strDeviceID = data->GetString(TEXT("deviceID"));

    bFlipVertical = data->GetInt(TEXT("flipImage")) != 0;
    bFlipHorizontal = data->GetInt(TEXT("flipImageHorizontal")) != 0;

    opacity = data->GetInt(TEXT("opacity"), 100);

    //------------------------------------------------

    bUseChromaKey = data->GetInt(TEXT("useChromaKey")) != 0;
    keyColor = data->GetInt(TEXT("keyColor"), 0xFFFFFFFF);
    keySimilarity = data->GetInt(TEXT("keySimilarity"));
    keyBlend = data->GetInt(TEXT("keyBlend"), 80);
    keySpillReduction = data->GetInt(TEXT("keySpillReduction"), 50);

    if(keyBaseColor.x < keyBaseColor.y && keyBaseColor.x < keyBaseColor.z)
        keyBaseColor -= keyBaseColor.x;
    else if(keyBaseColor.y < keyBaseColor.x && keyBaseColor.y < keyBaseColor.z)
        keyBaseColor -= keyBaseColor.y;
    else if(keyBaseColor.z < keyBaseColor.x && keyBaseColor.z < keyBaseColor.y)
        keyBaseColor -= keyBaseColor.z;

    //------------------------------------------------

    if(strDeviceName.IsValid())
    {
        deviceFilter = GetDeviceByValue(L"FriendlyName", strDeviceName, L"DevicePath", strDeviceID);
        if(!deviceFilter)
        {
            AppWarning(TEXT("DShowPlugin: Invalid device: name '%s', path '%s'"), strDeviceName.Array(), strDeviceID.Array());
            goto cleanFinish;
        }
    }
    else
    {
        if(!strDevice.IsValid())
        {
            AppWarning(TEXT("DShowPlugin: Invalid device specified"));
            goto cleanFinish;
        }

        deviceFilter = GetDeviceByValue(L"FriendlyName", strDevice);
        if(!deviceFilter)
        {
            AppWarning(TEXT("DShowPlugin: Could not create device filter"));
            goto cleanFinish;
        }
    }

    devicePin = GetOutputPin(deviceFilter);
    if(!devicePin)
    {
        AppWarning(TEXT("DShowPlugin: Could not create device ping"));
        goto cleanFinish;
    }

    GetOutputList(devicePin, outputList);

    //------------------------------------------------

    renderCX = renderCY = 0;
    frameInterval = 0;

    if(bUseCustomResolution)
    {
        renderCX = data->GetInt(TEXT("resolutionWidth"));
        renderCY = data->GetInt(TEXT("resolutionHeight"));
        frameInterval = data->GetInt(TEXT("frameInterval"));
    }
    else
    {
        SIZE size;
        if (!GetClosestResolution(outputList, size, frameInterval))
        {
            AppWarning(TEXT("DShowPlugin: Unable to find appropriate resolution"));
            renderCX = renderCY = 64;
            goto cleanFinish;
        }

        renderCX = size.cx;
        renderCY = size.cy;
    }

    if(!renderCX || !renderCY || !frameInterval)
    {
        AppWarning(TEXT("DShowPlugin: Invalid size/fps specified"));
        goto cleanFinish;
    }

    preferredOutputType = (data->GetInt(TEXT("usePreferredType")) != 0) ? data->GetInt(TEXT("preferredType")) : -1;

    int numThreads = MAX(OSGetTotalCores()-2, 1);
    for(int i=0; i<numThreads; i++)
    {
        convertData[i].width  = renderCX;
        convertData[i].height = renderCY;
        convertData[i].sample = NULL;
        convertData[i].hSignalConvert  = CreateEvent(NULL, FALSE, FALSE, NULL);
        convertData[i].hSignalComplete = CreateEvent(NULL, FALSE, FALSE, NULL);

        if(i == 0)
            convertData[i].startY = 0;
        else
            convertData[i].startY = convertData[i-1].endY;

        if(i == (numThreads-1))
            convertData[i].endY = renderCY;
        else
            convertData[i].endY = ((renderCY/numThreads)*(i+1)) & 0xFFFFFFFE;
    }

    bFirstFrame = true;

    //------------------------------------------------

    MediaOutputInfo *bestOutput = GetBestMediaOutput(outputList, renderCX, renderCY, preferredOutputType, frameInterval);
    if(!bestOutput)
    {
        AppWarning(TEXT("DShowPlugin: Could not find appropriate resolution to create device image source"));
        goto cleanFinish;
    }

    //------------------------------------------------

    {
        VIDEOINFOHEADER *pVih = reinterpret_cast<VIDEOINFOHEADER*>(bestOutput->mediaType->pbFormat);

        String strTest = FormattedString(TEXT("    device: %s,\r\n    device id %s,\r\n    chosen type: %s, usingFourCC: %s, res: %ux%u - %ux%u, fps: %g-%g"),
            strDevice.Array(), strDeviceID.Array(),
            EnumToName[(int)bestOutput->videoType],
            bestOutput->bUsingFourCC ? TEXT("true") : TEXT("false"),
            bestOutput->minCX, bestOutput->minCY, bestOutput->maxCX, bestOutput->maxCY,
            10000000.0/double(bestOutput->maxFrameInterval), 10000000.0/double(bestOutput->minFrameInterval));

        char fourcc[5];
        mcpy(fourcc, &pVih->bmiHeader.biCompression, 4);
        fourcc[4] = 0;

        if(pVih->bmiHeader.biCompression > 1000)
            strTest << FormattedString(TEXT(", fourCC: '%S'\r\n"), fourcc);
        else
            strTest << FormattedString(TEXT(", fourCC: %08lX\r\n"), pVih->bmiHeader.biCompression);

        Log(TEXT("------------------------------------------"));
        Log(strTest.Array());
    }

    //------------------------------------------------

    expectedMediaType = bestOutput->mediaType->subtype;

    colorType = DeviceOutputType_RGB;
    if(bestOutput->videoType == VideoOutputType_I420)
        colorType = DeviceOutputType_I420;
    else if(bestOutput->videoType == VideoOutputType_YV12)
        colorType = DeviceOutputType_YV12;
    else if(bestOutput->videoType == VideoOutputType_YVYU)
        colorType = DeviceOutputType_YVYU;
    else if(bestOutput->videoType == VideoOutputType_YUY2)
        colorType = DeviceOutputType_YUY2;
    else if(bestOutput->videoType == VideoOutputType_UYVY)
        colorType = DeviceOutputType_UYVY;
    else if(bestOutput->videoType == VideoOutputType_HDYC)
        colorType = DeviceOutputType_HDYC;
    else
    {
        colorType = DeviceOutputType_RGB;
        expectedMediaType = MEDIASUBTYPE_RGB32;
    }

    strShader = ChooseShader();
    if(strShader.IsValid())
        colorConvertShader = CreatePixelShaderFromFile(strShader);

    if(colorType != DeviceOutputType_RGB && !colorConvertShader)
    {
        AppWarning(TEXT("DShowPlugin: Could not create color space conversion pixel shader"));
        goto cleanFinish;
    }

    if(colorType == DeviceOutputType_YV12 || colorType == DeviceOutputType_I420)
    {
        for(int i=0; i<numThreads; i++)
            hConvertThreads[i] = OSCreateThread((XTHREAD)PackPlanarThread, convertData+i);
    }

    //------------------------------------------------

    keyBaseColor = Color4().MakeFromRGBA(keyColor);
    Matrix4x4TransformVect(keyChroma, (colorType == DeviceOutputType_HDYC) ? (float*)yuv709Mat : (float*)yuvMat, keyBaseColor);
    keyChroma *= 2.0f;

    //------------------------------------------------

    if(FAILED(err = devicePin->QueryInterface(IID_IAMStreamConfig, (void**)&config)))
    {
        AppWarning(TEXT("DShowPlugin: Could not get IAMStreamConfig for device pin, result = %08lX"), err);
        goto cleanFinish;
    }

    AM_MEDIA_TYPE outputMediaType;
    CopyMediaType(&outputMediaType, bestOutput->mediaType);

    VIDEOINFOHEADER *vih = reinterpret_cast<VIDEOINFOHEADER*>(outputMediaType.pbFormat);
    vih->AvgTimePerFrame = frameInterval;
    vih->bmiHeader.biWidth  = renderCX;
    vih->bmiHeader.biHeight = renderCY;
    vih->bmiHeader.biSizeImage = renderCX*renderCY*(vih->bmiHeader.biBitCount>>3);

    if(FAILED(err = config->SetFormat(&outputMediaType)))
    {
        AppWarning(TEXT("DShowPlugin: SetFormat on device pin failed, result = %08lX"), err);
        goto cleanFinish;
    }

    FreeMediaType(outputMediaType);

    //------------------------------------------------

    captureFilter = new CaptureFilter(this, expectedMediaType);

    if(FAILED(err = graph->AddFilter(captureFilter, NULL)))
    {
        AppWarning(TEXT("DShowPlugin: Failed to add capture filter to graph, result = %08lX"), err);
        goto cleanFinish;
    }

    bAddedCapture = true;

    if(FAILED(err = graph->AddFilter(deviceFilter, NULL)))
    {
        AppWarning(TEXT("DShowPlugin: Failed to add device filter to graph, result = %08lX"), err);
        goto cleanFinish;
    }

    bAddedDevice = true;

    //------------------------------------------------

    //THANK THE NINE DIVINES I FINALLY GOT IT WORKING
    bool bConnected = SUCCEEDED(err = capture->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, deviceFilter, NULL, captureFilter));
    if(!bConnected)
    {
        if(FAILED(err = graph->Connect(devicePin, captureFilter->GetCapturePin())))
        {
            AppWarning(TEXT("DShowPlugin: Failed to connect the device pin to the capture pin, result = %08lX"), err);
            goto cleanFinish;
        }
    }

    if(FAILED(err = graph->QueryInterface(IID_IMediaControl, (void**)&control)))
    {
        AppWarning(TEXT("DShowPlugin: Failed to get IMediaControl, result = %08lX"), err);
        goto cleanFinish;
    }

    bSucceeded = true;

cleanFinish:
    SafeRelease(config);
    SafeRelease(devicePin);

    for(UINT i=0; i<outputList.Num(); i++)
        outputList[i].FreeData();

    if(!bSucceeded)
    {
        bCapturing = false;

        if(bAddedCapture)
            graph->RemoveFilter(captureFilter);
        if(bAddedDevice)
            graph->RemoveFilter(deviceFilter);

        SafeRelease(deviceFilter);
        SafeRelease(captureFilter);
        SafeRelease(control);

        if(colorConvertShader)
        {
            delete colorConvertShader;
            colorConvertShader = NULL;
        }

        if(lpImageBuffer)
        {
            Free(lpImageBuffer);
            lpImageBuffer = NULL;
        }

        bReadyToDraw = true;
    }
    else
        bReadyToDraw = false;

    //-----------------------------------------------------
    // create the texture regardless, will just show up as red to indicate failure
    BYTE *textureData = (BYTE*)Allocate(renderCX*renderCY*4);

    if(colorType == DeviceOutputType_RGB) //you may be confused, but when directshow outputs RGB, it's actually outputting BGR
    {
        msetd(textureData, 0xFFFF0000, renderCX*renderCY*4);
        texture = CreateTexture(renderCX, renderCY, GS_BGR, textureData, FALSE, FALSE);
    }
    else //if we're working with planar YUV, we can just use regular RGB textures instead
    {
        msetd(textureData, 0xFF0000FF, renderCX*renderCY*4);
        texture = CreateTexture(renderCX, renderCY, GS_RGB, textureData, FALSE, FALSE);
    }

    if(bSucceeded && bUseThreadedConversion)
    {
        if(colorType == DeviceOutputType_I420 || colorType == DeviceOutputType_YV12)
        {
            LPBYTE lpData;
            if(texture->Map(lpData, texturePitch))
                texture->Unmap();
            else
                texturePitch = renderCX*4;

            lpImageBuffer = (LPBYTE)Allocate(texturePitch*renderCY);
        }
    }

    Free(textureData);

    bFiltersLoaded = bSucceeded;
    return bSucceeded;
}

void DeviceSource::UnloadFilters()
{
    if(texture)
    {
        delete texture;
        texture = NULL;
    }

    int numThreads = MAX(OSGetTotalCores()-2, 1);
    for(int i=0; i<numThreads; i++)
    {
        if(hConvertThreads[i])
        {
            convertData[i].bKillThread = true;
            SetEvent(convertData[i].hSignalConvert);

            OSTerminateThread(hConvertThreads[i], 10000);
            hConvertThreads[i] = NULL;
        }

        convertData[i].bKillThread = false;

        if(convertData[i].hSignalConvert)
        {
            CloseHandle(convertData[i].hSignalConvert);
            convertData[i].hSignalConvert = NULL;
        }

        if(convertData[i].hSignalComplete)
        {
            CloseHandle(convertData[i].hSignalComplete);
            convertData[i].hSignalComplete = NULL;
        }
    }

    if(bFiltersLoaded)
    {
        graph->RemoveFilter(captureFilter);
        graph->RemoveFilter(deviceFilter);

        SafeReleaseLogRef(captureFilter);
        SafeReleaseLogRef(deviceFilter);

        bFiltersLoaded = false;
    }

    if(colorConvertShader)
    {
        delete colorConvertShader;
        colorConvertShader = NULL;
    }

    if(lpImageBuffer)
    {
        Free(lpImageBuffer);
        lpImageBuffer = NULL;
    }

    SafeRelease(control);
}

void DeviceSource::Start()
{
    if(bCapturing || !control)
        return;

    HRESULT err;
    if(FAILED(err = control->Run()))
    {
        AppWarning(TEXT("DShowPlugin: control->Run failed, result = %08lX"), err);
        return;
    }

    bCapturing = true;
}

void DeviceSource::Stop()
{
    if(!bCapturing)
        return;

    bCapturing = false;
    control->Stop();
    FlushSamples();
}

void DeviceSource::BeginScene()
{
    Start();
}

void DeviceSource::EndScene()
{
    Stop();
}

void DeviceSource::Receive(IMediaSample *sample)
{
    if(bCapturing)
    {
        OSEnterMutex(hSampleMutex);

        SafeRelease(curSample);
        curSample = sample;
        curSample->AddRef();

        OSLeaveMutex(hSampleMutex);
    }
}

DWORD STDCALL PackPlanarThread(ConvertData *data)
{
    do
    {
        WaitForSingleObject(data->hSignalConvert, INFINITE);
        if(data->bKillThread) break;

        IMediaSample *sample = data->sample;
        PackPlanar(data->output, data->input, data->width, data->height, data->pitch, data->startY, data->endY);
        sample->Release();

        SetEvent(data->hSignalComplete);
    }while(!data->bKillThread);

    return 0;
}

void DeviceSource::Preprocess()
{
    if(!bCapturing)
        return;

    IMediaSample *lastSample = NULL;

    OSEnterMutex(hSampleMutex);
    if(curSample)
    {
        lastSample = curSample;
        curSample = NULL;
    }
    OSLeaveMutex(hSampleMutex);

    int numThreads = MAX(OSGetTotalCores()-2, 1);

    if(lastSample)
    {
        BYTE *lpImage = NULL;
        if(colorType == DeviceOutputType_RGB)
        {
            if(texture)
            {
                if(SUCCEEDED(lastSample->GetPointer(&lpImage)))
                    texture->SetImage(lpImage, GS_IMAGEFORMAT_BGRX, renderCX*4);

                bReadyToDraw = true;
            }
        }
        else if(colorType == DeviceOutputType_I420 || colorType == DeviceOutputType_YV12)
        {
            if(bUseThreadedConversion)
            {
                if(!bFirstFrame)
                {
                    List<HANDLE> events;
                    for(int i=0; i<numThreads; i++)
                        events << convertData[i].hSignalComplete;

                    WaitForMultipleObjects(numThreads, events.Array(), TRUE, INFINITE);
                    texture->SetImage(lpImageBuffer, GS_IMAGEFORMAT_RGBX, texturePitch);

                    bReadyToDraw = true;
                }
                else
                    bFirstFrame = false;

                if(SUCCEEDED(lastSample->GetPointer(&lpImage)))
                {
                    for(int i=0; i<numThreads; i++)
                        lastSample->AddRef();

                    for(int i=0; i<numThreads; i++)
                    {
                        convertData[i].input    = lpImage;
                        convertData[i].pitch    = texturePitch;
                        convertData[i].output   = lpImageBuffer;
                        convertData[i].sample   = lastSample;
                        SetEvent(convertData[i].hSignalConvert);
                    }
                }
            }
            else
            {
                if(SUCCEEDED(lastSample->GetPointer(&lpImage)))
                {
                    LPBYTE lpData;
                    UINT pitch;

                    if(texture->Map(lpData, pitch))
                    {
                        PackPlanar(lpData, lpImage, renderCX, renderCY, pitch, 0, renderCY);
                        texture->Unmap();
                    }
                }

                bReadyToDraw = true;
            }
        }
        else if(colorType == DeviceOutputType_YVYU || colorType == DeviceOutputType_YUY2)
        {
            if(SUCCEEDED(lastSample->GetPointer(&lpImage)))
            {
                LPBYTE lpData;
                UINT pitch;

                if(texture->Map(lpData, pitch))
                {
                    Convert422To444(lpData, lpImage, pitch, true);
                    texture->Unmap();
                }
            }

            bReadyToDraw = true;
        }
        else if(colorType == DeviceOutputType_UYVY || colorType == DeviceOutputType_HDYC)
        {
            if(SUCCEEDED(lastSample->GetPointer(&lpImage)))
            {
                LPBYTE lpData;
                UINT pitch;

                if(texture->Map(lpData, pitch))
                {
                    Convert422To444(lpData, lpImage, pitch, false);
                    texture->Unmap();
                }
            }

            bReadyToDraw = true;
        }

        lastSample->Release();
    }
}

void DeviceSource::Render(const Vect2 &pos, const Vect2 &size)
{
    if(texture && bReadyToDraw)
    {
        Shader *oldShader = GetCurrentPixelShader();
        if(colorConvertShader)
        {
            LoadPixelShader(colorConvertShader);

            if(bUseChromaKey)
            {
                float fSimilarity = float(keySimilarity)/1000.0f;
                float fBlendVal = float(max(keyBlend, 1)/1000.0f);
                float fSpillVal = (float(max(keySpillReduction, 1))/1000.0f);

                Vect2 pixelSize = 1.0f/GetSize();

                colorConvertShader->SetColor  (colorConvertShader->GetParameterByName(TEXT("keyBaseColor")),    Color4(keyBaseColor));
                colorConvertShader->SetColor  (colorConvertShader->GetParameterByName(TEXT("chromaKey")),       Color4(keyChroma));
                colorConvertShader->SetVector2(colorConvertShader->GetParameterByName(TEXT("pixelSize")),       pixelSize);
                colorConvertShader->SetFloat  (colorConvertShader->GetParameterByName(TEXT("keySimilarity")),   fSimilarity);
                colorConvertShader->SetFloat  (colorConvertShader->GetParameterByName(TEXT("keyBlend")),        fBlendVal);
                colorConvertShader->SetFloat  (colorConvertShader->GetParameterByName(TEXT("keySpill")),        fSpillVal);
            }
        }

        bool bFlip = bFlipVertical;

        if(colorType != DeviceOutputType_RGB)
            bFlip = !bFlip;

        float x, x2;
        if(bFlipHorizontal)
        {
            x2 = pos.x;
            x = x2+size.x;
        }
        else
        {
            x = pos.x;
            x2 = x+size.x;
        }

        float fOpacity = float(opacity)*0.01f;
        DWORD opacity255 = DWORD(fOpacity*255.0f);

        if(bFlip)
            DrawSprite(texture, (opacity255<<24) | 0xFFFFFF, x, pos.y, x2, pos.y+size.y);
        else
            DrawSprite(texture, (opacity255<<24) | 0xFFFFFF, x, pos.y+size.y, x2, pos.y);

        if(colorConvertShader)
            LoadPixelShader(oldShader);
    }
}

void DeviceSource::UpdateSettings()
{
    String strNewDevice     = data->GetString(TEXT("device"));
    UINT64 newFrameInterval = data->GetInt(TEXT("frameInterval"));
    UINT newCX              = data->GetInt(TEXT("resolutionWidth"));
    UINT newCY              = data->GetInt(TEXT("resolutionHeight"));
    BOOL bNewCustom         = data->GetInt(TEXT("customResolution"));
    UINT newPreferredType   = data->GetInt(TEXT("usePreferredType")) != 0 ? data->GetInt(TEXT("preferredType")) : -1;

    if(renderCX != newCX || renderCY != newCY || frameInterval != newFrameInterval || newPreferredType != preferredOutputType || !strDevice.CompareI(strNewDevice) || bNewCustom != bUseCustomResolution)
    {
        API->EnterSceneMutex();

        bool bWasCapturing = bCapturing;
        if(bWasCapturing) Stop();

        UnloadFilters();
        LoadFilters();

        if(bWasCapturing) Start();

        API->LeaveSceneMutex();
    }
}

void DeviceSource::SetInt(CTSTR lpName, int iVal)
{
    if(bCapturing)
    {
        if(scmpi(lpName, TEXT("useChromaKey")) == 0)
        {
            bool bNewVal = iVal != 0;
            if(bUseChromaKey != bNewVal)
            {
                API->EnterSceneMutex();
                bUseChromaKey = bNewVal;

                if(colorConvertShader)
                {
                    delete colorConvertShader;
                    colorConvertShader = NULL;
                }

                String strShader;
                strShader = ChooseShader();

                if(strShader.IsValid())
                    colorConvertShader = CreatePixelShaderFromFile(strShader);

                API->LeaveSceneMutex();
            }
        }
        else if(scmpi(lpName, TEXT("flipImage")) == 0)
        {
            bFlipVertical = iVal != 0;
        }
        else if(scmpi(lpName, TEXT("flipImageHorizontal")) == 0)
        {
            bFlipHorizontal = iVal != 0;
        }
        else if(scmpi(lpName, TEXT("keyColor")) == 0)
        {
            keyColor = (DWORD)iVal;

            keyBaseColor = Color4().MakeFromRGBA(keyColor);
            Matrix4x4TransformVect(keyChroma, (colorType == DeviceOutputType_HDYC) ? (float*)yuv709Mat : (float*)yuvMat, keyBaseColor);
            keyChroma *= 2.0f;

            if(keyBaseColor.x < keyBaseColor.y && keyBaseColor.x < keyBaseColor.z)
                keyBaseColor -= keyBaseColor.x;
            else if(keyBaseColor.y < keyBaseColor.x && keyBaseColor.y < keyBaseColor.z)
                keyBaseColor -= keyBaseColor.y;
            else if(keyBaseColor.z < keyBaseColor.x && keyBaseColor.z < keyBaseColor.y)
                keyBaseColor -= keyBaseColor.z;
        }
        else if(scmpi(lpName, TEXT("keySimilarity")) == 0)
        {
            keySimilarity = iVal;
        }
        else if(scmpi(lpName, TEXT("keyBlend")) == 0)
        {
            keyBlend = iVal;
        }
        else if(scmpi(lpName, TEXT("keySpillReduction")) == 0)
        {
            keySpillReduction = iVal;
        }
        else if(scmpi(lpName, TEXT("opacity")) == 0)
        {
            opacity = iVal;
        }
    }
}
