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
QString shortUuid(const QString &value)
{
    const auto s = BleProfileStore::normalizeUuid(value);
    if (s.endsWith("-0000-1000-8000-00805f9b34fb"))
        return (s.startsWith("0000") ? s.mid(4, 4) : s.left(8)).toUpper();
    return s.left(8).toUpper() + QStringLiteral("…") + s.right(4).toUpper();
}
void addChannelChoice(QComboBox *combo, const QJsonObject &c, bool receive, bool nordic)
{
    const QString service = c.value("service").toString(), characteristic = c.value("uuid").toString();
    const int handle = c.value("handle").toInt(), p = c.value("properties").toInt();
    const QString label = nordic ? (receive ? QStringLiteral("Nordic UART · 设备发出（TX）") : QStringLiteral("Nordic UART · 设备接收（RX）")) :
        QStringLiteral("%1%2 · 服务 %3 · #%4")
            .arg(BleChannelPolicy::firmwareService(service) ? QStringLiteral("【固件升级】 ") : QString(),
                 shortUuid(characteristic), shortUuid(service)).arg(handle);
    combo->addItem(label, handle);
    combo->setItemData(combo->count() - 1, QStringLiteral("特征 UUID：%1\n服务 UUID：%2\n属性：%3\n句柄：#%4")
        .arg(characteristic, service, properties(p)).arg(handle), Qt::ToolTipRole);
}

}

