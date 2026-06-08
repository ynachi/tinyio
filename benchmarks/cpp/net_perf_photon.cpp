#include <gflags/gflags.h>
#include <photon/common/alog.h>
#include <photon/net/socket.h>
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/photon.h>
#include <thread>
#include <vector>

// Command-line flags
DEFINE_string(ip, "0.0.0.0", "IP address to bind to");
DEFINE_uint64(port, 8080, "Port to listen on");
DEFINE_uint64(workers, 4, "Number of worker threads");
DEFINE_uint64(backlog, 4096, "Listen backlog");
DEFINE_uint64(buf_size, 8192, "Buffer size for reading");

static constexpr std::string_view HTTP_RESPONSE =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello from AIO!";

void handle_connection(photon::net::ISocketStream* stream)
{
    DEFER(delete stream);
    char buf[4096];

    while (true)
    {
        ssize_t ret = stream->recv(buf, sizeof(buf));
        if (ret <= 0)
        {
            return;
        }

        ssize_t wret = stream->send(HTTP_RESPONSE.data(), HTTP_RESPONSE.size());
        if (wret <= 0)
        {
            return;
        }
    }
}

int main(int argc, char** argv)
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    set_log_output_level(ALOG_FATAL);

    std::vector<std::thread> threads;
    for (uint64_t i = 0; i < FLAGS_workers; ++i) {
        threads.emplace_back([]() {
            if (photon::init(photon::INIT_EVENT_IOURING, photon::INIT_IO_NONE) != 0) {
                return;
            }
            DEFER(photon::fini());

            const auto server = photon::net::new_tcp_socket_server();
            if (!server) return;
            DEFER(delete server);

            server->setsockopt<int>(SOL_SOCKET, SO_REUSEADDR, 1);
            server->setsockopt<int>(SOL_SOCKET, SO_REUSEPORT, 1);

            photon::net::IPAddr addr{FLAGS_ip.c_str()};
            if (server->bind(FLAGS_port, addr) < 0) return;
            if (server->listen(FLAGS_backlog) != 0) return;

            while (true) {
                auto* stream = server->accept();
                if (stream) {
                    photon::thread_create11([stream] { handle_connection(stream); });
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
    
    return 0;
}
