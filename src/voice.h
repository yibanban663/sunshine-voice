#pragma once

namespace stream {
  struct session_t;
}

namespace voice {
  void voiceThread(stream::session_t *session);
}
