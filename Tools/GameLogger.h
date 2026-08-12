#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <Windows.h>
#endif

enum class GameLogLevel : std::uint8_t
{
    Trace = 0,
    Debug,
    Info,
    Warning,
    Error,
    Fatal,
    Off
};

class GameLogger final
{
public:
    struct Config
    {
        GameLogLevel minimumLevel = GameLogLevel::Trace;

        bool writeToConsole = true;
        bool writeToDebugger = true;
        bool writeToFile = false;

        bool appendFile = true;
        bool includeTimestamp = true;
        bool includeThreadId = false;
        bool includeSource = true;

        std::filesystem::path filePath = "GameTools.log";
    };

public:
    GameLogger() = delete;

    static bool Initialize(const Config& config = Config{})
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_file.is_open())
        {
            m_file.flush();
            m_file.close();
        }

        m_config = config;
        m_initialized = true;

        if (m_config.writeToFile)
        {
            const std::ios::openmode mode =
                std::ios::out |
                (m_config.appendFile ? std::ios::app : std::ios::trunc);

            m_file.open(m_config.filePath, mode);

            if (!m_file.is_open())
            {
                InternalDebuggerWrite(
                    "GameLogger: failed to open log file.\n");

                return false;
            }
        }

        return true;
    }

    static void Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_file.is_open())
        {
            m_file.flush();
            m_file.close();
        }

        m_initialized = false;
    }

    static void Flush()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_file.is_open())
        {
            m_file.flush();
        }

        std::cout.flush();
        std::cerr.flush();
    }

    static void SetMinimumLevel(GameLogLevel level)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config.minimumLevel = level;
    }

    static GameLogLevel GetMinimumLevel()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_config.minimumLevel;
    }

    static const Config& GetConfig()
    {
        return m_config;
    }

