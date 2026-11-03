#include "blefilesource.h"
#include "bleipc.h"

namespace {
bool unsignedSize(const QJsonValue &value, qint64 &size)
{
    const QString text = value.toString();
    if (text.isEmpty()) return false;
    for (const auto ch : text) if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
    bool ok = false; size = text.toLongLong(&ok); return ok && size >= 0;
}
}
BleFileSource::BleFileSource(QObject *parent, int timeout) : QObject(parent), m_ioTimeout(qMax(1, timeout))
{
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        fail(QStringLiteral("发送文件的后台%1超时，已停止本次文件发送。请检查磁盘或网络路径；BLE 接收连接不受此读取故障影响。")
             .arg(!m_hello ? QStringLiteral("启动") : m_opened ? QStringLiteral("读取") : QStringLiteral("打开")));
    });
}
BleFileSource::~BleFileSource()
{
    stop();
    if (m_process) {
        // Never let QProcess's destructor synchronously wait on a broken disk.
        // EOF on the control pipe also terminates the native reader on parent exit.
        auto process = m_process; m_process = nullptr;
        process->disconnect(this); process->setParent(nullptr);
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), process, &QObject::deleteLater);
        if (process->state() == QProcess::NotRunning) process->deleteLater();
        else process->kill();
    }
}
bool BleFileSource::start(const QString &host, const QString &path, const QString &session,
                          const QString &transfer, QString &error)
{
    if (m_process) { error = QStringLiteral("上一文件读取进程正在清理，请稍后重试。"); return false; }
    if (path.isEmpty() || path.contains(QChar(0)) || path.size() > 32767) {
        error = QStringLiteral("发送文件路径无效。"); return false;
    }
    m_path = path; m_session = session; m_transfer = transfer;
    m_total = -1; m_offset = 0; m_request.clear(); m_input.clear();
    m_active = true; m_hello = m_opened = false;
    auto process = new QProcess(this); m_process = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        if (m_process == process && m_active) readOutput(); else process->readAllStandardOutput();
    });
    connect(process, &QProcess::readyReadStandardError, this, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError code) {
        if (m_process != process) return;
        if (m_active && code != QProcess::Crashed) fail(QStringLiteral("无法运行后台文件读取：%1").arg(process->errorString()));
        // FailedToStart does not emit finished(). Allow a later retry.
        if (code == QProcess::FailedToStart && m_process == process) {
            m_process = nullptr; process->deleteLater(); emit cleanupFinished();
        }
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus) {
        if (m_process == process) {
            if (m_active) fail(QStringLiteral("后台文件读取意外结束（代码 %1），文件发送已停止。").arg(code));
            m_process = nullptr; emit cleanupFinished();
        }
        process->deleteLater();
    });
    m_deadline.start(qMin(5000, m_ioTimeout));
    process->start(host, {QStringLiteral("--file-reader")});
    return true;
}
QJsonObject BleFileSource::request(const QString &op)
{
    m_operation = op; m_request = QString::number(++m_requestCounter);
    return {{"op", op}, {"session", m_session}, {"transfer", m_transfer}, {"request", m_request}};
}
bool BleFileSource::send(QJsonObject object)
{
    if (!m_active || !m_process || m_process->state() != QProcess::Running) return false;
    const auto bytes = BleIpc::encode(object);
    if (m_process->bytesToWrite() + bytes.size() > 128 * 1024 || m_process->write(bytes) != bytes.size()) {
        fail(QStringLiteral("后台文件读取控制通道已关闭或阻塞。")); return false;
    }
    m_deadline.start(m_ioTimeout); return true;
}
bool BleFileSource::requestChunk(qint64 offset, int length)
{
    if (!m_active || !m_opened || readPending() || offset != m_offset || length < 1 || length > 4096 ||
        offset < 0 || offset >= m_total || length > m_total - offset) return false;
    m_length = length;
    auto object = request(QStringLiteral("read"));
    object.insert("offset", QString::number(offset)); object.insert("length", length);
    return send(object);
}
void BleFileSource::stop()
{
    m_active = false; m_deadline.stop(); m_request.clear(); m_input.clear();
    if (m_process) m_process->kill(); // Only this owned reader, not the BLE host.
}
void BleFileSource::fail(const QString &message)
{
    if (!m_active) return;
    stop(); emit failed(message);
}
void BleFileSource::readOutput()
{
    if (m_process->bytesAvailable() > 128 * 1024) { fail(QStringLiteral("后台文件读取返回积压超过安全上限。")); return; }
    m_input += m_process->read(128 * 1024);
    QJsonObject object; QString error;
    while (m_active && BleIpc::take(m_input, object, error)) consume(object);
    if (m_active && (!error.isEmpty() || m_input.size() > 128 * 1024))
        fail(error.isEmpty() ? QStringLiteral("后台文件读取消息超过大小限制。") : error);
}
void BleFileSource::consume(const QJsonObject &object)
{
    const auto type = object.value("event").toString();
    if (type == "hello") {
        if (m_hello || object.value("version").toInt() != 1 || object.value("role").toString() != "file-reader" ||
            object.value("fileReaderVersion").toInt() != 1) {
            fail(QStringLiteral("文件读取辅助程序版本或角色不匹配，请更新完整软件包。")); return;
        }
        m_hello = true; auto obj = request(QStringLiteral("open")); obj.insert("path", m_path); send(obj); return;
    }
    if (!m_hello) { fail(QStringLiteral("文件读取辅助程序未完成握手。")); return; }
    if (object.value("session").toString() != m_session || object.value("transfer").toString() != m_transfer ||
        m_request.isEmpty() || object.value("request").toString() != m_request) return;
    if (type == "error") {
        fail(QStringLiteral("发送文件%1失败（系统代码 %2）：\n%3")
             .arg(m_operation == "open" ? QStringLiteral("打开") : QStringLiteral("读取"))
             .arg(object.value("code").toInt()).arg(object.value("message").toString())); return;
    }
    if (type == "opened" && m_operation == "open" && !m_opened) {
        if (!unsignedSize(object.value("size"), m_total)) { fail(QStringLiteral("发送文件大小无效。")); return; }
        m_deadline.stop(); m_request.clear(); m_opened = true; emit opened(m_total); return;
    }
    if (type == "chunk" && m_operation == "read" && m_opened) {
        qint64 offset = -1;
        const auto encoded = object.value("data").toString().toLatin1();
        const auto bytes = QByteArray::fromBase64(encoded);
        if (!unsignedSize(object.value("offset"), offset) || offset != m_offset ||
            !object.value("data").isString() || bytes.toBase64() != encoded || bytes.isEmpty() ||
            bytes.size() > m_length || bytes.size() > m_total - m_offset || !object.value("eof").isBool() ||
            object.value("eof").toBool() != (m_offset + bytes.size() == m_total)) {
            fail(QStringLiteral("发送文件读取偏移、长度或结束标记不一致，已安全停止。")); return;
        }
        m_offset += bytes.size(); m_request.clear(); m_deadline.stop();
        emit chunkReady(bytes, object.value("eof").toBool()); return;
    }
    fail(QStringLiteral("后台文件读取返回了无效的任务结果。"));
}
