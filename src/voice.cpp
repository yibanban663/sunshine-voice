/**
 * @file src/voice.cpp
 * @brief x-ss-voice: host voice track capture and streaming.
 *
 * Captures the host microphone (WASAPI shared mode) → Opus 48kHz mono →
 * independent RTP stream on VOICE_STREAM_PORT.  Fully parallel to the game
 * audio track — no shared state beyond the session encryption key.
 */
#include <thread>
#include <chrono>
#include <array>
#include <vector>

#include <boost/asio.hpp>

#include <opus/opus.h>

#include "voice.h"
#include "stream.h"
#include "config.h"
#include "crypto.h"
#include "utility.h"
#include "platform/common.h"

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#define VOICE_WASAPI 1
#endif

namespace voice {
  using namespace stream;

#ifdef VOICE_WASAPI

  class wasapi_capture_t {
  public:
    static std::unique_ptr<wasapi_capture_t>
    open(const std::string &device_name) {
      HRESULT hr;
      auto c = std::unique_ptr<wasapi_capture_t>(new wasapi_capture_t());
      hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                            __uuidof(IMMDeviceEnumerator), (void **) &c->enum_);
      if (FAILED(hr)) return nullptr;

      IMMDevice *dev = nullptr;
      if (!device_name.empty()) {
        IMMDeviceCollection *coll = nullptr;
        c->enum_->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll);
        UINT n = 0; coll->GetCount(&n);
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
          // Case-insensitive substring match
          std::transform(name.begin(), name.end(), name.begin(), ::towlower);
          std::transform(want.begin(), want.end(), want.begin(), ::towlower);
          if (name.find(want) != std::wstring::npos) break;
          dev->Release(); dev = nullptr;
        }
        coll->Release();
      }
      if (!dev) c->enum_->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
      if (!dev) return nullptr;

      hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **) &c->client_);
      dev->Release();
      if (FAILED(hr)) return nullptr;

      WAVEFORMATEX wfx {};
      wfx.wFormatTag = WAVE_FORMAT_PCM;
      wfx.nChannels = 1;
      wfx.nSamplesPerSec = 48000;
      wfx.wBitsPerSample = 16;
      wfx.nBlockAlign = 2;
      wfx.nAvgBytesPerSec = 96000;
      hr = c->client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, &wfx, nullptr);
      if (FAILED(hr)) return nullptr;
      hr = c->client_->GetService(__uuidof(IAudioCaptureClient), (void **) &c->cap_);
      if (FAILED(hr)) return nullptr;
      c->client_->Start();
      return c;
    }

    bool read_frame(std::array<std::int16_t, 480> &out) {
      static thread_local std::vector<std::int16_t> pending;
      while (pending.size() < 480) {
        UINT32 n = 0;
        HRESULT hr = cap_->GetNextPacketSize(&n);
        if (FAILED(hr) || n == 0) {
          std::fill(out.begin(), out.end(), 0);
          return true;
        }
        BYTE *data; UINT32 samples; DWORD flags;
        hr = cap_->GetBuffer(&data, &samples, &flags, nullptr, nullptr);
        if (FAILED(hr)) return false;
        auto *s = (std::int16_t *) data;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          pending.resize(pending.size() + samples);
        } else {
          pending.insert(pending.end(), s, s + samples);
        }
        cap_->ReleaseBuffer(samples);
      }
      std::copy_n(pending.begin(), 480, out.begin());
      pending.erase(pending.begin(), pending.begin() + 480);
      return true;
    }

    ~wasapi_capture_t() {
      if (client_) client_->Stop();
    }

  private:
    IMMDeviceEnumerator *enum_ {};
    IAudioClient *client_ {};
    IAudioCaptureClient *cap_ {};
  };
