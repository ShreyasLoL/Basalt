#pragma once

#include "basalt/common/types.hpp"

#include <filesystem>
#include <string>

namespace basalt::persistence::detail {

[[nodiscard]] common::Result<void> durable_write(const std::filesystem::path& path,
                                                  const std::string& bytes);
[[nodiscard]] common::Result<void> flush_existing(const std::filesystem::path& path);
[[nodiscard]] common::Result<void> durable_resize(const std::filesystem::path& path,
                                                   std::uint64_t size);
[[nodiscard]] common::Result<void> atomic_replace(const std::filesystem::path& path,
                                                   const std::string& bytes);

} // namespace basalt::persistence::detail
