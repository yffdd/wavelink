#ifndef COMMANDPANELWINDOW_H
#define COMMANDPANELWINDOW_H

#include <QDialog>
#include <QListWidget>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QSlider>
#include <QRegularExpressionValidator>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QTextEdit>
#include <QColorDialog>
#include <QMenu>
#include <QFormLayout>
#include <QMessageBox>
#include <QUuid>
#include <QGridLayout>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDrag>
#include <QApplication>
#include <QTableWidget>
#include <array>
#include <functional>
#include "panellayout.h"

enum class CommandWidgetType { Button = 0, Slider = 1, Switch = 2 };
enum class CommandSliderMode { Regular = 0, List = 1 };

struct CommandSwitchState {
    QString payload;
    QString annotation;
    QString color;
};

struct CommandDef {
    QString id;
    QString name;
    QString payload;
    QString extraPayload;
    bool isHex = false;
    bool autoSend = false;
    int intervalMs = 1000;
    int appendIndex = 0; // 0: None, 1: \r\n, 2: \n\r, 3: \r, 4: \n
    QString btnColor;
    QString note;
    int widgetType = static_cast<int>(CommandWidgetType::Button);
    double minValue = 0.0;
    double maxValue = 100.0;
    double stepValue = 1.0;
    double defaultValue = 0.0;
    int decimals = 0;
    int sliderSendMode = 0;
    int sliderMode = static_cast<int>(CommandSliderMode::Regular);
    QStringList sliderValues;
    int defaultListIndex = 0;
    QString customAppendPayload;
    std::array<CommandSwitchState, 2> switchStates{{{"", "关闭", ""}, {"", "开启", ""}}};
    int switchDefaultState = 0;
    bool switchNeedsRepair = false;
};

struct PanelDef {
    QString id;
    QString name;
    QString remark;
    int columns = 3;
    int buttonWidth = 200;
    int buttonHeight = 60;
    QList<CommandDef> commands;
    PanelLayout::Layout layoutRows;
};

// Dialog to view and edit panel remarks
class RemarkDialog : public QDialog {
    Q_OBJECT
public:
    explicit RemarkDialog(const QString &remark, QWidget *parent = nullptr);
    QString getRemark() const;
private slots:
    void onEditClicked();
    void onSaveClicked();
private:
    QTextEdit *textEdit;
    QPushButton *btnEdit;
    QPushButton *btnSave;
    QPushButton *btnClose;
    bool isEditing;
};

// Dialog to edit command properties
class CommandEditDialog : public QDialog {
    Q_OBJECT
public:
    explicit CommandEditDialog(const CommandDef &def, QWidget *parent = nullptr);
    CommandDef getDef() const;
    void forceStopTimer();
    void sendDataPacket(bool isAuto);

signals:
    void deleteRequested();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private slots:
    void onHexToggled(bool checked);
    void onColorClicked();
    void onDeleteClicked();
    void updateCommandTypeUi();
    void updatePreview();
    void updateSliderModeUi();
    void refreshSliderList();
    void saveCommand();

private:
    QLineEdit *editName;
    QTextEdit *editPayload;
    QTextEdit *editExtraPayload;
    QTextEdit *editCustomAppend;
    QTextEdit *editNote;
    QCheckBox *chkHex;
    QCheckBox *chkAutoSend;
    QSpinBox *spinInterval;
    QComboBox *comboAppend;
    QComboBox *comboWidgetType;
    QWidget *sliderOptions;
    QWidget *timerOptions;
    QDoubleSpinBox *spinMinValue;
    QDoubleSpinBox *spinMaxValue;
    QDoubleSpinBox *spinStepValue;
    QDoubleSpinBox *spinDefaultValue;
    QSpinBox *spinDecimals;
    QComboBox *comboSliderSendMode;
    QComboBox *comboSliderMode = nullptr, *comboListDefault = nullptr;
    QWidget *regularSliderOptions = nullptr, *listSliderOptions = nullptr;
    QTableWidget *sliderList = nullptr;
    QPushButton *btnListAdd = nullptr, *btnListDelete = nullptr, *btnListUp = nullptr, *btnListDown = nullptr;
    bool m_refreshingList = false;
    void addSliderListItem(const QString &value);
    void moveSliderListItem(int delta);
    QLabel *lblPreview;
    QLabel *lblFormula;
    QLabel *lblPayloadHint;
    QLabel *lblExtraHint;
    QPushButton *btnColorPicker;
    QString m_selectedColor;
    bool m_originalAutoSend;
    bool m_lastSliderMode = false;
    QFormLayout *m_formLayout = nullptr;
    QWidget *switchOptions = nullptr;
    QComboBox *comboSwitchDefault = nullptr;
    std::array<QTextEdit *, 2> switchPayload{{nullptr, nullptr}};
    std::array<QLineEdit *, 2> switchAnnotation{{nullptr, nullptr}};
    std::array<QPushButton *, 2> switchColorButton{{nullptr, nullptr}};
    std::array<QString, 2> switchColors;
    void refreshSwitchDefaults();
    void chooseSwitchColor(int state);
};

