#pragma once
#include <QObject>
#include <QUrl>
#include <QDateTime>
#include <QFutureWatcher>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>

struct UpdateRelease {
    QString version, notes, sha256;
    QUrl downloadUrl;
    QDateTime published;
    qint64 size = 0;
};
struct UpdateSource { QUrl feedUrl; QByteArray publicKey; };

class UpdateManager final : public QObject {
    Q_OBJECT
public:
    enum class State { Idle, Checking, Available, Current, Downloading, Ready, Error };
    using Sink = std::function<bool(const QByteArray &)>;
    using Transport = std::function<bool(const QUrl &, const Sink &, const std::atomic_bool &, QString &)>;
    explicit UpdateManager(QObject *parent = nullptr);
    UpdateManager(const UpdateSource &source, Transport transport, QObject *parent = nullptr);
    ~UpdateManager() override;
    static UpdateSource builtInSource();
    static bool validHttps(const QUrl &url);
    static bool parseManifest(const QByteArray &envelope, const QByteArray &publicKey,
                              UpdateRelease &release, QString &error, const QDateTime &now = QDateTime::currentDateTimeUtc());
    static bool newer(const QString &candidate, const QString &current);
    State state() const { return m_state; }
    QString message() const { return m_message; }
    UpdateRelease release() const { return m_release; }
    QString packagePath() const;
    bool configured() const;
    bool automaticResult() const { return m_automatic; }
    void check(bool automatic = false);
    void download();
    void cancel();
    void ignoreVersion();
    // Called only after a successful backup and user-confirmed idle shutdown.
    bool launchInstaller(const QString &restartExe, const QString &backupPath, QString &error);
    void commitInstallerHandoff();
signals:
    void changed();
    void progress(qint64 received, qint64 total);
    void updateFound();
private:
    struct Job {
        std::atomic_bool cancelled{false}, timedOut{false}; std::atomic<qint64> received{0};
        QByteArray manifest; QString error, path; bool ok = false;
        std::shared_ptr<QTemporaryDir> folder;
    };
    UpdateSource m_source; Transport m_transport;
    State m_state = State::Idle; QString m_message; UpdateRelease m_release;
    bool m_automatic = false;
    std::shared_ptr<Job> m_job;
    std::shared_ptr<QTemporaryDir> m_downloadFolder;
    QString m_package;
    void *m_installCommit = nullptr;
    QFutureWatcher<void> *m_watcher = nullptr; QTimer m_progressTimer;
    void setState(State state, const QString &message);
    void finish(bool download);
};
