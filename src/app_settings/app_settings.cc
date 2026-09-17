#include "app_settings.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>
#include <QtGlobal>
#include <QtQml/qqml.h>
#include <algorithm>
#include <qcoreapplication.h>
#include <thread>

namespace pc {

static bool onObjectThread(QObject *obj) {
  return QThread::currentThread() == obj->thread();
}

AppSettings *AppSettings::instance() {
  QCoreApplication::setOrganizationName("matth");
  QCoreApplication::setApplicationName("pointcaster");
  static AppSettings *s_instance = new AppSettings(qApp);
  qmlRegisterSingletonInstance("Pointcaster", 1, 0, "AppSettings", s_instance);
  return s_instance;
}

AppSettings::AppSettings(QObject *parent)
    : QObject(parent), m_settings(QSettings::NativeFormat, QSettings::UserScope,
                                  QCoreApplication::organizationName(),
                                  QCoreApplication::applicationName()) {
  // Load initial cache
  m_restoreLastWorkspace =
      m_settings.value("restoreLastWorkspace", true).toBool();
  m_lastWorkspacePath = m_settings.value("lastWorkspacePath", "").toString();
  m_loadMalformedWorkspaces =
      m_settings.value("loadMalformedWorkspaces", true).toBool();

  // Store as string in QSettings, e.g. "debug", "info", ...
  const auto logLevelText =
      m_settings.value("logLevel", QStringLiteral("info")).toString();
  m_logLevel = logLevelFromString(logLevelText);
  m_logToFile = m_settings.value("logToFile", true).toBool();

  m_uiScale = m_settings.value("uiScale", 1.0).toDouble();
  m_gridSizeMetres = m_settings.value("gridSize", 10).toInt();
  m_backgroundColor =
      m_settings.value("viewport/backgroundColor", QStringLiteral("#00010A"))
          .toString();
  
          m_antialiasing = antialiasingFromString(
      m_settings.value("viewport/antialiasing", QStringLiteral("msaa"))
          .toString());

  const int stored_point_size =
      m_settings.value("viewport/pointSize", 5).toInt();
  m_pointSize = stored_point_size >= 1 ? qBound(1, stored_point_size, 250) : 5;

  m_enablePrometheusMetrics =
      m_settings.value("prometheusEnabled", true).toBool();
  m_prometheusAddress =
      m_settings.value("prometheusAddress", QStringLiteral("0.0.0.0:8080"))
          .toString();

  m_enableTracyProfiling = m_settings.value("tracyEnabled", true).toBool();

  m_workerThreads = std::max(
      1, m_settings.value("workerThreads", defaultWorkerThreads()).toInt());
  m_fileWriterThreads = std::max(
      1, m_settings.value("fileWriterThreads", defaultFileWriterThreads())
             .toInt());
}

AppSettings::LogLevel AppSettings::logLevel() const {
  return m_logLevel;
}

void AppSettings::setLogLevel(LogLevel level) {
  if (level == m_logLevel) return;
  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, level] { setLogLevel(level); }, Qt::QueuedConnection);
    return;
  }
  m_logLevel = level;
  write("logLevel", logLevelToString(m_logLevel));
  emit logLevelChanged();
}

AppSettings::LogLevel AppSettings::logLevelFromString(QStringView levelText) {
  const QString s = levelText.trimmed().toString().toLower();

  if (s == QLatin1String("trace")) return LogLevel::Trace;
  if (s == QLatin1String("debug")) return LogLevel::Debug;
  if (s == QLatin1String("info")) return LogLevel::Info;

  if (s == QLatin1String("warn") || s == QLatin1String("warning"))
    return LogLevel::Warn;

  if (s == QLatin1String("error") || s == QLatin1String("err"))
    return LogLevel::Error;

  if (s == QLatin1String("critical") || s == QLatin1String("crit"))
    return LogLevel::Critical;

  if (s == QLatin1String("off")) return LogLevel::Off;

  return LogLevel::Info;
}

QString AppSettings::logLevelToString(LogLevel level) {
  switch (level) {
  case LogLevel::Trace:
    return QStringLiteral("trace");
  case LogLevel::Debug:
    return QStringLiteral("debug");
  case LogLevel::Info:
    return QStringLiteral("info");
  case LogLevel::Warn:
    return QStringLiteral("warn");
  case LogLevel::Error:
    return QStringLiteral("error");
  case LogLevel::Critical:
    return QStringLiteral("critical");
  case LogLevel::Off:
    return QStringLiteral("off");
  }
  return QStringLiteral("info");
}

