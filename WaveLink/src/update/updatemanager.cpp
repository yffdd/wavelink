#include "updatemanager.h"
#include "updateconfig.h"
#include "updatecrypto.h"
#include <QtConcurrent>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVersionNumber>
#include <QCoreApplication>
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QProcess>
#include <QUuid>
#include <QElapsedTimer>
#ifdef Q_OS_WIN
#include <winhttp.h>
#endif

namespace {
constexpr qint64 ManifestLimit = 128 * 1024, PackageLimit = 1024LL * 1024 * 1024;
bool fail(QString &error, const QString &message) { error = message; return false; }
bool versionValid(const QString &s) { return QRegularExpression("^[0-9]{1,6}(\\.[0-9]{1,6}){1,3}$").match(s).hasMatch(); }
bool decode(const QJsonValue &value, QByteArray &bytes) {
    if (!value.isString()) return false;
    auto s = value.toString().toLatin1(); bytes = QByteArray::fromBase64(s);
    return !bytes.isEmpty() && bytes.toBase64() == s;
}
std::vector<unsigned char> vector(const QByteArray &bytes) { return {bytes.begin(), bytes.end()}; }
// WinHTTP uses Windows' TLS implementation/root store. Qt 5's optional,
// separately installed OpenSSL DLLs are not required by online updates.
bool fetch(QUrl url, const UpdateManager::Sink &sink, const std::atomic_bool &cancelled, QString &error) {
#ifdef Q_OS_WIN
    struct Internet { HINTERNET h = nullptr; ~Internet() { if (h) WinHttpCloseHandle(h); } } session;
    session.h = WinHttpOpen(L"WaveLink-Update/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return fail(error, QStringLiteral("无法初始化 Windows 网络服务。"));
    WinHttpSetTimeouts(session.h, 8000, 8000, 8000, 8000);
    for (int redirect = 0; redirect <= 5 && !cancelled; ++redirect) {
        if (!UpdateManager::validHttps(url)) return fail(error, QStringLiteral("更新地址必须为有效的 HTTPS 地址。"));
        Internet connection, request;
        connection.h = WinHttpConnect(session.h, reinterpret_cast<LPCWSTR>(url.host().utf16()), INTERNET_PORT(url.port(443)), 0);
        if (!connection.h) break;
        QString target = url.path(QUrl::FullyEncoded); if (target.isEmpty()) target = "/";
        if (url.hasQuery()) target += "?" + url.query(QUrl::FullyEncoded);
        request.h = WinHttpOpenRequest(connection.h, L"GET", reinterpret_cast<LPCWSTR>(target.utf16()), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request.h) break;
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        if (!WinHttpSendRequest(request.h, L"Accept-Encoding: identity\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request.h, nullptr)) break;
        DWORD status = 0, length = sizeof(status);
        if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &length, nullptr)) break;
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            wchar_t location[4096]; length = sizeof(location);
            if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_LOCATION, nullptr, location, &length, nullptr)) break;
            url = url.resolved(QUrl(QString::fromWCharArray(location)));
            continue;
        }
        if (status != 200) return fail(error, QStringLiteral("更新服务器返回 HTTP %1。请稍后重试。").arg(status));
        QElapsedTimer duration; duration.start();
        while (!cancelled) {
            if (duration.elapsed() > 2 * 60 * 60 * 1000) return fail(error, QStringLiteral("下载时间过长，请重试。"));
            char bytes[64 * 1024]; DWORD count = 0;
            if (!WinHttpReadData(request.h, bytes, sizeof(bytes), &count))
                return fail(error, QStringLiteral("网络读取中断，请重试。错误代码：%1").arg(GetLastError()));
            if (!count) return true;
            if (!sink(QByteArray(bytes, int(count)))) return false;
        }
        return fail(error, QStringLiteral("已取消。"));
    }
    return fail(error, cancelled ? QStringLiteral("已取消。") : QStringLiteral("无法连接更新服务器或 HTTPS 验证失败（%1）。请检查网络、系统时间及代理设置。").arg(GetLastError()));
