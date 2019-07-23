/* -LICENSE-START-
** Copyright (c) 2018 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
**
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

// OS includes
#include <stdint.h>

// Local includes
#include "DeckLinkDevice.h"

// STL includes
#include <codecvt>
#include <iomanip>
#include <locale>
#include <sstream>

namespace
{
  //----------------------------------------------------------------------------
  std::string DoubleTo4String(double _arg)
  {
    std::stringstream ss;
    ss << std::setfill('0') << std::setw(4) << _arg;
    return ss.str();
  }

  //----------------------------------------------------------------------------
  std::string WideStringToNormal(const std::wstring& wstr)
  {
    using convert_typeX = std::codecvt_utf8<wchar_t>;
    std::wstring_convert<convert_typeX, wchar_t> converterX;

    return converterX.to_bytes(wstr);
  }
}

//----------------------------------------------------------------------------
DeckLinkDevice::DeckLinkDevice(IDeckLink* device)
  : m_refCount(1),
    m_deckLink(device),
    m_deckLinkInput(NULL),
    m_deckLinkConfig(NULL),
    m_deckLinkHDMIInputEDID(NULL),
    m_deckLinkProfileManager(NULL),
    m_deckLinkAttributes(NULL),
    m_supportsFormatDetection(false),
    m_currentlyCapturing(false),
    m_applyDetectedInputMode(false)
{
  m_deckLink->AddRef();
}

//----------------------------------------------------------------------------
DeckLinkDevice::~DeckLinkDevice()
{
  if (m_deckLinkHDMIInputEDID != NULL)
  {
    m_deckLinkHDMIInputEDID->Release();
    m_deckLinkHDMIInputEDID = NULL;
  }

  if (m_deckLinkProfileManager)
  {
    m_deckLinkProfileManager->Release();
    m_deckLinkProfileManager = NULL;
  }

  if (m_deckLinkAttributes)
  {
    m_deckLinkAttributes->Release();
    m_deckLinkAttributes = NULL;
  }

  if (m_deckLinkConfig)
  {
    m_deckLinkConfig->Release();
    m_deckLinkConfig = NULL;
  }

  if (m_deckLinkInput != NULL)
  {
    m_deckLinkInput->Release();
    m_deckLinkInput = NULL;
  }

  if (m_deckLink != NULL)
  {
    m_deckLink->Release();
    m_deckLink = NULL;
  }
}

//----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE DeckLinkDevice::QueryInterface(REFIID iid, LPVOID* ppv)
{
  HRESULT result = E_NOINTERFACE;

  if (ppv == NULL)
  {
    return E_INVALIDARG;
  }

  // Initialize the return result
  *ppv = NULL;

  // Obtain the IUnknown interface and compare it the provided REFIID
  if (iid == IID_IUnknown)
  {
    *ppv = this;
    AddRef();
    result = S_OK;
  }
  else if (iid == IID_IDeckLinkInputCallback)
  {
    *ppv = (IDeckLinkInputCallback*)this;
    AddRef();
    result = S_OK;
  }
  else if (iid == IID_IDeckLinkNotificationCallback)
  {
    *ppv = (IDeckLinkNotificationCallback*)this;
    AddRef();
    result = S_OK;
  }

  return result;
}

//----------------------------------------------------------------------------
ULONG STDMETHODCALLTYPE DeckLinkDevice::AddRef(void)
{
  return InterlockedIncrement((LONG*)&m_refCount);
}

//----------------------------------------------------------------------------
ULONG STDMETHODCALLTYPE DeckLinkDevice::Release(void)
{
  int   newRefValue;

  newRefValue = InterlockedDecrement((LONG*)&m_refCount);
  if (newRefValue == 0)
  {
    delete this;
    return 0;
  }

  return newRefValue;
}

//----------------------------------------------------------------------------
bool DeckLinkDevice::Init()
{
  std::wstring deviceNameBSTR = NULL;

  // Get input interface
  if (m_deckLink->QueryInterface(IID_IDeckLinkInput, (void**) &m_deckLinkInput) != S_OK)
  {
    return false;
  }

  // Get attributes interface
  if (m_deckLink->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&m_deckLinkAttributes) != S_OK)
  {
    return false;
  }

  // Check if input mode detection is supported.
#if WIN32
  BOOL formatDetection;
#else
  bool formatDetection;
#endif
  if (m_deckLinkAttributes->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &formatDetection) != S_OK)
  {
    m_supportsFormatDetection = formatDetection;
  }

  // Get configuration interface to allow changing of input connector
  // We hold onto IDeckLinkConfiguration for lifetime of DeckLinkDevice to retain input connector setting
  if (m_deckLink->QueryInterface(IID_IDeckLinkConfiguration, (void**)&m_deckLinkConfig) != S_OK)
  {
    return false;
  }

  // Enable all EDID functionality if possible
  if (m_deckLink->QueryInterface(IID_IDeckLinkHDMIInputEDID, (void**)&m_deckLinkHDMIInputEDID) == S_OK && m_deckLinkHDMIInputEDID)
  {
    int64_t allKnownRanges = bmdDynamicRangeSDR | bmdDynamicRangeHDRStaticPQ | bmdDynamicRangeHDRStaticHLG;
    m_deckLinkHDMIInputEDID->SetInt(bmdDeckLinkHDMIInputEDIDDynamicRange, allKnownRanges);
    m_deckLinkHDMIInputEDID->WriteToEDID();
  }

  // Get device name
#if WIN32
  BSTR deviceName = NULL;
#else
  const char* deviceName = NULL;
#endif
  if (m_deckLink->GetDisplayName(&deviceName) == S_OK)
  {
#if WIN32
    m_deviceName = WideStringToNormal(std::wstring(deviceName));
#else
    m_deviceName = std::string(deviceName);
#endif
  }
  else
  {
    m_deviceName = "DeckLink";
  }

  // Get the profile manager interface
  // Will return S_OK when the device has > 1 profiles
  if (m_deckLink->QueryInterface(IID_IDeckLinkProfileManager, (void**)&m_deckLinkProfileManager) != S_OK)
  {
    m_deckLinkProfileManager = NULL;
  }

  return true;
}

//----------------------------------------------------------------------------
std::string DeckLinkDevice::GetDeviceName()
{
  return m_deviceName;
}

//----------------------------------------------------------------------------
bool DeckLinkDevice::IsCapturing()
{
  return m_currentlyCapturing;
}

//----------------------------------------------------------------------------
bool DeckLinkDevice::SupportsFormatDetection()
{
  return (m_supportsFormatDetection == TRUE);
}

//----------------------------------------------------------------------------
bool DeckLinkDevice::StartCapture(BMDDisplayMode displayMode, IDeckLinkScreenPreviewCallback* screenPreviewCallback, bool applyDetectedInputMode)
{
  BMDVideoInputFlags    videoInputFlags = bmdVideoInputFlagDefault;

  m_applyDetectedInputMode = applyDetectedInputMode;

  // Enable input video mode detection if the device supports it
  if (m_supportsFormatDetection == TRUE)
  {
    videoInputFlags |=  bmdVideoInputEnableFormatDetection;
  }

  // Set the screen preview
  m_deckLinkInput->SetScreenPreviewCallback(screenPreviewCallback);

  // Set capture callback
  m_deckLinkInput->SetCallback(this);

  // Set the video input mode
  if (m_deckLinkInput->EnableVideoInput(displayMode, bmdFormat8BitYUV, videoInputFlags) != S_OK)
  {
    return false;
  }

  // Start the capture
  if (m_deckLinkInput->StartStreams() != S_OK)
  {
    return false;
  }

  m_currentlyCapturing = true;

  return true;
}

//----------------------------------------------------------------------------
void DeckLinkDevice::StopCapture()
{
  if (m_deckLinkInput != NULL)
  {
    // Stop the capture
    m_deckLinkInput->StopStreams();

    //
    m_deckLinkInput->SetScreenPreviewCallback(NULL);

    // Delete capture callback
    m_deckLinkInput->SetCallback(NULL);
  }

  m_currentlyCapturing = false;
}

//----------------------------------------------------------------------------
IDeckLink* DeckLinkDevice::DeckLinkInstance()
{
  return m_deckLink;
}

//----------------------------------------------------------------------------
IDeckLinkProfileManager* DeckLinkDevice::GetDeviceProfileManager()
{
  return m_deckLinkProfileManager;
}

//----------------------------------------------------------------------------
IDeckLinkInput* DeckLinkDevice::GetDeckLinkInput()
{
  return m_deckLinkInput;
}

//----------------------------------------------------------------------------
IDeckLinkConfiguration* DeckLinkDevice::GetDeckLinkConfiguration()
{
  return m_deckLinkConfig;
}

//----------------------------------------------------------------------------
IDeckLinkProfileAttributes* DeckLinkDevice::GetDeckLinkAttributes()
{
  return m_deckLinkAttributes;
}

//----------------------------------------------------------------------------
HRESULT DeckLinkDevice::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents, IDeckLinkDisplayMode* newMode, BMDDetectedVideoInputFormatFlags detectedSignalFlags)
{
  BMDPixelFormat pixelFormat = bmdFormat10BitYUV;

  // Restart capture with the new video mode if told to
  if (!m_applyDetectedInputMode)
  {
    goto bail;
  }

  if (detectedSignalFlags & bmdDetectedVideoInputRGB444)
  {
    pixelFormat = bmdFormat10BitRGB;
  }

  // Stop the capture
  m_deckLinkInput->StopStreams();

  // Set the video input mode
  if (m_deckLinkInput->EnableVideoInput(newMode->GetDisplayMode(), pixelFormat, bmdVideoInputEnableFormatDetection) != S_OK)
  {
    goto bail;
  }

  // Start the capture
  if (m_deckLinkInput->StartStreams() != S_OK)
  {
    goto bail;
  }

  // Update the UI with detected display mode
bail:
  return S_OK;
}

//----------------------------------------------------------------------------
HRESULT DeckLinkDevice::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioPacket)
{
  AncillaryDataStruct ancillaryData;
  HDRMetadataStruct hdrMetadata;

  if (videoFrame == NULL)
  {
    return S_OK;
  }

  // Get the various timecodes and userbits attached to this frame
  GetAncillaryDataFromFrame(videoFrame, bmdTimecodeVITC, ancillaryData.vitcF1Timecode, ancillaryData.vitcF1UserBits);
  GetAncillaryDataFromFrame(videoFrame, bmdTimecodeVITCField2, ancillaryData.vitcF2Timecode, ancillaryData.vitcF2UserBits);
  GetAncillaryDataFromFrame(videoFrame, bmdTimecodeRP188VITC1, ancillaryData.rp188vitc1Timecode, ancillaryData.rp188vitc1UserBits);
  GetAncillaryDataFromFrame(videoFrame, bmdTimecodeRP188LTC, ancillaryData.rp188ltcTimecode, ancillaryData.rp188ltcUserBits);
  GetAncillaryDataFromFrame(videoFrame, bmdTimecodeRP188VITC2, ancillaryData.rp188vitc2Timecode, ancillaryData.rp188vitc2UserBits);
  GetAncillaryDataFromFrame(videoFrame, bmdTimecodeRP188HighFrameRate, ancillaryData.rp188hfrtcTimecode, ancillaryData.rp188hfrtcUserBits);
  GetHDRMetadataFromFrame(videoFrame, hdrMetadata);

  return S_OK;
}

//----------------------------------------------------------------------------
void DeckLinkDevice::GetAncillaryDataFromFrame(IDeckLinkVideoInputFrame* videoFrame, BMDTimecodeFormat timecodeFormat, std::string& timecodeString, std::string& userBitsString)
{
  IDeckLinkTimecode*  timecode = NULL;
  std::wstring        timecodeWide;
  BMDTimecodeUserBits userBits = 0;

  if ((videoFrame != NULL) && (videoFrame->GetTimecode(timecodeFormat, &timecode) == S_OK))
  {
#if WIN32
    BSTR timecodeStr = NULL;
#else
    const char* timecode = NULL;
#endif
    if (timecode->GetString(&timecodeStr) == S_OK)
    {
#if WIN32
      timecodeString = WideStringToNormal(std::wstring(timecodeStr));
#else
      timecodeString = std::string(timecodeStr);
#endif
    }

    timecode->GetTimecodeUserBits(&userBits);
    std::stringstream ss;
    ss << "0x" << std::hex << std::setw(8) << std::setfill('0') << userBits;
    userBitsString = ss.str();

    timecode->Release();
  }
  else
  {
    timecodeString = "";
    userBitsString = "";
  }
}

//----------------------------------------------------------------------------
void DeckLinkDevice::GetHDRMetadataFromFrame(IDeckLinkVideoInputFrame* videoFrame, HDRMetadataStruct& hdrMetadata)
{
  hdrMetadata.electroOpticalTransferFunction = "";
  hdrMetadata.displayPrimariesRedX = "";
  hdrMetadata.displayPrimariesRedY = "";
  hdrMetadata.displayPrimariesGreenX = "";
  hdrMetadata.displayPrimariesGreenY = "";
  hdrMetadata.displayPrimariesBlueX = "";
  hdrMetadata.displayPrimariesBlueY = "";
  hdrMetadata.whitePointX = "";
  hdrMetadata.whitePointY = "";
  hdrMetadata.maxDisplayMasteringLuminance = "";
  hdrMetadata.minDisplayMasteringLuminance = "";
  hdrMetadata.maximumContentLightLevel = "";
  hdrMetadata.maximumFrameAverageLightLevel = "";
  hdrMetadata.colorspace = "";

  IDeckLinkVideoFrameMetadataExtensions* metadataExtensions = NULL;
  if (videoFrame->QueryInterface(IID_IDeckLinkVideoFrameMetadataExtensions, (void**)&metadataExtensions) == S_OK)
  {
    double doubleValue = 0.0;
    int64_t intValue = 0;

    if (metadataExtensions->GetInt(bmdDeckLinkFrameMetadataHDRElectroOpticalTransferFunc, &intValue) == S_OK)
    {
      switch (intValue)
      {
        case 0:
          hdrMetadata.electroOpticalTransferFunction = ("SDR");
          break;
        case 1:
          hdrMetadata.electroOpticalTransferFunction = ("HDR");
          break;
        case 2:
          hdrMetadata.electroOpticalTransferFunction = ("PQ (ST2084)");
          break;
        case 3:
          hdrMetadata.electroOpticalTransferFunction = ("HLG");
          break;
        default:
          {
            std::stringstream ss;
            ss << "Unknown EOTF: " << (int32_t)intValue;
            hdrMetadata.electroOpticalTransferFunction = ss.str();
          }
          break;
      }
    }

    if (videoFrame->GetFlags() & bmdFrameContainsHDRMetadata)
    {
      if (metadataExtensions->GetFloat(bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedX, &doubleValue) == S_OK)
      {
        hdrMetadata.displayPrimariesRedX = DoubleTo4String(doubleValue);
      }

      if (metadataExtensions->GetFloat(bmdDeckLinkFrameMetadataHDRDisplayPrimariesRedY, &doubleValue) == S_OK)
      {
        hdrMetadata.displayPrimariesRedY = DoubleTo4String(doubleValue);
      }

      if (metadataExtensions->GetFloat(bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenX, &doubleValue) == S_OK)
      {
        hdrMetadata.displayPrimariesGreenX = DoubleTo4String(doubleValue);
      }

      if (metadataExtensions->GetFloat(bmdDeckLinkFrameMetadataHDRDisplayPrimariesGreenY, &doubleValue) == S_OK)
      {
        hdrMetadata.displayPrimariesGreenY = DoubleTo4String(doubleValue);
      }

      if (metadataExtensions->GetFloat(bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueX, &doubleValue) == S_OK)
      {
        hdrMetadata.displayPrimariesBlueX = DoubleTo4String(doubleValue);
      }

      if (metadataExtensions->GetFloat(bmdDeckLinkFrameMetadataHDRDisplayPrimariesBlueY, &doubleValue) == S_OK)
      {
        hdrMetadata.displayPrimariesBlueY = DoubleTo4String(doubleValue);
      }

      if (metadataExtensions->GetFloat(bmdDeckLinkFrameMetadataHDRWhitePointX, &doubleValue) == S_OK)
      {
        hdrMetadata.whitePointX = DoubleTo4String(doubleValue);
      }

      if (metadataExtensions->GetFloat(bmdDeckLinkFrameMetadataHDRWhitePointY, &doubleValue) == S_OK)
      {
        hdrMetadata.whitePointY = DoubleTo4String(doubleValue);
      }

      if (metadataExtensions->GetFloat(bmdDeckLinkFrameMetadataHDRMaxDisplayMasteringLuminance, &doubleValue) == S_OK)
      {
        hdrMetadata.maxDisplayMasteringLuminance = DoubleTo4String(doubleValue);
      }

      if (metadataExtensions->GetFloat(bmdDeckLinkFrameMetadataHDRMinDisplayMasteringLuminance, &doubleValue) == S_OK)
      {
        hdrMetadata.minDisplayMasteringLuminance = DoubleTo4String(doubleValue);
      }

      if (metadataExtensions->GetFloat(bmdDeckLinkFrameMetadataHDRMaximumContentLightLevel, &doubleValue) == S_OK)
      {
        hdrMetadata.maximumContentLightLevel = DoubleTo4String(doubleValue);
      }

      if (metadataExtensions->GetFloat(bmdDeckLinkFrameMetadataHDRMaximumFrameAverageLightLevel, &doubleValue) == S_OK)
      {
        hdrMetadata.maximumFrameAverageLightLevel = DoubleTo4String(doubleValue);
      }

      if (metadataExtensions->GetInt(bmdDeckLinkFrameMetadataColorspace, &intValue) == S_OK)
      {
        switch (intValue)
        {
          case bmdColorspaceRec601:
            hdrMetadata.colorspace = "Rec.601";
            break;
          case bmdColorspaceRec709:
            hdrMetadata.colorspace = "Rec.709";
            break;
          case bmdColorspaceRec2020:
            hdrMetadata.colorspace = "Rec.2020";
            break;
        }
      }
    }
    metadataExtensions->Release();
  }
}

//----------------------------------------------------------------------------
DeckLinkDeviceDiscovery::DeckLinkDeviceDiscovery()
  : m_deckLinkDiscovery(NULL),
    m_refCount(1)
{
  if (CoCreateInstance(CLSID_CDeckLinkDiscovery, NULL, CLSCTX_ALL, IID_IDeckLinkDiscovery, (void**)&m_deckLinkDiscovery) != S_OK)
  {
    m_deckLinkDiscovery = NULL;
  }
}

//----------------------------------------------------------------------------
DeckLinkDeviceDiscovery::~DeckLinkDeviceDiscovery()
{
  if (m_deckLinkDiscovery != NULL)
  {
    // Uninstall device arrival notifications and release discovery object
    m_deckLinkDiscovery->UninstallDeviceNotifications();
    m_deckLinkDiscovery->Release();
    m_deckLinkDiscovery = NULL;
  }
}

//----------------------------------------------------------------------------
bool DeckLinkDeviceDiscovery::Enable()
{
  HRESULT result = E_FAIL;

  // Install device arrival notifications
  if (m_deckLinkDiscovery != NULL)
  {
    result = m_deckLinkDiscovery->InstallDeviceNotifications(this);
  }

  return result == S_OK;
}

//----------------------------------------------------------------------------
void DeckLinkDeviceDiscovery::Disable()
{
  // Uninstall device arrival notifications
  if (m_deckLinkDiscovery != NULL)
  {
    m_deckLinkDiscovery->UninstallDeviceNotifications();
  }
}

//----------------------------------------------------------------------------
HRESULT DeckLinkDeviceDiscovery::DeckLinkDeviceArrived(IDeckLink* deckLink)
{
  // callback mechanism needed to tell listeners that event has happened
  deckLink->AddRef();
  return S_OK;
}

//----------------------------------------------------------------------------
HRESULT DeckLinkDeviceDiscovery::DeckLinkDeviceRemoved(IDeckLink* deckLink)
{
  // callback mechanism needed to tell listeners that event has happened
  deckLink->Release();
  return S_OK;
}

//----------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE DeckLinkDeviceDiscovery::QueryInterface(REFIID iid, LPVOID* ppv)
{
  HRESULT result = E_NOINTERFACE;

  if (ppv == NULL)
  {
    return E_INVALIDARG;
  }

  // Initialize the return result
  *ppv = NULL;

  // Obtain the IUnknown interface and compare it the provided REFIID
  if (iid == IID_IUnknown)
  {
    *ppv = this;
    AddRef();
    result = S_OK;
  }
  else if (iid == IID_IDeckLinkDeviceNotificationCallback)
  {
    *ppv = (IDeckLinkDeviceNotificationCallback*)this;
    AddRef();
    result = S_OK;
  }

  return result;
}

//----------------------------------------------------------------------------
unsigned long STDMETHODCALLTYPE DeckLinkDeviceDiscovery::AddRef(void)
{
  return InterlockedIncrement((LONG*)&m_refCount);
}

//----------------------------------------------------------------------------
unsigned long STDMETHODCALLTYPE DeckLinkDeviceDiscovery::Release(void)
{
  unsigned long newRefValue;

  newRefValue = InterlockedDecrement((LONG*)&m_refCount);
  if (newRefValue == 0)
  {
    delete this;
    return 0;
  }

  return newRefValue;
}