#endif // VOICE_WASAPI

  struct rtp_header_t {
    std::uint8_t header = 0x80;
    std::uint8_t packetType = 97;
    std::uint16_t sequenceNumber;
    std::uint32_t timestamp;
    std::uint32_t ssrc = 0;
  };

  void
  voiceThread(session_t *session) {
    platf::set_thread_name("stream::voice");

#ifdef VOICE_WASAPI
    // 1) Open the voice socket
    auto &io_context = *session->voice_sock_ctx;
    udp::socket sock(io_context);
    boost::system::error_code ec;
    sock.open(udp::v4(), ec);
    if (ec) return;

    auto bind_addr = boost::asio::ip::make_address(net::get_bind_address(net::IPV4), ec);
    sock.bind(udp::endpoint(bind_addr, net::map_port(VOICE_STREAM_PORT)), ec);
    if (ec) {
      BOOST_LOG(warning) << "x-ss-voice: couldn't bind port "sv << net::map_port(VOICE_STREAM_PORT);
      return;
    }

    // 2) Wait for first packet (client ping) to learn the peer endpoint
    std::array<char, 256> ping_buf;
    udp::endpoint peer;
    sock.receive_from(boost::asio::buffer(ping_buf), peer, 0, ec);
    if (ec) return;
    session->voice.peer = peer;
    BOOST_LOG(info) << "x-ss-voice: peer "sv << peer.address().to_string() << ":"sv << peer.port();

    // 3) Open the microphone
    auto cap = wasapi_capture_t::open(config::audio.voice_source);
    if (!cap) {
      BOOST_LOG(warning) << "x-ss-voice: couldn't open voice_source '"sv << config::audio.voice_source << "'"sv;
      return;
    }

    // 4) Create Opus encoder (VOIP mode, mono, 48kHz)
    int err = 0;
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>
      enc(opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err), &opus_encoder_destroy);
    if (err != OPUS_OK) return;

    BOOST_LOG(info) << "x-ss-voice: voice track started"sv;

    // 5) Capture → encode → RTP → send
    std::array<std::int16_t, 480> frame {};
    std::array<std::uint8_t, 4000> payload {};
    std::array<std::uint8_t, 1275> plain {};
    std::array<std::uint8_t, 16> iv {};

    while (!session->shutdown_event->peek()) {
      if (!cap->read_frame(frame)) break;

      auto opus_len = opus_encode(enc.get(), frame.data(), 480,
                                   plain.data(), (opus_int32) plain.size());
      if (opus_len < 0) continue;

      auto seq = session->voice.sequenceNumber;
      *(std::uint32_t *) iv.data() =
        util::endian::big<std::uint32_t>(session->voice.avRiKeyId + seq);

      // Encrypt (Moonlight audio format: 1-byte length prefix + AES-CBC ciphertext)
      std::uint8_t header[1] = { (std::uint8_t) opus_len };
      std::memcpy(payload.data(), header, 1);

      auto cipher_len = crypto::encrypt(
        session->voice.cipher,
        plain.data(), opus_len,
        payload.data() + 1,
        iv.data());

      auto send_len = (cipher_len > 0) ? cipher_len + 1 : opus_len + 1;

      rtp_header_t rtp;
      rtp.sequenceNumber = util::endian::big(seq);
      rtp.timestamp = util::endian::big(session->voice.timestamp);

      // Send: RTP header + payload in one datagram
      std::array<std::uint8_t, sizeof(rtp_header_t) + 4000> dg {};
      std::memcpy(dg.data(), &rtp, sizeof(rtp));
      std::memcpy(dg.data() + sizeof(rtp), payload.data(), send_len);

      sock.send_to(boost::asio::buffer(dg.data(), sizeof(rtp) + send_len), peer, 0, ec);
      if (ec) {
        BOOST_LOG(warning) << "x-ss-voice: send error "sv << ec.message();
        break;
      }

      session->voice.sequenceNumber++;
      session->voice.timestamp += 10;

      std::this_thread::sleep_for(std::chrono::milliseconds(9));
    }

    BOOST_LOG(info) << "x-ss-voice: voice track stopped"sv;
#endif // _WIN32
  }

} // namespace voice
