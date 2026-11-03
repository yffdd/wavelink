#include "mainwindow.h"
#include "protocolmanagerdialog.h"
#include "ProtocolParserFactory.h"
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QSettings>
#include <QSignalBlocker>
#include <QJsonArray>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QPlainTextEdit>
#include <QScreen>
#include <QGuiApplication>
#include "commandpanelwindow.h"

using namespace DataProtocols;
namespace {
QJsonObject parserRules(QJsonObject definition) {
    definition.remove("name"); definition.remove("description"); definition.remove("revision"); definition.remove("devicePreset");
    definition.remove("autoChannelLayout"); // Editor linkage metadata, not wire decoding.
    definition.remove("autoFrameLayout");
    if (definition.value("recordingValueMode") == QJsonValue("raw")) definition.remove("recordingValueMode");
    return definition;
}
QJsonObject displayRules(QJsonObject definition) {
    definition = parserRules(definition); definition.remove("recordingValueMode"); return definition;
}
void protocolMessage(QWidget *parent, const QString &title, const QString &message) {
    QDialog dialog(parent); dialog.setObjectName("protocolMessageDialog"); dialog.setWindowTitle(title); dialog.resize(560, 260); dialog.setSizeGripEnabled(true);
    auto layout = new QVBoxLayout(&dialog); auto text = new QPlainTextEdit;
    text->setReadOnly(true); text->setPlainText(message); layout->addWidget(text);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok); layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); dialog.exec();
}
}

