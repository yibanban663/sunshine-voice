/**
 * @file src/voice.h
 * @brief x-ss-voice: host microphone capture for the voice track.
 *
 * Only the capture abstraction lives here; the RTP send loop runs in
 * stream.cpp where the session state is visible.
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace voice {

  constexpr auto SAMPLE_RATE = 48000;
  constexpr auto FRAME_SAMPLES = 480;  ///< 10 ms of mono audio @ 48 kHz

  /**
   * @brief WASAPI shared-mode microphone capturer producing 10 ms mono
   * frames. Use open() to create; returns nullptr when the device cannot
   * be opened. On non-Windows platforms open() always fails.
   */
  class capturer {
  public:
    static std::unique_ptr<capturer> open(const std::string &device_name);
    ~capturer();

    /**
     * @brief Read the next 480-sample frame. Pads with silence when the
     * microphone is quiet so the timeline stays continuous. Returns false
     * on a fatal capture error.
     */
    bool read_frame(std::array<std::int16_t, FRAME_SAMPLES> &out);

  private:
    capturer();

    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };
}  // namespace voice
