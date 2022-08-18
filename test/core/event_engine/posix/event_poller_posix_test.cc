// Copyright 2022 The gRPC Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <ostream>

#include "src/core/lib/iomgr/port.h"

// This test won't work except with posix sockets enabled
#ifdef GRPC_POSIX_SOCKET_EV

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/status/status.h"

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>

#include "src/core/lib/event_engine/posix_engine/event_poller.h"
#include "src/core/lib/event_engine/posix_engine/event_poller_posix_default.h"
#include "src/core/lib/event_engine/posix_engine/posix_engine.h"
#include "src/core/lib/event_engine/posix_engine/posix_engine_closure.h"
#include "src/core/lib/gprpp/global_config.h"
#include "test/core/util/port.h"

GPR_GLOBAL_CONFIG_DECLARE_STRING(grpc_poll_strategy);

using ::grpc_event_engine::posix_engine::EventPoller;

static gpr_mu g_mu;
static EventPoller* g_event_poller = nullptr;

// buffer size used to send and receive data.
// 1024 is the minimal value to set TCP send and receive buffer.
#define BUF_SIZE 1024
// Max number of connections pending to be accepted by listen().
#define MAX_NUM_FD 1024
// Client write buffer size
#define CLIENT_WRITE_BUF_SIZE 10
// Total number of times that the client fills up the write buffer
#define CLIENT_TOTAL_WRITE_CNT 3

namespace grpc_event_engine {
namespace posix_engine {

namespace {

class TestScheduler : public Scheduler {
 public:
  explicit TestScheduler(experimental::EventEngine* engine) : engine_(engine) {}
  void Run(experimental::EventEngine::Closure* closure) override {
    engine_->Run(closure);
  }

