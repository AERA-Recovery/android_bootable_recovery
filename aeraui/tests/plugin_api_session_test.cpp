/* SPDX-License-Identifier: Apache-2.0 */
#include "plugin_api/session.hpp"

#include <cassert>
#include <sys/socket.h>
#include <unistd.h>

using namespace aeraui::plugin_api;

static void SendWorker(int fd, Kind kind, uint32_t request = 0,
                       uint32_t value = 0, uint32_t flags = 0) {
  Message message;
  message.kind = kind;
  message.request_id = request;
  message.value = value;
  message.flags = flags;
  assert(send(fd, &message, sizeof(message), MSG_NOSIGNAL) ==
         static_cast<ssize_t>(sizeof(message)));
}

static void SendWorker3(int fd, Kind kind, uint32_t request = 0,
                        uint32_t value = 0, uint32_t flags = 0) {
  Message message;
  message.version = kProtocolVersion3;
  message.kind = kind;
  message.request_id = request;
  message.value = value;
  message.flags = flags;
  assert(send(fd, &message, sizeof(message), MSG_NOSIGNAL) ==
         static_cast<ssize_t>(sizeof(message)));
}

static Message ReceiveHost(int fd, uint32_t version = kProtocolVersion) {
  Message message;
  assert(recv(fd, &message, sizeof(message), MSG_WAITALL) ==
         static_cast<ssize_t>(sizeof(message)));
  assert(Valid(message, false, version));
  return message;
}

int main() {
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
  Session session;
  assert(session.Adopt(pair[0]));
  SendWorker(pair[1], Kind::kHello, 0, 2, 2);
  assert(session.Poll().empty());
  assert(session.Negotiated());
  const Message hello = ReceiveHost(pair[1]);
  assert(hello.kind == Kind::kHelloAck);
  assert(hello.value == kProtocolVersion);
  assert((hello.flags & kFeatureMetrics) != 0);
  assert((hello.flags & kFeatureBackNavigation) != 0);
  assert((hello.flags & kFeatureFilePicker) == 0);
  const Message resume = ReceiveHost(pair[1]);
  assert(resume.kind == Kind::kLifecycle);
  assert(resume.value == static_cast<uint32_t>(Lifecycle::kResume));
  SendWorker(pair[1], Kind::kBeginPage);
  const auto page = session.Poll();
  assert(page.size() == 1 && page[0].kind == Kind::kBeginPage);
  SendWorker(pair[1], Kind::kAddMetric, 7, 0, kMetricAccent);
  const auto metric = session.Poll();
  assert(metric.size() == 1 && metric[0].kind == Kind::kAddMetric);
  assert(metric[0].request_id == 7 && metric[0].flags == kMetricAccent);
  SendWorker(pair[1], Kind::kSetBackAction, 42);
  const auto back = session.Poll();
  assert(back.size() == 1 && back[0].kind == Kind::kSetBackAction);
  assert(back[0].request_id == 42);
  close(pair[1]);
  assert(session.Poll().empty());
  assert(!session.Connected());

  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
  assert(session.Adopt(pair[0]));
  SendWorker(pair[1], Kind::kHello, 0, 3, 3);
  assert(session.Poll().empty());
  assert(!session.Connected());
  close(pair[1]);

  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
  assert(session.Adopt(pair[0]));
  SendWorker(pair[1], Kind::kHello, 0, 2, 2);
  session.Poll();
  (void)ReceiveHost(pair[1]);
  (void)ReceiveHost(pair[1]);
  for (unsigned index = 0; index < 129 && session.Connected(); ++index) {
    SendWorker(pair[1], Kind::kSetStatus);
    (void)session.Poll();
  }
  assert(!session.Connected());
  close(pair[1]);

  // Host API 3: the pixel scene offers exactly version 3 and sends SURFACE
  // between HELLO_ACK and RESUME.
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
  Message surface;
  surface.kind = Kind::kSurface;
  surface.value = 1080;
  surface.flags = 2400;
  assert(session.Adopt(pair[0], kProtocolVersion3, {surface}));
  SendWorker3(pair[1], Kind::kHello, 0, 3, 3);
  assert(session.Poll().empty());
  assert(session.Negotiated() && session.Version() == kProtocolVersion3);
  const Message ack3 = ReceiveHost(pair[1], kProtocolVersion3);
  assert(ack3.kind == Kind::kHelloAck && ack3.value == kProtocolVersion3);
  assert((ack3.flags & kFeaturePixelSurface) != 0);
  assert((ack3.flags & kFeatureKeyboardInset) != 0);
  assert((ack3.flags & kFeatureFilePicker) != 0);
  const Message surface3 = ReceiveHost(pair[1], kProtocolVersion3);
  assert(surface3.kind == Kind::kSurface && surface3.value == 1080);
  assert(ReceiveHost(pair[1], kProtocolVersion3).kind == Kind::kLifecycle);
  // Frames are outside the control rate limit.
  for (unsigned index = 0; index < 300 && session.Connected(); ++index) {
    SendWorker3(pair[1], Kind::kPresent, index + 1, 0);
    (void)session.Poll();
  }
  assert(session.Connected());
  // A v2 kind number past v3's range is still refused.
  SendWorker3(pair[1], static_cast<Kind>(99));
  (void)session.Poll();
  assert(!session.Connected());
  close(pair[1]);

  // A Host API 2 plugin cannot open a pixel scene.
  assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
  assert(session.Adopt(pair[0], kProtocolVersion3));
  SendWorker(pair[1], Kind::kHello, 0, 2, 2);
  assert(session.Poll().empty());
  assert(!session.Connected());
  close(pair[1]);
}
