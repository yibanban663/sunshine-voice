/**
 * @file src/voice.cpp
 * @brief x-ss-voice: host microphone capture (WASAPI shared mode).
 */
#include "voice.h"

#include <algorithm>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#endif

namespace voice {

#ifdef _WIN32

  struct capturer::impl_t {
    IMMDeviceEnumerator *enum_ {};
    IAudioClient *client_ {};
    IAudioCaptureClient *cap_ {};
    std::vector<std::int16_t> pending;

    bool open(const std::string &device_name) {
      HRESULT hr;

      // WASAPI requires COM on the calling thread
      CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);

      hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                            __uuidof(IMMDeviceEnumerator), (void **) &enum_);
      if (FAILED(hr)) return false;

      IMMDevice *dev = nullptr;
      if (!device_name.empty()) {
        // Match the capture endpoint by friendly name (case-insensitive substring)
        IMMDeviceCollection *coll = nullptr;
        enum_->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll);
        UINT n = 0;
        coll->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
          coll->Item(i, &dev);
          IPropertyStore *ps;
          dev->OpenPropertyStore(STGM_READ, &ps);
          PROPVARIANT pv;
          PropVariantInit(&pv);
          ps->GetValue(PKEY_Device_FriendlyName, &pv);
          std::wstring name(pv.pwszVal ? pv.pwszVal : L"");
          PropVariantClear(&pv);
          ps->Release();
          std::wstring want(device_name.begin(), device_name.end());
          std::transform(name.begin(), name.end(), name.begin(), ::towlower);
          std::transform(want.begin(), want.end(), want.begin(), ::towlower);
          if (name.find(want) != std::wstring::npos) break;
          dev->Release();
          dev = nullptr;
        }
        coll->Release();
      }
      if (!dev) enum_->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
      if (!dev) return false;

      hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **) &client_);
      dev->Release();
      if (FAILED(hr)) return false;

      // Voice: 48 kHz mono 16-bit, 100 ms buffer
      WAVEFORMATEX wfx {};
      wfx.wFormatTag = WAVE_FORMAT_PCM;
      wfx.nChannels = 1;
      wfx.nSamplesPerSec = SAMPLE_RATE;
      wfx.wBitsPerSample = 16;
      wfx.nBlockAlign = 2;
      wfx.nAvgBytesPerSec = SAMPLE_RATE * 2;
      hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000 /*100ms*/, 0, &wfx, nullptr);
      if (FAILED(hr)) return false;
      hr = client_->GetService(__uuidof(IAudioCaptureClient), (void **) &cap_);
      if (FAILED(hr)) return false;
      client_->Start();
      return true;
    }

    bool read_frame(std::array<std::int16_t, FRAME_SAMPLES> &out) {
      while (pending.size() < FRAME_SAMPLES) {
        UINT32 n = 0;
        HRESULT hr = cap_->GetNextPacketSize(&n);
        if (FAILED(hr) || n == 0) {
          // Not enough captured yet: pad with silence to keep the timeline
          std::fill(out.begin(), out.end(), 0);
          return true;
        }
        BYTE *data;
        UINT32 samples;
        DWORD flags;
        hr = cap_->GetBuffer(&data, &samples, &flags, nullptr, nullptr);
        if (FAILED(hr)) return false;
        auto *s = (std::int16_t *) data;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          pending.resize(pending.size() + samples);
        }
        else {
          pending.insert(pending.end(), s, s + samples);
        }
        cap_->ReleaseBuffer(samples);
      }
      std::copy_n(pending.begin(), FRAME_SAMPLES, out.begin());
      pending.erase(pending.begin(), pending.begin() + FRAME_SAMPLES);
      return true;
    }

    ~impl_t() {
      if (client_) client_->Stop();
    }
  };

  capturer::capturer() :
      impl(std::make_unique<impl_t>()) {}
  capturer::~capturer() = default;

  std::unique_ptr<capturer>
  capturer::open(const std::string &device_name) {
    auto cap = std::unique_ptr<capturer>(new capturer());
    if (!cap->impl->open(device_name)) {
      return nullptr;
    }
    return cap;
  }

  bool
  capturer::read_frame(std::array<std::int16_t, FRAME_SAMPLES> &out) {
    return impl->read_frame(out);
  }

#else  // !_WIN32

  struct capturer::impl_t {};

  capturer::capturer() {}
  capturer::~capturer() {}

  std::unique_ptr<capturer>
  capturer::open(const std::string &) {
    return nullptr;
  }

  bool
  capturer::read_frame(std::array<std::int16_t, FRAME_SAMPLES> &) {
    return false;
  }

#endif

}  // namespace voice