spdlog::level::level_enum AppSettings::spdlogLogLevel() const {
  return static_cast<spdlog::level::level_enum>(static_cast<int>(m_logLevel));
}

bool AppSettings::logToFile() const {
  return m_logToFile;
}

void AppSettings::setLogToFile(bool value) {
  if (value == m_logToFile) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, value] { setLogToFile(value); }, Qt::QueuedConnection);
    return;
  }

  m_logToFile = value;
  write("logToFile", m_logToFile);
  emit logToFileChanged();
}

double AppSettings::uiScale() const {
  return m_uiScale;
}

void AppSettings::setUiScale(double value) {
  const double v = qBound(0.5, value, 3.0);
  if (qFuzzyCompare(v, m_uiScale)) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, v] { setUiScale(v); }, Qt::QueuedConnection);
    return;
  }

  m_uiScale = v;
  write("uiScale", m_uiScale);
  emit uiScaleChanged();
}

int AppSettings::gridSizeMetres() const {
  return m_gridSizeMetres;
}

void AppSettings::setGridSizeMetres(int value) {
  if (value == m_gridSizeMetres) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, v = value] { setGridSizeMetres(v); },
        Qt::QueuedConnection);
    return;
  }

  m_gridSizeMetres = value;
  write("gridSize", m_gridSizeMetres);
  emit gridSizeMetresChanged();
}

int AppSettings::defaultWorkerThreads() const {
  const auto hardware_threads = std::thread::hardware_concurrency();
  return hardware_threads > 0 ? static_cast<int>(hardware_threads) : 1;
}

int AppSettings::defaultFileWriterThreads() const {
  return 2;
}

int AppSettings::workerThreads() const {
  return m_workerThreads;
}

void AppSettings::setWorkerThreads(int value) {
  if (value < 1) value = 1;
  if (value == m_workerThreads) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, v = value] { setWorkerThreads(v); }, Qt::QueuedConnection);
    return;
  }

  m_workerThreads = value;
  write("workerThreads", m_workerThreads);
  emit workerThreadsChanged();
}

int AppSettings::fileWriterThreads() const {
  return m_fileWriterThreads;
}

void AppSettings::setFileWriterThreads(int value) {
  if (value < 1) value = 1;
  if (value == m_fileWriterThreads) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, v = value] { setFileWriterThreads(v); },
        Qt::QueuedConnection);
    return;
  }

  m_fileWriterThreads = value;
  write("fileWriterThreads", m_fileWriterThreads);
  emit fileWriterThreadsChanged();
}

void AppSettings::setBackgroundColor(const QString &value) {
  if (value == m_backgroundColor) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, value] { setBackgroundColor(value); },
        Qt::QueuedConnection);
    return;
  }

  m_backgroundColor = value;
  write("viewport/backgroundColor", m_backgroundColor);
  emit backgroundColorChanged();
}

QString AppSettings::backgroundColor() const {
  return m_backgroundColor;
}

AppSettings::Antialiasing AppSettings::antialiasing() const {
  return m_antialiasing;
}

void AppSettings::setAntialiasing(Antialiasing value) {
  if (value == m_antialiasing) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, value] { setAntialiasing(value); }, Qt::QueuedConnection);
    return;
  }

  m_antialiasing = value;
  write("viewport/antialiasing", antialiasingToString(m_antialiasing));
  emit antialiasingChanged();
}

AppSettings::Antialiasing
AppSettings::antialiasingFromString(QStringView modeText) {
  const QString s = modeText.trimmed().toString().toLower();

  if (s == QLatin1String("none") || s == QLatin1String("noaa"))
    return Antialiasing::NoAA;
  if (s == QLatin1String("ssaa")) return Antialiasing::SSAA;
  if (s == QLatin1String("msaa")) return Antialiasing::MSAA;
  if (s == QLatin1String("progressive") || s == QLatin1String("progressiveaa"))
    return Antialiasing::ProgressiveAA;

  return Antialiasing::MSAA;
}

