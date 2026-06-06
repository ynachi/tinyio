/// \file tinyio.hpp
/// \brief Single public include for the tinyio async runtime.
///
/// \code
///   auto rt = tinyio::Runtime::create().value();
///   rt->spawn([](tinyio::IO& io) noexcept -> void {
///       std::array<std::byte, 64> buf{};
///       auto n = io.read(0, buf);          // returns tinyio::Result<size_t>
///       // ...
///   });
///   (void)rt->run();
/// \endcode
#pragma once

// #include "uringx/context.hpp"
// #include "uringx/fiber.hpp"
// #include "uringx/io.hpp"
// #include "uringx/result.hpp"
// #include "uringx/runtime.hpp"
