/// \file tinyio.hpp
/// \brief Single public include for the tinyio async runtime.
///
/// \code
///   auto listener = TcpListener::Bind(8080).value();
///   IO io{0, IoOptions{}, {}};
///   io.spawn([&listener](tinyio::IO& io) noexcept {
///       while (true) {
///           TRY(client, io.Accept(listener));
///           // handle client ...
///       }
///   });
///   io.run_until_done();
/// \endcode
#pragma once

#include "buffer_pool.hpp"
#include "fd.hpp"
#include "net.hpp"
#include "tcp_listener.hpp"
#include "tinyio/error.hpp"
#include "tinyio/fiber_sync.hpp"
#include "tinyio/runtime.hpp"
