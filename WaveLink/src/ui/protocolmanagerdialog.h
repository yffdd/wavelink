#pragma once
#include "ProtocolProfileStore.h"
#include "CustomProtocolParser.h"
#include <QDialog>
#include <memory>
#include <functional>
class ProtocolEditorWidget;
class QListWidget;
class QLabel;
class QComboBox;
class QPlainTextEdit;
class QTableWidget;
class QPushButton;
class QTimer;
class QCheckBox;
class ProtocolSampleEdit;
class QSpinBox;

class ProtocolManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProtocolManagerDialog(const QString &storePath = DataProtocols::ProtocolProfileStore::defaultPath(), QWidget *parent = nullptr,
                                   const QString &selectedProfileId = {});
    // Used by the file picker and by isolated UI tests; no connection changes.
    bool importBytes(const QByteArray &bytes, DataProtocols::ProtocolImportMode mode, bool accepted, QString &error);
    void setMutationGuard(std::function<bool(const DataProtocols::ProtocolSnapshot &, QString &)> guard) { m_guard = std::move(guard); }
protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;
private:
    bool leaveDraft();
    bool saveDraft();
    void syncDraftTitle();
    void refreshPreviewProtocols(bool followCurrent);
    void refresh(const QString &selectId = {});
    void updateEditorState();
    void selectRow(int row);
    void newProfile(int type, bool cbfTemplate = false);
    void chooseNewProfile();
    void copyCurrentProfile();
    void renameCurrentProfile();
    void deleteSelectedProfiles();
    void moveCurrentProfile(int delta);
    void showProfileDescription();
    void restoreBackup();
    void showListMenu(const QPoint &pos);
    void importFile();
    void exportFile(int mode);
    void startPreview();
    bool previewProfile(DataProtocols::ProtocolProfile &profile, QString &error) const;
    bool binarySampleProtocol() const;
    void syncSampleProtocol();
    void generateSample();
    void changeInputMode(int mode);
    void updateSampleControls();
    void previewStep();
    void stopPreview();
    void appendDiagnostic(const QString &message);
    void report(const QString &title, const QString &message);
    bool allowProfile(const QJsonObject &definition, QString &error) const;
    DataProtocols::ProtocolProfileStore m_store;
    ProtocolEditorWidget *m_editor = nullptr;
    QWidget *m_welcome = nullptr;
    QLabel *m_welcomeTitle = nullptr, *m_welcomeText = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_status = nullptr, *m_stats = nullptr;
    ProtocolSampleEdit *m_input = nullptr;
    QPlainTextEdit *m_diagnostics = nullptr;
    QComboBox *m_previewProtocol = nullptr, *m_inputMode = nullptr, *m_chunkMode = nullptr;
    QTableWidget *m_results = nullptr;
    QPushButton *m_test = nullptr, *m_cancelTest = nullptr;
    QPushButton *m_generateSample = nullptr;
    QCheckBox *m_spaceHex = nullptr;
    QLabel *m_chunkHint = nullptr;
    QLabel *m_arrayCountLabel = nullptr;
    QSpinBox *m_arrayCount = nullptr;
    QPushButton *m_recoveryButton = nullptr;
    QTimer *m_previewTimer = nullptr;
    std::unique_ptr<DataProtocols::CustomProtocolParser> m_parser;
    DataProtocols::ProtocolProfile m_testProfile;
    QByteArray m_testBytes, m_buffer;
    int m_position = 0, m_lastRow = -1, m_diagnosticCount = 0;
    quint32 m_random = 0x57a1;
    quint64 m_previewEpoch = 0;
    int m_inputRepresentation = 0, m_packetCount = 0, m_minChunk = 0, m_maxChunk = 0;
    bool m_dirty = false, m_refreshing = false, m_testing = false, m_newDraft = false;
    QString m_currentId;
    QString m_draftReturnId;
    QString m_sampleProtocolContext;
    std::function<bool(const DataProtocols::ProtocolSnapshot &, QString &)> m_guard;
};

