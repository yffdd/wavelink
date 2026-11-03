#include "blestreamworker.h"
#include "ProtocolParserFactory.h"
#include <QElapsedTimer>
#include <QMutexLocker>

BleStreamWorker::BleStreamWorker(QObject *parent) : QObject(parent) {
    m_wlr.setErrorHandler([this](const QString &error) {
        m_recordError = error;
        emit recordingError(m_session, m_recordRequest, error);
    });
}
BleStreamWorker::~BleStreamWorker() { m_csv.stop(); m_wlr.stop(); }
qint64 BleStreamWorker::queuedBytes() { QMutexLocker lock(&m_mutex); return m_queuedBytes; }
void BleStreamWorker::post(Task task)
{
    QMutexLocker lock(&m_mutex);
    m_tasks.enqueue(std::move(task));
    if (!m_scheduled) { m_scheduled = true; QMetaObject::invokeMethod(this, "drain", Qt::QueuedConnection); }
}
void BleStreamWorker::beginSession(quint64 session, int protocol, bool bigEndian)
{ beginSession(session, DataProtocols::ProtocolSelection::fromLegacy(protocol, bigEndian), {}); }
void BleStreamWorker::beginSession(quint64 session, DataProtocols::ProtocolSelection selection, DataProtocols::ProtocolProfile profile)
{
    { QMutexLocker lock(&m_mutex); m_acceptSession = session; }
    Task task; task.kind = Begin; task.session = session; task.selection = selection; task.profile = profile;
    task.plotEpoch = ++m_requestedPlotEpoch; post(task);
}
void BleStreamWorker::endSession()
{
    { QMutexLocker lock(&m_mutex); m_acceptSession = 0; }
    Task task; task.kind = End; task.plotEpoch = ++m_requestedPlotEpoch; post(task);
}
bool BleStreamWorker::enqueueData(quint64 session, const QByteArray &data, qint64 timestampNs)
{
    QMutexLocker lock(&m_mutex);
    if (!session || session != m_acceptSession) return false;
    // Do not silently discard acquisition data: the caller disconnects and
    // stops recording with an explicit diagnostic if this bound is exceeded.
    if (m_queuedBytes + data.size() > 4 * 1024 * 1024 || m_tasks.size() >= 8192) return false;
    Task task; task.session = session; task.data = data; task.timestampNs = timestampNs;
    task.statusMonotonicMs=m_statusMonitor->monotonicNow();
    m_queuedBytes += data.size(); m_tasks.enqueue(std::move(task));
    if (!m_scheduled) { m_scheduled = true; QMetaObject::invokeMethod(this, "drain", Qt::QueuedConnection); }
    return true;
}
void BleStreamWorker::setProtocol(int protocol, bool bigEndian)
{ applyProtocol(0, DataProtocols::ProtocolSelection::fromLegacy(protocol, bigEndian), {}); }
void BleStreamWorker::applyProtocol(quint64 request, DataProtocols::ProtocolSelection selection, DataProtocols::ProtocolProfile profile)
{ Task task; task.kind = Protocol; task.selection = selection; task.profile = profile; task.request = request;
  task.plotEpoch = ++m_requestedPlotEpoch; post(task); }
