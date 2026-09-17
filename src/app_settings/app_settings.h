// app_settings.h
#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QVariant>
#include <spdlog/common.h>

#if defined(_WIN32)
#if defined(APP_SETTINGS_DLL)
#define APP_SETTINGS_API __declspec(dllexport)
#else
#define APP_SETTINGS_API __declspec(dllimport)
#endif
#else
#define APP_SETTINGS_API
#endif

namespace pc {

class APP_SETTINGS_API AppSettings final : public QObject {
  Q_OBJECT

  Q_PROPERTY(bool restoreLastWorkspace READ restoreLastWorkspace WRITE
                 setrestoreLastWorkspace NOTIFY restoreLastWorkspaceChanged)
  Q_PROPERTY(QString lastWorkspacePath READ lastWorkspacePath WRITE
                 setlastWorkspacePath NOTIFY lastWorkspacePathChanged)
  Q_PROPERTY(
      bool loadMalformedWorkspaces READ loadMalformedWorkspaces WRITE
          setLoadMalformedWorkspaces NOTIFY loadMalformedWorkspacesChanged)

  Q_PROPERTY(
      LogLevel logLevel READ logLevel WRITE setLogLevel NOTIFY logLevelChanged)
  Q_PROPERTY(
      bool logToFile READ logToFile WRITE setLogToFile NOTIFY logToFileChanged)

  Q_PROPERTY(double uiScale READ uiScale WRITE setUiScale NOTIFY uiScaleChanged)
  Q_PROPERTY(int gridSizeMetres READ gridSizeMetres WRITE setGridSizeMetres
                 NOTIFY gridSizeMetresChanged)
  Q_PROPERTY(QString backgroundColor READ backgroundColor WRITE
                 setBackgroundColor NOTIFY backgroundColorChanged)
  Q_PROPERTY(Antialiasing antialiasing READ antialiasing WRITE setAntialiasing
                 NOTIFY antialiasingChanged)

  Q_PROPERTY(
      int pointSize READ pointSize WRITE setPointSize NOTIFY pointSizeChanged)

  Q_PROPERTY(
      bool enablePrometheusMetrics READ enablePrometheusMetrics WRITE
          setEnablePrometheusMetrics NOTIFY enablePrometheusMetricsChanged)
  Q_PROPERTY(QString prometheusAddress READ prometheusAddress WRITE
                 setPrometheusAddress NOTIFY prometheusAddressChanged)

  Q_PROPERTY(bool enableTracyProfiling READ enableTracyProfiling WRITE
                 setEnableTracyProfiling NOTIFY enableTracyProfilingChanged)

  Q_PROPERTY(int workerThreads READ workerThreads WRITE setWorkerThreads NOTIFY
                 workerThreadsChanged)
  Q_PROPERTY(int defaultWorkerThreads READ defaultWorkerThreads CONSTANT)
  Q_PROPERTY(int fileWriterThreads READ fileWriterThreads WRITE
                 setFileWriterThreads NOTIFY fileWriterThreadsChanged)
  Q_PROPERTY(int defaultFileWriterThreads READ defaultFileWriterThreads CONSTANT)

public:
  static AppSettings *instance();

  explicit AppSettings(QObject *parent = nullptr);

  // -- General

  bool restoreLastWorkspace() const;
  void setrestoreLastWorkspace(bool value);

  QString lastWorkspacePath() const;
  void setlastWorkspacePath(const QString &value);

  bool loadMalformedWorkspaces() const;
  void setLoadMalformedWorkspaces(bool value);

  enum class LogLevel : int {
    Trace = static_cast<int>(spdlog::level::trace),
    Debug = static_cast<int>(spdlog::level::debug),
    Info = static_cast<int>(spdlog::level::info),
    Warn = static_cast<int>(spdlog::level::warn),
    Error = static_cast<int>(spdlog::level::err),
    Critical = static_cast<int>(spdlog::level::critical),
    Off = static_cast<int>(spdlog::level::off),
  };
  Q_ENUM(LogLevel)

  LogLevel logLevel() const;
  void setLogLevel(LogLevel level);
  spdlog::level::level_enum spdlogLogLevel() const;

  bool logToFile() const;
  void setLogToFile(bool value);

  // -- User interface

  double uiScale() const;
  void setUiScale(double value);

  int gridSizeMetres() const;
  void setGridSizeMetres(int value);

  QString backgroundColor() const;
  void setBackgroundColor(const QString &value);

  enum class Antialiasing : int { NoAA, SSAA, MSAA, ProgressiveAA };
  Q_ENUM(Antialiasing)

  Antialiasing antialiasing() const;
  void setAntialiasing(Antialiasing value);

  int pointSize() const;
  void setPointSize(int value);

  // -- Metrics

  bool enablePrometheusMetrics() const;
  void setEnablePrometheusMetrics(bool value);

  QString prometheusAddress() const;
  void setPrometheusAddress(const QString &value);

  bool enableTracyProfiling() const;
  void setEnableTracyProfiling(bool value);

  // -- Performance

  // threads for device processing and sequence loading, defaulting to every
  // hardware thread the machine reports
  int workerThreads() const;
  void setWorkerThreads(int value);
  int defaultWorkerThreads() const;

  // threads for blocking recording writes, sized for the storage device rather
  // than the cpu so the default stays small
  int fileWriterThreads() const;
  void setFileWriterThreads(int value);
  int defaultFileWriterThreads() const;

  // -- for plugins that lookup values from a map

  Q_INVOKABLE QVariant value(const QString &key,
                             const QVariant &defaultValue = {}) const;
  Q_INVOKABLE void setValue(const QString &key, const QVariant &value);
  Q_INVOKABLE void sync();

signals:
  void restoreLastWorkspaceChanged();
  void lastWorkspacePathChanged();
  void loadMalformedWorkspacesChanged();

  void logLevelChanged();
  void logToFileChanged();

  void uiScaleChanged();
  void gridSizeMetresChanged();
  void backgroundColorChanged();
  void antialiasingChanged();
  void pointSizeChanged();

  void enablePrometheusMetricsChanged();
  void prometheusAddressChanged();

  void enableTracyProfilingChanged();

  void workerThreadsChanged();
  void fileWriterThreadsChanged();

  void valueChanged(const QString &key);

private:
  template <typename T> T read(const QString &key, const T &def) const {
    return m_settings.value(key, def).template value<T>();
  }

  void write(const QString &key, const QVariant &v);

  static LogLevel logLevelFromString(QStringView levelText);
  static QString logLevelToString(LogLevel level);

  static Antialiasing antialiasingFromString(QStringView modeText);
  static QString antialiasingToString(Antialiasing mode);

  QSettings m_settings;

  bool m_restoreLastWorkspace = true;
  QString m_lastWorkspacePath;
  bool m_loadMalformedWorkspaces = true;

  LogLevel m_logLevel = LogLevel::Info;
  bool m_logToFile = true;

  double m_uiScale = 1.0;
  int m_gridSizeMetres = 10;
  QString m_backgroundColor = "#00010A";
  
  Antialiasing m_antialiasing = Antialiasing::MSAA;
  int m_pointSize = 5;

  bool m_enablePrometheusMetrics = true;
  QString m_prometheusAddress;

  bool m_enableTracyProfiling = false;

  int m_workerThreads = 1;
  int m_fileWriterThreads = 1;
};

} // namespace pc
