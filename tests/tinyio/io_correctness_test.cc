#include <fcntl.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <stop_token>
#include <system_error>
#include <thread>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include "tinyio/detail/sanitizer_hooks.hpp"
#include "tinyio/runtime.hpp"
#include "tinyio/tcp_listener.hpp"

using namespace tinyio;
using namespace std::chrono_literals;

TEST(IOCorrectnessTest, FileWriteReadRoundtrip) {
  IoOptions opts;
  IO io{0, opts};
  (void)io.Spawn([](IO& fio) noexcept {
    auto file_path = std::filesystem::temp_directory_path() / "tinyio_test_file.txt";

    auto fd_res = fio.Open(file_path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    ASSERT_TRUE(fd_res.has_value());
    UniqueFd fd = std::move(*fd_res);

    std::string_view test_data = "Hello, tinyio!";
    auto write_res = fio.Write(fd, std::as_bytes(std::span(test_data)), 0);
    ASSERT_TRUE(write_res.has_value());
    EXPECT_EQ(*write_res, test_data.size());

    auto fsync_res = fio.Fsync(fd);
    EXPECT_TRUE(fsync_res.has_value());

    std::array<char, 128> buf{};
    auto read_res = fio.Read(fd, std::as_writable_bytes(std::span(buf)), 0);
    ASSERT_TRUE(read_res.has_value());
    EXPECT_EQ(*read_res, test_data.size());
    EXPECT_EQ(std::string_view(buf.data(), *read_res), test_data);

    (void)fio.Close(std::move(fd));
    (void)fio.Remove(file_path);
  });
  io.RunUntilDone();
}

TEST(IOCorrectnessTest, SleepForAccuracy) {
  IoOptions opts;
  IO io{0, opts};
  (void)io.Spawn([](IO& fio) noexcept {
    auto start = std::chrono::steady_clock::now();
    auto res = fio.SleepForNs(50ms);
    ASSERT_TRUE(res.has_value());
    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    EXPECT_GE(elapsed.count(), 40);
    EXPECT_LE(elapsed.count(), 200);
  });
  io.RunUntilDone();
}

TEST(IOCorrectnessTest, FixedBufferWriteRead) {
  IoOptions opts;
  opts.buffer_pool = BufferPoolConfig{.slot_size = 4096, .slots = 1};
  IO io{0, opts};

  (void)io.Spawn([](IO& fio) noexcept {
    auto file_path = std::filesystem::temp_directory_path() / "tinyio_test_fixed.txt";
    auto fd_res = fio.Open(file_path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    ASSERT_TRUE(fd_res.has_value());
    UniqueFd fd = std::move(*fd_res);

    auto buf_res = fio.TakeFixedBuffer();
    ASSERT_TRUE(buf_res.has_value());
    auto& fixed_buf = *buf_res;

    std::string_view test_data = "Fixed buffer data!";
    std::memcpy(fixed_buf.Ptr(), test_data.data(), test_data.size());

    auto write_res = fio.WriteFixed(fd, fixed_buf, test_data.size(), 0);
    ASSERT_TRUE(write_res.has_value());
    EXPECT_EQ(*write_res, test_data.size());

    std::memset(fixed_buf.Ptr(), 0, test_data.size());

    auto read_res = fio.ReadFixed(fd, fixed_buf, test_data.size(), 0);
    ASSERT_TRUE(read_res.has_value());
    EXPECT_EQ(*read_res, test_data.size());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(fixed_buf.Ptr()), *read_res),
              test_data);

    (void)fio.Close(std::move(fd));
    (void)fio.Remove(file_path);
  });
  io.RunUntilDone();
}

