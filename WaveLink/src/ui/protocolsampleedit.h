#pragma once
#include <QPlainTextEdit>

class ProtocolSampleEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit ProtocolSampleEdit(QWidget *parent = nullptr);
    void setHexMode(bool hex);
    void setSpacedHex(bool spaced);
    bool isHexMode() const { return m_hex; }
    static QString displayBytes(const QByteArray &bytes, bool hex, bool spaced, bool escapeAll = false);
    static bool textBytes(const QString &text, QByteArray &bytes, QString &error);
signals:
    void invalidInput();
protected:
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void insertFromMimeData(const QMimeData *source) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
private:
    void insertHex(const QString &text);
    void normalize();
    void replaceHex(const QString &digits, int cursor, bool joinPrevious = false);
    QString format(const QString &digits) const;
    static bool digits(const QString &text, QString &result);
    static int digitPosition(const QString &text, int position);
    bool m_hex = false, m_spaced = true, m_normalizing = false;
    QString m_valid;
};
