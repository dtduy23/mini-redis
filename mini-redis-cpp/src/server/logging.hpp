#ifndef LOGGING_H
#define LOGGING_H

#include <chrono>
#include <format>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

class Logger {
public:
    enum LogLevel { DEBUG, INFO, WARNING, ERROR };

    Logger() = default;

    template <typename... Args>
    void debug(std::string_view fmt, Args&&... args) {
        log(DEBUG, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(std::string_view fmt, Args&&... args) {
        log(INFO, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warning(std::string_view fmt, Args&&... args) {
        log(WARNING, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(std::string_view fmt, Args&&... args) {
        log(ERROR, fmt, std::forward<Args>(args)...);
    }

private:
    // ANSI color codes
    static constexpr std::string_view RESET   = "\033[0m";
    static constexpr std::string_view GRAY    = "\033[90m";
    static constexpr std::string_view CYAN    = "\033[96m";
    static constexpr std::string_view YELLOW  = "\033[93m";
    static constexpr std::string_view RED     = "\033[91m";
    static constexpr std::string_view BOLD    = "\033[1m";

    static std::string_view level_color(LogLevel level) {
        switch (level) {
            case DEBUG:   return GRAY;
            case INFO:    return CYAN;
            case WARNING: return YELLOW;
            case ERROR:   return RED;
        }
        return RESET;
    }

    static std::string_view level_tag(LogLevel level) {
        switch (level) {
            case DEBUG:   return "DEBUG";
            case INFO:    return "INFO ";
            case WARNING: return "WARN ";
            case ERROR:   return "ERROR";
        }
        return "?????";
    }

    static std::string current_time() {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()) % 1000;

        std::ostringstream oss;
        oss << std::put_time(std::localtime(&t), "%H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }

    template <typename... Args>
    void log(LogLevel level, std::string_view fmt, Args&&... args) {
        std::string message = std::vformat(fmt, std::make_format_args(args...));

        std::cout << GRAY    << current_time() << RESET << " "
                  << BOLD    << level_color(level)
                  << level_tag(level) << RESET << "  "
                  << message << "\n";
    }
};

#endif  // LOGGING_H