#include "bledialogs.h"
#include "ble/blehostclient.h"
#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QScreen>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QDateTime>
#include <QMap>
#include <QUuid>
#include "ble/bleprofilestore.h"
#include <QInputDialog>
#include <QFileDialog>
#include <QFile>
#include <QSaveFile>
#include <QMessageBox>
#include <QScrollArea>
#include <QScrollBar>
#include <memory>
#include <QSpinBox>
#include <QToolButton>
#include "ble/blesendpolicy.h"
#include "ble/blechannelpolicy.h"

namespace {
void prepare(QDialog &dialog, const QString &title, QSize desired)
{
    dialog.setWindowTitle(title);
    dialog.setWindowFlags(dialog.windowFlags() | Qt::WindowMinMaxButtonsHint);
    dialog.setSizeGripEnabled(true);
    auto screen = QApplication::primaryScreen();
    dialog.resize(screen ? desired.boundedTo(screen->availableGeometry().size() - QSize(60, 100)) : desired);
}
QString uuid(const QString &text) { return QUuid(text).toString(QUuid::WithoutBraces).toLower(); }
bool confirmAction(QWidget *parent, const QString &title, const QString &message)
{
    QDialog dialog(parent); prepare(dialog, title, QSize(660, 380));
    auto layout = new QVBoxLayout(&dialog); auto text = new QPlainTextEdit;
    text->setReadOnly(true); text->setPlainText(message); layout->addWidget(text, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No);
    auto yes = buttons->button(QDialogButtonBox::Yes), no = buttons->button(QDialogButtonBox::No);
    yes->setText(QStringLiteral("确认")); yes->setObjectName("bleConfirmYes"); yes->setAutoDefault(false);
    no->setText(QStringLiteral("取消")); no->setDefault(true); no->setFocus();
    QObject::connect(yes, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(no, &QPushButton::clicked, &dialog, &QDialog::reject);
    layout->addWidget(buttons); return dialog.exec() == QDialog::Accepted;
}
QString properties(int p)
{
    QStringList list;
    if (p & 2) list << "Read";
    if (p & 8) list << "Write";
    if (p & 4) list << "WriteWithoutResponse";
    if (p & 16) list << "Notify";
    if (p & 32) list << "Indicate";
    return list.join(" | ");
}
QString characteristicLabel(const QJsonObject &c)
{
    return QStringLiteral("%1 [#%2] — 服务 %3").arg(c.value("uuid").toString()).arg(c.value("handle").toInt()).arg(c.value("service").toString());
}
QString shortCharacteristicLabel(const QJsonObject &c)
{
    auto compact = [](const QString &value) {
        const auto s = BleProfileStore::normalizeUuid(value);
        return s.endsWith("-0000-1000-8000-00805f9b34fb") ? s.mid(s.startsWith("0000") ? 4 : 0, s.startsWith("0000") ? 4 : 8).toUpper() : s;
    };
    return (BleChannelPolicy::firmwareService(c.value("service").toString()) ? QStringLiteral("【固件升级】 ") : QString()) +
        QStringLiteral("%1 · 服务 %2 · %3").arg(compact(c.value("uuid").toString()), compact(c.value("service").toString()), properties(c.value("properties").toInt()));
}
QString profileText(const QJsonObject &profile)
{
    if (profile.isEmpty()) return QStringLiteral("尚未保存收发配置");
    auto direction = [&](const QString &prefix) {
        return profile.value(prefix + "Uuid").toString().isEmpty() ? QStringLiteral("未使用") :
            profile.value(prefix + "Service").toString() + " / " + profile.value(prefix + "Uuid").toString();
    };
    return QStringLiteral("电脑接收：%1\n电脑发送：%2\n写入模式：%3\n设置分包：%5 字节 / %6 ms；透传确认：%4")
        .arg(direction("rx"), direction("tx"), profile.value("withoutResponse").toBool() ? QStringLiteral("无响应（提交不等于业务确认）") : QStringLiteral("有响应"),
             profile.value("stream").toBool() ? QStringLiteral("是") : QStringLiteral("否"))
        .arg(profile.value("packetSize").toInt(20)).arg(profile.value("intervalMs").toInt(10));
}
struct SendControls {
    QWidget *widget; QSpinBox *packet, *interval; QLabel *summary;
    void fill(const QJsonObject &profile) const { packet->setValue(profile.value("packetSize").toInt(20)); interval->setValue(profile.value("intervalMs").toInt(10)); }
    void insert(QJsonObject &profile) const { profile["packetSize"] = packet->value(); profile["intervalMs"] = interval->value(); }
};
SendControls sendControls(QWidget *owner, BleHostClient *client)
{
    auto widget = new QWidget(owner); auto layout = new QVBoxLayout(widget); layout->setContentsMargins(0, 0, 0, 0);
    auto toggle = new QToolButton; toggle->setObjectName("bleAdvancedToggle"); toggle->setText(QStringLiteral("高级发送控制（默认 20 字节 / 10 ms）"));
    toggle->setCheckable(true); toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); toggle->setArrowType(Qt::RightArrow); layout->addWidget(toggle);
    auto content = new QWidget; auto form = new QFormLayout(content); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto packet = new QSpinBox; packet->setObjectName("blePacketSize"); packet->setRange(1, 512); packet->setValue(20); packet->setSuffix(QStringLiteral(" 字节")); packet->setKeyboardTracking(false);
    auto interval = new QSpinBox; interval->setObjectName("bleSendInterval"); interval->setRange(0, 1000); interval->setValue(10); interval->setSuffix(" ms"); interval->setKeyboardTracking(false);
    form->addRow(QStringLiteral("设置分包大小"), packet); form->addRow(QStringLiteral("每包完成后的最小间隔"), interval);
    auto reset = new QPushButton(QStringLiteral("恢复兼容设置：20 字节 / 10 ms")); reset->setObjectName("bleResetSendSettings"); form->addRow(reset);
    auto note = new QLabel(QStringLiteral("参数只在发送空闲时应用。0 ms 仍逐包等待 GATT 完成，不并发发送。Windows 协商 MTU 不是模块缓冲容量；较大分包或较短间隔需要确认风险。无响应写入完成只表示提交，不能确认外设收到。"));
    note->setWordWrap(true); form->addRow(note); layout->addWidget(content); content->hide();
    auto summary = new QLabel; summary->setObjectName("bleEffectiveSendSettings"); summary->setWordWrap(true); summary->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(summary);
    auto refresh = [client, packet, interval, summary] {
        const auto s = client ? client->sendSettings() : QJsonObject{};
        const bool ready = client && client->state() == BleHostClient::State::Ready;
        const int mtu = ready ? s.value("mtu").toInt() : 0;
        const int preview = BleSendPolicy::effective(packet->value(), mtu);
        QString text = QStringLiteral("设置：%1 字节 / %2 ms；%3\n预期分包：%4 字节（%5）")
            .arg(packet->value()).arg(interval->value())
            .arg(mtu ? QStringLiteral("Windows 协商 MTU：%1").arg(mtu) : QStringLiteral("MTU 未知，保守上限 20 字节"))
            .arg(preview).arg(packet->value() > preview ? (mtu ? QStringLiteral("受 MTU−3 / 后端 512 字节上限限制") : QStringLiteral("能力未知，不自动放大")) : QStringLiteral("设置值未超过已知上限"));
        if (ready) text += QStringLiteral("\n当前实际生效：%1 字节 / %2 ms%3%4。能力增大不会自动提速，需手动应用。")
            .arg(s.value("effectivePacket").toInt(20)).arg(s.value("intervalMs").toInt(10))
            .arg(s.value("blocked").toBool() ? QStringLiteral("（发送暂停，需重新应用参数）") : QString())
            .arg(s.value("pending").toBool() ? QStringLiteral("（等待参数确认）") : QString());
        else text += QStringLiteral("\n未应用到连接；连接后以实际生效值为准。");
        if (client && !client->supportsSendControl() && client->state() != BleHostClient::State::Idle)
            text += QStringLiteral("\n旧辅助程序只支持 20 字节 / 10 ms，请更新完整软件包后使用高级设置。");
        summary->setText(text);
    };
    QObject::connect(toggle, &QToolButton::toggled, widget, [content, toggle](bool checked) { content->setVisible(checked); toggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow); });
    QObject::connect(reset, &QPushButton::clicked, widget, [packet, interval] { packet->setValue(20); interval->setValue(10); });
    QObject::connect(packet, QOverload<int>::of(&QSpinBox::valueChanged), widget, refresh);
    QObject::connect(interval, QOverload<int>::of(&QSpinBox::valueChanged), widget, refresh);
    if (client) {
        QObject::connect(client, &BleHostClient::sendSettingsChanged, widget, refresh);
        QObject::connect(client, &BleHostClient::stateChanged, widget, refresh);
    }
    refresh(); return {widget, packet, interval, summary};
}
bool confirmSendSettings(QWidget *parent, int packet, int interval)
{
    return !BleSendPolicy::needsConfirmation(packet, interval) || confirmAction(parent, QStringLiteral("确认高级发送参数？"),
        QStringLiteral("设置分包：%1 字节；间隔：%2 ms。\n\n这些参数比默认兼容设置更激进。软件只能检查 Windows 的普通写入上限，不能确认模块串口缓冲容量或外设应用处理速度。即使 GATT 提交成功，也可能发生外设丢包。\n\n请确认外设支持透传并能承受该速率。建议先用连续序号数据测试；软件不会对失败的数据自动重发。\n能力未知时仍限制为 20 字节；0 ms 仍保持单包等待。\n\n确认只对本次应用有效，不随模板导入或保存自动授权。")
            .arg(packet).arg(interval));
}
QWidget *templateTools(QWidget *owner, const std::function<QJsonObject()> &current,
                       const std::function<void(const QJsonObject &)> &apply, bool editable)
{
    auto widget = new QWidget(owner); auto layout = new QVBoxLayout(widget); layout->setContentsMargins(0, 0, 0, 0);
    auto combo = new QComboBox; combo->setObjectName("bleTemplateChoice"); layout->addWidget(combo);
    auto row = new QHBoxLayout; auto save = new QPushButton(QStringLiteral("保存为模板…"));
    auto import = new QPushButton(QStringLiteral("导入模板…")); auto exportButton = new QPushButton(QStringLiteral("导出当前…"));
    row->addWidget(save); row->addWidget(import); row->addWidget(exportButton); layout->addLayout(row);
    auto settings = std::make_shared<QSettings>("WaveLink", "WaveLink");
    auto reload = [combo, settings] {
        combo->clear(); combo->addItem(QStringLiteral("选择模板（仅预选，仍需核对）"));
        combo->addItem(QStringLiteral("通用手动配置（清空预选）"), QJsonObject());
        combo->addItem(QStringLiteral("Nordic UART Service（结构匹配候选）"), BleProfileStore::nus());
        QString error; const auto templates = BleProfileStore(*settings).templates(error);
        for (const auto &item : templates) { const auto obj = item.toObject(); combo->addItem(obj.value("name").toString(), obj.value("profile").toObject()); }
        combo->setToolTip(error.isEmpty() ? QStringLiteral("不按设备名称猜测通道；每次应用都核对实际 GATT。") : error);
    };
    reload(); combo->setEnabled(editable); save->setEnabled(editable); import->setEnabled(editable);
    QObject::connect(combo, QOverload<int>::of(&QComboBox::activated), widget, [combo, apply](int index) {
        if (index > 0) apply(combo->itemData(index).toJsonObject());
    });
    auto store = [owner, settings, reload](const QString &name, const QJsonObject &profile) {
        QString error; BleProfileStore storage(*settings);
        const auto list = storage.templates(error);
        if (!error.isEmpty()) { QMessageBox::warning(owner, QStringLiteral("模板未保存"), error); return; }
        bool duplicate = false;
        for (const auto &v : list) if (v.toObject().value("name").toString().compare(name, Qt::CaseInsensitive) == 0) duplicate = true;
        if (duplicate && !confirmAction(owner, QStringLiteral("覆盖模板？"), QStringLiteral("已有同名模板“%1”。是否覆盖？").arg(name))) return;
        if (!storage.saveTemplate(name, profile, duplicate, error)) QMessageBox::warning(owner, QStringLiteral("模板未保存"), error);
        else reload();
    };
    QObject::connect(save, &QPushButton::clicked, widget, [owner, current, store] {
        QJsonObject normalized; QString error;
        if (!BleProfileStore::normalize(current(), normalized, error)) { QMessageBox::warning(owner, QStringLiteral("配置无效"), error); return; }
        bool ok; const QString name = QInputDialog::getText(owner, QStringLiteral("保存 BLE 模板"), QStringLiteral("模板名称（不包含设备身份）："), QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok) store(name, normalized);
    });
    QObject::connect(import, &QPushButton::clicked, widget, [owner, store] {
        const auto path = QFileDialog::getOpenFileName(owner, QStringLiteral("导入 BLE 模板"), QString(), "BLE template (*.json)"); if (path.isEmpty()) return;
        QFile file(path); QString error, name; QJsonObject profile;
        if (!file.open(QIODevice::ReadOnly)) error = file.errorString();
        else { const auto bytes = file.read(65537); if (file.error() != QFileDevice::NoError) error = file.errorString(); else BleProfileStore::parseTemplate(bytes, name, profile, error); }
        if (!error.isEmpty()) { QMessageBox::warning(owner, QStringLiteral("模板未导入"), error); return; }
        if (confirmAction(owner, QStringLiteral("确认导入模板"), name + "\n\n" + profileText(profile) + QStringLiteral("\n\n仅保存模板，不会连接或发送数据。"))) store(name, profile);
    });
    QObject::connect(exportButton, &QPushButton::clicked, widget, [owner, current, combo] {
        const QString name = combo->currentIndex() >= 3 ? combo->currentText() : QStringLiteral("自定义透传");
        QString error; const auto bytes = BleProfileStore::exportTemplate(name, current(), error);
        if (!error.isEmpty()) { QMessageBox::warning(owner, QStringLiteral("无法导出模板"), error); return; }
        const auto path = QFileDialog::getSaveFileName(owner, QStringLiteral("导出 BLE 模板"), "ble-template.json", "BLE template (*.json)"); if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) QMessageBox::warning(owner, QStringLiteral("模板导出失败"), file.errorString());
    });
    return widget;
}
}
QJsonObject BleDialogs::selectDevice(BleHostClient *client, QWidget *parent)
{
    QDialog dialog(parent); prepare(dialog, QStringLiteral("选择 BLE 设备"), QSize(820, 520));
    auto layout = new QVBoxLayout(&dialog);
    auto toolbar = new QHBoxLayout;
    auto search = new QLineEdit; search->setPlaceholderText(QStringLiteral("按名称或地址搜索"));
    auto scan = new QPushButton(QStringLiteral("重新扫描")); auto stop = new QPushButton(QStringLiteral("停止扫描"));
    toolbar->addWidget(search, 1); toolbar->addWidget(scan); toolbar->addWidget(stop); layout->addLayout(toolbar);
    auto info = new QLabel(QStringLiteral("扫描约 10 秒；模块需处于可连接的 BLE 外设模式。"));
    info->setWordWrap(true); layout->addWidget(info);
    auto table = new QTableWidget(0, 5);
    table->setHorizontalHeaderLabels({QStringLiteral("设备名称"), QStringLiteral("蓝牙地址"), QStringLiteral("RSSI"), QStringLiteral("可连接"), QStringLiteral("最近发现")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows); table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(table, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto choose = buttons->addButton(QStringLiteral("选择设备"), QDialogButtonBox::AcceptRole); choose->setEnabled(false);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消")); layout->addWidget(buttons);
    QMap<QString, QJsonObject> devices; QMap<QString, int> rows;
    auto filter = [&] {
        for (int i = 0; i < table->rowCount(); ++i)
            table->setRowHidden(i, !(table->item(i, 0)->text() + table->item(i, 1)->text()).contains(search->text(), Qt::CaseInsensitive));
        choose->setEnabled(table->currentRow() >= 0 && !table->isRowHidden(table->currentRow()));
    };
    QObject::connect(search, &QLineEdit::textChanged, &dialog, filter);
    QObject::connect(client, &BleHostClient::deviceFound, &dialog, [&](const QJsonObject &device) {
        const QString key = device.value("address").toString() + "/" + QString::number(device.value("addressType").toInt());
        auto updated = device;
        if (updated.value("name").toString().isEmpty()) updated.insert("name", devices.value(key).value("name"));
        // Scan-response packets may have no connectable flag of their own.
        updated.insert("connectable", device.value("connectable").toBool() || devices.value(key).value("connectable").toBool());
        devices[key] = updated;
        int row;
        if (!rows.contains(key)) {
            if (table->rowCount() >= 1000) return; // Bound crowded-environment scan memory.
            row = table->rowCount(); rows[key] = row; table->insertRow(row);
            for (int col = 0; col < 5; ++col) table->setItem(row, col, new QTableWidgetItem);
            table->item(row, 0)->setData(Qt::UserRole, key);
        } else row = rows.value(key);
        auto name = updated.value("name").toString(); if (name.isEmpty()) name = QStringLiteral("未命名设备");
        table->item(row, 0)->setText(name);
        bool ok; auto address = device.value("address").toString().toULongLong(&ok);
        table->item(row, 1)->setText(ok ? QString::number(address, 16).rightJustified(12, '0').toUpper() : "—");
        table->item(row, 2)->setText(QStringLiteral("%1 dBm").arg(device.value("rssi").toInt()));
        table->item(row, 3)->setText(updated.value("connectable").toBool() ? QStringLiteral("是") : QStringLiteral("未确认"));
        table->item(row, 4)->setText(QTime::currentTime().toString("HH:mm:ss")); filter();
    });
    QObject::connect(client, &BleHostClient::stateChanged, &dialog, [&](BleHostClient::State state) {
        stop->setEnabled(state == BleHostClient::State::Scanning);
        info->setText(state == BleHostClient::State::Scanning ? QStringLiteral("正在扫描附近 BLE 设备…（约 10 秒）") : QStringLiteral("扫描已停止。可选择设备或重新扫描。"));
    });
    QObject::connect(client, &BleHostClient::errorOccurred, &dialog, [&](const QString &message, bool) { info->setText(message); });
    QObject::connect(table, &QTableWidget::itemSelectionChanged, &dialog, filter);
    QObject::connect(scan, &QPushButton::clicked, client, &BleHostClient::scan);
    QObject::connect(stop, &QPushButton::clicked, client, &BleHostClient::stopScan);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(table, &QTableWidget::cellDoubleClicked, &dialog, [&](int, int) { if (choose->isEnabled()) dialog.accept(); });
    QTimer::singleShot(0, &dialog, [client] { client->scan(); });
    const bool accepted = dialog.exec() == QDialog::Accepted;
    client->stopScan();
    if (!accepted || table->currentRow() < 0) return {};
    return devices.value(table->item(table->currentRow(), 0)->data(Qt::UserRole).toString());
}

bool BleDialogs::configure(BleHostClient *client, const QJsonArray &entries,
                          const QJsonObject &savedProfile, QJsonObject &profile, QWidget *parent, const QString &deviceName)
{
    QDialog dialog(parent); prepare(dialog, QStringLiteral("BLE 快速收发配置"), QSize(920, 560));
    auto outer = new QVBoxLayout(&dialog);
    auto heading = new QLabel(deviceName.isEmpty() ? QStringLiteral("选择 BLE 收发通道") : deviceName);
    heading->setObjectName("bleDeviceHeading"); heading->setTextFormat(Qt::PlainText);
    auto headingFont = heading->font(); headingFont.setPointSize(headingFont.pointSize() + 3); headingFont.setBold(true); heading->setFont(headingFont);
    heading->setWordWrap(true); outer->addWidget(heading);
    auto scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setObjectName("bleGattScroll");
    auto content = new QWidget; auto layout = new QVBoxLayout(content);
    layout->setContentsMargins(8, 8, 8, 8); layout->setSizeConstraint(QLayout::SetMinAndMaxSize); outer->addWidget(scroll, 1);
    auto info = new QLabel(QStringLiteral("选择接收/发送通道即可开始；也可以只接收。候选仅依据属性，不会发送探测命令。"));
    info->setWordWrap(true); layout->addWidget(info);
    auto candidateRow = new QHBoxLayout;
    auto candidate = new QComboBox; candidate->setObjectName("bleCandidateChoice"); candidate->setMinimumWidth(0);
    candidate->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); candidate->setMinimumContentsLength(16);
    const auto recommendations = BleChannelPolicy::candidates(entries);
    for (const auto &c : recommendations) candidate->addItem(c.name);
    if (recommendations.isEmpty()) candidate->addItem(QStringLiteral("未找到明确候选，请手动选择收发通道"));
    auto useCandidate = new QPushButton(QStringLiteral("使用候选通道")); useCandidate->setObjectName("bleUseCandidate");
    auto receiveOnly = new QPushButton(QStringLiteral("仅接收候选通道")); receiveOnly->setObjectName("bleReceiveOnlyCandidate");
    useCandidate->setToolTip(QStringLiteral("只预选下方通道，不会连接应用或发送数据；请核对外设的通道用途。"));
    receiveOnly->setToolTip(QStringLiteral("预选候选接收通道并关闭发送；点击应用后订阅通知，不发送业务命令。"));
    candidateRow->addWidget(candidate, 1); candidateRow->addWidget(useCandidate); candidateRow->addWidget(receiveOnly); layout->addLayout(candidateRow);
    auto advancedToggle = new QToolButton; advancedToggle->setObjectName("bleGattAdvancedToggle");
    advancedToggle->setText(QStringLiteral("高级配置 · 服务详情、模板与发送参数")); advancedToggle->setCheckable(true);
    advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); advancedToggle->setArrowType(Qt::RightArrow);
    auto advanced = new QWidget; advanced->setObjectName("bleGattAdvanced"); auto advancedLayout = new QVBoxLayout(advanced);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    auto tree = new QTreeWidget; tree->setHeaderLabels({QStringLiteral("服务 / 特征 UUID（右键可复制）"), QStringLiteral("属性")});
    tree->setObjectName("bleGattTree"); tree->setMinimumHeight(180); tree->setMaximumHeight(300);
    tree->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto copy = new QAction(QStringLiteral("复制 UUID"), tree); tree->addAction(copy);
    QObject::connect(copy, &QAction::triggered, &dialog, [tree] { if (tree->currentItem()) QApplication::clipboard()->setText(tree->currentItem()->data(0, Qt::UserRole + 1).toString()); });
    advancedLayout->addWidget(tree);
    auto form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setVerticalSpacing(10); form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto rx = new QComboBox; auto tx = new QComboBox; auto mode = new QComboBox;
    rx->setObjectName("bleReceiveCharacteristic"); tx->setObjectName("bleTransmitCharacteristic"); mode->setObjectName("bleWriteMode");
    rx->addItem(QStringLiteral("不接收"), 0); tx->addItem(QStringLiteral("不发送"), 0);
    for (auto combo : {rx, tx}) { combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); combo->setMinimumContentsLength(16); }
    mode->addItem(QStringLiteral("有响应写入（优先）"), false); mode->addItem(QStringLiteral("无响应写入（已提交不等于设备确认）"), true);
    QMap<int, QJsonObject> byHandle; QMap<int, QTreeWidgetItem *> serviceNodes;
    for (const auto &value : entries) {
        auto c = value.toObject(); const int serviceHandle = c.value("serviceHandle").toInt();
        if (!serviceNodes.contains(serviceHandle)) {
            auto item = new QTreeWidgetItem(tree, {c.value("service").toString(), QString()});
            item->setData(0, Qt::UserRole + 1, c.value("service").toString()); serviceNodes[serviceHandle] = item;
        }
        const int handle = c.value("handle").toInt(), p = c.value("properties").toInt();
        if (!handle) { serviceNodes[serviceHandle]->setText(1, QStringLiteral("无法访问，请检查配对/权限")); continue; }
        byHandle[handle] = c;
        auto item = new QTreeWidgetItem(serviceNodes[serviceHandle], {QStringLiteral("%1 [#%2]").arg(c.value("uuid").toString()).arg(handle), properties(p)});
        item->setData(0, Qt::UserRole, handle); item->setData(0, Qt::UserRole + 1, c.value("uuid").toString());
        if (BleChannelPolicy::firmwareService(c.value("service").toString())) item->setText(1, QStringLiteral("【固件升级，勿作普通透传】 ") + properties(p));
        if (p & (16 | 32)) { rx->addItem(shortCharacteristicLabel(c), handle); rx->setItemData(rx->count() - 1, characteristicLabel(c), Qt::ToolTipRole); }
        if (p & (4 | 8)) { tx->addItem(shortCharacteristicLabel(c), handle); tx->setItemData(tx->count() - 1, characteristicLabel(c), Qt::ToolTipRole); }
    }
    tree->expandAll(); tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    auto useRx = new QAction(QStringLiteral("设为电脑接收通道"), tree), useTx = new QAction(QStringLiteral("设为电脑发送通道"), tree);
    tree->insertAction(copy, useRx); tree->insertAction(copy, useTx);
    QObject::connect(useRx, &QAction::triggered, &dialog, [=] {
        if (tree->currentItem()) { const int index = rx->findData(tree->currentItem()->data(0, Qt::UserRole).toInt()); if (index > 0) rx->setCurrentIndex(index); }
    });
    QObject::connect(useTx, &QAction::triggered, &dialog, [=] {
        if (tree->currentItem()) { const int index = tx->findData(tree->currentItem()->data(0, Qt::UserRole).toInt()); if (index > 0) tx->setCurrentIndex(index); }
    });
    form->addRow(QStringLiteral("电脑接收（设备 → PC）"), rx); form->addRow(QStringLiteral("电脑发送（PC → 设备）"), tx);
    layout->addLayout(form);
    auto modeHint = new QLabel; modeHint->setObjectName("bleAutomaticWriteMode"); modeHint->setWordWrap(true); layout->addWidget(modeHint);
    auto stream = new QCheckBox(QStringLiteral("确认所选通道用于透传字节流：发送时按实际生效大小分包")); layout->addWidget(stream);
    stream->setObjectName("bleStreamConfirmation");
    layout->addWidget(advancedToggle); layout->addWidget(advanced); advanced->hide();
    QObject::connect(advancedToggle, &QToolButton::toggled, &dialog, [advanced, advancedToggle](bool checked) {
        advanced->setVisible(checked); advancedToggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
    });
    auto modeForm = new QFormLayout; modeForm->addRow(QStringLiteral("写入模式（自动适配，可调整）"), mode); advancedLayout->addLayout(modeForm);
    const auto sending = sendControls(&dialog, client); advancedLayout->addWidget(sending.widget);
    auto read = new QPushButton(QStringLiteral("读取选中特征")); advancedLayout->addWidget(read);
    auto readValue = new QPlainTextEdit; readValue->setReadOnly(true); readValue->setMaximumHeight(90); advancedLayout->addWidget(readValue); readValue->hide();
    layout->addStretch();
    auto reason = new QLabel; reason->setObjectName("bleApplyReason"); reason->setWordWrap(true); outer->addWidget(reason);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消并断开"));
    auto apply = buttons->addButton(QStringLiteral("应用并开始调试"), QDialogButtonBox::AcceptRole); outer->addWidget(buttons);
    apply->setObjectName("bleApplyConfiguration"); apply->setMinimumHeight(36);
    apply->setStyleSheet("QPushButton { background:#147dad; color:white; padding:6px 20px; border-radius:4px; } QPushButton:disabled { background:#8799a1; color:#f0f0f0; } QPushButton:hover:enabled { background:#1996cb; }");
    auto restoreSelection = [&](QComboBox *combo, const QString &service, const QString &characteristic) {
        if (service.isEmpty() || characteristic.isEmpty()) return;
        int match = -1, matches = 0;
        for (int i = 1; i < combo->count(); ++i) {
            auto c = byHandle.value(combo->itemData(i).toInt());
            if (uuid(c.value("service").toString()) == uuid(service) && uuid(c.value("uuid").toString()) == uuid(characteristic)) { match = i; ++matches; }
        }
        if (matches == 1) combo->setCurrentIndex(match);
    };
    auto selectedProfile = [&] {
        const auto receive = byHandle.value(rx->currentData().toInt()), transmit = byHandle.value(tx->currentData().toInt());
        QJsonObject p{{"schemaVersion", 3}, {"rxService", receive.value("service").toString()}, {"rxUuid", receive.value("uuid").toString()},
                          {"txService", transmit.value("service").toString()}, {"txUuid", transmit.value("uuid").toString()}, {"withoutResponse", mode->currentData().toBool()}, {"stream", stream->isChecked()}};
        sending.insert(p); return p;
    };
    auto templates = templateTools(&dialog, selectedProfile, [&](const QJsonObject &p) {
        rx->setCurrentIndex(0); tx->setCurrentIndex(0);
        restoreSelection(rx, p.value("rxService").toString(), p.value("rxUuid").toString());
        restoreSelection(tx, p.value("txService").toString(), p.value("txUuid").toString());
        mode->setCurrentIndex(p.value("withoutResponse").toBool() ? 1 : 0); stream->setChecked(false);
        sending.fill(p);
        info->setText(QStringLiteral("模板只预选唯一匹配的服务/特征；未匹配或重复 UUID 的方向保持未选。请核对属性，并重新确认透传用途。"));
    }, true);
    advancedLayout->insertWidget(0, templates);
    if (!savedProfile.isEmpty()) {
        restoreSelection(rx, savedProfile.value("rxService").toString(), savedProfile.value("rxUuid").toString());
        restoreSelection(tx, savedProfile.value("txService").toString(), savedProfile.value("txUuid").toString());
        mode->setCurrentIndex(savedProfile.value("withoutResponse").toBool() ? 1 : 0);
        // A manual dialog means legacy/unverified/changed GATT or a parameter
        // requiring renewed consent. Do not carry send consent into it.
        stream->setChecked(false);
        sending.fill(savedProfile);
    } else {
        // Only a structural NUS match is prefilled. Never choose an arbitrary
        // write characteristic, or identify a module solely from its name.
        const QString service = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
        restoreSelection(rx, service, "6e400003-b5a3-f393-e0a9-e50e24dcca9e");
        restoreSelection(tx, service, "6e400002-b5a3-f393-e0a9-e50e24dcca9e");
    }
    auto selectCandidate = [&](bool onlyReceive) {
        const int index = candidate->currentIndex(); if (index < 0 || index >= recommendations.size()) return;
        const auto c = recommendations[index];
        rx->setCurrentIndex(qMax(0, rx->findData(c.rx))); tx->setCurrentIndex(onlyReceive ? 0 : qMax(0, tx->findData(c.tx)));
        stream->setChecked(false);
        info->setText(onlyReceive ? QStringLiteral("已预选仅接收通道。应用后订阅通知，不会发送业务命令；请观察数据是否来自目标通道。") :
            QStringLiteral("已预选属性匹配候选；不保证是透传通道，请核对设备说明并确认发送用途。"));
    };
    QObject::connect(useCandidate, &QPushButton::clicked, &dialog, [&] { selectCandidate(false); });
    QObject::connect(receiveOnly, &QPushButton::clicked, &dialog, [&] { selectCandidate(true); });
    auto update = [&] {
        const bool waiting = client->state() == BleHostClient::State::AwaitingConfiguration && !client->readPending();
        templates->setEnabled(waiting);
        const auto selected = tree->currentItem();
        read->setEnabled(waiting && selected && (byHandle.value(selected->data(0, Qt::UserRole).toInt()).value("properties").toInt() & 2));
        useRx->setEnabled(waiting && selected && rx->findData(selected->data(0, Qt::UserRole).toInt()) > 0);
        useTx->setEnabled(waiting && selected && tx->findData(selected->data(0, Qt::UserRole).toInt()) > 0);
        const int p = byHandle.value(tx->currentData().toInt()).value("properties").toInt();
        const bool writeValid = tx->currentData().toInt() == 0 || (p & (mode->currentData().toBool() ? 4 : 8));
        const bool supported = client->supportsSendControl() || (sending.packet->value() == 20 && sending.interval->value() == 10);
        QString why;
        if (!waiting) why = client->readPending() ? QStringLiteral("正在读取特征，请稍候…") : QStringLiteral("正在配置连接并确认通知订阅，请稍候…");
        else if (!rx->currentData().toInt() && !tx->currentData().toInt()) why = recommendations.isEmpty() ? QStringLiteral("请选择至少一个接收或发送通道；请按设备说明核对实际用途。") : QStringLiteral("请选择至少一个接收或发送通道；可以先点击“仅接收候选通道”。");
        else if (!supported) why = QStringLiteral("当前辅助程序仅支持 20 字节 / 10 ms，请在高级配置中恢复兼容设置。");
        else if (!writeValid) why = QStringLiteral("所选发送特征不支持此写入模式，请在高级配置中调整。");
        else if (tx->currentData().toInt() && !stream->isChecked()) why = QStringLiteral("请勾选透传用途确认；若只需要接收，请将发送通道设为“不发送”。");
        apply->setEnabled(waiting && why.isEmpty());
        reason->setText(why.isEmpty() ? QStringLiteral("配置就绪。成功后会记住此设备，服务未变化且参数无需风险确认时可快捷连接；不会自动发送业务命令。") : why);
        reason->setStyleSheet(waiting && !why.isEmpty() ? "color:#d04b36;" : QString());
        apply->setToolTip(reason->text());
        const bool firmware = BleChannelPolicy::firmwareService(byHandle.value(rx->currentData().toInt()).value("service").toString()) || BleChannelPolicy::firmwareService(byHandle.value(tx->currentData().toInt()).value("service").toString());
        if (firmware) { reason->setText(QStringLiteral("警告：已选择固件升级服务，不建议用于普通收发；建议改选数据服务。") + "\n" + reason->text()); reason->setStyleSheet("color:#d04b36;"); }
        modeHint->setText(tx->currentData().toInt() ? QStringLiteral("发送方式：%1（按特征自动选择，可在高级配置中调整）").arg(mode->currentText()) : QStringLiteral("当前不启用发送；只接收时无需透传发送确认。"));
        stream->setVisible(tx->currentData().toInt() != 0);
        useCandidate->setEnabled(waiting && !recommendations.isEmpty()); receiveOnly->setEnabled(waiting && !recommendations.isEmpty()); candidate->setEnabled(waiting && !recommendations.isEmpty());
        for (auto combo : {rx, tx, mode}) combo->setEnabled(waiting);
        stream->setEnabled(waiting);
        sending.widget->setEnabled(waiting);
        rx->setToolTip(characteristicLabel(byHandle.value(rx->currentData().toInt())));
        tx->setToolTip(characteristicLabel(byHandle.value(tx->currentData().toInt())));
    };
    QObject::connect(tree, &QTreeWidget::itemSelectionChanged, &dialog, update);
    QObject::connect(tx, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&] {
        stream->setChecked(false); // Confirmation belongs to the selected send channel.
        const int p = byHandle.value(tx->currentData().toInt()).value("properties").toInt();
        if (p & 8) mode->setCurrentIndex(0);
        else if (p & 4) mode->setCurrentIndex(1);
    });
    for (auto combo : {rx, tx, mode}) QObject::connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, update);
    QObject::connect(stream, &QCheckBox::toggled, &dialog, update);
    QObject::connect(sending.packet, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, update);
    QObject::connect(sending.interval, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, update);
    QObject::connect(read, &QPushButton::clicked, &dialog, [&] { readValue->show(); if (tree->currentItem()) client->readCharacteristic(tree->currentItem()->data(0, Qt::UserRole).toInt()); update(); });
    QObject::connect(client, &BleHostClient::characteristicRead, &dialog, [&](int handle, const QByteArray &data) { readValue->setPlainText(QStringLiteral("特征 #%1\nHEX: %2\nUTF-8: %3").arg(handle).arg(QString::fromLatin1(data.toHex(' ')), QString::fromUtf8(data))); });
    QObject::connect(client, &BleHostClient::operationFinished, &dialog, update);
    QObject::connect(client, &BleHostClient::stateChanged, &dialog, update);
    QObject::connect(client, &BleHostClient::errorOccurred, &dialog, [&](const QString &message, bool fatal) { advancedToggle->setChecked(true); readValue->show(); readValue->setPlainText(message); if (fatal) dialog.reject(); else update(); });
    QObject::connect(client, &BleHostClient::ready, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(apply, &QPushButton::clicked, &dialog, [&] {
        if (tx->currentData().toInt() && !confirmSendSettings(&dialog, sending.packet->value(), sending.interval->value())) return;
        if (client->configure(rx->currentData().toInt(), tx->currentData().toInt(), mode->currentData().toBool(), sending.packet->value(), sending.interval->value(), true))
            info->setText(QStringLiteral("正在配置收发通道并等待订阅确认…"));
        update();
    });
    // Install the completed content, so Qt 5 accounts for every nested form
    // row when calculating scroll extents and showing child controls.
    scroll->setWidget(content);
    const int initialProperties = byHandle.value(tx->currentData().toInt()).value("properties").toInt();
    if (tx->currentData().toInt() && !(initialProperties & (mode->currentData().toBool() ? 4 : 8)))
        mode->setCurrentIndex((initialProperties & 8) ? 0 : 1);
    update();
    if (dialog.exec() != QDialog::Accepted || client->state() != BleHostClient::State::Ready) return false;
    QString error; if (!BleProfileStore::normalize(selectedProfile(), profile, error)) return false;
    return true;
}
void BleDialogs::showError(const QString &message, const QString &device, QWidget *parent)
{
    QDialog dialog(parent); prepare(dialog, QStringLiteral("BLE 通信异常"), QSize(720, 460));
    auto layout = new QVBoxLayout(&dialog); auto text = new QPlainTextEdit;
    text->setReadOnly(true);
    text->setPlainText(QStringLiteral("WaveLink BLE 诊断信息\n时间：%1\n设备：%2\n\n%3\n\n当前 BLE 会话和待发送队列已停止。\n请检查设备供电、距离、系统蓝牙开关、配对和特征权限，然后重新连接。")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"), device, message));
    layout->addWidget(text, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    auto copy = buttons->addButton(QStringLiteral("复制全部信息"), QDialogButtonBox::ActionRole);
    QObject::connect(copy, &QPushButton::clicked, &dialog, [text] { QApplication::clipboard()->setText(text->toPlainText()); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons); dialog.exec();
}
void BleDialogs::showSendStopped(const QString &message, QWidget *parent)
{
    QDialog dialog(parent); prepare(dialog, QStringLiteral("BLE 发送已安全停止"), QSize(720, 420));
    auto layout = new QVBoxLayout(&dialog); auto text = new QPlainTextEdit;
    text->setReadOnly(true); text->setPlainText(message); layout->addWidget(text, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close); buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    auto copy = buttons->addButton(QStringLiteral("复制全部信息"), QDialogButtonBox::ActionRole);
    QObject::connect(copy, &QPushButton::clicked, &dialog, [text] { QApplication::clipboard()->setText(text->toPlainText()); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons); dialog.exec();
}
void BleDialogs::showFileError(const QString &message, QWidget *parent)
{
    QDialog dialog(parent); prepare(dialog, QStringLiteral("BLE 文件发送已停止"), QSize(720, 420));
    auto layout = new QVBoxLayout(&dialog); auto text = new QPlainTextEdit;
    text->setReadOnly(true);
    text->setPlainText(QStringLiteral("发送源文件读取异常\n时间：%1\n\n%2\n\n本次文件发送已停止，未提交的数据不会自动重发。\n文件读取故障不会主动断开仍健康的 BLE 连接或停止录制。\n请检查源文件、访问权限、磁盘和网络路径后重试。")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz"), message));
    layout->addWidget(text, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    auto copy = buttons->addButton(QStringLiteral("复制全部信息"), QDialogButtonBox::ActionRole);
    QObject::connect(copy, &QPushButton::clicked, &dialog, [text] { QApplication::clipboard()->setText(text->toPlainText()); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons); dialog.exec();
}
BleDialogs::ProfileAction BleDialogs::manageProfiles(const QJsonObject &active, const QJsonObject &next,
                                                     const QJsonArray &entries, bool ready, bool editable, QWidget *parent, BleHostClient *client)
{
    QDialog dialog(parent); prepare(dialog, QStringLiteral("BLE 收发配置与模板"), QSize(850, 680));
    auto layout = new QVBoxLayout(&dialog);
    auto scroll = new QScrollArea; scroll->setWidgetResizable(true); auto content = new QWidget;
    auto body = new QVBoxLayout(content); body->setSizeConstraint(QLayout::SetMinAndMaxSize); layout->addWidget(scroll, 1);
    auto info = new QLabel(ready ? QStringLiteral("UUID/写入模式只保存给下次连接，不在线替换订阅。高级发送参数可在发送空闲时单独应用到当前连接。") : QStringLiteral("保存配置尚未与本次实际设备核对；连接后仍需发现服务并确认。"));
    if (!editable) info->setText(QStringLiteral("发送、录制或连接操作进行中：只读查看，不修改当前或下次连接配置。"));
    info->setWordWrap(true); body->addWidget(info);
    auto currentText = new QPlainTextEdit; currentText->setObjectName("bleProfileSummary"); currentText->setReadOnly(true); currentText->setMaximumHeight(145);
    QString summary = (ready ? QStringLiteral("当前已确认配置\n") : QStringLiteral("上次保存配置（未核对）\n")) + profileText(ready ? active : next);
    if (!entries.isEmpty()) {
        summary += QStringLiteral("\n\n上次发现的服务/特征（句柄只在当次连接有效）\n");
        for (const auto &v : entries) { const auto c = v.toObject(); summary += characteristicLabel(c) + " " + properties(c.value("properties").toInt()) + "\n"; }
    }
    currentText->setPlainText(summary); body->addWidget(currentText);
    auto form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    QList<QLineEdit *> fields;
    for (const auto label : {QStringLiteral("接收服务 UUID"), QStringLiteral("接收特征 UUID"), QStringLiteral("发送服务 UUID"), QStringLiteral("发送特征 UUID")}) {
        auto field = new QLineEdit; field->setMinimumWidth(0); form->addRow(label, field); fields.append(field);
    }
    fields[0]->setObjectName("bleNextRxService"); fields[1]->setObjectName("bleNextRxUuid"); fields[2]->setObjectName("bleNextTxService"); fields[3]->setObjectName("bleNextTxUuid");
    auto mode = new QComboBox; mode->addItems({QStringLiteral("有响应写入"), QStringLiteral("无响应写入")}); form->addRow(QStringLiteral("下次写入模式"), mode);
    auto stream = new QCheckBox(QStringLiteral("透传字节流（实际连接时仍需核对用途）")); form->addRow(stream);
    const auto sending = sendControls(&dialog, client);
    auto fill = [fields, mode, stream, sending](const QJsonObject &profile) {
        fields[0]->setText(profile.value("rxService").toString()); fields[1]->setText(profile.value("rxUuid").toString());
        fields[2]->setText(profile.value("txService").toString()); fields[3]->setText(profile.value("txUuid").toString());
        mode->setCurrentIndex(profile.value("withoutResponse").toBool() ? 1 : 0); stream->setChecked(profile.value("stream").toBool());
        sending.fill(profile);
    };
    auto value = [fields, mode, stream, sending] {
        QJsonObject p{{"schemaVersion", 3}, {"rxService", fields[0]->text()}, {"rxUuid", fields[1]->text()},
            {"txService", fields[2]->text()}, {"txUuid", fields[3]->text()}, {"withoutResponse", mode->currentIndex() == 1}, {"stream", stream->isChecked()}};
        sending.insert(p); return p;
    };
    fill(!editable && ready ? active : next);
    body->addWidget(templateTools(&dialog, value, fill, editable)); body->addLayout(form);
    for (auto field : fields) field->setReadOnly(!editable); mode->setEnabled(editable); stream->setEnabled(editable);
    body->addWidget(sending.widget); sending.widget->setEnabled(editable);
    auto limits = new QLabel(QStringLiteral("UUID 可填 16/32 位短格式或完整格式。接收或发送不用时，将该方向两项同时留空。模板保存设置值，不保存协商 MTU 或提速授权。"));
    limits->setWordWrap(true); body->addWidget(limits); body->addStretch();
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close); buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭（不保存）")); layout->addWidget(buttons);
    ProfileAction result;
    auto save = buttons->addButton(QStringLiteral("保存供下次连接"), QDialogButtonBox::ActionRole); save->setObjectName("bleSaveNextProfile"); save->setEnabled(editable);
    auto reconnect = buttons->addButton(QStringLiteral("保存并重新连接…"), QDialogButtonBox::ActionRole); reconnect->setEnabled(editable && ready);
    auto live = new QPushButton(QStringLiteral("仅应用发送参数到当前连接")); live->setObjectName("bleApplySendSettings"); body->insertWidget(body->count() - 1, live);
    bool applying = false;
    auto updateLive = [&] {
        const bool connected = client && client->state() == BleHostClient::State::Ready;
        const bool canEdit = editable && !applying && (!client || (!client->sendBusy() && !client->readPending()));
        live->setEnabled(canEdit && connected && client->supportsSendControl() && !active.value("txUuid").toString().isEmpty());
        sending.widget->setEnabled(canEdit); save->setEnabled(canEdit); reconnect->setEnabled(canEdit && connected);
    };
    if (client) {
        QObject::connect(client, &BleHostClient::operationFinished, &dialog, updateLive);
        QObject::connect(client, &BleHostClient::stateChanged, &dialog, [&] {
            if (applying && client->state() != BleHostClient::State::Ready) dialog.reject();
            else {
                if (ready && client->state() != BleHostClient::State::Ready) {
                    info->setText(QStringLiteral("连接已结束，当前参数不再生效；保存只用于下次连接，不会自动重连。"));
                    currentText->setPlainText(QStringLiteral("上次确认配置（连接已结束）\n") + profileText(active));
                }
                updateLive();
            }
        });
        QObject::connect(client, &BleHostClient::sendSettingsApplied, &dialog, [&] {
            if (!applying) return;
            result.applyCurrent = true; result.profile = active; result.profile["schemaVersion"] = 3;
            const auto s = client->sendSettings(); result.profile["packetSize"] = s.value("packetSize"); result.profile["intervalMs"] = s.value("intervalMs"); dialog.accept();
        });
        QObject::connect(client, &BleHostClient::errorOccurred, &dialog, [&](const QString &message, bool fatal) {
            if (applying) { applying = false; info->setText(message); if (fatal) dialog.reject(); else updateLive(); }
        });
    }
    QObject::connect(live, &QPushButton::clicked, &dialog, [&] {
        if (!client || !confirmSendSettings(&dialog, sending.packet->value(), sending.interval->value())) return;
        // Recheck after the modal confirmation: disconnect/queued commands may
        // have occurred while the user was deciding.
        if (client->applySendSettings(sending.packet->value(), sending.interval->value(), true)) { applying = true; info->setText(QStringLiteral("等待辅助程序确认发送参数…")); }
        else info->setText(QStringLiteral("发送参数未应用：连接状态已变化或发送尚未空闲，请稍后再试。"));
        updateLive();
    });
    auto accept = [&](bool reconnectNow) {
        QString error; QJsonObject profile;
        if (!BleProfileStore::normalize(value(), profile, error)) { QMessageBox::warning(&dialog, QStringLiteral("配置未保存"), error); return; }
        if (reconnectNow && !confirmAction(&dialog, QStringLiteral("重新连接并核对配置？"), QStringLiteral("将断开当前连接，重新发现服务并由你确认收发特征。不会自动发送命令或重新启动录制。"))) return;
        result.saveNext = true; result.reconnect = reconnectNow; result.profile = profile; dialog.accept();
    };
    QObject::connect(save, &QPushButton::clicked, &dialog, [&] { accept(false); });
    QObject::connect(reconnect, &QPushButton::clicked, &dialog, [&] { accept(true); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    scroll->setWidget(content); updateLive();
    if (dialog.exec() != QDialog::Accepted) return {};
    return result;
}
void BleDialogs::showDiagnostics(const std::function<QString(bool)> &snapshot, QWidget *parent)
{
    QDialog dialog(parent); prepare(dialog, QStringLiteral("BLE 连接诊断"), QSize(820, 600));
    auto layout = new QVBoxLayout(&dialog); auto identity = new QCheckBox(QStringLiteral("包含完整设备身份和错误路径（复制前请确认隐私）"));
    identity->setObjectName("bleDiagnosticIdentity"); layout->addWidget(identity);
    auto text = new QPlainTextEdit; text->setObjectName("bleDiagnosticText"); text->setReadOnly(true); layout->addWidget(text, 1);
    auto refresh = [&] {
        if (text->textCursor().hasSelection()) return; // Do not disrupt copying.
        const int scroll = text->verticalScrollBar()->value(); const auto value = snapshot(identity->isChecked());
        if (value != text->toPlainText()) { text->setPlainText(value); text->verticalScrollBar()->setValue(scroll); }
    };
    QTimer timer; timer.setInterval(1000); QObject::connect(&timer, &QTimer::timeout, &dialog, refresh);
    QObject::connect(identity, &QCheckBox::toggled, &dialog, [&] { text->setPlainText(snapshot(identity->isChecked())); });
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close); buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    auto copy = buttons->addButton(QStringLiteral("复制全部信息"), QDialogButtonBox::ActionRole);
    QObject::connect(copy, &QPushButton::clicked, &dialog, [text] { QApplication::clipboard()->setText(text->toPlainText()); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(buttons);
    refresh(); timer.start(); dialog.exec();
}