public:
    template <typename... Args>
    static void Trace(Args&&... args)
    {
        Write(GameLogLevel::Trace, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Debug(Args&&... args)
    {
        Write(GameLogLevel::Debug, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Info(Args&&... args)
    {
        Write(GameLogLevel::Info, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Warning(Args&&... args)
    {
        Write(GameLogLevel::Warning, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Error(Args&&... args)
    {
        Write(GameLogLevel::Error, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Fatal(Args&&... args)
    {
        Write(GameLogLevel::Fatal, std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void Write(GameLogLevel level, Args&&... args)
    {
        WriteSource(
            level,
            nullptr,
            0,
            nullptr,
            std::forward<Args>(args)...);
    }

    template <typename... Args>
    static void WriteSource(
        GameLogLevel level,
        const char* file,
        int line,
        const char* function,
        Args&&... args)
    {
        if (level == GameLogLevel::Off)
        {
            return;
        }

        std::ostringstream message;
        (message << ... << std::forward<Args>(args));

        Emit(level, message.str(), file, line, function);
    }

private:
    static void Emit(
        GameLogLevel level,
        const std::string& message,
        const char* file,
        int line,
        const char* function)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!ShouldLog(level))
        {
            return;
        }

        const std::string finalLine =
            FormatLine(level, message, file, line, function);

        if (m_config.writeToConsole)
        {
            WriteConsole(level, finalLine);
        }

        if (m_config.writeToDebugger)
        {
            InternalDebuggerWrite(finalLine);
            InternalDebuggerWrite("\n");
        }

        if (m_config.writeToFile && m_file.is_open())
        {
            m_file << finalLine << '\n';
        }

        if (level == GameLogLevel::Fatal)
        {
            if (m_file.is_open())
            {
                m_file.flush();
            }

            std::cerr.flush();
            std::cout.flush();
        }
    }

    static bool ShouldLog(GameLogLevel level)
    {
        if (m_config.minimumLevel == GameLogLevel::Off)
        {
            return false;
        }

        return static_cast<std::uint8_t>(level) >=
            static_cast<std::uint8_t>(m_config.minimumLevel);
    }

    static std::string FormatLine(
        GameLogLevel level,
        const std::string& message,
        const char* file,
        int line,
        const char* function)
    {
        std::ostringstream out;

        if (m_config.includeTimestamp)
        {
            out << '[' << Timestamp() << "] ";
        }

        out << '[' << LevelToString(level) << ']';

        if (m_config.includeThreadId)
        {
            out << "[T:" << std::this_thread::get_id() << ']';
        }

        if (m_config.includeSource && file != nullptr)
        {
            out << '[' << ShortFileName(file);

            if (line > 0)
            {
                out << ':' << line;
            }

            if (function != nullptr)
            {
                out << " | " << function;
            }

            out << ']';
        }

        out << ' ' << message;

        return out.str();
    }

    static std::string Timestamp()
    {
        using namespace std::chrono;

        const auto now = system_clock::now();
        const auto time = system_clock::to_time_t(now);
        const auto milliseconds =
            duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;

        std::tm localTime{};

#if defined(_WIN32)
        localtime_s(&localTime, &time);
#else
        localtime_r(&time, &localTime);
#endif

        std::ostringstream out;
        out << std::put_time(&localTime, "%H:%M:%S")
            << '.'
            << std::setfill('0')
            << std::setw(3)
            << milliseconds.count();

        return out.str();
    }

    static std::string ShortFileName(const char* file)
    {
        if (file == nullptr)
        {
            return {};
        }

        return std::filesystem::path(file).filename().string();
    }

    static constexpr std::string_view LevelToString(GameLogLevel level)
    {
        switch (level)
        {
        case GameLogLevel::Trace:   return "Trace";
        case GameLogLevel::Debug:   return "Debug";
        case GameLogLevel::Info:    return "Info";
        case GameLogLevel::Warning: return "Warning";
        case GameLogLevel::Error:   return "Error";
        case GameLogLevel::Fatal:   return "Fatal";
        case GameLogLevel::Off:     return "Off";
        }

        return "Unknown";
    }

    static void WriteConsole(GameLogLevel level, const std::string& line)
    {
        if (level == GameLogLevel::Error ||
            level == GameLogLevel::Fatal)
        {
            std::cerr << line << '\n';
        }
        else
        {
            std::cout << line << '\n';
        }
    }

    static void InternalDebuggerWrite(std::string_view text)
    {
#if defined(_WIN32)
        OutputDebugStringA(std::string(text).c_str());
#else
        (void)text;
#endif
    }

private:
    inline static Config m_config{};
    inline static bool m_initialized = false;

    inline static std::ofstream m_file{};
    inline static std::mutex m_mutex{};
};

#if defined(_MSC_VER)
#define GAME_DEBUG_BREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
#define GAME_DEBUG_BREAK() __builtin_trap()
#else
#define GAME_DEBUG_BREAK() ((void)0)
#endif

#define GAME_LOG_TRACE(...) \
    GameLogger::WriteSource(GameLogLevel::Trace, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define GAME_LOG_DEBUG(...) \
    GameLogger::WriteSource(GameLogLevel::Debug, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define GAME_LOG_INFO(...) \
    GameLogger::WriteSource(GameLogLevel::Info, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define GAME_LOG_WARNING(...) \
    GameLogger::WriteSource(GameLogLevel::Warning, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define GAME_LOG_ERROR(...) \
    GameLogger::WriteSource(GameLogLevel::Error, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#define GAME_LOG_FATAL(...) \
    GameLogger::WriteSource(GameLogLevel::Fatal, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#if defined(_DEBUG)
#define GAME_ASSERT(expression, ...)                                      \
    do                                                                    \
    {                                                                     \
        if (!(expression))                                                \
        {                                                                 \
            GameLogger::WriteSource(                                      \
                GameLogLevel::Fatal,                                      \
                __FILE__,                                                 \
                __LINE__,                                                 \
                __FUNCTION__,                                             \
                "Assertion failed: ", #expression                         \
                __VA_OPT__(, " | ", __VA_ARGS__));                       \
            GAME_DEBUG_BREAK();                                           \
        }                                                                 \
    } while (false)
#else
#define GAME_ASSERT(expression, ...) ((void)0)
#endif
