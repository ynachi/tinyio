/// \file tinyio.hpp
/// \brief Single public include for the tinyio async runtime.
///
/// \code
///   auto listener = TcpListener::Bind(8080).value();
///   IO io{0, IoOptions{}, {}};
///   io.spawn([&listener](tinyio::IO& io) noexcept {
///       while (true) {
///           auto client = TRY(io.accept(listener));
///           // handle client ...
///       }
///   });
///   io.run_until_done();
/// \endcode
#pragma once

#include "tinyio/buffer_pool.hpp"
#include "tinyio/error.hpp"
#include "tinyio/fd.hpp"
#include "tinyio/net.hpp"
#include "tinyio/runtime.hpp"
#include "tinyio/tcp_listener.hpp"