 private:
  experimental::EventEngine* engine_;
};

absl::Status SetSocketSendBuf(int fd, int buffer_size_bytes) {
  return 0 == setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_size_bytes,
                         sizeof(buffer_size_bytes))
             ? absl::OkStatus()
             : absl::Status(absl::StatusCode::kInternal, strerror(errno));
}

// Create a test socket with the right properties for testing.
// port is the TCP port to listen or connect to.
// Return a socket FD and sockaddr_in.
void CreateTestSocket(int port, int* socket_fd, struct sockaddr_in6* sin) {
  int fd;
  int one = 1;
  int buffer_size_bytes = BUF_SIZE;
  int flags;

  fd = socket(AF_INET6, SOCK_STREAM, 0);
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  // Reset the size of socket send buffer to the minimal value to facilitate
  // buffer filling up and triggering notify_on_write
  EXPECT_TRUE(SetSocketSendBuf(fd, buffer_size_bytes).ok());
  EXPECT_TRUE(SetSocketSendBuf(fd, buffer_size_bytes).ok());
  // Make fd non-blocking.
  flags = fcntl(fd, F_GETFL, 0);
  EXPECT_EQ(fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0);
  *socket_fd = fd;

  // Use local address for test.
  memset(sin, 0, sizeof(struct sockaddr_in6));
  sin->sin6_family = AF_INET6;
  (reinterpret_cast<char*>(&sin->sin6_addr))[15] = 1;
  EXPECT_TRUE(port >= 0 && port < 65536);
  sin->sin6_port = htons(static_cast<uint16_t>(port));
}

//  =======An upload server to test notify_on_read===========
//    The server simply reads and counts a stream of bytes.

// An upload server.
typedef struct {
  EventHandle* em_fd;       /* listening fd */
  ssize_t read_bytes_total; /* total number of received bytes */
  int done;                 /* set to 1 when a server finishes serving */
  IomgrEngineClosure* listen_closure;
} server;

void ServerInit(server* sv) {
  sv->read_bytes_total = 0;
  sv->done = 0;
}

// An upload session.
// Created when a new upload request arrives in the server.
typedef struct {
  server* sv;              /* not owned by a single session */
  EventHandle* em_fd;      /* fd to read upload bytes */
  char read_buf[BUF_SIZE]; /* buffer to store upload bytes */
  IomgrEngineClosure* session_read_closure;
} session;

// Called when an upload session can be safely shutdown.
// Close session FD and start to shutdown listen FD.
void SessionShutdownCb(session* se, bool /*success*/) {
  server* sv = se->sv;
  se->em_fd->OrphanHandle(nullptr, nullptr, "a");
  gpr_free(se);
  // Start to shutdown listen fd.
  sv->em_fd->ShutdownHandle(
      absl::Status(absl::StatusCode::kUnknown, "SessionShutdownCb"));
}

// Called when data become readable in a session.
void SessionReadCb(session* se, absl::Status status) {
  int fd = se->em_fd->WrappedFd();

  ssize_t read_once = 0;
  ssize_t read_total = 0;

  if (!status.ok()) {
    SessionShutdownCb(se, true);
    return;
  }

  do {
    read_once = read(fd, se->read_buf, BUF_SIZE);
    if (read_once > 0) read_total += read_once;
  } while (read_once > 0);
  se->sv->read_bytes_total += read_total;

  // read() returns 0 to indicate the TCP connection was closed by the
  // client read(fd, read_buf, 0) also returns 0 which should never be called as
  // such. It is possible to read nothing due to spurious edge event or data has
  // been drained, In such a case, read() returns -1 and set errno to
  // EAGAIN.
  if (read_once == 0) {
    SessionShutdownCb(se, true);
  } else if (read_once == -1) {
    EXPECT_EQ(errno, EAGAIN);
    // An edge triggered event is cached in the kernel until next poll.
    // In the current single thread implementation, SessionReadCb is called
    // in the polling thread, such that polling only happens after this
    // callback, and will catch read edge event if data is available again
    // before notify_on_read.
    se->session_read_closure = IomgrEngineClosure::TestOnlyToClosure(
        [se](absl::Status status) { SessionReadCb(se, status); });
    se->em_fd->NotifyOnRead(se->session_read_closure);
  }
}

// Called when the listen FD can be safely shutdown. Close listen FD and
// signal that server can be shutdown.
void ListenShutdownCb(server* sv) {
  sv->em_fd->OrphanHandle(nullptr, nullptr, "b");
  gpr_mu_lock(&g_mu);
  sv->done = 1;
  g_event_poller->Kick();
  gpr_mu_unlock(&g_mu);
}

// Called when a new TCP connection request arrives in the listening port.
void ListenCb(server* sv, absl::Status status) {
  int fd;
  int flags;
  session* se;
  struct sockaddr_storage ss;
  socklen_t slen = sizeof(ss);
  EventHandle* listen_em_fd = sv->em_fd;

  if (!status.ok()) {
    ListenShutdownCb(sv);
    return;
  }

  do {
    fd = accept(listen_em_fd->WrappedFd(),
                reinterpret_cast<struct sockaddr*>(&ss), &slen);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0 && errno == EAGAIN) {
    sv->listen_closure = IomgrEngineClosure::TestOnlyToClosure(
        [sv](absl::Status status) { ListenCb(sv, status); });
    listen_em_fd->NotifyOnRead(sv->listen_closure);
    return;
  }
  EXPECT_GE(fd, 0);
  EXPECT_LT(fd, FD_SETSIZE);
  flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  se = static_cast<session*>(gpr_malloc(sizeof(*se)));
  se->sv = sv;
  se->em_fd = g_event_poller->CreateHandle(fd, "listener", false);
  se->session_read_closure = IomgrEngineClosure::TestOnlyToClosure(
      [se](absl::Status status) { SessionReadCb(se, status); });
  se->em_fd->NotifyOnRead(se->session_read_closure);
  sv->listen_closure = IomgrEngineClosure::TestOnlyToClosure(
      [sv](absl::Status status) { ListenCb(sv, status); });
  listen_em_fd->NotifyOnRead(sv->listen_closure);
}

// Start a test server, return the TCP listening port bound to listen_fd.
// ListenCb() is registered to be interested in reading from listen_fd.
// When connection request arrives, ListenCb() is called to accept the
// connection request.
int ServerStart(server* sv) {
  int port = grpc_pick_unused_port_or_die();
  int fd;
  struct sockaddr_in6 sin;
  socklen_t addr_len;

  CreateTestSocket(port, &fd, &sin);
  addr_len = sizeof(sin);
  EXPECT_EQ(bind(fd, (struct sockaddr*)&sin, addr_len), 0);
  EXPECT_EQ(getsockname(fd, (struct sockaddr*)&sin, &addr_len), 0);
  port = ntohs(sin.sin6_port);
  EXPECT_EQ(listen(fd, MAX_NUM_FD), 0);

  sv->em_fd = g_event_poller->CreateHandle(fd, "server", false);
  sv->listen_closure = IomgrEngineClosure::TestOnlyToClosure(
      [sv](absl::Status status) { ListenCb(sv, status); });
  sv->em_fd->NotifyOnRead(sv->listen_closure);
  return port;
}

// ===An upload client to test notify_on_write===

// An upload client.
typedef struct {
  EventHandle* em_fd;
  char write_buf[CLIENT_WRITE_BUF_SIZE];
  ssize_t write_bytes_total;
  // Number of times that the client fills up the write buffer and calls
  // notify_on_write to schedule another write.
  int client_write_cnt;
  int done;
  IomgrEngineClosure* write_closure;
} client;

void ClientInit(client* cl) {
  memset(cl->write_buf, 0, sizeof(cl->write_buf));
  cl->write_bytes_total = 0;
  cl->client_write_cnt = 0;
  cl->done = 0;
}

// Called when a client upload session is ready to shutdown.
void ClientSessionShutdownCb(client* cl) {
  cl->em_fd->OrphanHandle(nullptr, nullptr, "c");
  gpr_mu_lock(&g_mu);
  cl->done = 1;
  g_event_poller->Kick();
  gpr_mu_unlock(&g_mu);
}

// Write as much as possible, then register notify_on_write.
void ClientSessionWrite(client* cl, absl::Status status) {
  int fd = cl->em_fd->WrappedFd();
  ssize_t write_once = 0;

  if (!status.ok()) {
    ClientSessionShutdownCb(cl);
    return;
  }

  do {
    write_once = write(fd, cl->write_buf, CLIENT_WRITE_BUF_SIZE);
    if (write_once > 0) cl->write_bytes_total += write_once;
  } while (write_once > 0);

  EXPECT_EQ(errno, EAGAIN);
  gpr_mu_lock(&g_mu);
  if (cl->client_write_cnt < CLIENT_TOTAL_WRITE_CNT) {
    cl->write_closure = IomgrEngineClosure::TestOnlyToClosure(
        [cl](absl::Status status) { ClientSessionWrite(cl, status); });
    cl->client_write_cnt++;
    gpr_mu_unlock(&g_mu);
    cl->em_fd->NotifyOnWrite(cl->write_closure);
  } else {
    gpr_mu_unlock(&g_mu);
    ClientSessionShutdownCb(cl);
  }
}

// Start a client to send a stream of bytes.
void ClientStart(client* cl, int port) {
  int fd;
  struct sockaddr_in6 sin;
  CreateTestSocket(port, &fd, &sin);
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin)) ==
      -1) {
    if (errno == EINPROGRESS) {
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      if (poll(&pfd, 1, -1) == -1) {
        gpr_log(GPR_ERROR, "poll() failed during connect; errno=%d", errno);
        abort();
      }
    } else {
      gpr_log(GPR_ERROR, "Failed to connect to the server (errno=%d)", errno);
      abort();
    }
  }

  cl->em_fd = g_event_poller->CreateHandle(fd, "client", false);
  ClientSessionWrite(cl, absl::OkStatus());
}