// Macro button representing a single command
class CommandButton : public QPushButton {
    Q_OBJECT
public:
    explicit CommandButton(const CommandDef &def, QWidget *parent = nullptr);
    virtual CommandDef getDef() const;
    virtual void forceStopTimer();
    virtual void sendDataPacket(bool isAuto);
    virtual void updateDef(const CommandDef &def);
    virtual void applyTheme(bool isDark);
    void confirmDelete() { deleteCommand(); }
    void openEditor() { editCommand(); }
    virtual void sendOnce() { sendDataPacket(false); }
    void setTimedSending(bool enabled);
    bool timedSending() const;

signals:
    void sendRequested(const QByteArray &data, bool isAuto);
    void deleteRequested(CommandButton *btn);
    void replaceRequested(CommandButton *btn, const CommandDef &def);
    void contentChanged();
    void dragStarted(CommandButton *btn);
    void dragMoved(CommandButton *btn, const QPoint &globalPos);
    void dragReleased(CommandButton *btn);
    void commandMenuRequested(CommandButton *btn, const QPoint &globalPos);

protected:
    bool event(QEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

protected slots:
    void onSendClicked(bool isAuto = false);
    void onTimerTimeout();
    virtual void editCommand();
    void deleteCommand();

private:
    CommandDef m_def;
    bool m_tooltipIsDark = true;
    QTimer *m_timer;
    QPoint m_dragStartPos;
    bool m_isDragging = false;
    QTimer *m_rightPressTimer = nullptr;
    bool m_rightPressed = false;
    bool m_rightLongPressed = false;
    QPoint m_rightPressGlobalPos;
};

class SliderCommandWidget : public CommandButton {
    Q_OBJECT
public:
    explicit SliderCommandWidget(const CommandDef &def, QWidget *parent = nullptr);
    CommandDef getDef() const override;
    void forceStopTimer() override;
    void sendDataPacket(bool isAuto) override;
    void updateDef(const CommandDef &def) override;
    void applyTheme(bool isDark) override;
    void sendOnce() override;
    void sendDefaultValue();
    void sendListItem(int index);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void editCommand() override { editSliderCommand(); }

private slots:
    void onSliderValueChanged(int value);
    void onSliderReleased();
    void onSpinValueChanged(double value);
    void onValueEditingFinished();
    void onThrottleTimeout();
    void editSliderCommand();
    void deleteSliderCommand();

private:
    CommandDef m_sliderDef;
    bool m_isDark = true;
    QSlider *m_slider = nullptr;
    QDoubleSpinBox *m_valueEdit = nullptr;
    QLabel *m_nameLabel = nullptr;
    QLabel *m_rangeLabel = nullptr;
    QLabel *m_previewLabel = nullptr;
    QTimer *m_sendTimer = nullptr;
    bool m_pendingSend = false;
    bool m_updatingValueWidgets = false;
    bool m_sendInProgress = false;
    bool m_hasLastSentValue = false;
    double m_lastSentValue = 0.0;

    double normalizedValue(double value) const;
    double sliderToValue(int value) const;
    int valueToSlider(double value) const;
    void updateValueWidgets(double value);
    void sendCurrentValue(bool isAuto);
    void updateSliderStyle();
    void configureValueRange();
};

// Runtime state is deliberately separate from the saved default state.
class SwitchCommandWidget : public CommandButton {
    Q_OBJECT
public:
    explicit SwitchCommandWidget(const CommandDef &def, QWidget *parent = nullptr);
    CommandDef getDef() const override { return m_switchDef; }
    void forceStopTimer() override {} // A switch never runs a timer.
    void sendDataPacket(bool isAuto) override;
    void updateDef(const CommandDef &def) override;
    void applyTheme(bool isDark) override;
    void setSendHandler(const std::function<bool(const QByteArray &)> &handler) { m_sendHandler = handler; }
    int currentState() const { return m_state; }
    void sendState(int state);
protected:
    void resizeEvent(QResizeEvent *event) override;
    void editCommand() override;
private:
    CommandDef m_switchDef;
    int m_state = 0;
    bool m_isDark = true;
    bool m_submitting = false;
    QString m_lastError;
    QWidget *m_indicator = nullptr;
    std::function<bool(const QByteArray &)> m_sendHandler;
    void refreshDisplay();
};
// Widget representing the content of a single panel
class PanelContentWidget : public QWidget {
    Q_OBJECT
public:
    explicit PanelContentWidget(const PanelDef &def, QWidget *parent = nullptr);
    PanelDef getDef() const;
    void addCommand(const CommandDef &cmdDef);
    void forceStopAllTimers();
    void applyTheme(bool isDark);
    void reloadConfig();
    void setSwitchSendHandler(const std::function<bool(const QByteArray &)> &handler);
    