void BleStreamWorker::startRecording(const QString &path, double sampleRate, quint64 request)
{ Task task; task.kind = StartRecording; task.path = path; task.sampleRate = sampleRate; task.request = request; post(task); }
void BleStreamWorker::stopRecording(quint64 request)
{ Task task; task.kind = StopRecording; task.request = request; post(task); }
void BleStreamWorker::drain()
{
    QElapsedTimer budget; budget.start();
    for (;;) {
        Task task;
        { QMutexLocker lock(&m_mutex);
          if (m_tasks.isEmpty()) { m_scheduled = false; return; }
          task = m_tasks.dequeue(); m_queuedBytes -= task.data.size(); }
        process(task);
        if (budget.elapsed() >= 4) { QMetaObject::invokeMethod(this, "drain", Qt::QueuedConnection); return; }
    }
}
QString BleStreamWorker::stopRecordingNow()
{
    for (;;) {
        Task task;
        { QMutexLocker lock(&m_mutex);
          if (m_tasks.isEmpty()) break;
          task = m_tasks.dequeue(); m_queuedBytes -= task.data.size(); }
        process(task);
    }
    finishRecording();
    return m_recordError;
}
void BleStreamWorker::finishRecording()
{
    QString csvError, wlrError;
    const bool csvOk = m_csv.finish(csvError), wlrOk = m_wlr.finish(wlrError);
    if (!csvOk || !wlrOk) {
        m_recordError = csvOk ? wlrError : csvError;
        emit recordingError(m_session, m_recordRequest, m_recordError);
    }
    m_loggedBytes = m_useWlr ? m_wlr.totalBytes() : m_csv.totalBytes();
    m_recordingChannels.reset();
}
bool BleStreamWorker::resetParser(const DataProtocols::ProtocolSelection &selection, const DataProtocols::ProtocolProfile &profile, QString &error)
{
    auto candidate = DataProtocols::ProtocolParserFactory::create(selection, profile, this, error);
    if (!candidate) return false;
    delete m_parser; m_parser = candidate; m_buffer.clear(); m_plotFrames.clear();
    m_statusMonitor->bindProtocol(m_parser, selection, profile);
    connect(m_parser, &ProtocolParser::channelsUpdated, this, &BleStreamWorker::frameReady);
    connect(m_parser, &ProtocolParser::recordingFrameReady, this, &BleStreamWorker::recordFrame);
    connect(m_parser, &ProtocolParser::bufferOverflow, this, [this] {
        if (!m_session) return;
        const auto session = m_session; finishRecording(); m_session = 0;
        m_statusMonitor->setConnected(false);
        emit pipelineError(session, QStringLiteral("BLE 协议内部缓冲超过安全上限，已停止采集。请检查协议和外设数据格式。"));
    });
    return true;
}
void BleStreamWorker::process(const Task &task)
{
    switch (task.kind) {
    case Begin: {
        finishRecording(); m_session = task.session; m_loggedBytes = 0; m_recordError.clear();
        m_parsedFrames = 0; m_recordedFrames = 0;
        m_plotFrames.resetDropCount();
        QString error;
        if (!resetParser(task.selection, task.profile, error)) {
            m_session = 0; emit pipelineError(task.session, QStringLiteral("BLE 协议应用失败：") + error);
        }
        m_statusMonitor->setConnected(m_session != 0);
        m_processedPlotEpoch = task.plotEpoch; break;
    }
    case End:
        m_statusMonitor->setConnected(false);
        finishRecording(); m_session = 0; m_buffer.clear(); m_plotFrames.clear(); m_processedPlotEpoch = task.plotEpoch; break;
    case Protocol: {
        QString error;
        bool ok = !(m_csv.isActive() || m_wlr.isActive());
        if (!ok) error = QStringLiteral("录制期间不能切换协议。");
        else ok = resetParser(task.selection, task.profile, error);
        m_processedPlotEpoch = task.plotEpoch;
        emit protocolApplyFinished(task.request, ok, error); break;
    }
    case StartRecording: {
        finishRecording(); QString error; m_recordError.clear(); m_recordRequest = task.request;
        m_useWlr = task.path.endsWith(".wlr", Qt::CaseInsensitive);
        const auto names = m_parser ? m_parser->channelNames() : QStringList();
        const bool ok = m_session && (m_useWlr ? m_wlr.start(task.path, error, task.sampleRate, names) : m_csv.start(task.path, error, task.sampleRate, names));
        if (!ok && error.isEmpty()) error = QStringLiteral("BLE 会话已经结束，无法开始录制。");
        if (ok) { m_loggedBytes = 0; m_recordedFrames = 0; }
        if (!ok) m_recordError = error;
        emit recordingStartFinished(m_session, m_recordRequest, ok, error); break;
    }
    case StopRecording:
        finishRecording(); emit recordingStopped(task.request, m_recordError, m_loggedBytes.load()); break;
    case Data:
        if (!m_session || task.session != m_session || !m_parser) break;
        m_timestampNs = task.timestampNs; m_buffer += task.data;
        m_statusMonitor->setReceiveTimestamp(m_timestampNs);
        m_statusMonitor->feedReceived(task.data,m_timestampNs,false,task.statusMonotonicMs);
        { const int consumed = m_parser->processData(m_buffer.constData(), m_buffer.size());
          if (consumed > 0) m_buffer.remove(0, consumed); }
        if (m_buffer.size() > 1024 * 1024) {
            const auto session = m_session;
            m_buffer.clear(); finishRecording(); m_session = 0;
            m_statusMonitor->setConnected(false);
            emit pipelineError(session, QStringLiteral("BLE 协议未能解析的数据超过 1 MB，已停止采集。请检查协议和数据格式。"));
        }
        break;
    }
}
void BleStreamWorker::frameReady(const QVector<double> &frame)
{
    if (!m_session || frame.isEmpty()) return;
    if (frame.size() > 256) {
        m_statusMonitor->setConnected(false);
        const auto session = m_session; finishRecording(); m_session = 0;
        emit pipelineError(session, QStringLiteral("BLE 数值帧超过 256 通道，已停止采集。请检查协议和外设数据格式。")); return;
    }
    ++m_parsedFrames;
    // Display uses converted values. The selected recording values have already
    // reached the recorder before this bounded mailbox and GUI filtering.
    m_plotFrames.push(frame);
}
void BleStreamWorker::recordFrame(const QVector<double> &frame)
{
    if (!m_session || frame.isEmpty() || frame.size() > 256) return;
    if (m_csv.isActive() || m_wlr.isActive()) {
        if (!m_recordingChannels.accept(frame.size())) return;
        QString error;
        const bool ok = m_useWlr ? m_wlr.append(frame, error, m_timestampNs) : m_csv.append(frame, error, m_timestampNs);
        m_loggedBytes = m_useWlr ? m_wlr.totalBytes() : m_csv.totalBytes();
        if (!ok) { m_csv.stop(); m_wlr.stop(); m_recordError = error; emit recordingError(m_session, m_recordRequest, error); }
        else ++m_recordedFrames;
    }
}