#else
    Q_UNUSED(url); Q_UNUSED(sink); Q_UNUSED(cancelled);
    return fail(error, QStringLiteral("当前平台暂不支持在线更新。"));
#endif
}
}

UpdateSource UpdateManager::builtInSource() {
    return {QUrl(QStringLiteral(WAVELINK_UPDATE_FEED_URL)), QByteArray::fromBase64(QByteArray(WAVELINK_UPDATE_PUBLIC_KEY))};
}
bool UpdateManager::validHttps(const QUrl &url) {
    return url.isValid() && url.scheme() == "https" && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasFragment() && url.toEncoded().size() <= 4096;
}
bool UpdateManager::configured() const { return validHttps(m_source.feedUrl) && !m_source.publicKey.isEmpty(); }
bool UpdateManager::newer(const QString &candidate, const QString &current) {
    return versionValid(candidate) && versionValid(current) && QVersionNumber::compare(QVersionNumber::fromString(candidate).normalized(), QVersionNumber::fromString(current).normalized()) > 0;
}
bool UpdateManager::parseManifest(const QByteArray &envelope, const QByteArray &publicKey, UpdateRelease &release, QString &error, const QDateTime &now) {
    if (envelope.size() > ManifestLimit || publicKey.isEmpty()) return fail(error, QStringLiteral("更新清单过大或缺少受信任的发布公钥。"));
    QJsonParseError parse; auto doc = QJsonDocument::fromJson(envelope, &parse);
    QByteArray payload, signature;
    if (parse.error != QJsonParseError::NoError || !doc.isObject() || !decode(doc.object().value("payload"), payload) || !decode(doc.object().value("signature"), signature))
        return fail(error, QStringLiteral("更新清单格式错误，未接受服务器内容。"));
#ifdef Q_OS_WIN
    if (!UpdateCrypto::verify(vector(publicKey), vector(QCryptographicHash::hash(payload, QCryptographicHash::Sha256)), vector(signature)))
        return fail(error, QStringLiteral("更新清单签名验证失败，已阻止更新。"));
#else
    return fail(error, QStringLiteral("当前平台不支持签名验证。"));
#endif
    doc = QJsonDocument::fromJson(payload, &parse); auto object = doc.object();
    if (parse.error != QJsonParseError::NoError || !doc.isObject() || object.value("schema").toInt() != 1 ||
        object.value("product").toString() != "WaveLink" || object.value("platform").toString() != "windows-x64" || object.value("channel").toString() != "stable")
        return fail(error, QStringLiteral("更新清单的产品、平台或发布渠道不匹配。"));
    const auto expires = QDateTime::fromString(object.value("expires").toString(), Qt::ISODate);
    if (!expires.isValid() || expires <= now || expires > now.addDays(90)) return fail(error, QStringLiteral("更新清单已过期或有效期异常，请联系发布者刷新清单。"));
    UpdateRelease value; value.version = object.value("version").toString(); value.notes = object.value("notes").toString();
    value.downloadUrl = QUrl(object.value("url").toString()); value.sha256 = object.value("sha256").toString();
    value.published = QDateTime::fromString(object.value("published").toString(), Qt::ISODate);
    const double size = object.value("size").toDouble(-1); value.size = qint64(qBound(0.0, size, double(PackageLimit + 1)));
    if (!versionValid(value.version) || !validHttps(value.downloadUrl) || !value.published.isValid() || value.published > now.addSecs(24 * 3600) ||
        value.notes.size() > 20000 || size != double(value.size) || value.size <= 0 || value.size > PackageLimit ||
        !QRegularExpression("^[0-9a-fA-F]{64}$").match(value.sha256).hasMatch()) return fail(error, QStringLiteral("更新版本、下载地址、大小或摘要无效。"));
    value.sha256 = value.sha256.toLower(); release = value; return true;
}
UpdateManager::UpdateManager(QObject *parent) : UpdateManager(builtInSource(), fetch, parent) {}
UpdateManager::UpdateManager(const UpdateSource &source, Transport transport, QObject *parent) : QObject(parent), m_source(source), m_transport(std::move(transport)) {
    m_progressTimer.setInterval(150);
    connect(&m_progressTimer, &QTimer::timeout, this, [this] { if (m_job) emit progress(m_job->received, m_release.size); });
    m_message = configured() ? QStringLiteral("点击“检查更新”获取最新版本。") : QStringLiteral("在线更新暂未配置发布地址或发布公钥。当前可使用手动下载入口。" );
}
UpdateManager::~UpdateManager() {
    if (m_job) m_job->cancelled = true;
#ifdef Q_OS_WIN
    if (m_installCommit) CloseHandle(m_installCommit);
#endif
}
void UpdateManager::setState(State state, const QString &message) { m_state = state; m_message = message; emit changed(); }
QString UpdateManager::packagePath() const { return m_state == State::Ready ? m_package : QString(); }
void UpdateManager::check(bool automatic) {
    if (m_watcher || m_state == State::Ready) return;
    m_automatic = automatic;
    if (!configured()) { if (!automatic) setState(State::Error, QStringLiteral("在线更新尚未配置。请使用手动下载入口，或等待发布者配置更新服务。")); return; }
    m_job = std::make_shared<Job>(); auto job = m_job; auto transport = m_transport; auto url = m_source.feedUrl;
    QTimer::singleShot(30000, this, [weak = std::weak_ptr<Job>(job)] {
        if (auto pending = weak.lock()) { pending->timedOut = true; pending->cancelled = true; }
    });
    setState(State::Checking, QStringLiteral("正在安全检查更新…"));
    m_watcher = new QFutureWatcher<void>(this); connect(m_watcher, &QFutureWatcher<void>::finished, this, [this] { finish(false); });
    m_watcher->setFuture(QtConcurrent::run([job, transport, url] {
        job->ok = transport(url, [job](const QByteArray &bytes) {
            if (job->cancelled || job->manifest.size() + bytes.size() > ManifestLimit) { job->error = QStringLiteral("更新清单超过大小限制或已取消。"); return false; }
            job->manifest += bytes; return true;
        }, job->cancelled, job->error);
    }));
}
void UpdateManager::download() {
    if (m_watcher || m_state != State::Available) return;
    m_job = std::make_shared<Job>(); auto job = m_job; auto transport = m_transport; auto release = m_release;
    setState(State::Downloading, QStringLiteral("正在下载安装包，采集与录制可继续；下载不会自动安装。"));
    m_progressTimer.start(); m_watcher = new QFutureWatcher<void>(this);
    connect(m_watcher, &QFutureWatcher<void>::finished, this, [this] { finish(true); });
    m_watcher->setFuture(QtConcurrent::run([job, transport, release] {
        job->folder = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/WaveLink-update-XXXXXX");
        job->path = job->folder->filePath("WaveLink_Setup.exe");
        QFile file(job->path); QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!job->folder->isValid() || !file.open(QIODevice::WriteOnly)) { job->error = QStringLiteral("无法创建更新临时文件，请检查磁盘空间及权限。"); return; }
        job->ok = transport(release.downloadUrl, [job, &file, &hash, release](const QByteArray &bytes) {
            if (job->cancelled || job->received + bytes.size() > release.size) { job->error = QStringLiteral("安装包超过声明大小或已取消。"); return false; }
            if (file.write(bytes) != bytes.size()) { job->error = QStringLiteral("写入安装包失败，请检查磁盘空间。"); return false; }
            hash.addData(bytes); job->received += bytes.size(); return true;
        }, job->cancelled, job->error);
        if (job->ok && (!file.flush() || file.error() != QFile::NoError || job->received != release.size || hash.result().toHex() != release.sha256.toLatin1())) {
            job->ok = false; job->error = QStringLiteral("安装包大小或 SHA-256 校验失败，已阻止安装；请重新下载。");
        }
        file.close();
    }));
}
void UpdateManager::finish(bool download) {
    m_progressTimer.stop(); m_watcher->deleteLater(); m_watcher = nullptr;
    auto job = m_job; m_job.reset();
    if (job->timedOut) { setState(State::Error, QStringLiteral("检查更新超时，请稍后重试。")); return; }
    if (job->cancelled) { setState(download ? State::Available : State::Idle, QStringLiteral("已取消，不影响当前软件及数据。")); return; }
    if (!job->ok) { setState(download ? State::Available : State::Error, job->error.isEmpty() ? QStringLiteral("网络请求失败，请重试。") : job->error); return; }
    if (download) {
        m_downloadFolder = job->folder; m_package = job->path; emit progress(m_release.size, m_release.size);
        setState(State::Ready, QStringLiteral("下载及完整性校验完成。安装前将备份配置并退出软件，不会自动中断采集录制。")); return;
    }
    QString error;
    if (!parseManifest(job->manifest, m_source.publicKey, m_release, error)) { setState(State::Error, error); return; }
    QSettings settings("WaveLink", "WaveLink"); settings.setValue("Update/LastCheckUtc", QDateTime::currentDateTimeUtc());
    if (newer(m_release.version, QCoreApplication::applicationVersion())) {
        setState(State::Available, QStringLiteral("发现新版本 v%1。").arg(m_release.version));
        if (!m_automatic || settings.value("Update/IgnoredVersion").toString() != m_release.version) emit updateFound();
    } else setState(State::Current, QStringLiteral("当前已是此稳定渠道的最新版本（未发现更高版本）。"));
}
void UpdateManager::cancel() { if (m_job) { m_job->cancelled = true; m_message = QStringLiteral("正在取消…"); emit changed(); } }
void UpdateManager::ignoreVersion() {
    QSettings settings("WaveLink", "WaveLink"); settings.setValue("Update/IgnoredVersion", m_release.version); settings.sync();
    emit changed();
}
bool UpdateManager::launchInstaller(const QString &restartExe, const QString &backupPath, QString &error) {
#ifdef Q_OS_WIN
    if (m_state != State::Ready || !m_downloadFolder || !QFileInfo::exists(m_package)) return fail(error, QStringLiteral("请先完成安装包下载与校验。"));
    const auto helper = QDir(QCoreApplication::applicationDirPath()).filePath("WaveLinkUpdateLauncher.exe");
    const auto copiedHelper = m_downloadFolder->filePath("WaveLinkUpdateLauncher.exe");
    if (!QFileInfo::exists(copiedHelper) && !QFile::copy(helper, copiedHelper)) return fail(error, QStringLiteral("无法准备更新启动器，请重新安装完整版本。"));
    const auto eventName = "Local\\WaveLinkUpdate-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<LPCWSTR>(eventName.utf16()));
    if (!ready) return fail(error, QStringLiteral("无法创建更新交接信号。"));
    const auto commitName = eventName + "-commit";
    HANDLE commit = CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<LPCWSTR>(commitName.utf16()));
    if (!commit) { CloseHandle(ready); return fail(error, QStringLiteral("无法创建安装确认信号。")); }
    const bool started = QProcess::startDetached(copiedHelper, {QString::number(QCoreApplication::applicationPid()), eventName,
        m_package, m_release.sha256, restartExe, backupPath}, m_downloadFolder->path());
    const bool armed = started && WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0; CloseHandle(ready);
    if (!armed) { CloseHandle(commit); return fail(error, QStringLiteral("更新启动器未能准备就绪，软件不会退出。")); }
    if (m_installCommit) CloseHandle(m_installCommit);
    m_installCommit = commit;
    m_downloadFolder->setAutoRemove(false); return true;
#else
    Q_UNUSED(restartExe); Q_UNUSED(backupPath); return fail(error, QStringLiteral("当前平台不支持在线安装。"));
#endif
}
void UpdateManager::commitInstallerHandoff() {
#ifdef Q_OS_WIN
    if (m_installCommit) { SetEvent(m_installCommit); CloseHandle(m_installCommit); m_installCommit = nullptr; }
#endif
}
