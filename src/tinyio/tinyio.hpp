/// \file tinyio.hpp
/// \brief Single public include for the tinyio async runtime.
///
/// \code
///   auto listener = TcpListener::Bind(8080).value();
///   IO io{0, IoOptions{}};
///   io.spawn([&listener](tinyio::IO& io) noexcept {
///       while (true) {
///           TRY(client, io.Accept(listener));
///           // handle client ...
///       }
///   });
///   io.run_until_done();
/// \endcode
#pragma once

#include "tinyio/error.hpp"
#include "tinyio/net/fd.hpp"
#include "tinyio/net/net.hpp"
#include "tinyio/net/tcp_listener.hpp"
#include "tinyio/runtime/buffer_pool.hpp"
#include "tinyio/runtime/fiber_sync.hpp"
#include "tinyio/runtime/runtime.hpp"
