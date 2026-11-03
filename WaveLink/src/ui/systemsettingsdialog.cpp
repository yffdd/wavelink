#include "systemsettingsdialog.h"
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QGuiApplication>
#include <QScreen>

SystemSettingsDialog::SystemSettingsDialog(const SystemPreferences &preferences,
        const std::function<void(const SystemPreferences &)> &apply, QWidget *parent, Category initialCategory) : QDialog(parent)
{
    setObjectName("systemSettingsDialog"); setWindowTitle(QStringLiteral("系统设置"));
    setWindowFlags((windowFlags() | Qt::WindowMinMaxButtonsHint) & ~Qt::WindowContextHelpButtonHint);
    setSizeGripEnabled(true);
    const auto screen = QGuiApplication::primaryScreen();
    resize(screen ? QSize(900, 600).boundedTo(screen->availableGeometry().size() - QSize(60, 100)) : QSize(900, 600));
    auto layout = new QVBoxLayout(this); auto splitter = new QSplitter;
    auto categories = new QListWidget; categories->setObjectName("systemSettingsCategories");
    categories->addItems({QStringLiteral("界面设置"), QStringLiteral("数据收发"), QStringLiteral("绘图显示"), QStringLiteral("软件更新")}); categories->setMinimumWidth(130);
    categories->setSpacing(3); for (int i = 0; i < categories->count(); ++i) categories->item(i)->setSizeHint(QSize(130, 42));
    auto pages = new QStackedWidget; pages->setObjectName("systemSettingsPages");
    splitter->addWidget(categories); splitter->addWidget(pages); splitter->setStretchFactor(1, 1); splitter->setSizes({170, 690});
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter, 1);
    auto page = [&](const QString &title, const QString &description) {
        auto scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        auto content = new QWidget; auto body = new QVBoxLayout(content); body->setAlignment(Qt::AlignTop); body->setSpacing(16);
        auto heading = new QLabel(title); auto font = heading->font(); font.setPointSize(font.pointSize() + 3); font.setBold(true); heading->setFont(font);
        body->addWidget(heading); auto note = new QLabel(description); note->setWordWrap(true); body->addWidget(note);
        auto form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setRowWrapPolicy(QFormLayout::WrapLongRows); form->setVerticalSpacing(16);
        body->addLayout(form); scroll->setWidget(content); pages->addWidget(scroll); return form;
    };
    auto appearance = page(QStringLiteral("界面设置"), QStringLiteral("设置点击“恢复默认布局”时主视图区与下方三个面板的高度分配。保存设置不会立即重排当前界面；手动拖动的布局仍按原方式记忆。首次启动且没有已保存布局时，也采用此比例。"));
    auto bottomPercent = new QSpinBox; bottomPercent->setObjectName("systemDefaultBottomPercent"); bottomPercent->setRange(20, 50);
    bottomPercent->setSuffix(" %"); bottomPercent->setSingleStep(5); bottomPercent->setValue(preferences.defaultBottomPercent);
    bottomPercent->setToolTip(QStringLiteral("下方的数据收发、频谱分析、信号分析面板占主窗口工作区的高度比例。默认 30%，范围 20%–50%；仅在恢复默认布局或无已保存布局时使用。"));
    appearance->addRow(QStringLiteral("默认布局下方高度占比"), bottomPercent);
    auto ratioPreview = new QLabel; ratioPreview->setObjectName("systemDefaultLayoutPreview"); ratioPreview->setWordWrap(true);
    auto updatePreview = [=] { ratioPreview->setText(QStringLiteral("恢复后：上方主视图区约 %1%，下方三个面板约 %2%。").arg(100 - bottomPercent->value()).arg(bottomPercent->value())); };
    connect(bottomPercent, QOverload<int>::of(&QSpinBox::valueChanged), this, updatePreview); updatePreview(); appearance->addRow(ratioPreview);
    auto layoutNote = new QLabel(QStringLiteral("建议 30%（默认）；希望下方面板更宽裕时可选 35% 或 40%。按当前窗口工作区计算，不含菜单、工具栏和状态栏，不再使用屏幕高度或 320 像素上限。实际高度仍受控件最小尺寸限制；不影响数据收发模式内的接收/发送分割比例。此设置包含在全局配置导入导出中。"));
    layoutNote->setWordWrap(true); appearance->addRow(layoutNote);
    auto console = page(QStringLiteral("数据收发"), QStringLiteral("控制接收文本的显示缓存和刷新频率。达到任一缓存上限时，只移除最旧内容并保留最新内容；不影响协议解析、状态监视、波形数据或 CSV/WLR 录制。保存接收与复制接收只能取得当前保留的显示内容。"));
    auto lines = new QComboBox; lines->setObjectName("systemReceiveLines");
    lines->addItem(QStringLiteral("1000 行（默认）"), 1000); lines->addItem(QStringLiteral("2000 行"), 2000); lines->addItem(QStringLiteral("5000 行"), 5000);
    lines->setCurrentIndex(lines->findData(preferences.receiveLines));
    constexpr int charactersPerUnit = ReceiveConsoleBuffer::CharacterUnit;
    auto characters = new QSpinBox; characters->setObjectName("systemReceiveCharacters");
    characters->setRange(ReceiveConsoleBuffer::MinCharacters / charactersPerUnit, ReceiveConsoleBuffer::MaxCharacters / charactersPerUnit);
    characters->setSingleStep(1); characters->setSuffix(QStringLiteral(" 万字符")); characters->setKeyboardTracking(false);
    characters->setValue(ReceiveConsoleBuffer::validCharacters(preferences.receiveCharacters) / charactersPerUnit);
    auto refresh = new QComboBox; refresh->setObjectName("systemReceiveRefreshMs");
    for (int ms : {50, 100, 150, 200, 250, 300, 500, 1000})
        refresh->addItem(QStringLiteral("%1 ms%2").arg(ms).arg(ms == 100 ? QStringLiteral("（默认）") : QString()), ms);
    if (refresh->findData(preferences.receiveRefreshMs) < 0) refresh->addItem(QStringLiteral("%1 ms（已保存）").arg(preferences.receiveRefreshMs), preferences.receiveRefreshMs);
    refresh->setCurrentIndex(refresh->findData(preferences.receiveRefreshMs));
    console->addRow(QStringLiteral("接收缓存行数"), lines); console->addRow(QStringLiteral("接收字符上限"), characters);
    auto characterHint = new QLabel(QStringLiteral("可设置范围：1–25 万字符（仅整数）。建议：5 万字符（安装默认值，即 50,000 字符）。1 万字符 = 10,000 字符。"));
    characterHint->setObjectName("systemReceiveCharactersHint"); characterHint->setWordWrap(true); console->addRow(characterHint);
    console->addRow(QStringLiteral("接收显示刷新间隔"), refresh);
    auto retention = new QLabel(QStringLiteral("默认 1000 行、5 万字符，每 100 ms 刷新一次。二进制/HEX 可能没有换行，因此字符上限同样重要；高速接收建议保持默认或降低上限，刷新间隔可选 100–200 ms 以减轻界面负担。行数按换行分段计算，自动折行不计入行数。")); retention->setWordWrap(true); console->addRow(retention);
    lines->setToolTip(QStringLiteral("只保留最近的指定行数；换行产生新行，自动折行不额外计数。"));
    characters->setToolTip(characterHint->text() + QStringLiteral("限制无换行文本和 HEX 的显示开销；降低上限只移除最旧内容，不会清空全部内容。"));
    refresh->setToolTip(QStringLiteral("默认 100 ms。只控制文本批量刷新的间隔；间隔越长，界面开销越低，设备接收和录制仍保持原速率。"));
    auto plot = page(QStringLiteral("绘图显示"), QStringLiteral("调整波形界面的刷新频率和线宽。设置统一作用于主视图与独立视图，不改变采样率、Y 轴自适应、协议解析或录制数据。"));
    auto fps = new QComboBox; fps->setObjectName("systemPlotFps");
    for (int value : {5, 10, 15, 20, 25, 30, 40, 50, 60})
        fps->addItem(QStringLiteral("%1 帧/秒%2").arg(value).arg(value == 30 ? QStringLiteral("（默认）") : QString()), value);
    if (fps->findData(preferences.plotFps) < 0) fps->addItem(QStringLiteral("%1 帧/秒（已保存）").arg(preferences.plotFps), preferences.plotFps);
    fps->setCurrentIndex(fps->findData(preferences.plotFps));
    fps->setToolTip(QStringLiteral("波形界面的目标刷新率，默认 30 帧/秒；不等于设备采样率，实际刷新速度还受数据量与设备性能影响。")); plot->addRow(QStringLiteral("波形目标刷新率"), fps);
    auto plotNote = new QLabel(QStringLiteral("默认 30 帧/秒。大数据量时可尝试 15–20 帧/秒；采样率、历史缓存、坐标轴和滤波仍在主界面对应区域设置。")); plotNote->setWordWrap(true); plot->addRow(plotNote);
    auto lineWidth = new QComboBox; lineWidth->setObjectName("systemPlotLineWidth");
    for (int width : {1, 2, 3}) lineWidth->addItem(QStringLiteral("%1 像素%2").arg(width).arg(width == 1 ? QStringLiteral("（默认，推荐）") : QString()), width);
    lineWidth->setCurrentIndex(lineWidth->findData(SystemPreferences::validLineWidth(preferences.plotLineWidth)));
    lineWidth->setToolTip(QStringLiteral("全局波形线宽，默认 1 个实际屏幕像素，高 DPI 下不自动变粗。只改变主视图、独立视图及波形图片的线条外观，不改变原始数值、CSV/WLR 录制、数值导出、滤波、频谱或测量精度。较粗线条会增加绘图负担。"));
    plot->addRow(QStringLiteral("波形线宽"), lineWidth);
    auto widthHint = new QLabel; widthHint->setObjectName("systemPlotLineWidthHint"); widthHint->setWordWrap(true);
    auto updateWidthHint = [=] {
        const int width = lineWidth->currentData().toInt();
        widthHint->setText(width == 1
            ? QStringLiteral("当前：1 像素（默认，推荐）。适合高频、多通道和长时间窗口。线宽仅改变外观，不改变采样数据及录制/分析精度。")
            : QStringLiteral("注意：当前为 %1 像素。粗线更醒目，但会增加重绘负担；高频、密集波形、多通道或高 DPI 下，刷新、拖动及缩放可能变慢。若出现卡顿，建议恢复 1 像素，或降低目标刷新率。线宽不改变采样数据及录制/分析精度。").arg(width));
        auto font = widthHint->font(); font.setBold(width > 1); widthHint->setFont(font);
    };
    connect(lineWidth, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateWidthHint); updateWidthHint();
    plot->addRow(widthHint);
    auto offlineMemory = new QSpinBox; offlineMemory->setObjectName("systemOfflineMemoryMB"); offlineMemory->setRange(16, 4096);
    offlineMemory->setSuffix(QStringLiteral(" MB")); offlineMemory->setSingleStep(64); offlineMemory->setKeyboardTracking(false);
    offlineMemory->setValue(SystemPreferences::validOfflineMemoryMB(preferences.offlineMemoryMB));
    plot->addRow(QStringLiteral("离线完整加载上限"), offlineMemory);
    auto offlineNote = new QLabel(QStringLiteral("默认 256 MB，范围 16–4096 MB，按展开后的原始数值计算。超过此额度或可用内存保护额度时，自动使用磁盘缓存按需浏览，全部数据仍可查看；放大后读取原始采样点。提高额度可让更多文件直接完整加载，但滤波、绘图和独立视图还会额外占用内存。大文件首次扫描和滤波需要时间及缓存磁盘空间；失败时请按提示检查路径、权限、格式或调整额度。"));
    offlineNote->setObjectName("systemOfflineMemoryHint"); offlineNote->setWordWrap(true); offlineMemory->setToolTip(offlineNote->text()); plot->addRow(offlineNote);
    auto updates = page(QStringLiteral("软件更新"), QStringLiteral("检查和下载更新请打开“帮助 → 版本信息”。自动检查仅提示新版本，不会自动下载、安装或停止采集录制；没有发布地址时不会联网检查。"));
    auto automaticUpdates = new QCheckBox(QStringLiteral("启动后自动检查更新（默认开启）")); automaticUpdates->setObjectName("systemAutomaticUpdates");
    automaticUpdates->setChecked(preferences.automaticUpdates);
    automaticUpdates->setToolTip(QStringLiteral("每天最多自动检查一次；联网获取版本信息，不上传采集数据、命令或面板配置。关闭后仍可手动检查。")); updates->addRow(automaticUpdates);
    auto updateNote = new QLabel(QStringLiteral("安装前需主动结束录制并断开设备。配置会自动备份；录制缓存和保存的数据不会由更新功能清理。此选项包含在全局配置导入导出中。")); updateNote->setWordWrap(true); updates->addRow(updateNote);
    connect(categories, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    categories->setCurrentRow(qBound(0, int(initialCategory), categories->count() - 1));
    auto feedback = new QLabel; feedback->setObjectName("systemSettingsFeedback"); feedback->setWordWrap(true); layout->addWidget(feedback);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Apply)->setText(QStringLiteral("应用")); buttons->button(QDialogButtonBox::Apply)->setObjectName("systemSettingsApply");
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存并关闭")); buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Ok)->setStyleSheet("QPushButton { background:#147dad; color:white; padding:6px 18px; border-radius:4px; }");
    auto defaults = buttons->addButton(QStringLiteral("恢复本页默认"), QDialogButtonBox::ResetRole); defaults->setObjectName("systemSettingsDefaults");
    connect(defaults, &QPushButton::clicked, this, [=] {
        if (pages->currentIndex() == 1) { lines->setCurrentIndex(0); characters->setValue(ReceiveConsoleBuffer::DefaultCharacters / charactersPerUnit); refresh->setCurrentIndex(refresh->findData(100)); }
        else if (pages->currentIndex() == 2) { fps->setCurrentIndex(fps->findData(30)); lineWidth->setCurrentIndex(lineWidth->findData(1)); offlineMemory->setValue(256); }
        else if (pages->currentIndex() == 3) automaticUpdates->setChecked(true);
        else bottomPercent->setValue(30);
        feedback->setText(QStringLiteral("已恢复本页默认值，点击应用或保存后生效。"));
    });
    auto save = [=] {
        // Commit in-progress spinbox edits before reading their values.
        characters->interpretText(); bottomPercent->interpretText(); offlineMemory->interpretText();
        SystemPreferences value; value.receiveLines = lines->currentData().toInt(); value.receiveCharacters = characters->value() * charactersPerUnit; value.receiveRefreshMs = refresh->currentData().toInt(); value.plotFps = fps->currentData().toInt();
        value.defaultBottomPercent = bottomPercent->value();
        value.plotLineWidth = lineWidth->currentData().toInt();
        value.offlineMemoryMB = offlineMemory->value();
        value.automaticUpdates = automaticUpdates->isChecked();
        apply(value); feedback->setText(QStringLiteral("设置已应用并保存；全局配置导入/导出会包含这些设置。"));
    };
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, save);
    connect(buttons, &QDialogButtonBox::accepted, this, [=] { save(); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto shortcut = new QShortcut(QKeySequence::Save, this); shortcut->setObjectName("systemSettingsSaveShortcut"); connect(shortcut, &QShortcut::activated, this, save);
    layout->addWidget(buttons);
}
