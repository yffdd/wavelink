#pragma once
#include "updatemanager.h"
#include <QDialog>
#include <functional>
class QLabel; class QPushButton; class QProgressBar; class QPlainTextEdit;
class VersionDialog final : public QDialog {
    Q_OBJECT
public:
    VersionDialog(UpdateManager *manager, std::function<void()> install, QWidget *parent = nullptr);
private:
    UpdateManager *m_manager;
    QLabel *m_status, *m_release;
    QPlainTextEdit *m_notes;
    QProgressBar *m_progress;
    QPushButton *m_check, *m_download, *m_cancel, *m_install, *m_ignore;
    void refresh();
};
