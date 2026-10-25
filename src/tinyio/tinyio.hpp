/// \file tinyio.hpp
/// \brief Single public include for the tinyio async runtime.
///
/// \code
///   auto listener = TcpListener::Bind(8080).value();
///   IO io{0, IoOptions{}};
///   (void)io.Spawn([&listener](tinyio::IO& rt) noexcept {
///       while (true) {
///           auto client = rt.Accept(listener);
///           if (!client) break;
///           // handle client ...
///       }
///   });
///   io.RunUntilDone();
/// \endcode
#pragma once

#include "tinyio/error.hpp"
#include "tinyio/net/fd.hpp"
#include "tinyio/net/net.hpp"
#include "tinyio/net/tcp_listener.hpp"
#include "tinyio/runtime/buffer_pool.hpp"
#include "tinyio/runtime/fiber_sync.hpp"
#include "tinyio/runtime/runtime.hpp"