QString AppSettings::antialiasingToString(Antialiasing mode) {
  switch (mode) {
  case Antialiasing::NoAA:
    return QStringLiteral("none");
  case Antialiasing::SSAA:
    return QStringLiteral("ssaa");
  case Antialiasing::MSAA:
    return QStringLiteral("msaa");
  case Antialiasing::ProgressiveAA:
    return QStringLiteral("progressive");
  }
  return QStringLiteral("msaa");
}

int AppSettings::pointSize() const {
  return m_pointSize;
}

void AppSettings::setPointSize(int value) {
  const int v = qBound(1, value, 250);
  if (v == m_pointSize) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, v] { setPointSize(v); }, Qt::QueuedConnection);
    return;
  }

  m_pointSize = v;
  write("viewport/pointSize", m_pointSize);
  emit pointSizeChanged();
}

bool AppSettings::restoreLastWorkspace() const {
  return m_restoreLastWorkspace;
}

void AppSettings::setrestoreLastWorkspace(bool value) {
  if (value == m_restoreLastWorkspace) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, value] { setrestoreLastWorkspace(value); },
        Qt::QueuedConnection);
    return;
  }

  m_restoreLastWorkspace = value;
  write("restoreLastWorkspace", m_restoreLastWorkspace);
  emit restoreLastWorkspaceChanged();
}

QString AppSettings::lastWorkspacePath() const {
  return m_lastWorkspacePath;
}

void AppSettings::setlastWorkspacePath(const QString &value) {
  if (value == m_lastWorkspacePath) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, value] { setlastWorkspacePath(value); },
        Qt::QueuedConnection);
    return;
  }

  m_lastWorkspacePath = value;
  write("lastWorkspacePath", m_lastWorkspacePath);
  emit lastWorkspacePathChanged();
}

bool AppSettings::loadMalformedWorkspaces() const {
  return m_loadMalformedWorkspaces;
}

void AppSettings::setLoadMalformedWorkspaces(bool value) {
  if (value == m_loadMalformedWorkspaces) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, value] { setLoadMalformedWorkspaces(value); },
        Qt::QueuedConnection);
    return;
  }

  m_loadMalformedWorkspaces = value;
  write("loadMalformedWorkspaces", m_loadMalformedWorkspaces);
  emit loadMalformedWorkspacesChanged();
}

bool AppSettings::enablePrometheusMetrics() const {
  return m_enablePrometheusMetrics;
}

void AppSettings::setEnablePrometheusMetrics(bool value) {
  if (value == m_enablePrometheusMetrics) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, value] { setEnablePrometheusMetrics(value); },
        Qt::QueuedConnection);
    return;
  }

  m_enablePrometheusMetrics = value;
  write("prometheusEnabled", m_enablePrometheusMetrics);
  emit enablePrometheusMetricsChanged();
}

QString AppSettings::prometheusAddress() const {
  return m_prometheusAddress;
}

void AppSettings::setPrometheusAddress(const QString &value) {
  if (value == m_prometheusAddress) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, value] { setPrometheusAddress(value); },
        Qt::QueuedConnection);
    return;
  }

  m_prometheusAddress = value;
  write("prometheusAddress", m_prometheusAddress);
  emit prometheusAddressChanged();
}

bool AppSettings::enableTracyProfiling() const {
  return m_enableTracyProfiling;
}

void AppSettings::setEnableTracyProfiling(bool value) {
  if (value == m_enableTracyProfiling) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, value] { setEnableTracyProfiling(value); },
        Qt::QueuedConnection);
    return;
  }

  m_enableTracyProfiling = value;
  write("tracyEnabled", m_enableTracyProfiling);
  emit enableTracyProfilingChanged();
}

QVariant AppSettings::value(const QString &key,
                            const QVariant &defaultValue) const {
  return m_settings.value(key, defaultValue);
}

void AppSettings::setValue(const QString &key, const QVariant &value) {
  if (key.isEmpty()) return;

  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(
        this, [this, key, value] { setValue(key, value); },
        Qt::QueuedConnection);
    return;
  }

  write(key, value);
  emit valueChanged(key);
}

void AppSettings::sync() {
  if (!onObjectThread(this)) {
    QMetaObject::invokeMethod(this, [this] { sync(); }, Qt::QueuedConnection);
    return;
  }
  m_settings.sync();
}

void AppSettings::write(const QString &key, const QVariant &v) {
  m_settings.setValue(key, v);
}

} // namespace pc
