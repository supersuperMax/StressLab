#pragma once

#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fem::log::detail {

/// A file sink rotated by size, like spdlog's `rotating_file_sink`, but opened through
/// std::filesystem: spdlog opens files with narrow names in the ANSI code page on Windows, which
/// fails for a user folder with letters outside it (a Cyrillic user name on an English Windows).
class RotatingFileSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    /// Opens `file` for appending, rotating it first if `rotate_on_open` and it is not empty.
    /// Throws std::runtime_error if it cannot be opened.
    RotatingFileSink(std::filesystem::path file, std::uint64_t max_size, std::size_t max_files,
                     bool rotate_on_open);

    /// The name of a rotated file: "stresslab.log" → "stresslab.2.log" for 2; `file` for 0.
    [[nodiscard]] static std::filesystem::path numbered(const std::filesystem::path& file,
                                                        std::size_t index);

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override;

private:
    void open();
    /// Shifts the numbered files up, dropping the last, and starts an empty current file. If a
    /// file cannot be renamed (another process holds it), writing goes on in the current one.
    void rotate();

    std::filesystem::path file_;
    std::uint64_t max_size_;
    std::size_t max_files_;
    std::ofstream out_;
    std::uint64_t size_ = 0;
};

}  // namespace fem::log::detail