GattChannels::GattChannels(const QJsonArray &entries, BleHostClient *client, QWidget *parent,
                           const std::function<void(int)> &selectRx, const std::function<void(int)> &selectTx,
                           bool compact)
    : QWidget(parent)
{
    const auto nus = BleChannelPolicy::nordicUart(entries);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto tree = new QTreeWidget;
    tree->setObjectName(compact ? "bleInlineGattTree" : "bleGattTree");
    tree->setHeaderLabels(compact ? QStringList{QStringLiteral("BLE 服务与特征")} :
        QStringList{QStringLiteral("服务 / 特征"), QStringLiteral("操作")});
    tree->setMinimumHeight(compact ? 210 : 160);
    tree->setMaximumHeight(230);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    if (compact) {
        tree->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        tree->setIndentation(12);
    } else {
        tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    }
    layout->addWidget(tree);
    auto output = new QPlainTextEdit;
    output->setObjectName("bleGattReadResult");
    output->setReadOnly(true);
    output->setMaximumHeight(90);
    output->hide();
    layout->addWidget(output);
    tree->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto copy = new QAction(QStringLiteral("复制完整 UUID"), tree);
    tree->addAction(copy);
    connect(copy, &QAction::triggered, this, [tree] {
        if (auto item = tree->currentItem()) QApplication::clipboard()->setText(item->data(0, Qt::UserRole).toString());
    });
    QMap<QPair<int, QString>, QTreeWidgetItem *> services;
    for (const auto &value : entries) {
        const auto c = value.toObject();
        const QString service = c.value("service").toString(), id = c.value("uuid").toString();
        const int handle = c.value("handle").toInt(), p = c.value("properties").toInt();
        const bool firmware = BleChannelPolicy::firmwareService(service);
        const auto key = qMakePair(c.value("serviceHandle").toInt(), service);
        auto &group = services[key];
        if (!group) {
            const QString shortId = shortUuid(service);
            auto title = shortId;
            if (uuid(service) == BleProfileStore::nus().value("rxService").toString()) title = QStringLiteral("Nordic UART");
            else if (shortId == "1800") title = QStringLiteral("通用访问 · 1800");
            else if (shortId == "1801") title = QStringLiteral("通用属性 · 1801");
            else if (firmware) title = QStringLiteral("固件升级 · %1").arg(shortId);
            group = new QTreeWidgetItem(tree, {title});
            auto font = group->font(0); font.setBold(true); group->setFont(0, font);
            group->setToolTip(0, service);
            group->setData(0, Qt::UserRole, service);
            group->setExpanded(!firmware && shortId != "1800" && shortId != "1801");
        }
        if (handle <= 0) {
            if (compact) group->setText(0, group->text(0) + QStringLiteral(" · 无法访问"));
            else group->setText(1, QStringLiteral("无法访问"));
            continue;
        }
        const auto title = handle == nus.rx ? QStringLiteral("设备发出（TX）") :
            handle == nus.tx ? QStringLiteral("设备接收（RX）") : shortUuid(id);
        auto item = new QTreeWidgetItem(group, {QStringLiteral("%1  ·  #%2").arg(title).arg(handle)});
        item->setData(0, Qt::UserRole, id);
        item->setToolTip(0, QStringLiteral("特征 UUID：%1\n服务 UUID：%2\n属性：%3").arg(id, service, properties(p)));

        auto actions = new QWidget(tree->viewport());
        auto row = new QHBoxLayout;
        row->setContentsMargins(compact ? 0 : 4, compact ? 0 : 2, compact ? 0 : 4, compact ? 0 : 2);
        row->setSpacing(4);
        if (compact) {
            auto column = new QVBoxLayout(actions);
            column->setContentsMargins(2, 2, 2, 2);
            column->setSpacing(2);
            auto name = new QLabel(item->text(0), actions);
            name->setObjectName("bleInlineCharacteristicName");
            name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            name->setToolTip(item->toolTip(0));
            column->addWidget(name);
            column->addLayout(row);
        } else {
            actions->setLayout(row);
            row->addStretch();
        }
        const auto add = [&](const char *name, const QString &text, const QString &tip, bool checkable) {
            auto button = new QToolButton(actions);
            button->setObjectName(QString::fromLatin1(name));
            button->setProperty("gattHandle", handle);
            button->setText(text);
            button->setToolTip(tip);
            button->setCheckable(checkable);
            const QString border = QStringLiteral("QToolButton:enabled:!checked:!hover { border-color: palette(mid); }");
            button->setStyleSheet(compact ? QStringLiteral("QToolButton { padding: 2px 5px; margin: 0px; }") + border : border);
            button->ensurePolished();
            const auto font = button->fontMetrics();
            button->setMinimumSize(button->sizeHint().expandedTo(QSize(
                qMax(compact ? 44 : 72, font.horizontalAdvance(text) + (compact ? 14 : 30)),
                qMax(compact ? 26 : 32, qMax(font.height(), font.boundingRect(text).height()) + (compact ? 8 : 14)))));
            row->addWidget(button);
            return button;
        };
        Row controls{handle};
        if ((p & 2) && client) {
            controls.read = add("bleGattRead", QStringLiteral("读取"), QStringLiteral("读取一次当前值"), false);
            connect(controls.read, &QToolButton::clicked, this, [this, client, output, handle] {
                output->show();
                reading = client->readCharacteristic(handle);
                output->setPlainText(reading ? QStringLiteral("正在读取特征 #%1…").arg(handle) : QStringLiteral("连接正在忙，请稍后重试。"));
                if (refresh) refresh();
            });
        }
        if (p & (16 | 32)) {
            controls.rx = add("bleGattRx", QStringLiteral("接收"), QStringLiteral("设备 → 电脑。再次点击取消选择。"), true);
            connect(controls.rx, &QToolButton::clicked, this, [selectRx, handle](bool on) { selectRx(on ? handle : 0); });
        }
        if (p & (4 | 8)) {
            controls.tx = add("bleGattTx", QStringLiteral("发送"), QStringLiteral("电脑 → 设备。再次点击取消选择。"), true);
            connect(controls.tx, &QToolButton::clicked, this, [selectTx, handle](bool on) { selectTx(on ? handle : 0); });
        }
        if (controls.read || controls.rx || controls.tx) {
            if (compact) {
                row->addStretch();
                // The embedded label owns the caption in the compact view.
                // Keep it available to accessibility without painting it twice.
                item->setData(0, Qt::AccessibleTextRole, item->text(0));
                item->setText(0, QString());
            }
            // The header and rows size from the item data, even before
            // Qt has laid out the action widget or expanded this service.
            const int actionColumn = compact ? 0 : 1;
            item->setSizeHint(actionColumn, actions->sizeHint());
            tree->setItemWidget(item, actionColumn, actions);
        } else {
            delete actions;
        }
        rows.append(controls);
    }
    if (client) {
        connect(client, &BleHostClient::characteristicRead, this, [this, output](int handle, const QByteArray &data) {
            if (!reading) return;
            reading = false;
            output->setPlainText(QStringLiteral("特征 #%1\nHEX: %2\nUTF-8: %3").arg(handle)
                .arg(QString::fromLatin1(data.toHex(' ')), QString::fromUtf8(data)));
        });
        connect(client, &BleHostClient::errorOccurred, this, [this, output](const QString &message) {
            if (!reading) return;
            reading = false;
            output->setPlainText(message);
        });
    }
}

