#ifndef HELPDIALOG_H
#define HELPDIALOG_H

#include <QDialog>
#include <QListWidget>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QPushButton>
#include <QLabel>
#include <QTreeWidget>

class HelpDialog : public QDialog
{
    Q_OBJECT
public:
    explicit HelpDialog(bool darkTheme, QWidget *parent = nullptr);
    ~HelpDialog() = default;

private slots:
    void onSidebarItemChanged(int currentRow);
    void onClearLogsClicked();
    void onLogAdded(const QString &formattedMessage);
    void onLogsCleared();

private:
    void setupUi();
    void applyTheme(bool darkTheme);
    QWidget* createOverviewPage();
    QWidget* createGuidePage();
    QWidget* createDownloadPage();
    QWidget* createChangelogPage();
    QWidget* createDebugPage();

    QListWidget *m_sidebarList;
    QStackedWidget *m_stackedWidget;
    QTextBrowser *m_debugTextBrowser;
    QPushButton *m_btnClearLogs;
};

#endif // HELPDIALOG_H