    QString getRemark() const { return m_remark; }
    void openPanelSettings() { onSettingsClicked(); }
    void setRemark(const QString &remark) { m_remark = remark; emit contentChanged(); }
    void setPanelName(const QString &name) { 
        m_panelName = name; 
        QLabel *header = findChild<QLabel*>("headerName");
        if (header) header->setText(name);
        emit contentChanged();
    }

signals:
    void sendRequested(const QByteArray &data, bool isAuto);
    void contentChanged();

private slots:
    void onAddCommand();
    void onCommandDeleted(CommandButton *btn);
    void onCommandReplaced(CommandButton *btn, const CommandDef &def);
    void onCommandMenuRequested(CommandButton *btn, const QPoint &globalPos);
    void onViewRemark();
    void onSettingsClicked();
    
    void onDragStarted(CommandButton *btn);
    void onDragMoved(CommandButton *btn, const QPoint &globalPos);
    void onDragReleased(CommandButton *btn);

protected:
    void resizeEvent(QResizeEvent *event) override;

public:
    bool isDarkTheme() const { return m_isDark; }
private:
    bool m_isDark = true;
    QString m_panelId;
    QString m_panelName;
    QString m_remark;
    int m_columns = 3;
    int m_buttonWidth = 200;
    int m_buttonHeight = 60;
    QWidget *scrollContainer;
    QList<CommandButton*> m_buttons;
    std::function<bool(const QByteArray &)> m_switchSendHandler;
    
    CommandButton *m_draggedButton = nullptr;
    int m_dragTargetIndex = -1;
    QPoint m_dragOffset;
    
    friend class CommandPanelWindow;
    PanelLayout::Layout m_layoutRows;
    std::function<bool(QString &)> m_saveLayout;
    QSet<QString> m_commandIds;

    QVector<PanelLayout::Item> layoutItems() const;
    void editLayout();
    void appendCommandWidget(CommandDef def);
    void connectCommandWidget(CommandButton *btn);
    void rebuildLayout(bool animate = false);
};

// The main dialog
class CommandPanelWindow : public QDialog {
    Q_OBJECT
public:
    explicit CommandPanelWindow(QWidget *parent = nullptr);
    ~CommandPanelWindow();
    void forceStopAllTimers();
    void applyTheme(bool isDark);
    void reloadConfig();
    bool loadConfigFromString(const QString &jsonStr, bool append = false);
    void setSwitchSendHandler(const std::function<bool(const QByteArray &)> &handler);

signals:
    void sendDataRequested(const QByteArray &data, bool isAuto);

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private slots:
    void onAddPanel();
    void onRemovePanel();
    void onImportPanels();
    void onExportPanels();
    void onRepairConfig();
    void onPanelSelectionChanged(int row);
    void onContentChanged();
    void onListContextMenu(const QPoint &pos);
    void onCopyPanel(int row);
    void onPanelDoubleClicked(QListWidgetItem *item);

private:
    bool loadConfig(const QString &filePath, bool append = false);
    bool saveConfig(const QString &filePath);
    void addPanelToUI(const PanelDef &panelDef, int index = -1);
    void clearUI();
    void updateEmptyState();
    QString selectedPanelId() const;
    void restorePanelSelection(const QString &panelId);

    QListWidget *listPanels;
    QStackedWidget *stackPanels;
    QStackedWidget *rightContentStack;
    QWidget *emptyStateWidget;
    QLabel *emptyStateLabel;
    QLabel *configStatusLabel = nullptr;
    QPushButton *btnRepairConfig = nullptr;
    QString m_recoveryMessage;
    bool m_isDark = true;
    
    QString defaultConfigPath;
    bool m_allowAutoSave = true;
    bool m_knownConfigExists = false;
    bool m_saveFailureReported = false;
    QByteArray m_knownConfigBytes;
    QString m_configSaveError;
    std::function<bool(const QByteArray &)> m_switchSendHandler;
};

#endif // COMMANDPANELWINDOW_H