// Wait for the signal to shutdown client and server.
void WaitAndShutdown(server* sv, client* cl) {
  std::vector<EventHandle*> pending_events;
  gpr_mu_lock(&g_mu);
  while (!sv->done || !cl->done) {
    gpr_mu_unlock(&g_mu);
    (void)g_event_poller->Work(grpc_core::Timestamp::InfFuture(),
                               pending_events);
    for (auto it = pending_events.begin(); it != pending_events.end(); ++it) {
      (*it)->ExecutePendingActions();
    }
    pending_events.clear();
    gpr_mu_lock(&g_mu);
  }
  gpr_mu_unlock(&g_mu);
}

std::string TestScenarioName(
    const ::testing::TestParamInfo<std::string>& info) {
  return info.param;
}

class EventPollerTest : public ::testing::TestWithParam<std::string> {
  void SetUp() override {
    engine_ =
        absl::make_unique<grpc_event_engine::experimental::PosixEventEngine>();
    EXPECT_NE(engine_, nullptr);
    scheduler_ =
        absl::make_unique<grpc_event_engine::posix_engine::TestScheduler>(
            engine_.get());
    EXPECT_NE(scheduler_, nullptr);
    GPR_GLOBAL_CONFIG_SET(grpc_poll_strategy, GetParam().c_str());
    g_event_poller = GetDefaultPoller(scheduler_.get());
  }
  void TearDown() override {
    if (g_event_poller != nullptr) {
      g_event_poller->Shutdown();
    }
  }

 private:
  std::unique_ptr<grpc_event_engine::experimental::PosixEventEngine> engine_;
  std::unique_ptr<grpc_event_engine::posix_engine::TestScheduler> scheduler_;
};

// Test grpc_fd. Start an upload server and client, upload a stream of bytes
// from the client to the server, and verify that the total number of sent
// bytes is equal to the total number of received bytes.
TEST_P(EventPollerTest, TestEventPollerHandle) {
  server sv;
  client cl;
  int port;
  if (g_event_poller == nullptr) {
    return;
  }
  ServerInit(&sv);
  port = ServerStart(&sv);
  ClientInit(&cl);
  ClientStart(&cl, port);

  WaitAndShutdown(&sv, &cl);
  EXPECT_EQ(sv.read_bytes_total, cl.write_bytes_total);
}

