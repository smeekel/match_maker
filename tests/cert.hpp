#pragma once

#include <filesystem>

namespace matchmaker::test
{

void write_test_certificate(const std::filesystem::path& certificate,
                            const std::filesystem::path& key);

} // namespace matchmaker::test
