#pragma once

#include <QCoreApplication>
#include <QHash>
#include <QTranslator>

// Translate only Qt's built-in text menus. Do not alter user text, application
// actions or shortcut bindings, and do not depend on external translation files.
class ChineseTextMenuTranslator final : public QTranslator {
public:
    using QTranslator::QTranslator;
    bool isEmpty() const override { return false; }
    QString translate(const char *context, const char *sourceText,
                      const char * = nullptr, int = -1) const override {
        const QByteArray ctx(context);
        if (ctx != "QWidgetTextControl" && ctx != "QTextControl" && ctx != "QLineEdit"
            && ctx != "QAbstractSpinBox" && ctx != "QUnicodeControlCharacterMenu") return {};
        const QString source = QString::fromUtf8(sourceText);
        QString key = source; key.remove('&');
        static const QHash<QString, QString> labels {
            {"Undo", "撤销"}, {"Redo", "重做"}, {"Cut", "剪切"},
            {"Copy", "复制"}, {"Paste", "粘贴"}, {"Delete", "删除"},
            {"Select All", "全选"}, {"Copy Link Location", "复制链接地址"},
            {"Copy Link", "复制链接"}, {"Open Link", "打开链接"},
            {"Clear", "清空"}, {"Step up", "增加"}, {"Step down", "减少"},
            {"Insert Unicode control character", "插入 Unicode 控制字符"},
            {"LRM Left-to-right mark", "LRM 从左到右标记"},
            {"RLM Right-to-left mark", "RLM 从右到左标记"},
            {"ZWJ Zero width joiner", "ZWJ 零宽连接符"},
            {"ZWNJ Zero width non-joiner", "ZWNJ 零宽非连接符"},
            {"ZWSP Zero width space", "ZWSP 零宽空格"},
            {"LRE Start of left-to-right embedding", "LRE 开始从左到右嵌入"},
            {"RLE Start of right-to-left embedding", "RLE 开始从右到左嵌入"},
            {"LRO Start of left-to-right override", "LRO 开始从左到右覆盖"},
            {"RLO Start of right-to-left override", "RLO 开始从右到左覆盖"},
            {"PDF Pop directional formatting", "PDF 结束方向格式"},
            {"LRI Left-to-right isolate", "LRI 从左到右隔离"},
            {"RLI Right-to-left isolate", "RLI 从右到左隔离"},
            {"FSI First strong isolate", "FSI 首个强方向隔离"},
            {"PDI Pop directional isolate", "PDI 结束方向隔离"}
        };
        auto entry = labels.constFind(key);
        if (entry == labels.cend()) return {};
        QString result = entry.value();
        const int mnemonic = source.indexOf('&');
        if (mnemonic >= 0 && mnemonic + 1 < source.size())
            result += QStringLiteral("(&%1)").arg(source.at(mnemonic + 1).toUpper());
        return result;
    }
};

inline void installChineseTextMenus(QCoreApplication &app) {
    if (app.findChild<QTranslator *>("waveLinkChineseTextMenus")) return;
    auto translator = new ChineseTextMenuTranslator(&app);
    translator->setObjectName("waveLinkChineseTextMenus");
    app.installTranslator(translator);
}