void GattChannels::updateSelection(int rx, int tx, bool edit, bool read)
{
    for (const auto &r : rows) {
        if (r.rx) { r.rx->setChecked(r.handle == rx); r.rx->setEnabled(edit); }
        if (r.tx) { r.tx->setChecked(r.handle == tx); r.tx->setEnabled(edit); }
        if (r.read) r.read->setEnabled(read);
    }
}

namespace {
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

bool BleDialogs::confirmSendParameters(QWidget *parent, int packet, int interval)
{
    return confirmSendSettings(parent, packet, interval);
}

bool BleDialogs::confirmByteStream(QWidget *parent)
{
    return confirmAction(parent, QStringLiteral("确认 BLE 透传通道"),
        QStringLiteral("请确认新选的发送通道支持透传字节流。确认后会保存通道并重新连接设备。"));
}

BleDialogs::ProfileAction BleDialogs::configure(BleHostClient *client, const QJsonArray &entries,
                                                 QJsonObject active, const QJsonObject &saved, bool editable,
                                                 QWidget *parent, const QString &deviceName)
{
    if (!client) return {};
    const bool initialSetup = client->state() == BleHostClient::State::AwaitingConfiguration;
    const bool connectedAtOpen = client->state() == BleHostClient::State::Ready;
    const bool hasServices = initialSetup || connectedAtOpen;
    QDialog dialog(parent);
    dialog.setObjectName("bleConfigurationDialog");
    prepare(dialog, QStringLiteral("BLE 收发配置"), hasServices ? QSize(920, 560) : QSize(920, 360));
    auto outer = new QVBoxLayout(&dialog);
    auto heading = new QLabel(deviceName.isEmpty() ? QStringLiteral("未选择 BLE 设备") : deviceName);
    heading->setObjectName("bleDeviceHeading");
    heading->setTextFormat(Qt::PlainText);
    auto headingFont = heading->font();
    headingFont.setPointSize(headingFont.pointSize() + 3);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    heading->setWordWrap(true);
    outer->addWidget(heading);

    auto scroll = new QScrollArea;
    scroll->setObjectName("bleGattScroll");
    scroll->setWidgetResizable(true);
    outer->addWidget(scroll, 1);
    auto content = new QWidget;
    auto body = new QVBoxLayout(content);
    body->setContentsMargins(8, 8, 8, 8);
    body->setSizeConstraint(QLayout::SetMinAndMaxSize);
    auto info = new QLabel(!editable ? QStringLiteral("发送或录制进行中，当前只读。") :
        initialSetup ? QStringLiteral("选择接收和发送通道，或仅配置一个方向。") :
        connectedAtOpen ? QStringLiteral("可读取当前值，或为下次连接选择收发通道。") :
        QStringLiteral("连接设备后可从服务列表选择通道，也可在高级配置中编辑预设。"));
    info->setWordWrap(true);
    body->addWidget(info);

    const auto recommendations = hasServices ? BleChannelPolicy::candidates(entries) : QList<BleChannelPolicy::Candidate>();
    const auto nus = hasServices ? BleChannelPolicy::nordicUart(entries) : BleChannelPolicy::Candidate();
    auto candidate = new QComboBox(content);
    candidate->setObjectName("bleCandidateChoice");
    candidate->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    candidate->setMinimumContentsLength(16);
    for (const auto &c : recommendations) candidate->addItem(c.name);
    auto useCandidate = new QPushButton(QStringLiteral("使用候选通道"), content);
    useCandidate->setObjectName("bleUseCandidate");
    auto receiveOnly = new QPushButton(QStringLiteral("仅接收候选通道"), content);
    receiveOnly->setObjectName("bleReceiveOnlyCandidate");
    if (!recommendations.isEmpty()) {
        auto row = new QHBoxLayout;
        row->addWidget(candidate, 1);
        row->addWidget(useCandidate);
        row->addWidget(receiveOnly);
        body->addLayout(row);
    } else {
        for (QWidget *widget : QList<QWidget *>{candidate, useCandidate, receiveOnly}) widget->hide();
    }

    auto advancedToggle = new QToolButton;
    advancedToggle->setObjectName("bleGattAdvancedToggle");
    advancedToggle->setText(QStringLiteral("高级配置 · 通道、模板与发送参数"));
    advancedToggle->setCheckable(true);
    advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedToggle->setArrowType(Qt::RightArrow);
    auto advanced = new QWidget(content);
    auto details = new QVBoxLayout(advanced);
    details->setContentsMargins(0, 0, 0, 0);
    auto summary = new QPlainTextEdit;
    summary->setObjectName("bleProfileSummary");
    summary->setReadOnly(true);
    summary->setMaximumHeight(145);
    summary->setPlainText((connectedAtOpen ? QStringLiteral("当前已确认配置\n") : QStringLiteral("上次保存配置\n")) +
        profileText(connectedAtOpen ? active : saved));
    details->addWidget(summary);
    auto form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    details->addLayout(form);
    auto rx = new QComboBox(advanced), tx = new QComboBox(advanced), mode = new QComboBox;
    rx->setObjectName("bleReceiveCharacteristic");
    tx->setObjectName("bleTransmitCharacteristic");
    mode->setObjectName("bleWriteMode");
    rx->addItem(QStringLiteral("不接收"), 0);
    tx->addItem(QStringLiteral("不发送"), 0);
    mode->addItem(QStringLiteral("有响应写入（优先）"), false);
    mode->addItem(QStringLiteral("无响应写入"), true);
    QList<QLineEdit *> fields;
    if (initialSetup) {
        for (auto combo : {rx, tx}) {
            combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            combo->setMinimumContentsLength(16);
        }
        form->addRow(QStringLiteral("电脑接收（设备 → PC）"), rx);
        form->addRow(QStringLiteral("电脑发送（PC → 设备）"), tx);
    } else {
        rx->hide();
        tx->hide();
        const QStringList labels{QStringLiteral("接收服务 UUID"), QStringLiteral("接收特征 UUID"),
            QStringLiteral("发送服务 UUID"), QStringLiteral("发送特征 UUID")};
        const QStringList names{"bleNextRxService", "bleNextRxUuid", "bleNextTxService", "bleNextTxUuid"};
        for (int i = 0; i < labels.size(); ++i) {
            auto field = new QLineEdit;
            field->setObjectName(names[i]);
            form->addRow(labels[i], field);
            fields.append(field);
        }
    }
    form->addRow(QStringLiteral("写入模式"), mode);
    const auto sending = sendControls(&dialog, client);
    details->addWidget(sending.widget);
    auto live = new QPushButton(QStringLiteral("仅应用发送参数到当前连接"), advanced);
    live->setObjectName("bleApplySendSettings");
    live->setVisible(connectedAtOpen);
    details->addWidget(live);

    QMap<int, QJsonObject> byHandle;
    if (hasServices) for (const auto &value : entries) {
        const auto c = value.toObject();
        const int handle = c.value("handle").toInt(), props = c.value("properties").toInt();
        if (handle <= 0) continue;
        byHandle.insert(handle, c);
        if (props & (16 | 32)) addChannelChoice(rx, c, true, handle == nus.rx);
        if (props & (4 | 8)) addChannelChoice(tx, c, false, handle == nus.tx);
    }
    auto selection = new QLabel;
    selection->setWordWrap(true);
    auto modeHint = new QLabel;
    modeHint->setObjectName("bleAutomaticWriteMode");
    modeHint->setWordWrap(true);
    auto stream = new QCheckBox(QStringLiteral("确认所选发送通道支持透传字节流"));
    stream->setObjectName("bleStreamConfirmation");

    auto selectChannel = [&](bool receive, int handle) {
        if (initialSetup) {
            auto combo = receive ? rx : tx;
            combo->setCurrentIndex(qMax(0, combo->findData(handle)));
        } else {
            const int offset = receive ? 0 : 2;
            const auto c = byHandle.value(handle);
            fields[offset]->setText(c.value("service").toString());
            fields[offset + 1]->setText(c.value("uuid").toString());
            if (!receive && handle) mode->setCurrentIndex((c.value("properties").toInt() & 8) ? 0 : 1);
        }
        stream->setChecked(false);
    };
    auto channels = new GattChannels(hasServices ? entries : QJsonArray(), client, content,
        [&](int handle) { selectChannel(true, handle); }, [&](int handle) { selectChannel(false, handle); });
    channels->setVisible(hasServices);
    body->addWidget(channels);
    body->addWidget(selection);
    body->addWidget(modeHint);
    body->addWidget(stream);
    body->addWidget(advancedToggle);
    body->addWidget(advanced);
    advanced->hide();
    body->addStretch();
    QObject::connect(advancedToggle, &QToolButton::toggled, &dialog, [advanced, advancedToggle](bool on) {
        advanced->setVisible(on);
        advancedToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });

    const auto handles = [&]() -> QPair<int, int> {
        if (initialSetup) return {rx->currentData().toInt(), tx->currentData().toInt()};
        if (!connectedAtOpen) return {};
        return {BleChannelPolicy::resolve(entries, fields[0]->text(), fields[1]->text(), 16 | 32),
            BleChannelPolicy::resolve(entries, fields[2]->text(), fields[3]->text(), 4 | 8)};
    };
    const auto selectedProfile = [&] {
        const auto chosen = handles();
        const auto receive = byHandle.value(chosen.first), transmit = byHandle.value(chosen.second);
        const bool known = nus.tx && chosen == qMakePair(nus.rx, nus.tx);
        QJsonObject profile{{"schemaVersion", 3}, {"withoutResponse", mode->currentData().toBool()},
            {"rxService", initialSetup ? receive.value("service").toString() : fields[0]->text()},
            {"rxUuid", initialSetup ? receive.value("uuid").toString() : fields[1]->text()},
            {"txService", initialSetup ? transmit.value("service").toString() : fields[2]->text()},
            {"txUuid", initialSetup ? transmit.value("uuid").toString() : fields[3]->text()}};
        profile["stream"] = !profile.value("txUuid").toString().isEmpty() && (known || stream->isChecked());
        sending.insert(profile);
        return profile;
    };
    const auto fill = [&](const QJsonObject &profile) {
        if (initialSetup) {
            rx->setCurrentIndex(qMax(0, rx->findData(BleChannelPolicy::resolve(entries,
                profile.value("rxService").toString(), profile.value("rxUuid").toString(), 16 | 32))));
            tx->setCurrentIndex(qMax(0, tx->findData(BleChannelPolicy::resolve(entries,
                profile.value("txService").toString(), profile.value("txUuid").toString(), 4 | 8))));
        } else {
            fields[0]->setText(profile.value("rxService").toString());
            fields[1]->setText(profile.value("rxUuid").toString());
            fields[2]->setText(profile.value("txService").toString());
            fields[3]->setText(profile.value("txUuid").toString());
        }
        bool withoutResponse = profile.value("withoutResponse").toBool();
        const int props = byHandle.value(tx->currentData().toInt()).value("properties").toInt();
        if (initialSetup && props && !(props & (withoutResponse ? 4 : 8))) withoutResponse = !(props & 8);
        mode->setCurrentIndex(withoutResponse ? 1 : 0);
        stream->setChecked(!initialSetup && profile.value("stream").toBool());
        sending.fill(profile);
    };
    auto templates = templateTools(&dialog, selectedProfile, fill, editable);
    details->insertWidget(1, templates);
    const auto draft = connectedAtOpen && (saved.isEmpty() || !editable) ? active : saved;
    fill(initialSetup && draft.isEmpty() && nus.tx ? BleProfileStore::nus() : draft);

    auto reason = new QLabel;
    reason->setObjectName("bleApplyReason");
    reason->setWordWrap(true);
    outer->addWidget(reason);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Cancel)->setText(initialSetup ? QStringLiteral("取消并断开") : QStringLiteral("关闭（不保存）"));
    outer->addWidget(buttons);
    QPushButton *apply = nullptr, *save = nullptr, *reconnect = nullptr;
    if (initialSetup) {
        apply = buttons->addButton(QStringLiteral("应用并连接"), QDialogButtonBox::AcceptRole);
        apply->setObjectName("bleApplyConfiguration");
        apply->setMinimumHeight(36);
        apply->setStyleSheet("QPushButton { background:#147dad; color:white; padding:6px 20px; border-radius:4px; }"
                             "QPushButton:disabled { background:#8799a1; color:#f0f0f0; } QPushButton:hover:enabled { background:#1996cb; }");
    }
    save = buttons->addButton(QStringLiteral("保存供下次连接"), QDialogButtonBox::ActionRole);
    save->setObjectName("bleSaveNextProfile");
    if (connectedAtOpen) {
        reconnect = buttons->addButton(QStringLiteral("保存并重新连接…"), QDialogButtonBox::ActionRole);
        reconnect->setObjectName("bleSaveAndReconnect");
    }
    const auto canEdit = [&] {
        return editable && !client->readPending() && !client->sendBusy() &&
            (!initialSetup || client->state() == BleHostClient::State::AwaitingConfiguration);
    };
    const auto update = [&] {
        const bool connected = client->state() == BleHostClient::State::Ready;
        const bool editing = canEdit();
        const bool liveChannels = initialSetup || connected;
        const auto chosen = handles();
        const bool known = nus.tx && chosen == qMakePair(nus.rx, nus.tx);
        const bool hasTx = initialSetup ? chosen.second != 0 : !fields[3]->text().trimmed().isEmpty();
        const auto label = [](const QString &id) {
            return BleProfileStore::normalizeUuid(id).isEmpty() ? QStringLiteral("未选") : shortUuid(id);
        };
        const auto shortProfile = [&](const QJsonObject &profile) {
            return QStringLiteral("接收 %1  /  发送 %2").arg(label(profile.value("rxUuid").toString()), label(profile.value("txUuid").toString()));
        };
        selection->setText(initialSetup
            ? QStringLiteral("已选 · 接收 %1  /  发送 %2").arg(chosen.first ? rx->currentText() : QStringLiteral("未选"),
                chosen.second ? tx->currentText() : QStringLiteral("未选"))
            : (connected ? QStringLiteral("当前连接 · %1\n").arg(shortProfile(active)) : QString()) +
                QStringLiteral("下次连接 · %1").arg(shortProfile(selectedProfile())));
        modeHint->setText(known ? QStringLiteral("已识别 Nordic UART 透传 · %1").arg(mode->currentText()) :
            initialSetup && hasTx ? QStringLiteral("发送方式：%1").arg(mode->currentText()) : QString());
        modeHint->setVisible(!modeHint->text().isEmpty());
        stream->setVisible(hasTx && !known);
        for (auto field : fields) field->setReadOnly(!editing);
        for (auto combo : {rx, tx, mode}) combo->setEnabled(editing);
        for (auto combo : {rx, tx}) combo->setToolTip(combo->currentData(Qt::ToolTipRole).toString());
        templates->setEnabled(editing);
        sending.widget->setEnabled(editing);
        stream->setEnabled(editing);
        channels->updateSelection(chosen.first, chosen.second, editing && liveChannels, editing && liveChannels);
        for (QWidget *widget : QList<QWidget *>{candidate, useCandidate, receiveOnly}) widget->setEnabled(editing && liveChannels);
        live->setEnabled(editing && connected && client->supportsSendControl() && !active.value("txUuid").toString().isEmpty());
        if (reconnect) reconnect->setEnabled(editing && connected);

        QString why;
        if (client->readPending()) why = QStringLiteral("正在读取特征，请稍候…");
        else if (initialSetup && !editing) why = QStringLiteral("正在应用收发配置…");
        else if (initialSetup && !chosen.first && !chosen.second) why = QStringLiteral("请选择至少一个接收或发送通道。");
        else if (initialSetup && !client->supportsSendControl() && (sending.packet->value() != 20 || sending.interval->value() != 10))
            why = QStringLiteral("当前辅助程序仅支持 20 字节 / 10 ms，请在高级配置中恢复兼容设置。");
        else if (initialSetup && hasTx && !(byHandle.value(chosen.second).value("properties").toInt() & (mode->currentData().toBool() ? 4 : 8)))
            why = QStringLiteral("所选发送特征不支持此写入模式，请在高级配置中调整。");
        else if (initialSetup && hasTx && !known && !stream->isChecked()) why = QStringLiteral("请勾选透传用途确认，或取消选择发送通道。");
        if (apply) apply->setEnabled(editing && why.isEmpty());
        save->setEnabled(editing && (!initialSetup || why.isEmpty()));
        const bool firmware = BleChannelPolicy::firmwareService(byHandle.value(chosen.first).value("service").toString()) ||
            BleChannelPolicy::firmwareService(byHandle.value(chosen.second).value("service").toString());
        reason->setText(firmware ? QStringLiteral("已选择固件升级服务，请核对用途。 ") + why : why);
        reason->setStyleSheet(firmware || (editing && !why.isEmpty()) ? "color:#d04b36;" : QString());
        reason->setVisible(!reason->text().isEmpty());
    };
    channels->refresh = update;
    const auto chooseCandidate = [&](bool receiveOnly) {
        const int index = candidate->currentIndex();
        if (index < 0 || index >= recommendations.size()) return;
        selectChannel(true, recommendations[index].rx);
        selectChannel(false, receiveOnly ? 0 : recommendations[index].tx);
        update();
    };
    QObject::connect(useCandidate, &QPushButton::clicked, &dialog, [&] { chooseCandidate(false); });
    QObject::connect(receiveOnly, &QPushButton::clicked, &dialog, [&] { chooseCandidate(true); });
    QObject::connect(rx, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&] { stream->setChecked(false); update(); });
    QObject::connect(tx, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&] {
        stream->setChecked(false);
        const int props = byHandle.value(tx->currentData().toInt()).value("properties").toInt();
        if (props & (4 | 8)) mode->setCurrentIndex((props & 8) ? 0 : 1);
        update();
    });
    for (auto field : fields) QObject::connect(field, &QLineEdit::textChanged, &dialog, [&] { stream->setChecked(false); update(); });
    QObject::connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, update);
    QObject::connect(stream, &QCheckBox::toggled, &dialog, update);
    QObject::connect(sending.packet, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, update);
    QObject::connect(sending.interval, QOverload<int>::of(&QSpinBox::valueChanged), &dialog, update);
    QObject::connect(client, &BleHostClient::operationFinished, &dialog, update);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    ProfileAction result;
    QJsonObject submitted;
    bool applyingLive = false;
    QObject::connect(client, &BleHostClient::stateChanged, &dialog, [&] {
        if (client->state() == BleHostClient::State::Idle) {
            if (initialSetup || applyingLive) { dialog.reject(); return; }
            if (connectedAtOpen) {
                info->setText(QStringLiteral("连接已结束，可保存配置供下次连接使用。"));
                summary->setPlainText(QStringLiteral("上次确认配置（连接已结束）\n") + profileText(active));
            }
        }
        update();
    });
    QObject::connect(client, &BleHostClient::errorOccurred, &dialog, [&](const QString &message, bool fatal) {
        applyingLive = false;
        if (fatal && initialSetup) dialog.reject();
        else { info->setText(message); update(); }
    });
    if (apply) {
        QObject::connect(client, &BleHostClient::ready, &dialog, [&] {
            if (submitted.isEmpty()) return;
            result = {ProfileAction::Applied, submitted};
            dialog.accept();
        });
        QObject::connect(apply, &QPushButton::clicked, &dialog, [&] {
            const auto chosen = handles();
            if (chosen.second && !confirmSendSettings(&dialog, sending.packet->value(), sending.interval->value())) return;
            if (!canEdit()) return;
            QString error;
            if (!BleProfileStore::normalize(selectedProfile(), submitted, error)) { info->setText(error); return; }
            if (!client->configure(chosen.first, chosen.second, mode->currentData().toBool(),
                                   sending.packet->value(), sending.interval->value(), true)) submitted = {};
            update();
        });
    }
    const auto saveProfile = [&](bool reconnectNow) {
        QJsonObject profile;
        QString error;
        if (!BleProfileStore::normalize(selectedProfile(), profile, error)) {
            QMessageBox::warning(&dialog, QStringLiteral("配置未保存"), error);
            return;
        }
        if (reconnectNow && !confirmAction(&dialog, QStringLiteral("重新连接并核对配置？"),
                                          QStringLiteral("将重新发现设备服务，并使用刚保存的通道重新连接。"))) return;
        if (!canEdit()) return;
        result = {reconnectNow && client->state() == BleHostClient::State::Ready ? ProfileAction::Reconnect : ProfileAction::SaveNext, profile};
        dialog.accept();
    };
    QObject::connect(save, &QPushButton::clicked, &dialog, [&] { saveProfile(false); });
    if (reconnect) QObject::connect(reconnect, &QPushButton::clicked, &dialog, [&] { saveProfile(true); });
    QObject::connect(live, &QPushButton::clicked, &dialog, [&] {
        if (!confirmSendSettings(&dialog, sending.packet->value(), sending.interval->value()) || !canEdit()) return;
        applyingLive = client->applySendSettings(sending.packet->value(), sending.interval->value(), true);
        info->setText(applyingLive ? QStringLiteral("正在应用发送参数…") : QStringLiteral("连接状态已变化，请稍后重试。"));
        update();
    });
    QObject::connect(client, &BleHostClient::sendSettingsApplied, &dialog, [&] {
        if (!applyingLive) return;
        result.kind = ProfileAction::SendSettingsApplied;
        dialog.accept();
    });
    scroll->setWidget(content);
    update();
    return dialog.exec() == QDialog::Accepted ? result : ProfileAction();
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
