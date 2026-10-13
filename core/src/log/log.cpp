#include "fem/core/log/log.h"

#include "fem/core/product.h"

#include "rotating_file_sink.h"
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace fem::log {

struct Logging::Sinks {
    std::shared_ptr<detail::RotatingFileSink> file;
    std::shared_ptr<spdlog::sinks::sink> console;
    Level level = Level::info;

    [[nodiscard]] std::vector<std::shared_ptr<spdlog::sinks::sink>> all() const {
        std::vector<std::shared_ptr<spdlog::sinks::sink>> sinks;
        if (file) {
            sinks.push_back(file);
        }
        if (console) {
            sinks.push_back(console);
        }
        return sinks;
    }
};

namespace {

constexpr const char* file_pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [%n] [%t] %v";
constexpr const char* console_pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [%n] %v";

/// The loggers and the sinks they share. Loggers are kept at the lowest level of the open
/// `Logging` objects (off without any), so a message nobody would see is not even formatted.
class Registry {
public:
    std::shared_ptr<spdlog::logger> logger(std::string_view category) {
        const std::lock_guard lock(mutex_);
        const auto found = loggers_.find(category);
        if (found != loggers_.end()) {
            return found->second;
        }
        auto created = std::make_shared<spdlog::logger>(std::string(category), sinks_);
        created->set_level(level_);
        loggers_.emplace(std::string(category), created);
        return created;
    }

    void add(Logging::Sinks* sinks) {
        const std::lock_guard lock(mutex_);
        for (const auto& sink : sinks->all()) {
            sinks_->add_sink(sink);
        }
        open_.push_back(sinks);
        update_level();
    }

    void remove(Logging::Sinks* sinks) {
        const std::lock_guard lock(mutex_);
        for (const auto& sink : sinks->all()) {
            sinks_->remove_sink(sink);
        }
        std::erase(open_, sinks);
        update_level();
    }

    void set_level(Logging::Sinks* sinks, Level level) {
        const std::lock_guard lock(mutex_);
        sinks->level = level;
        for (const auto& sink : sinks->all()) {
            sink->set_level(level);
        }
        update_level();
    }

private:
    void update_level() {
        level_ = Level::off;
        for (const Logging::Sinks* sinks : open_) {
            level_ = std::min(level_, sinks->level);
        }
        for (const auto& [name, logger] : loggers_) {
            logger->set_level(level_);
        }
    }

    std::mutex mutex_;
    std::shared_ptr<spdlog::sinks::dist_sink_mt> sinks_ =
        std::make_shared<spdlog::sinks::dist_sink_mt>();
    std::map<std::string, std::shared_ptr<spdlog::logger>, std::less<>> loggers_;
    std::vector<Logging::Sinks*> open_;
    Level level_ = Level::off;
};

/// Never destroyed: loggers may be used from destructors of other static objects.
Registry& registry() {
    static auto* instance = new Registry();
    return *instance;
}

std::string default_file_name() {
    std::string name(kProductName);
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name + ".log";
}

}  // namespace

std::string_view to_string(Level level) noexcept {
    const auto name = spdlog::level::to_string_view(level);
    return {name.data(), name.size()};
}

std::optional<Level> parse_level(std::string_view name) noexcept {
    for (const Level level : {Level::trace, Level::debug, Level::info, Level::warn, Level::err,
                              Level::critical, Level::off}) {
        if (to_string(level) == name) {
            return level;
        }
    }
    return std::nullopt;
}

std::shared_ptr<spdlog::logger> logger(std::string_view category) {
    return registry().logger(category);
}

Logging::Logging(LogOptions options) : sinks_(std::make_unique<Sinks>()) {
    if (!options.folder.empty()) {
        const std::string name =
            options.file_name.empty() ? default_file_name() : options.file_name;
        file_ = options.folder / std::filesystem::path(std::u8string(name.begin(), name.end()));
        sinks_->file = std::make_shared<detail::RotatingFileSink>(
            file_, options.max_file_size, options.max_files, options.rotate_on_open);
    }
    // The console sinks of spdlog flush every line themselves.
    switch (options.console) {
        case Console::none:
            break;
        case Console::standard_output:
            sinks_->console = std::make_shared<spdlog::sinks::stdout_sink_mt>();
            break;
        case Console::standard_error:
            sinks_->console = std::make_shared<spdlog::sinks::stderr_sink_mt>();
            break;
    }
    sinks_->level = options.level;
    for (const auto& sink : sinks_->all()) {
        // '\n' on every OS: spdlog ends lines with "\r\n" on Windows by default.
        sink->set_formatter(std::make_unique<spdlog::pattern_formatter>(
            sink == sinks_->file ? file_pattern : console_pattern, spdlog::pattern_time_type::local,
            "\n"));
        sink->set_level(options.level);
    }
    registry().add(sinks_.get());
}

Logging::~Logging() {
    registry().remove(sinks_.get());
    flush();
}

Level Logging::level() const noexcept {
    return sinks_->level;
}

void Logging::set_level(Level level) {
    registry().set_level(sinks_.get(), level);
}

void Logging::flush() {
    for (const auto& sink : sinks_->all()) {
        sink->flush();
    }
}

}  // namespace fem::log