void MainWindow::initializeProtocols() {
    qRegisterMetaType<ProtocolSelection>("DataProtocols::ProtocolSelection");
    qRegisterMetaType<ProtocolProfile>("DataProtocols::ProtocolProfile");
    m_protocolStore.reset(new ProtocolProfileStore);
    QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp"); QString error;
    const bool loaded = m_protocolStore->load(error, ProtocolProfileStore::legacySelections(main, advanced));
    connect(serialHandler, &SerialHandler::protocolApplyFinished, this, &MainWindow::finishProtocolApply);
    connect(networkHandler, &NetworkHandler::protocolApplyFinished, this, &MainWindow::finishProtocolApply);
    connect(bleWorker, &BleStreamWorker::protocolApplyFinished, this, &MainWindow::finishProtocolApply);
    m_protocolInitialized = true;
    refreshProtocolChoices(); restoreProtocolUi();
    if (loaded && !m_configRecoveryPending) requestProtocolApply();
    else if (!loaded) {
        // A damaged repository must not be overwritten with a default library.
        // Explicitly choose a builtin to continue, or restore it in the manager.
        QTimer::singleShot(0, this, [this, error] { protocolMessage(this, "协议库加载失败", error + "\n原文件未覆盖。请先选择‘自定义协议’，再点击‘管理自定义协议’恢复备份；也可显式选择内置协议临时使用。"); });
    }
}
void MainWindow::refreshProtocolChoices() {
    if (!m_protocolStore) return;
    const QString selected = comboCustomProfile->currentData().toString();
    const QSignalBlocker blocker(comboCustomProfile); comboCustomProfile->clear();
    for (const auto &profile : m_protocolStore->snapshot().profiles) {
        comboCustomProfile->addItem(profile.name, profile.id);
        comboCustomProfile->setItemData(comboCustomProfile->count() - 1, profile.description, Qt::ToolTipRole);
    }
    const int row = comboCustomProfile->findData(selected);
    if (row >= 0) comboCustomProfile->setCurrentIndex(row);
    updateProtocolControls();
}
void MainWindow::restoreProtocolUi() {
    if (!m_protocolStore) return;
    const auto selection = m_protocolStore->snapshot().shared;
    const QSignalBlocker a(comboProtocol), b(comboEndianness), c(comboCustomProfile);
    comboProtocol->setCurrentIndex(comboProtocol->findData(selection.kind == "custom" ? QString("custom") : selection.id));
    comboEndianness->setCurrentIndex(selection.bigEndian ? 1 : 0);
    if (selection.kind == "custom") comboCustomProfile->setCurrentIndex(comboCustomProfile->findData(selection.id));
    updateProtocolControls();
}
bool MainWindow::protocolChangeBlocked() const {
    return m_configRecoveryPending || isRecording || m_bleRecordStopPending || m_connectionOperationPending || m_protocolApplyPending ||
        m_serialSendCancelPending || (fileSendTimer && fileSendTimer->isActive()) || (consoleSendTimer && consoleSendTimer->isActive()) ||
        (serialHandler && serialHandler->pendingSendBytes() > 0) ||
        (networkHandler && networkHandler->pendingSendBytes() > 0) || (bleClient && bleClient->sendBusy());
}
bool MainWindow::protocolUiMatchesApplied() const {
    if (!m_protocolReady || m_protocolApplyPending || m_protocolAppliedInterface != comboInterface->currentData().toInt()) return false;
    if (comboInterface->currentData().toInt() == 3) return true;
    if (comboProtocol->currentData().toString() == "custom") {
        if (m_appliedProtocol.kind != "custom" || m_appliedProtocol.id != comboCustomProfile->currentData().toString()) return false;
        for (const auto &profile : m_protocolStore->snapshot().profiles) if (profile.id == m_appliedProtocol.id)
            return parserRules(profile.definition) == parserRules(m_appliedProfile.definition);
        return false;
    }
    const auto selection = ProtocolSelection::fromLegacy(comboProtocol->currentIndex(), comboEndianness->currentIndex() == 1);
    return selection.toJson() == m_appliedProtocol.toJson();
}
bool MainWindow::protocolHasNumericData() const {
    return comboInterface->currentData().toInt() == 3 || (protocolUiMatchesApplied() && m_appliedProtocol.id != "builtin.raw");
}
void MainWindow::updateProtocolControls() {
    if (!comboCustomProfile || !comboInterface) return;
    const bool custom = comboProtocol->currentData().toString() == "custom";
    const bool demo = comboInterface->currentData().toInt() == 3;
    const bool blocked = m_protocolInitialized && protocolChangeBlocked();
    if (btnCustomProtocols) btnCustomProtocols->setVisible(custom);
    comboEndianness->setVisible(comboProtocol->currentIndex() == 2);
    comboCustomProfile->setVisible(custom); lblProtocolSummary->setVisible(custom || m_protocolApplyPending || m_configRecoveryPending);
    comboProtocol->setEnabled(!blocked && !demo); comboEndianness->setEnabled(!blocked && !demo); comboCustomProfile->setEnabled(!blocked && !demo);
    if (m_configRecoveryPending) lblProtocolSummary->setText("配置导入恢复未完成，请先处理恢复记录后重启，暂不应用协议。");
    else if (m_protocolApplyPending) lblProtocolSummary->setText("正在应用协议，请稍候…");
    else if (custom && m_protocolStore) {
        QString text = "没有可用的自定义协议，请先在管理窗口新建或导入，再选择协议。";
        for (const auto &p : m_protocolStore->snapshot().profiles) if (p.id == comboCustomProfile->currentData().toString()) {
            const bool isText = p.definition.value("type") == QJsonValue("text");
            text = QString("%1 · %2\n%3").arg(p.isKeyValue() ? "多通道键值帧" : isText ? "文本帧" : p.isDelimiterFramed() ? "二进制首尾分隔帧" : p.definition.value("frame").toObject().value("mode") == QJsonValue("length") ? "二进制长度字段帧" : "二进制定长帧")
                .arg(isText ? QString("自动读取全部列") : p.isDynamicArray() ? QString("动态数组 · %1 · 自动读取通道").arg(p.definition.value("array").toObject().value("type").toString()) : QString("%1 通道").arg(p.definition.value("channels").toArray().size()))
                .arg(protocolUiMatchesApplied() ? "已应用到当前接口" : m_protocolAutoApplyDeferred ? "等待自动应用；录制、发送或连接操作结束后继续" : "协议尚未生效，请重新选择或检查协议设置");
            break;
        }
        if (comboInterface->currentData().toInt() == 3) text += "\nDemo 使用自身生成的数据，不按此协议解码。";
        lblProtocolSummary->setText(text);
    }
    // Other connection/recording update paths may adjust button states. These
    // guards also run at the actions themselves, not only in their appearance.
    if (m_protocolInitialized && m_protocolApplyPending) { comboInterface->setEnabled(false); btnOpenClose->setEnabled(false); btnToggleRecord->setEnabled(false); }
    if (m_protocolAutoApplyDeferred && !blocked) queueProtocolAutoApply();
    updateConsoleSendControls();
}
void MainWindow::handleCustomProfileChanged() {
    if (!m_protocolInitialized || comboProtocol->currentData().toString() != "custom") { updateProtocolControls(); return; }
    if (protocolChangeBlocked()) { restoreProtocolUi(); statusBar()->showMessage("录制、发送或连接操作期间不能切换协议。", 5000); return; }
    queueProtocolAutoApply(); updateProtocolControls();
}
void MainWindow::queueProtocolAutoApply() {
    if (!m_protocolInitialized || comboInterface->currentData().toInt() == 3) return;
    m_protocolAutoApplyDeferred = true;
    if (m_protocolAutoApplyScheduled || protocolChangeBlocked()) return;
    m_protocolAutoApplyScheduled = true;
    // Coalesce mode + profile changes in the same UI action. Resolve the
    // latest selection at execution, never apply an intermediate combo row.
    QTimer::singleShot(0, this, [this] {
        m_protocolAutoApplyScheduled = false;
        if (!m_protocolAutoApplyDeferred || protocolChangeBlocked() || comboInterface->currentData().toInt() == 3) return;
        m_protocolAutoApplyDeferred = false; requestProtocolApply();
    });
}
void MainWindow::requestProtocolApply() {
    if (!m_protocolInitialized || protocolChangeBlocked()) return;
    m_protocolAutoApplyDeferred = false;
    ProtocolSelection selection = ProtocolSelection::fromLegacy(comboProtocol->currentIndex(), comboEndianness->currentIndex() == 1);
    ProtocolProfile profile;
    if (comboProtocol->currentData().toString() == "custom") {
        selection.kind = "custom"; selection.id = comboCustomProfile->currentData().toString(); selection.bigEndian = false;
        for (const auto &p : m_protocolStore->snapshot().profiles) if (p.id == selection.id) { profile = p; break; }
        if (profile.id.isEmpty()) {
            // An empty library is a UI state, not an invalid parser request.
            // Do not silently fall back to Raw or start with stale rules.
            if (isPortOpen) { restoreProtocolUi(); protocolMessage(this, "没有可用的自定义协议", "请先新建或导入协议。当前连接仍保留原来的有效协议，没有切换到空方案。"); }
            updateProtocolControls(); return;
        }
    }
    QString error; std::unique_ptr<ProtocolParser> checked(ProtocolParserFactory::create(selection, profile, nullptr, error));
    if (!checked) { protocolMessage(this, "无法应用协议", error); restoreProtocolUi(); return; }
    const bool sameRules = m_protocolReady && m_protocolAppliedInterface == comboInterface->currentData().toInt() &&
        selection.toJson() == m_appliedProtocol.toJson() && parserRules(profile.definition) == parserRules(m_appliedProfile.definition);
    const bool sameDisplayRules = m_protocolReady && m_protocolAppliedInterface == comboInterface->currentData().toInt() &&
        selection.toJson() == m_appliedProtocol.toJson() && displayRules(profile.definition) == displayRules(m_appliedProfile.definition);
    if (sameRules && profile.definition == m_appliedProfile.definition) { updateProtocolControls(); return; }
    bool hasHistory = false; for (auto graph : channelGraphs) if (graph && !graph->data()->isEmpty()) { hasHistory = true; break; }
    if (hasHistory && !sameDisplayRules && (selection.toJson() != m_appliedProtocol.toJson() || displayRules(profile.definition) != displayRules(m_appliedProfile.definition))) {
        QDialog dialog(this); dialog.setObjectName("protocolApplyConfirm"); dialog.setWindowTitle("切换数据协议");
        dialog.setSizeGripEnabled(true);
        const auto screen = QGuiApplication::screenAt(frameGeometry().center());
        const int width = qMin(560, screen ? screen->availableGeometry().width() - 32 : 560);
        dialog.setMinimumWidth(width); dialog.resize(width, 220);
        auto layout = new QVBoxLayout(&dialog); layout->setContentsMargins(20, 20, 20, 16); layout->setSpacing(16);
        auto text = new QLabel("应用新协议将清空当前绘图历史及尚未解析的旧数据。\n\n已有磁盘录制文件不会删除。\n是否继续切换？"); text->setObjectName("protocolApplyMessage");
        text->setWordWrap(true); text->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(text);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(buttons);
        buttons->button(QDialogButtonBox::Ok)->setText("切换协议"); buttons->button(QDialogButtonBox::Cancel)->setText("取消");
        dialog.setMinimumHeight(qMax(220, text->heightForWidth(width - 40) + buttons->sizeHint().height() + 60));
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted) { restoreProtocolUi(); return; }
        if (protocolChangeBlocked()) { restoreProtocolUi(); return; }
    }
    m_pendingProtocol = selection; m_pendingProfile = profile;
    m_protocolReplacesData = !sameDisplayRules;
    m_protocolApplyInterface = comboInterface->currentData().toInt(); m_protocolApplyPending = true;
    const auto request = ++m_protocolRequest;
    if (cmdWindow && !sameRules) cmdWindow->forceStopAllTimers();
    updateProtocolControls();
    if (sameRules) { finishProtocolApply(request, true, {}); return; }
    if (m_protocolApplyInterface == 3) { finishProtocolApply(request, true, {}); return; }
    if (m_protocolApplyInterface == 4) { bleWorker->applyProtocol(request, selection, profile); return; }
    QObject *worker = m_protocolApplyInterface == 0 ? static_cast<QObject *>(serialHandler) : static_cast<QObject *>(networkHandler);
    if (!QMetaObject::invokeMethod(worker, "applyProtocol", Qt::QueuedConnection, Q_ARG(quint64, request),
                                  Q_ARG(DataProtocols::ProtocolSelection, selection), Q_ARG(DataProtocols::ProtocolProfile, profile)))
        finishProtocolApply(request, false, QStringLiteral("无法向通信线程提交协议。"));
}
void MainWindow::finishProtocolApply(quint64 request, bool success, const QString &error) {
    if (!m_protocolApplyPending || request != m_protocolRequest) return;
    m_protocolApplyPending = false;
    if (success) {
        m_appliedProtocol = m_pendingProtocol; m_appliedProfile = m_pendingProfile; m_protocolReady = true;
        const auto preset = m_appliedProfile.definition.value("devicePreset").toObject();
        m_devicePresetViewsPending = preset.value("enabled").toBool() && preset.value("separateViews").toBool();
        m_protocolAppliedInterface = m_protocolApplyInterface;
        if (comboInterface->currentData().toInt() == m_protocolAppliedInterface) {
            const QSignalBlocker a(comboProtocol), b(comboEndianness), c(comboCustomProfile);
            comboProtocol->setCurrentIndex(comboProtocol->findData(m_appliedProtocol.kind == "custom" ? QString("custom") : m_appliedProtocol.id));
            comboEndianness->setCurrentIndex(m_appliedProtocol.bigEndian ? 1 : 0);
            if (m_appliedProtocol.kind == "custom") comboCustomProfile->setCurrentIndex(comboCustomProfile->findData(m_appliedProtocol.id));
        }
        if (m_protocolReplacesData) clearProtocolPlots();
        if (m_protocolStore->ready() && m_protocolAppliedInterface != 3) {
            const auto shared = m_appliedProtocol;
            QString saveError;
            if (!m_protocolStore->setSelection(shared, saveError)) protocolMessage(this, "协议已应用，但未保存", saveError + "\n当前采集规则已生效；请处理保存问题，否则重启后不能保持此次选择。");
            else {
                QSettings main("WaveLink", "WaveLink"), advanced("WaveLink", "AdvancedApp");
                main.setValue("comboProtocol", shared.kind == "custom" ? 4 : shared.legacyIndex());
                main.remove("Ble/protocol");
                advanced.setValue("Protocol", shared.kind == "custom" ? 4 : shared.legacyIndex()); advanced.setValue("Endianness", shared.bigEndian ? 1 : 0);
            }
        }
    } else {
        // Keep the old immutable snapshot; the worker likewise retained its
        // old parser. No graph cleanup takes place on a failed application.
        restoreProtocolUi(); protocolMessage(this, "协议应用失败", error);
    }
    comboInterface->setEnabled(!isPortOpen && !m_connectionOperationPending);
    btnOpenClose->setEnabled(!m_connectionOperationPending);
    btnToggleRecord->setEnabled(!m_bleRecordStopPending);
    if (bleClient) updateBleControls(); updateProtocolControls();
    // Also reconcile programmatic interface changes made while a request was
    // pending (configuration reloads/tests), without accepting an old ACK as
    // confirmation for the new interface.
    if (success && comboInterface->currentData().toInt() != m_protocolAppliedInterface) {
        restoreProtocolUi(); requestProtocolApply(); return;
    }
    const auto preset = m_appliedProfile.definition.value("devicePreset").toObject();
    if (success && m_appliedProtocol.kind == "custom" && preset.value("enabled").toBool() && !isPortOpen) {
        spinServerPort->setValue(preset.value("port").toInt(4970));
        if (m_protocolAppliedInterface != 2) comboInterface->setCurrentIndex(comboInterface->findData(2));
    }
}
void MainWindow::clearProtocolPlots() {
    onClearPlots(); resetFilters(); onMergeAllViews();
    for (auto graph : channelGraphs) if (!allPlots.isEmpty() && allPlots[0].plot) allPlots[0].plot->removeGraph(graph);
    channelGraphs.clear(); channelBuffers.clear(); channelToPlotMap.clear(); channelValueLabels.clear(); channelColorButtons.clear(); listChannels->clear();
    menuSpectrumChannels->clear(); if (comboMeasureChannel) comboMeasureChannel->clear();
    timeBuffer.clear(); sampleCount = 0; lastLatestTime = 0;
}
QString MainWindow::channelDisplayName(int channel) const {
    if (m_isImportedData && channel >= 0 && channel < m_importedChannelNames.size()) return m_importedChannelNames[channel];
    if (!m_isImportedData && comboInterface->currentData().toInt() != 3 && m_appliedProtocol.kind == "custom") {
        const auto fields = m_appliedProfile.outputChannels(channel + 1);
        if (channel >= 0 && channel < fields.size()) {
            const auto field = fields[channel].toObject(); const auto unit = field.value("unit").toString();
            return field.value("name").toString() + (unit.isEmpty() ? QString() : QString(" (%1)").arg(unit));
        }
    }
    return QString("通道 %1").arg(channel + 1);
}
QString MainWindow::channelSettingKey(int channel, const QString &property) const {
    if (!m_isImportedData && comboInterface->currentData().toInt() != 3 && m_appliedProtocol.kind == "custom") {
        const auto fields = m_appliedProfile.outputChannels(channel + 1);
        if (channel >= 0 && channel < fields.size()) return QString("CustomChannels/%1/%2/%3").arg(m_appliedProtocol.id, fields[channel].toObject().value("id").toString(), property);
    }
    return QString("Channel%1_%2").arg(property).arg(channel);
}
QString MainWindow::channelUnit(int channel) const {
    if (m_isImportedData || !comboInterface || comboInterface->currentData().toInt() == 3 || m_appliedProtocol.kind != "custom") return {};
    const auto fields = m_appliedProfile.outputChannels(channel + 1);
    return channel >= 0 && channel < fields.size() ? fields[channel].toObject().value("unit").toString() : QString();
}

