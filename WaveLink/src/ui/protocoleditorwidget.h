#pragma once
#include "ProtocolProfile.h"
#include <QWidget>
#include <QMap>
#include <QStringList>
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class QFormLayout;
class QLabel;

class ProtocolEditorWidget : public QWidget {
    Q_OBJECT
public:
    explicit ProtocolEditorWidget(QWidget *parent = nullptr);
    void setProfile(const DataProtocols::ProtocolProfile &profile);
    bool profile(DataProtocols::ProtocolProfile &output, QString &error) const;
    bool isBinaryProtocol() const;
    bool isKeyValueProtocol() const;
    bool isDynamicArrayProtocol() const { return dynamicArray(); }
    static QJsonObject example(bool binary);
    static QJsonObject lengthExample();
    static QJsonObject dynamicArrayExample();
    static QJsonObject delimitedArrayExample();
    static QStringList protocolTypeNames();
signals:
    void edited();
protected:
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
private:
    void updateNameAppearance();
    QJsonObject definition() const;
    void updateType();
    void updateBinaryControls();
    void updateArrayLayout();
    bool dynamicArray() const;
    bool automaticFrameLayout() const;
    void updateFrameSummary();
    void addField();
    void fitChannelHeaders();
    void updateChannelTableHeight();
    void recalculateOffsets();
    bool sequentialTextColumns() const;
    void updateDataRegionHint();
    void syncDataRegionFromFields();
    void syncFieldsFromDataRegion(int bytes);
    void adjustFrameForDataRegion(int oldBytes, int newBytes);
    void appendDefaultField(int offset);
    bool automaticLayout() const;
    QWidget *control(const QString &key, QWidget *widget);
    QString value(const QString &key) const;
    int number(const QString &key) const;
    QMap<QString, QWidget *> m_controls;
    QJsonObject m_base;
    QComboBox *m_type = nullptr;
    QComboBox *m_headerPreset = nullptr, *m_tailPreset = nullptr;
    QLineEdit *m_name = nullptr;
    QPlainTextEdit *m_description = nullptr;
    QTableWidget *m_fields = nullptr, *m_conditions = nullptr;
    QWidget *m_frames = nullptr, *m_textFrame = nullptr, *m_binaryFrame = nullptr;
    QFormLayout *m_binaryForm = nullptr;
    QFormLayout *m_textForm = nullptr;
    QVector<QPair<QWidget *, QWidget *>> m_textRows;
    QWidget *m_devicePresetRow = nullptr;
    // Keep widgets alive when inactive rows are taken out of the Qt 5 form.
    QVector<QPair<QWidget *, QWidget *>> m_binaryRows;
    QLabel *m_lengthSummary = nullptr, *m_checksumSummary = nullptr, *m_channelHint = nullptr;
    QLabel *m_frameSummary = nullptr;
    QLabel *m_dataRegionHint = nullptr, *m_legacyLengthHint = nullptr;
    bool m_loading = false;
    // Follow contiguous columns after conversion/structural edits, but keep
    // deliberate sparse/reordered column mappings when loaded or edited.
    bool m_textColumnsAutomatic = true;
    bool m_updatingFields = false;
    bool m_syncingDataRegion = false;
    bool m_syncingFrame = false;
    int m_linkedDataBytes = 0;
    int m_checksumUiWidth = 1;
};