typedef struct FdChangeData {
  void (*cb_that_ran)(struct FdChangeData*, absl::Status);
} FdChangeData;

void InitChangeData(FdChangeData* fdc) { fdc->cb_that_ran = nullptr; }

void DestroyChangeData(FdChangeData* /*fdc*/) {}

void FirstReadCallback(FdChangeData* fdc, absl::Status /*status*/) {
  gpr_mu_lock(&g_mu);
  fdc->cb_that_ran = FirstReadCallback;
  g_event_poller->Kick();
  gpr_mu_unlock(&g_mu);
}

void SecondReadCallback(FdChangeData* fdc, absl::Status /*status*/) {
  gpr_mu_lock(&g_mu);
  fdc->cb_that_ran = SecondReadCallback;
  g_event_poller->Kick();
  gpr_mu_unlock(&g_mu);
}

// Test that changing the callback we use for notify_on_read actually works.
// Note that we have two different but almost identical callbacks above -- the
// point is to have two different function pointers and two different data
// pointers and make sure that changing both really works.
TEST_P(EventPollerTest, TestEventPollerHandleChange) {
  EventHandle* em_fd;
  FdChangeData a, b;
  int flags;
  int sv[2];
  char data;
  ssize_t result;
  if (g_event_poller == nullptr) {
    return;
  }
  IomgrEngineClosure* first_closure = IomgrEngineClosure::TestOnlyToClosure(
      [a = &a](absl::Status status) { FirstReadCallback(a, status); });
  IomgrEngineClosure* second_closure = IomgrEngineClosure::TestOnlyToClosure(
      [b = &b](absl::Status status) { SecondReadCallback(b, status); });
  InitChangeData(&a);
  InitChangeData(&b);

  EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  flags = fcntl(sv[0], F_GETFL, 0);
  EXPECT_EQ(fcntl(sv[0], F_SETFL, flags | O_NONBLOCK), 0);
  flags = fcntl(sv[1], F_GETFL, 0);
  EXPECT_EQ(fcntl(sv[1], F_SETFL, flags | O_NONBLOCK), 0);

  em_fd =
      g_event_poller->CreateHandle(sv[0], "TestEventPollerHandleChange", false);
  EXPECT_NE(em_fd, nullptr);
  // Register the first callback, then make its FD readable
  em_fd->NotifyOnRead(first_closure);
  data = 0;
  result = write(sv[1], &data, 1);
  EXPECT_EQ(result, 1);

  // And now wait for it to run.
  auto poller_work = [](FdChangeData* fdc) {
    std::vector<EventHandle*> pending_events;
    gpr_mu_lock(&g_mu);
    while (fdc->cb_that_ran == nullptr) {
      gpr_mu_unlock(&g_mu);
      (void)g_event_poller->Work(grpc_core::Timestamp::InfFuture(),
                                 pending_events);
      for (auto it = pending_events.begin(); it != pending_events.end(); ++it) {
        (*it)->ExecutePendingActions();
      }
      pending_events.clear();
      gpr_mu_lock(&g_mu);
    }
  };
  poller_work(&a);
  EXPECT_EQ(a.cb_that_ran, FirstReadCallback);
  gpr_mu_unlock(&g_mu);

  // And drain the socket so we can generate a new read edge
  result = read(sv[0], &data, 1);
  EXPECT_EQ(result, 1);

  // Now register a second callback with distinct change data, and do the same
  // thing again.
  em_fd->NotifyOnRead(second_closure);
  data = 0;
  result = write(sv[1], &data, 1);
  EXPECT_EQ(result, 1);

  // And now wait for it to run.
  poller_work(&b);
  // Except now we verify that SecondReadCallback ran instead.
  EXPECT_EQ(b.cb_that_ran, SecondReadCallback);
  gpr_mu_unlock(&g_mu);

  em_fd->OrphanHandle(nullptr, nullptr, "d");
  DestroyChangeData(&a);
  DestroyChangeData(&b);
  close(sv[1]);
}

INSTANTIATE_TEST_SUITE_P(EventPoller, EventPollerTest,
                         ::testing::ValuesIn({std::string("epoll1"),
                                              std::string("poll")}),
                         &TestScenarioName);

}  // namespace
}  // namespace posix_engine
}  // namespace grpc_event_engine

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  gpr_mu_init(&g_mu);
  return RUN_ALL_TESTS();
}

#else /* GRPC_POSIX_SOCKET_EV */

int main(int argc, char** argv) { return 1; }

#endif /* GRPC_POSIX_SOCKET_EV */