// A range outside the slot is an EINVAL, not a wild pointer into the next
// slot: the kernel resolves the buf_index against the registered table and
// would happily write past the end of the buffer it was given.
TEST(IOCorrectnessTest, FixedBufferRangeOutsideTheSlotIsRejected) {
  IoOptions opts;
  opts.buffer_pool = BufferPoolConfig{.slot_size = 4096, .slots = 1};
  IO io{0, opts};

  (void)io.Spawn([](IO& fio) noexcept {
    auto file_path =
        std::filesystem::temp_directory_path() / "tinyio_test_fixed_range.txt";
    auto fd_res = fio.Open(file_path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    ASSERT_TRUE(fd_res.has_value());
    UniqueFd fd = std::move(*fd_res);

    auto buf_res = fio.TakeFixedBuffer();
    ASSERT_TRUE(buf_res.has_value());
    auto& fixed_buf = *buf_res;

    // The buffer offset is past the end of the slot:
    // ReadFixed(fd, buf, len, file_offset, buffer_offset).
    auto read_res = fio.ReadFixed(fd, fixed_buf, 16, 0, 4096);
    ASSERT_FALSE(read_res.has_value());
    EXPECT_EQ(read_res.error().value(), EINVAL);

    // The length is zero.
    auto write_res = fio.WriteFixed(fd, fixed_buf, 0, 0);
    ASSERT_FALSE(write_res.has_value());
    EXPECT_EQ(write_res.error().value(), EINVAL);

    // A length that runs past the end of the slot is clamped to what is
    // left of it, so the transfer stays inside the registered buffer.
    auto clamped = fio.WriteFixed(fd, fixed_buf, 8192, 0, 4090);
    ASSERT_TRUE(clamped.has_value());
    EXPECT_EQ(*clamped, 6u);

    (void)fio.Close(std::move(fd));
    (void)fio.Remove(file_path);
  });
  io.RunUntilDone();
}

// The destructor does not run when the constructor throws, so a failed
// initialization has to clean up after itself and leak nothing. Run in a
// child: pinning RLIMIT_NOFILE to the process's current fd count makes
// io_uring_queue_init_params fail with EMFILE (the ring setup syscall
// allocates a descriptor), and the constructor must throw without having
// consumed any.
TEST(IOCorrectnessTest, FailedInitThrowsAndLeaksNoFds) {
#if TINYIO_ASAN_ENABLED
  // ASan's runtime opens a pipe of its own while the exception is thrown;
  // with no descriptor to spare it fails its internal CHECK and exits.
  GTEST_SKIP() << "needs fd headroom the ASan runtime takes; covered by dev/tsan/release";
#endif
  const auto construct_with_no_fd_headroom = [] {
    const auto open_fds = [] {
      return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                           std::filesystem::directory_iterator{});
    };
    const auto count = static_cast<rlim_t>(open_fds());
    const rlimit limit{.rlim_cur = count, .rlim_max = count};
    if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) {
      std::_Exit(2);
    }

    IoOptions opts;
    const auto before = open_fds();
    bool threw = false;
    try {
      IO io{0, opts};
    } catch (const std::system_error&) {
      threw = true;
    }
    std::_Exit(threw && open_fds() == before ? 0 : 1);
  };

  EXPECT_EXIT(construct_with_no_fd_headroom(), ::testing::ExitedWithCode(0), "");
}

TEST(IOCorrectnessTest, TcpAcceptEcho) {
  IoOptions opts;
  IO io{0, opts};

  auto server_fd_res = TcpListener::Bind(0, "127.0.0.1");
  ASSERT_TRUE(server_fd_res.has_value());
  UniqueFd server_fd = std::move(*server_fd_res);

  struct sockaddr_in sin;
  socklen_t len = sizeof(sin);
  if (getsockname(server_fd.Get(), reinterpret_cast<sockaddr*>(&sin), &len) == -1) {
    FAIL() << "getsockname failed";
  }
  int port = ntohs(sin.sin_port);

  std::stop_source stop_source;

  // The server fiber accepts, then hands the connection to its own fiber.
  (void)io.Spawn([server_fd = std::move(server_fd)](IO& fio) mutable noexcept {
    auto client_fd_res = fio.Accept(server_fd);
    ASSERT_TRUE(client_fd_res.has_value());
    (void)fio.Spawn([client_fd =
                         std::move(*client_fd_res)](IO& client_fio) mutable noexcept {
      std::array<char, 128> buf{};
      auto read_res = client_fio.Read(client_fd, std::as_writable_bytes(std::span(buf)));
      if (read_res.has_value() && *read_res > 0) {
        (void)client_fio.Write(client_fd,
                               std::as_bytes(std::span(buf).subspan(0, *read_res)));
      }
      (void)client_fio.Close(std::move(client_fd));
    });
  });

  (void)io.Spawn([port, stop_source](IO& fio) mutable noexcept {
    UniqueFd client_fd(socket(AF_INET, SOCK_STREAM, 0));
    auto addr_res = SocketAddress::V4(static_cast<std::uint16_t>(port), "127.0.0.1");
    ASSERT_TRUE(addr_res.has_value());
    SocketAddress addr = std::move(*addr_res);
    auto conn_res = fio.Connect(client_fd, addr);
    ASSERT_TRUE(conn_res.has_value());

    std::string_view test_data = "Echo this!";
    auto write_res = fio.Write(client_fd, std::as_bytes(std::span(test_data)));
    ASSERT_TRUE(write_res.has_value());

    std::array<char, 128> buf{};
    auto read_res = fio.Read(client_fd, std::as_writable_bytes(std::span(buf)));
    ASSERT_TRUE(read_res.has_value());
    EXPECT_EQ(std::string_view(buf.data(), *read_res), test_data);
    (void)fio.Close(std::move(client_fd));

    (void)stop_source.request_stop();
  });

  io.Run(stop_source.get_token());
}
