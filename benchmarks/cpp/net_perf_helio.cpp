
#include <chrono>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <netinet/tcp.h>

#include "base/init.h"
#include "util/accept_server.h"
#include "util/fibers/pool.h"
#include "util/listener_interface.h"
#include "util/connection.h"

using namespace util;

const std::string FLAGS_ip = "0.0.0.0";
const std::uint64_t FLAGS_port = 8080;
const std::uint64_t FLAGS_workers = 4;
const std::uint64_t FLAGS_backlog = 4096;

static constexpr std::string_view HTTP_RESPONSE =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 16\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, io_uring!";

class BenchConnection : public Connection {
public:
    void HandleRequests() final {
        char buf[4096];
        int yes = 1;
        setsockopt(socket_->native_handle(), IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        while (true) {
            auto res = socket_->Recv(io::MutableBytes(reinterpret_cast<uint8_t*>(buf), sizeof(buf)), 0);
            if (!res.has_value() || res.value() == 0) {
                break;
            }

            auto wres = socket_->Write(io::Bytes(reinterpret_cast<const uint8_t*>(HTTP_RESPONSE.data()), HTTP_RESPONSE.size()));
            if (wres) {
                break;
            }
        }
    }
};

class BenchListener : public ListenerInterface {
public:
    Connection* NewConnection(fb2::ProactorBase* context) final {
        return new BenchConnection;
    }
};

int main(int argc, char** argv) {
    MainInitGuard guard(&argc, &argv);

    std::unique_ptr<ProactorPool> pp;
    pp.reset(fb2::Pool::IOUring(256, FLAGS_workers));
    pp->Run();

    AcceptServer acceptor(pp.get());
    acceptor.set_back_log(FLAGS_backlog);

    BenchListener* listener = new BenchListener;
    acceptor.AddListener(FLAGS_ip.c_str(), FLAGS_port, listener);

    acceptor.Run();
    acceptor.Wait();

    pp->Stop();
    return 0;
}