void MainWindow::showCustomProtocols() {
    if (m_protocolApplyPending) return;
    const auto selectedId = comboCustomProfile ? comboCustomProfile->currentData().toString() : QString();
    ProtocolManagerDialog dialog(DataProtocols::ProtocolProfileStore::defaultPath(), this, selectedId);
    QJsonObject protectedDefinition = m_appliedProfile.definition;
    for (const auto &p : m_protocolStore->snapshot().profiles) if (p.id == m_appliedProtocol.id) { protectedDefinition = p.definition; break; }
    dialog.setMutationGuard([this, protectedDefinition](const ProtocolSnapshot &candidate, QString &error) {
        if (m_configRecoveryPending) { error = "配置导入恢复未完成，不能继续修改协议库。请先处理恢复记录后重启。"; return false; }
        if (m_appliedProtocol.kind != "custom") return true;
        for (const auto &p : candidate.profiles) if (p.id == m_appliedProtocol.id) {
            auto old = protectedDefinition, updated = p.definition; old.remove("revision"); updated.remove("revision");
            if ((isRecording || m_bleRecordStopPending || m_protocolApplyPending) && old != updated) {
                error = "当前正在录制或处理协议切换，不能修改正在使用的方案。可复制方案后修改，或先停止录制。"; return false;
            }
            return true;
        }
        error = "不能删除当前正在使用的协议方案。请先应用其他协议。"; return false;
    });
    dialog.exec();
    QString error;
    if (!m_protocolStore->load(error)) { protocolMessage(this, "协议库重新加载失败", error); return; }
    refreshProtocolChoices();
    if (!m_protocolReady && !m_configRecoveryPending) {
        // A successful recovery must also bootstrap the worker, otherwise
        // a previously failed initial load would leave every connection gated.
        restoreProtocolUi(); requestProtocolApply();
    } else if (comboProtocol->currentData().toString() == "custom") queueProtocolAutoApply();
    updateProtocolControls();
}
