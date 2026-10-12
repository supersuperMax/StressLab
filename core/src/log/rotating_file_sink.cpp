#include "rotating_file_sink.h"

#include "../io/files.h"

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace fem::log::detail {

namespace {

std::uint64_t size_of(const std::filesystem::path& file) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    return error ? 0 : static_cast<std::uint64_t>(size);
}

}  // namespace

RotatingFileSink::RotatingFileSink(std::filesystem::path file, std::uint64_t max_size,
                                   std::size_t max_files, bool rotate_on_open)
    : file_(std::move(file)), max_size_(max_size), max_files_(max_files < 1 ? 1 : max_files) {
    std::error_code error;
    std::filesystem::create_directories(file_.parent_path(), error);
    if (error) {
        throw std::runtime_error("cannot create the log folder '" +
                                 io::display(file_.parent_path()) + "': " + error.message());
    }
    open();
    if (rotate_on_open && size_ > 0) {
        rotate();
    }
}

std::filesystem::path RotatingFileSink::numbered(const std::filesystem::path& file,
                                                 std::size_t index) {
    if (index == 0) {
        return file;
    }
    std::filesystem::path name = file.stem();
    name += "." + std::to_string(index);
    name += file.extension();
    return file.parent_path() / name;
}

void RotatingFileSink::open() {
    out_.open(file_, std::ios::binary | std::ios::app);
    if (!out_) {
        throw std::runtime_error("cannot open the log file '" + io::display(file_) + "'");
    }
    size_ = size_of(file_);
}

void RotatingFileSink::rotate() {
    out_.close();
    if (max_files_ == 1) {
        out_.open(file_, std::ios::binary | std::ios::trunc);
    } else {
        std::error_code error;
        std::filesystem::remove(numbered(file_, max_files_ - 1), error);
        for (std::size_t index = max_files_ - 1; index > 0; --index) {
            const std::filesystem::path from = numbered(file_, index - 1);
            if (!std::filesystem::exists(from, error)) {
                continue;
            }
            std::filesystem::rename(from, numbered(file_, index), error);
            if (error) {
                break;  // Another process holds a file; keep writing to the current one.
            }
        }
        out_.open(file_, std::ios::binary | std::ios::app);
    }
    size_ = size_of(file_);
}

void RotatingFileSink::sink_it_(const spdlog::details::log_msg& msg) {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    if (size_ > 0 && size_ + formatted.size() > max_size_) {
        rotate();
    }
    if (!out_.is_open()) {
        open();
    }
    out_.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
    out_.flush();
    size_ += formatted.size();
}

void RotatingFileSink::flush_() {
    out_.flush();
}

}  // namespace fem::log::detail
