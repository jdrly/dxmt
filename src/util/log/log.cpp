/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#include <cstring>
#include <iostream>
#include <utility>

#include "log.hpp"
#include "log_split.hpp"

#include "../util_env.hpp"
#include "util_string.hpp"

namespace dxmt {

Logger::Logger(const std::string &fileName)
    : m_minLevel(getMinLogLevel()), m_fileName(fileName) {}

Logger::~Logger() {}

void Logger::trace(const std::string &message) {
  s_instance.emitMsg(LogLevel::Trace, message);
}

void Logger::debug(const std::string &message) {
  s_instance.emitMsg(LogLevel::Debug, message);
}

void Logger::info(const std::string &message) {
  s_instance.emitMsg(LogLevel::Info, message);
}

void Logger::warn(const std::string &message) {
  s_instance.emitMsg(LogLevel::Warn, message);
}

void Logger::err(const std::string &message) {
  s_instance.emitMsg(LogLevel::Error, message);
}

void Logger::log(LogLevel level, const std::string &message) {
  s_instance.emitMsg(level, message);
}

void Logger::emitMsg(LogLevel level, const std::string &message) {
  if (level >= m_minLevel) {
    std::lock_guard<dxmt::mutex> lock(m_mutex);

    static std::array<const char *, 5> s_prefixes = {
        {"trace: ", "debug: ", "info:  ", "warn:  ", "err:   "}};

    const char *prefix = s_prefixes.at(static_cast<uint32_t>(level));

    if (!std::exchange(m_initialized, true)) {
#ifdef _WIN32
      HMODULE ntdll = GetModuleHandleA("ntdll.dll");

      if (ntdll)
        m_wineLogOutput = reinterpret_cast<PFN_wineLogOutput>(
            GetProcAddress(ntdll, "__wine_dbg_output"));
#endif
      auto path = getFileName(m_fileName);

      if (!path.empty())
        m_fileStream = std::ofstream(str::topath(path.c_str()).c_str());
    }

    // gw2 (#43): one pass finds the lines and their NULs (forEachLogLine), and a line is formatted
    // without stringstreams
    forEachLogLine(message, [&](const std::string &line, bool hasNul) {
      std::string adjusted;
      adjusted.reserve(strlen(prefix) + line.size() + 1);
      adjusted += prefix;
      adjusted += line;
      adjusted += '\n';

      // Wine raises 0x80000005 for a line over its 1020-byte buffer; a long line (or one with a
      // NUL) goes in pieces
      if (m_wineLogOutput)
        writeWineLine(prefix, line, adjusted, hasNul, [this](const std::string &piece) {
          m_wineLogOutput(piece.c_str());
        });
      else
        std::cerr << adjusted;

      // the file keeps the whole line
      if (m_fileStream)
        m_fileStream << adjusted;
    });
  }
}

std::string Logger::getFileName(const std::string &base) {
  std::string path = env::getEnvVar("DXMT_LOG_PATH");

  if (path == "none")
    return std::string();

  // Don't create a log file if we're writing to wine's console output
  if (path.empty() && m_wineLogOutput)
    return std::string();

  if (!path.empty() && *path.rbegin() != '/')
    path += '/';

  std::string exeName = env::getExeBaseName();
  path += exeName + "_" + base;
  return path;
}

LogLevel Logger::getMinLogLevel() {
  const std::array<std::pair<const char *, LogLevel>, 6> logLevels = {{
      {"trace", LogLevel::Trace},
      {"debug", LogLevel::Debug},
      {"info", LogLevel::Info},
      {"warn", LogLevel::Warn},
      {"error", LogLevel::Error},
      {"none", LogLevel::None},
  }};

  const std::string logLevelStr = env::getEnvVar("DXMT_LOG_LEVEL");

  for (const auto &pair : logLevels) {
    if (logLevelStr == pair.first)
      return pair.second;
  }

  return LogLevel::Info;
}

} // namespace dxmt
