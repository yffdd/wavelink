#pragma once
#include <QApplication>
#include <QColorDialog>
#include <QLabel>
#include <QPushButton>
#include <QTranslator>
#include <QVBoxLayout>
#include <QMap>
#include <QEvent>

namespace PanelUi {
inline const QMap<QString,QString> &colorTranslations(){
    static const QMap<QString,QString> words{{"Basic colors","基本颜色"},{"Custom colors","自定义颜色"},
        {"Pick Screen Color","拾取屏幕颜色"},{"Add to Custom Colors","添加到自定义颜色"},
        {"Hue:","色相："},{"Sat:","饱和度："},{"Val:","亮度："},{"Red:","红色："},
        {"Green:","绿色："},{"Blue:","蓝色："},{"Alpha channel:","透明度："},{"HTML:","颜色代码："},
        {"OK","确定"},{"Cancel","取消"},{"Select Color","选择颜色"},
        {"Cursor at %1, %2\nPress ESC to cancel","光标位置：%1，%2\n按 ESC 取消拾取"}};
    return words;
}
// Scoped to the modal picker, including Qt's live screen-picker hint. Never
// change the application's other dialogs or leave a translator installed.
class ColorTranslator : public QTranslator {
public:
    ColorTranslator(){QApplication::installTranslator(this);}
    ~ColorTranslator() override {QApplication::removeTranslator(this);}
    bool isEmpty() const override {return false;}
    QString translate(const char *context,const char *source,const char *,int) const override {
        const QByteArray category(context);
        if(category!="QColorDialog"&&category!="QColorShower"&&category!="QPlatformTheme"&&category!="QDialogButtonBox")return {};
        return colorTranslations().value(QString::fromUtf8(source).remove('&').trimmed());
    }
};
class ColorDialog : public QColorDialog {
public:
    ColorDialog(const QColor &initial,QWidget *parent,const QString &prefix,const QString &title,
                const QString &transparentText,const QString &transparentTip):QColorDialog(initial,parent){
        setObjectName(prefix+"Dialog");setWindowTitle(title);setOption(QColorDialog::DontUseNativeDialog);
        const auto &words=colorTranslations();
        for(auto label:findChildren<QLabel*>()){const auto key=QString(label->text()).remove('&').trimmed();if(words.contains(key))label->setText(words[key]);}
        for(auto button:findChildren<QAbstractButton*>()){
            const auto key=QString(button->text()).remove('&').trimmed();
            if(key=="Pick Screen Color"||key=="拾取屏幕颜色")m_picker=qobject_cast<QPushButton*>(button);
            if(words.contains(key))button->setText(words[key]);
        }
        m_transparent=new QPushButton(transparentText,this);m_transparent->setObjectName(prefix+"Transparent");m_transparent->setToolTip(transparentTip);
        if(m_picker){
            m_picker->setObjectName(prefix+"ScreenPicker");m_picker->installEventFilter(this);
            auto row=new QWidget(this);auto column=new QVBoxLayout(row);column->setContentsMargins(0,0,0,0);column->setSpacing(6);
            if(auto replaced=layout()->replaceWidget(m_picker,row)){delete replaced;column->addWidget(m_picker);column->addWidget(m_transparent);}
            else {delete row;layout()->addWidget(m_transparent);}
        }else if(layout())layout()->addWidget(m_transparent);
        connect(m_transparent,&QPushButton::clicked,this,[this]{useTransparent=true;accept();});
    }
    bool useTransparent=false;
protected:
    bool eventFilter(QObject *object,QEvent *event) override {
        if(object==m_picker&&event->type()==QEvent::EnabledChange)m_transparent->setEnabled(m_picker->isEnabled());
        return QColorDialog::eventFilter(object,event);
    }
private:
    QPushButton *m_picker=nullptr,*m_transparent=nullptr;
};
}
