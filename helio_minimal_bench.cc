#include <system_error>
#include "base/init.h"
#include "util/accept_server.h"
#include "util/fibers/pool.h"
#include "util/listener_interface.h"
#include "util/connection.h"

using namespace util;
using namespace std;

class EchoConnection : public Connection {
 private:
  void HandleRequests() final {
    ThisFiber::SetName("EchoHandler");

    uint8_t buf[4096];
    const char* response = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/plain\r\n"
                           "Content-Length: 13\r\n"
                           "Connection: keep-alive\r\n"
                           "\r\n"
                           "Hello, World!";
    size_t response_len = strlen(response);

    while (true) {
      auto recv_res = socket_->Recv(buf, sizeof(buf));
      if (!recv_res)               // peer closed / error
        break;

      std::error_code ec = socket_->Write(io::Bytes(reinterpret_cast<const uint8_t*>(response), response_len));
      if (ec)
        break;
    }
  }
};

class EchoListener : public ListenerInterface {
 public:
  Connection* NewConnection(ProactorBase* proactor) final {
    return new EchoConnection;
  }
};

int main(int argc, char* argv[]) {
  MainInitGuard guard(&argc, &argv);

  // 4 threads to match our bench
  std::unique_ptr<util::ProactorPool> pool{fb2::Pool::IOUring(256, 4)};
  pool->Run();

  AcceptServer server(pool.get());
  // Listen on port 8082
  server.AddListener("0.0.0.0", 8082, new EchoListener);
  server.Run();
  server.Wait();   // blocks until shutdown

  pool->Stop();
  return 0;
}
