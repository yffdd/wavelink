#include "statusitemdialog.h"
#include "paneluistyle.h"
#include "panelcolordialog.h"
#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QScrollArea>
#include <QAbstractSpinBox>
#include <QLineEdit>
#include <QComboBox>
#include <QListView>
#include <QShowEvent>
#include <QResizeEvent>
#include <QStandardItemModel>
#include <QGroupBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QShortcut>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QDoubleValidator>
#include <QRegularExpressionValidator>
#include <QColorDialog>
#include <QPainter>
#include <QHelpEvent>
#include <QToolTip>
#include <QTextCodec>
#include <cmath>
using namespace StatusMonitoring;
namespace {
class FullHeightCombo final : public QComboBox {
protected:
    void showEvent(QShowEvent *event) override {QComboBox::showEvent(event);setMinimumHeight(sizeHint().height());}
    void changeEvent(QEvent *event) override {QComboBox::changeEvent(event);if(event->type()==QEvent::StyleChange||event->type()==QEvent::FontChange)setMinimumHeight(sizeHint().height());}
};
class WrappedBindingLabel final : public QLabel {
public:
    void updateMinimumHeight(){if(width()>0){const int height=heightForWidth(width());if(height>=0&&minimumHeight()!=height)setMinimumHeight(height);}}
protected:
    void resizeEvent(QResizeEvent *event) override {QLabel::resizeEvent(event);updateMinimumHeight();}
    void changeEvent(QEvent *event) override {QLabel::changeEvent(event);updateMinimumHeight();}
};
class NumericCell : public QStyledItemDelegate {
    bool integer;
public: NumericCell(bool whole,QObject *p):QStyledItemDelegate(p),integer(whole){}
    QWidget *createEditor(QWidget *p,const QStyleOptionViewItem &,const QModelIndex &)const override {
        auto e=new QLineEdit(p);if(integer)e->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,10}"),e));
        else {auto v=new QDoubleValidator(-1e308,1e308,16,e);v->setLocale(QLocale::c());e->setValidator(v);}return e;
    }
};
class RuleColorCell final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QString displayText(const QVariant &,const QLocale &) const override {return {};}
    QWidget *createEditor(QWidget *,const QStyleOptionViewItem &,const QModelIndex &)const override {return nullptr;}
    QSize sizeHint(const QStyleOptionViewItem &option,const QModelIndex &)const override {return QSize(80,qMax(26,option.fontMetrics.height()+8));}
    void paint(QPainter *p,const QStyleOptionViewItem &option,const QModelIndex &index)const override {
        auto view=option;initStyleOption(&view,index);view.text.clear();view.icon=QIcon();
        auto style=view.widget?view.widget->style():QApplication::style();style->drawControl(QStyle::CE_ItemViewItem,&view,p,view.widget);
        const QColor color(index.data(Qt::EditRole).toString());const int width=qMin(52,qMax(12,option.rect.width()-18)),height=qMin(20,qMax(10,option.rect.height()-10));
        QRect swatch(0,0,width,height);swatch.moveCenter(option.rect.center());p->save();p->setClipRect(option.rect);
        if(color.isValid()&&color.alpha()==255)p->fillRect(swatch,color);
        else {
            const bool dark=option.palette.color(QPalette::Base).lightness()<128;const QColor a(dark?"#68737e":"#ffffff"),b(dark?"#414b55":"#d5dce3");
            for(int y=swatch.top();y<=swatch.bottom();y+=5)for(int x=swatch.left();x<=swatch.right();x+=5)p->fillRect(QRect(x,y,qMin(5,swatch.right()-x+1),qMin(5,swatch.bottom()-y+1)),((x-swatch.left())/5+(y-swatch.top())/5)%2?a:b);
            if(color.isValid())p->fillRect(swatch,color);
        }
        p->setPen(option.palette.color(QPalette::Mid));p->setBrush(Qt::NoBrush);p->drawRect(swatch.adjusted(0,0,-1,-1));p->restore();
    }
    bool helpEvent(QHelpEvent *event,QAbstractItemView *view,const QStyleOptionViewItem &,const QModelIndex &index)override {
        if(!event||!view||!index.isValid())return false;QToolTip::showText(event->globalPos(),index.data(Qt::EditRole).toString().isEmpty()?"主题默认颜色（透明）；双击可选择颜色。":"自定义规则颜色；双击修改，可选择“使用透明颜色”恢复主题默认。",view);return true;
    }
};
class ChoiceCell : public QStyledItemDelegate {
    QStringList choices;
public:ChoiceCell(QStringList list,QObject *p):QStyledItemDelegate(p),choices(list){}
    QWidget *createEditor(QWidget *p,const QStyleOptionViewItem &,const QModelIndex &)const override {auto c=new QComboBox(p);c->addItems(choices);return c;}
    void setEditorData(QWidget *e,const QModelIndex &i)const override {static_cast<QComboBox*>(e)->setCurrentText(i.data().toString());}
    void setModelData(QWidget *e,QAbstractItemModel *m,const QModelIndex &i)const override {m->setData(i,static_cast<QComboBox*>(e)->currentText());}
};
class SilenceValueCell : public QStyledItemDelegate {
public:using QStyledItemDelegate::QStyledItemDelegate;
    static QLineEdit *numberEdit(QWidget *parent,const QString &name){auto edit=new QLineEdit(parent);edit->setObjectName(name);auto validator=new QDoubleValidator(-1e308,1e308,16,edit);validator->setLocale(QLocale::c());edit->setValidator(validator);return edit;}
    void paint(QPainter *p,const QStyleOptionViewItem &option,const QModelIndex &index)const override{
        auto view=option;initStyleOption(&view,index);const auto kind=index.sibling(index.row(),1).data().toString();
        view.text=kind=="数值区间"?QString("%1 ≤ 数值 ≤ %2（含上下限）").arg(index.data().toString(),index.sibling(index.row(),3).data().toString()):(kind=="固定文本"?"文本：":"数值：")+index.data().toString();
        auto style=view.widget?view.widget->style():QApplication::style();style->drawControl(QStyle::CE_ItemViewItem,&view,p,view.widget);
    }
    QWidget *createEditor(QWidget *p,const QStyleOptionViewItem &,const QModelIndex &index)const override{
        const auto kind=index.sibling(index.row(),1).data().toString();
        if(kind=="数值区间"){auto editor=new QWidget(p);editor->setObjectName("statusSilenceRangeEditor");auto layout=new QHBoxLayout(editor);layout->setContentsMargins(0,0,0,0);layout->setSpacing(3);auto lower=numberEdit(editor,"statusSilenceLower");auto upper=numberEdit(editor,"statusSilenceUpper");layout->addWidget(new QLabel("下限（含）",editor));layout->addWidget(lower);layout->addWidget(new QLabel("上限（含）",editor));layout->addWidget(upper);editor->setFocusProxy(lower);
            // The composite itself does not receive FocusOut when a child loses
            // focus. Commit both fields before a Save click or Ctrl+S collects.
            auto delegate=const_cast<SilenceValueCell*>(this);for(auto edit:{lower,upper})connect(edit,&QLineEdit::editingFinished,delegate,[delegate,editor]{emit delegate->commitData(editor);});return editor;}
        auto edit=kind=="固定文本"?new QLineEdit(p):numberEdit(p,"statusSilenceNumber");edit->setPlaceholderText(kind=="固定文本"?"完整文本":"单个固定数值");if(kind=="固定文本")edit->setMaxLength(4096);return edit;
    }
    void setEditorData(QWidget *editor,const QModelIndex &index)const override{
        if(auto edit=qobject_cast<QLineEdit*>(editor)){edit->setText(index.data(Qt::EditRole).toString());return;}
        editor->findChild<QLineEdit*>("statusSilenceLower")->setText(index.data(Qt::EditRole).toString());editor->findChild<QLineEdit*>("statusSilenceUpper")->setText(index.sibling(index.row(),3).data(Qt::EditRole).toString());
    }
    void setModelData(QWidget *editor,QAbstractItemModel *model,const QModelIndex &index)const override{
        if(auto edit=qobject_cast<QLineEdit*>(editor)){model->setData(index,edit->text());return;}
        const auto lower=editor->findChild<QLineEdit*>("statusSilenceLower")->text(),upper=editor->findChild<QLineEdit*>("statusSilenceUpper")->text();const auto end=index.sibling(index.row(),3);
        {QSignalBlocker block(model);model->setData(index,lower);model->setData(end,upper);}emit model->dataChanged(index,end);
    }
};
QString qualityText(Quality q){switch(q){case Quality::Valid:return "有效";case Quality::Waiting:return "等待数据";case Quality::Unknown:return "未匹配";case Quality::Disabled:return "已停用";default:return "数据异常";}}
QString escaped(const QByteArray &bytes){QString s;for(auto c:bytes){auto n=uchar(c);if(c=='\r')s+="\\r";else if(c=='\n')s+="\\n";else if(c=='\t')s+="\\t";else if(c=='\\')s+="\\\\";else if(n>=32&&n<127)s+=QChar(n);else s+="\\x"+QString::number(n,16).rightJustified(2,'0').toUpper();}return s;}
bool unescape(const QString &text,const QString &encoding,QByteArray &bytes,QString &error){
    auto codec=QTextCodec::codecForName(encoding=="ASCII"?QByteArray("ISO-8859-1"):encoding.toLatin1());QString literal;
    auto flush=[&](){QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);bytes+=codec->fromUnicode(literal.constData(),literal.size(),&state);literal.clear();return !state.invalidChars&&!state.remainingChars;};
    for(int i=0;i<text.size();++i){if(text[i]!='\\'){literal+=text[i];continue;}if(!flush()){error="文本不能用所选编码表示。";return false;}if(++i>=text.size()){error="末尾转义符不完整。";return false;}
        auto c=text[i];if(c=='r')bytes+='\r';else if(c=='n')bytes+='\n';else if(c=='t')bytes+='\t';else if(c=='\\')bytes+='\\';else if(c=='x'&&i+2<text.size()){bool ok;auto n=text.mid(i+1,2).toUInt(&ok,16);if(!ok){error="HEX 转义无效。";return false;}bytes+=char(n);i+=2;}else {error="仅支持 \\r、\\n、\\t、\\\\ 和 \\xHH。";return false;}}
    if(!flush()){error="文本不能用所选编码表示。";return false;}return true;
}
}
QString statusBindingSummary(const StatusPanels::Panel &panel,const StatusUiContext &context){
    auto p=panel;if(p.bindingMode==StatusPanels::BindingMode::Legacy)StatusPanels::normalizeBinding(p);
    if(p.bindingMode==StatusPanels::BindingMode::Legacy)return "旧配置存在不同协议绑定；原绑定已保留，请在面板设置中统一。";
    if(p.bindingMode==StatusPanels::BindingMode::Follow)return "跟随当前协议："+context.protocolName+"；按通道序号读取，请确认通道含义。";
    QString name=p.protocolId=="builtin.justfloat"&&!p.protocolName.isEmpty()?p.protocolName:context.protocolNames.value(p.protocolId,p.protocolName);if(name.isEmpty()&&p.protocolId==context.protocolId)name=context.protocolName;if(name.isEmpty())name=p.protocolId;
    QString status=p.protocolId!=context.protocolId?"当前使用其他协议":p.protocolShape!=context.protocolShape?"协议结构已变更":"协议及结构匹配";
    return "锁定："+name+" · "+status+(p.protocolId!=context.protocolId||p.protocolShape!=context.protocolShape?"；暂停通道数值监视，文本监视不受影响。":"；按通道序号读取。");
}
StatusPanelBindingEditor::StatusPanelBindingEditor(const StatusPanels::Panel &panel,const StatusUiContext &context,QWidget *parent):QWidget(parent),m_context(context){
    auto p=panel;StatusPanels::normalizeBinding(p);
    if(m_context.protocolCatalog){m_context.protocols=m_context.protocolCatalog();m_context.protocolCatalog={};}
    m_id=p.protocolId.isEmpty()?context.protocolId:p.protocolId;m_shape=p.protocolId.isEmpty()?context.protocolShape:p.protocolShape;m_savedName=p.protocolName;
    auto ensure=[this](const QString &id,const QByteArray &shape,const QString &name,const QStringList &channels,const QString &warning){
        if(id.isEmpty())return;for(const auto &o:m_context.protocols)if(o.id==id&&o.shape==shape)return;
        StatusProtocolOption o;o.id=id;o.shape=shape;o.name=name.isEmpty()?"协议未找到（"+id+"）":name;o.channelNames=channels;o.custom=!id.startsWith("builtin.");o.numeric=id!="builtin.raw";o.warning=warning;m_context.protocols.append(o);
    };
    ensure(context.protocolId,context.protocolShape,context.protocolName,context.channelNames,{});
    ensure(m_id,m_shape,context.protocolNames.value(m_id,m_savedName),{},"保留的旧结构；请核对通道含义后，明确选择最新协议。");
    auto form=new QFormLayout(this);form->setContentsMargins(0,0,0,0);form->setFormAlignment(Qt::AlignTop);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_mode=new FullHeightCombo;m_mode->setObjectName("statusProtocolBinding");m_mode->addItem("锁定指定协议及结构（推荐）",int(StatusPanels::BindingMode::Locked));m_mode->addItem("跟随主界面当前协议",int(StatusPanels::BindingMode::Follow));
    if(p.bindingMode==StatusPanels::BindingMode::Legacy)m_mode->addItem("保留旧绑定（待统一）",int(StatusPanels::BindingMode::Legacy));
    m_mode->setCurrentIndex(m_mode->findData(int(p.bindingMode)));form->addRow("协议绑定",m_mode);m_mode->setToolTip("统一作用于本面板的通道数值监视项；接收文本监视保持独立。跟随模式按通道序号读取，切换协议需确认通道含义。");
    m_choice=new FullHeightCombo;m_choice->setObjectName("statusProtocolChoice");m_choice->setView(new QListView(m_choice));m_choice->setMaxVisibleItems(10);m_choice->setStyleSheet("QComboBox {combobox-popup:0;}");m_choice->setMinimumContentsLength(18);m_choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);form->addRow("选择协议",m_choice);
    m_choice->setToolTip("按系统协议与自定义协议分组。选择仅修改本面板绑定，不切换主界面的采集协议。Raw 没有数值通道，不可选。");
    auto model=static_cast<QStandardItemModel*>(m_choice->model());
    auto heading=[this,model](QString text){m_choice->addItem(text,-1);auto item=model->item(model->rowCount()-1);item->setFlags(Qt::NoItemFlags);auto font=item->font();font.setBold(true);item->setFont(font);};
    for(bool custom:{false,true}){heading(custom?"自定义协议":"系统协议");for(int i=0;i<m_context.protocols.size();++i){const auto &o=m_context.protocols[i];if(o.custom!=custom)continue;m_choice->addItem("    "+o.name+(o.warning.isEmpty()?QString():"（保留结构）"),i);m_choice->setItemData(m_choice->count()-1,o.name+"\nID："+o.id+"\n"+o.warning,Qt::ToolTipRole);if(!o.numeric)model->item(m_choice->count()-1)->setFlags(Qt::NoItemFlags);}}
    m_name=new QLineEdit;m_name->setObjectName("statusBoundProtocolName");m_name->setReadOnly(true);form->addRow("已锁定协议",m_name);
    m_hint=new WrappedBindingLabel;m_hint->setObjectName("statusBindingHint");m_hint->setWordWrap(true);m_hint->setTextFormat(Qt::PlainText);m_hint->setTextInteractionFlags(Qt::TextSelectableByMouse);form->addRow(m_hint);
    auto rebind=new QPushButton("绑定主界面当前协议");rebind->setObjectName("statusRebindCurrentProtocol");rebind->setToolTip("明确更新面板绑定的协议及结构；不会切换采集协议，不改变任何监视项的通道编号。");rebind->setEnabled(!context.protocolId.isEmpty()&&!context.protocolShape.isEmpty()&&context.protocolId!="builtin.raw");form->addRow(rebind);
    auto warning=new QLabel("保存绑定后统一应用到本面板所有通道数值监视项，保留原通道编号；请核对通道含义。文本监视不受影响。");warning->setWordWrap(true);warning->setTextFormat(Qt::PlainText);form->addRow(warning);
    selectProtocol();
    connect(m_mode,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{updateUi();});
    connect(m_choice,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int index){int i=m_choice->itemData(index).toInt();if(i<0||i>=m_context.protocols.size()||!m_context.protocols[i].numeric)return;const auto &o=m_context.protocols[i];m_id=o.id;m_shape=o.shape;m_savedName=o.name;updateUi();});
    connect(rebind,&QPushButton::clicked,this,[this]{m_id=m_context.protocolId;m_shape=m_context.protocolShape;m_savedName=m_context.protocolName;selectProtocol();m_mode->setCurrentIndex(0);updateUi();});updateUi();
}
void StatusPanelBindingEditor::selectProtocol(){QSignalBlocker block(m_choice);int selected=-1;for(int i=0;i<m_choice->count();++i){int n=m_choice->itemData(i).toInt();if(n>=0&&n<m_context.protocols.size()&&m_context.protocols[n].id==m_id&&m_context.protocols[n].shape==m_shape){selected=i;break;}}m_choice->setCurrentIndex(selected);}
void StatusPanelBindingEditor::updateUi(){
    const bool locked=m_mode->currentData().toInt()==int(StatusPanels::BindingMode::Locked);
    auto form=static_cast<QFormLayout*>(layout());for(QWidget *w:QList<QWidget*>{m_choice,m_name}){w->setVisible(locked);form->labelForField(w)->setVisible(locked);}
    StatusPanels::Panel p;p.bindingMode=StatusPanels::BindingMode(m_mode->currentData().toInt());if(locked){p.protocolId=m_id;p.protocolShape=m_shape;p.protocolName=m_savedName;}
    QString name=m_context.protocolNames.value(m_id,m_savedName);for(const auto &o:m_context.protocols)if(o.id==m_id&&o.shape==m_shape){name=o.name;break;}if(name.isEmpty())name=m_id;m_name->setText(name);m_name->setCursorPosition(0);m_name->setToolTip(name+"\nID："+m_id);
    m_hint->setText(p.bindingMode==StatusPanels::BindingMode::Legacy?"旧配置存在不同协议绑定；原绑定已保留，请选择锁定协议或跟随当前协议，保存后统一。":statusBindingSummary(p,m_context));static_cast<WrappedBindingLabel*>(m_hint)->updateMinimumHeight();
}
bool StatusPanelBindingEditor::collect(StatusPanels::Panel &p,QString &error)const{
    p.bindingMode=StatusPanels::BindingMode(m_mode->currentData().toInt());p.protocolId.clear();p.protocolShape.clear();p.protocolName.clear();
    if(p.bindingMode==StatusPanels::BindingMode::Locked){if(m_id.isEmpty()||m_shape.isEmpty()||m_id=="builtin.raw"){error="请在面板设置中选择有效的数值协议，或选择跟随当前协议。";return false;}p.protocolId=m_id;p.protocolShape=m_shape;p.protocolName=m_name->text();}
    StatusPanels::normalizeBinding(p);return true;
}

StatusItemDialog::StatusItemDialog(const Rule &rule,const QVector<Rule> &siblings,const StatusUiContext &context,QWidget *parent)
    :QDialog(parent),m_original(rule),m_result(rule),m_context(context),m_siblings(siblings){
    PanelUi::windowControls(this);
    if(m_context.protocolCatalog){m_context.protocols=m_context.protocolCatalog();m_context.protocolCatalog={};}
    m_independentSourceId=rule.textSource.id.isEmpty()?"source-"+rule.id:rule.textSource.id;
    for(const auto &other:siblings)if(other.id!=rule.id&&other.source==Source::Text&&!rule.textSource.id.isEmpty()&&other.textSource.id==rule.textSource.id){m_independentSourceId=StatusPanels::newId();break;}
    setObjectName("statusItemDialog");setWindowTitle("编辑状态监视项");resize(1080,760);setMinimumSize(720,480);auto font=this->font();if(font.pointSizeF()<10)font.setPointSizeF(10);setFont(font);
    auto root=new QVBoxLayout(this);auto split=new QSplitter(this);root->addWidget(split,1);
    auto editSide=new QWidget;editSide->setObjectName("statusEditorSide");auto editLayout=new QVBoxLayout(editSide);editLayout->setContentsMargins(0,0,0,0);split->addWidget(editSide);
    auto scroll=new QScrollArea;scroll->setObjectName("statusEditorScroll");scroll->setWidgetResizable(true);editLayout->addWidget(scroll,1);auto body=new QWidget;scroll->setWidget(body);
    auto outer=new QVBoxLayout(body);outer->setContentsMargins(10,10,10,10);outer->setSpacing(0);
    auto contents=new QWidget;contents->setObjectName("statusEditorForm");contents->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Maximum);outer->addWidget(contents);outer->addStretch(1);
    auto left=new QVBoxLayout(contents);left->setContentsMargins(0,0,0,0);left->setSpacing(8);
    auto form=new QFormLayout;form->setVerticalSpacing(6);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setFormAlignment(Qt::AlignTop);left->addLayout(form);
    auto row=[&](QFormLayout *f,const QString &label,QWidget *w,const QString &tip){f->addRow(label,w);auto l=f->labelForField(w);if(l)l->setToolTip(tip);w->setToolTip(tip);};
    m_name=new QLineEdit(rule.name);m_name->setObjectName("statusItemName");m_name->setMaxLength(1024);row(form,"监视名称",m_name,"显示在状态卡片顶部，不参与数据解析。");
    m_source=new QComboBox;m_source->setObjectName("statusSourceKind");m_source->addItems({"通道数值","接收文本"});row(form,"解析来源",m_source,"通道数值来自协议解码；接收文本独立读取当前接口的 RX 字节。");
    m_mode=new FullHeightCombo;m_mode->setObjectName("statusMonitorMode");m_mode->addItems({"实时状态（默认）","事件提示"});m_mode->setCurrentIndex(rule.event.enabled?1:0);row(form,"监视模式",m_mode,"实时状态按最新数据更新；事件提示逐帧捕获瞬时事件，不被背景/未匹配数据覆盖。新事件覆盖旧提示，全部触发进入有限历史记录；不会弹出打断采集的对话框。");
    m_enabled=new QCheckBox("启用此监视项");m_enabled->setChecked(rule.enabled);row(form,"启用",m_enabled,"停用后停止规则计算；关闭面板窗口只隐藏显示。");
    m_numberPage=new QWidget;m_numberPage->setObjectName("statusNumericOptions");auto nf=new QFormLayout(m_numberPage);nf->setContentsMargins(0,0,0,0);nf->setVerticalSpacing(6);nf->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->addRow(m_numberPage);
    m_bindingHint=new WrappedBindingLabel;m_bindingHint->setObjectName("statusBindingHint");m_bindingHint->setWordWrap(true);m_bindingHint->setTextFormat(Qt::PlainText);m_bindingHint->setTextInteractionFlags(Qt::TextSelectableByMouse);row(nf,"面板协议",m_bindingHint,"继承面板设置中的统一协议绑定；要更改协议，请到面板设置。文本监视不依赖数值协议。");
    m_channel=new QComboBox;m_channel->setObjectName("statusChannel");for(int i=0;i<256;++i)m_channel->addItem(QString("Ch%1%2").arg(i+1).arg(i<context.channelNames.size()?" · "+context.channelNames[i]:""));m_channel->setCurrentIndex(rule.channel);row(nf,"来源通道",m_channel,"界面序号从 1 开始；缺少通道显示异常，不用 0 代替。");
    m_value=new QComboBox;m_value->addItems({"原始数值（默认）","换算后数值"});m_value->setCurrentIndex(rule.converted?1:0);row(nf,"数值来源",m_value,"原始值为协议解码后的数字；换算值含协议缩放和范围处理，二者均在数字滤波之前。");
    m_channel->setView(new QListView(m_channel));m_channel->setMaxVisibleItems(10);m_channel->setStyleSheet("QComboBox {combobox-popup:0;}");m_channel->view()->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    updateBindingUi();connect(m_channel,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{updateBindingUi();});
    m_kind=new QComboBox;m_kind->setObjectName("statusNumericKind");m_kind->addItems({"数值映射","按位解析","阈值判断"});m_kind->setCurrentIndex(rule.kind==RuleKind::TextMatch?0:int(rule.kind));row(nf,"解析方式",m_kind,"整数映射、状态字位段或测量值区间。");
    // Keep width in its own form row: hiding individual rows inside the same
    // nested Qt 5 form can leave stale height-for-width geometry and clip it.
    m_width=new FullHeightCombo;m_width->addItems({"8","16","32"});m_width->setCurrentText(QString::number(rule.width));row(nf,"状态字位宽",m_width,"8/16/32 位无符号整数；不将浮点内存重新解释为状态字。");
    m_bits=new QWidget;auto bf=new QFormLayout(m_bits);bf->setContentsMargins(0,0,0,0);bf->setVerticalSpacing(6);bf->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_first=new QSpinBox;m_first->setRange(0,31);m_first->setValue(rule.firstBit);row(bf,"起始位（bit0 起）",m_first,"最低位为 bit0；映射方式不使用起始位。");m_count=new QSpinBox;m_count->setRange(1,32);m_count->setValue(rule.bitCount);row(bf,"位数",m_count,"提取连续位段，再按下方表格映射。");nf->addRow(m_bits);
    m_threshold=new QWidget;auto tf=new QFormLayout(m_threshold);tf->setContentsMargins(0,0,0,0);m_hysteresis=new QDoubleSpinBox;m_hysteresis->setRange(0,1e12);m_hysteresis->setDecimals(6);m_hysteresis->setValue(rule.hysteresis);row(tf,"恢复回差",m_hysteresis,"仅对表格中启用回差的区间生效；如报警下限 80、回差 2，可在降到 78 时恢复。");nf->addRow(m_threshold);
    m_first->setObjectName("statusFirstBit");m_count->setObjectName("statusBitCount");m_width->setObjectName("statusWordWidth");
    auto duration=[](QSpinBox *spin,const QString &name,const QString &zero,const QList<int> &values){auto w=new QWidget;auto l=new QHBoxLayout(w);l->setContentsMargins(0,0,0,0);auto presets=new QComboBox;presets->setObjectName(name);presets->addItem(zero,0);for(int n:values)presets->addItem(QString::number(n)+" ms",n);presets->addItem("自定义时间",-1);l->addWidget(presets,1);l->addWidget(spin,1);auto sync=[spin,presets]{int i=presets->findData(spin->value());QSignalBlocker block(presets);presets->setCurrentIndex(i<0?presets->count()-1:i);};QObject::connect(presets,QOverload<int>::of(&QComboBox::activated),w,[spin,presets](int i){if(presets->itemData(i).toInt()>=0)spin->setValue(presets->itemData(i).toInt());else{spin->setFocus();spin->selectAll();}});QObject::connect(spin,QOverload<int>::of(&QSpinBox::valueChanged),w,[sync]{sync();});sync();return w;};
    m_hold=new QSpinBox;m_hold->setObjectName("statusHoldTime");m_hold->setRange(0,3600000);m_hold->setSuffix(" ms");m_hold->setValue(rule.holdMs);row(nf,"生效延迟（防抖）",duration(m_hold,"statusHoldPreset","立即生效",{100,300,500,1000}),"0 表示立即生效；其他值要求连续满足条件指定时间后切换。事件模式不使用防抖或恢复回差，避免漏掉单帧事件。");
    m_textPage=new QWidget;m_textPage->setObjectName("statusTextOptions");auto tx=new QFormLayout(m_textPage);tx->setContentsMargins(0,0,0,0);tx->setVerticalSpacing(6);tx->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->addRow(m_textPage);
    m_shared=new QComboBox;m_shared->setObjectName("statusTextSource");m_shared->addItem("独立来源（仅修改此项）",QString());QSet<QString> known;for(const auto &s:siblings)if(s.source==Source::Text&&s.id!=rule.id&&!s.textSource.id.isEmpty()&&!known.contains(s.textSource.id)){known.insert(s.textSource.id);m_shared->addItem("复用："+s.name,s.textSource.id);}row(tx,"文本来源",m_shared,"选择复用时使用已有来源的分帧设置；如需不同设置，请选择独立来源，不会静默更改其他卡片。");
    m_framing=new QComboBox;m_framing->addItems({"结束符分帧","每个 UDP 数据报一条消息"});m_framing->setCurrentIndex(rule.textSource.datagram?1:0);row(tx,"分帧方式",m_framing,"串口/TCP/BLE 必须使用结束符。UDP 报文模式不自动删除 CR/LF。");
    m_encoding=new QComboBox;m_encoding->addItems({"UTF-8","ASCII","GBK"});m_encoding->setCurrentText(rule.textSource.encoding);row(tx,"文本编码",m_encoding,"严格解码，非法编码不参与成功匹配；不受主界面接收显示编码影响。");
    m_delimiter=new QComboBox;m_delimiter->setEditable(true);m_delimiter->addItems({"0D0A","0A","0D"});m_delimiter->setCurrentText(QString::fromLatin1(rule.textSource.delimiter.toHex().toUpper()));m_delimiter->lineEdit()->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9A-Fa-f ]{0,47}"),m_delimiter));
    connect(m_delimiter->lineEdit(),&QLineEdit::textEdited,this,[this](const QString &s){const auto cursor=m_delimiter->lineEdit()->cursorPosition();m_delimiter->setEditText(s.toUpper());m_delimiter->lineEdit()->setCursorPosition(cursor);});row(tx,"结束符 HEX",m_delimiter,"0D0A 为 CRLF，0A 为 LF，0D 为 CR；支持自定义 1–16 字节。正文不要再填写结束符。");
    m_trim=new QCheckBox("去除消息首尾空格/TAB");m_trim->setChecked(rule.textSource.trim);row(tx,"空白处理",m_trim,"不修改消息中间空格，也不擅自修改规则正文。");
    m_case=new QCheckBox("区分大小写");m_case->setChecked(rule.textSource.caseSensitive);row(tx,"大小写",m_case,"取消后仅忽略 ASCII 字母大小写。");
    m_sync=new QCheckBox("等待首个结束符后再开始");m_sync->setChecked(rule.textSource.strictSync);row(tx,"严格首条同步",m_sync,"适合从半条连续消息中接入；可能舍弃首条本来完整的反馈。UDP 不舍弃首个完整报文。");
    m_max=new QSpinBox;m_max->setRange(1,65536);m_max->setValue(rule.textSource.maxBytes);row(tx,"最大正文长度",m_max,"按字节计，不含结束符；超长时拒绝整条并重同步。");
    m_partial=new QSpinBox;m_partial->setRange(0,3600000);m_partial->setSuffix(" ms");m_partial->setValue(rule.textSource.partialTimeoutMs);row(tx,"半包超时",m_partial,"0 关闭；只清理不完整消息，不把静默当成消息结束。");
    m_every=new QCheckBox("记录每次匹配（受事件上限约束）");m_every->setChecked(rule.recordEveryMatch);row(tx,"事件记录",m_every,"默认相同反馈只更新时间和次数，不重复记状态变化。");
    m_timeout=new QSpinBox;m_timeout->setObjectName("statusTimeout");m_timeout->setRange(0,3600000);m_timeout->setSuffix(" ms");m_timeout->setValue(rule.timeoutMs);row(form,"状态有效期",duration(m_timeout,"statusTimeoutPreset","不自动过期",{1000,3000,5000,10000}),"0 表示持续到后续有效数据或反馈更新，不因静默过期；设备断开仍立即失效。其他值为无有效数据后的超时期限，无关文本不续期。支持直接输入毫秒。");
    m_timeoutTitle=static_cast<QLabel*>(form->labelForField(m_timeout->parentWidget()));
    m_eventBox=new QGroupBox("事件提示设置");m_eventBox->setObjectName("statusEventOptions");auto eventLayout=new QVBoxLayout(m_eventBox);auto eventForm=new QFormLayout;eventLayout->addLayout(eventForm);
    m_eventDuration=new QSpinBox;m_eventDuration->setObjectName("statusEventDuration");m_eventDuration->setRange(0,86400000);m_eventDuration->setSuffix(" ms");m_eventDuration->setValue(rule.event.displayMs);row(eventForm,"提示保持时间",duration(m_eventDuration,"statusEventDurationPreset","保持至手动清除 / 下一事件",{500,1000,2000,5000,10000}),"新建默认保持至手动清除 / 下一事件（0 ms），也可选择定时清除。新事件立即替换当前提示；手动清除在卡片右键菜单。断线、缺少通道、非法数值及数据超时优先显示异常。");
    m_eventTrigger=new QComboBox;m_eventTrigger->setObjectName("statusEventTrigger");m_eventTrigger->addItems({"进入匹配状态时触发","每次匹配均触发"});m_eventTrigger->setCurrentIndex(rule.event.onChange?0:1);row(eventForm,"重复触发",m_eventTrigger,"进入匹配状态：连续命中同一数值、位段、区间或文本规则只触发一次，遇到屏蔽/未匹配数据后可再次触发。每次匹配：每个匹配帧/消息都计数并刷新提示时间，受历史上限约束。");
    auto silenceHint=new QLabel("屏蔽条件：任一启用条件命中即静默，不触发、不覆盖、不延长事件提示。\n数值条件针对所选原始/换算值（位段提取前）；文本按编码、空白与大小写设置匹配，数值类条件要求整条正文能转换为有限十进制数字。区间包含两端；不影响其他监视项、绘图或录制。");silenceHint->setWordWrap(true);eventLayout->addWidget(silenceHint);
    m_silence=new QTableWidget(0,4);m_silence->setObjectName("statusEventSilence");m_silence->setHorizontalHeaderLabels({"启用","屏蔽类型","屏蔽内容","上限（含）"});m_silence->setSelectionBehavior(QAbstractItemView::SelectRows);m_silence->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);m_silence->setColumnHidden(3,true);m_silence->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch);m_silence->setItemDelegateForColumn(2,new SilenceValueCell(m_silence));m_silence->setItemDelegateForColumn(3,new SilenceValueCell(m_silence));eventLayout->addWidget(m_silence);
    auto silenceButtons=new QHBoxLayout;eventLayout->addLayout(silenceButtons);auto addFilter=new QPushButton("添加屏蔽条件");addFilter->setObjectName("statusAddSilence");addFilter->setToolTip("最多 64 条；数值来源可屏蔽固定数值或闭区间，文本来源还可屏蔽固定文本。");auto removeFilter=new QPushButton("删除屏蔽条件");removeFilter->setObjectName("statusDeleteSilence");removeFilter->setToolTip("仅删除当前选中条件，不影响接收数据或规则。");silenceButtons->addWidget(addFilter);silenceButtons->addWidget(removeFilter);connect(addFilter,&QPushButton::clicked,this,&StatusItemDialog::addSilence);connect(removeFilter,&QPushButton::clicked,this,[this]{if(m_silence->currentRow()>=0)m_silence->removeRow(m_silence->currentRow());});
    auto sizeSilence=[this]{m_silence->setFixedHeight(m_silence->horizontalHeader()->height()+m_silence->rowCount()*m_silence->verticalHeader()->defaultSectionSize()+30);};connect(m_silence->model(),&QAbstractItemModel::rowsInserted,this,[sizeSilence]{sizeSilence();});connect(m_silence->model(),&QAbstractItemModel::rowsRemoved,this,[sizeSilence]{sizeSilence();});connect(m_silence,&QTableWidget::cellChanged,this,[this](int row,int col){if(col==1)updateSilenceRow(row);});sizeSilence();
    left->addWidget(m_eventBox);
    const QStringList silenceTips{"取消勾选可停用此条件，不删除配置。","启用条件按或关系组合，任一命中即静默。固定文本仅用于文本来源。","固定数值填写一个值；固定文本填写完整正文；区间双击后分别填写下限和上限，包含两端。","仅数值区间使用，包含上限；下限和上限必须为有限数字。"};for(int c=0;c<4;++c)m_silence->horizontalHeaderItem(c)->setToolTip(silenceTips[c]);
    m_hint=new QLabel;m_hint->setWordWrap(true);left->addWidget(m_hint);m_table=new QTableWidget;m_table->setObjectName("statusRuleTable");m_table->setWordWrap(false);m_table->setMinimumHeight(70);m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);m_table->setSelectionBehavior(QAbstractItemView::SelectRows);m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);left->addWidget(m_table);
    auto sizeTable=[this]{m_table->setFixedHeight(m_table->horizontalHeader()->height()+m_table->rowCount()*m_table->verticalHeader()->defaultSectionSize()+30);};connect(m_table->model(),&QAbstractItemModel::rowsInserted,this,[sizeTable]{sizeTable();});connect(m_table->model(),&QAbstractItemModel::rowsRemoved,this,[sizeTable]{sizeTable();});
    auto actions=new QHBoxLayout;left->addLayout(actions);auto add=new QPushButton("添加规则");add->setObjectName("statusAddRule");auto remove=new QPushButton("删除规则");auto up=new QPushButton("↑");auto down=new QPushButton("↓");actions->addWidget(add);actions->addWidget(remove);actions->addWidget(up);actions->addWidget(down);
    remove->setObjectName("statusDeleteRule");add->setToolTip("添加一条规则；Ctrl+Z 可撤销增删或排序。");remove->setToolTip("删除选中规则；Ctrl+Z 可撤销。");
    connect(add,&QPushButton::clicked,this,[this]{if(m_table->rowCount()<256){rememberRules();addRow();m_table->selectRow(m_table->rowCount()-1);}});connect(remove,&QPushButton::clicked,this,[this]{if(m_table->currentRow()>=0){rememberRules();m_table->removeRow(m_table->currentRow());}});connect(up,&QPushButton::clicked,this,[this]{moveRow(-1);});connect(down,&QPushButton::clicked,this,[this]{moveRow(1);});
    auto undo=new QShortcut(QKeySequence::Undo,this);undo->setObjectName("statusUndoRules");undo->setAutoRepeat(false);connect(undo,&QShortcut::activated,this,[this]{if(!m_eventBox->isAncestorOf(QApplication::focusWidget()))undoRules();});
    connect(m_table,&QTableWidget::cellDoubleClicked,this,[this](int row,int col){const int colorColumn=m_source->currentIndex()==1?3:m_kind->currentIndex()==2?6:2;if(col!=colorColumn)return;const auto current=QColor(m_table->item(row,col)->text());PanelUi::ColorTranslator translator;PanelUi::ColorDialog dialog(current.isValid()?current:palette().color(QPalette::WindowText),this,"statusColor","选择状态颜色","使用透明颜色","清除规则的自定义颜色，恢复主题默认显示；状态文字不会隐藏，保存监视项后生效。");if(dialog.exec()!=QDialog::Accepted)return;if(dialog.useTransparent)m_table->item(row,col)->setText(QString());else if(dialog.selectedColor().isValid())m_table->item(row,col)->setText(dialog.selectedColor().name());});
    m_note=new QPlainTextEdit(rule.note);m_note->setObjectName("statusItemDescription");m_note->setPlaceholderText("填写用途或设备约定；仅用于说明，不参与发送或匹配。");m_note->setMaximumHeight(140);auto noteForm=new QFormLayout;left->addLayout(noteForm);row(noteForm,"说明",m_note,"悬停卡片显示摘要，详情可查看完整内容。");
    auto right=new QWidget;split->addWidget(right);auto test=new QVBoxLayout(right);test->addWidget(new QLabel("规则测试 · 不向设备发送"));m_sampleFormat=new QComboBox;m_sampleFormat->addItems({"单条正文 / 数值","接收流（文本转义）","接收流（HEX）"});test->addWidget(m_sampleFormat);
    m_chunks=new QComboBox;m_chunks->addItems({"整块输入","逐字节输入","每 7 字节输入"});m_chunks->setToolTip("仅模拟串流读取边界，解析结果应相同；UDP 以一个报文输入。");test->addWidget(m_chunks);
    m_sample=new QPlainTextEdit;m_sample->setObjectName("statusTestInput");m_sample->setPlaceholderText("数值如 5、0x05；文本如 DEVICE READY。");test->addWidget(m_sample,1);
    auto gen=new QPushButton("根据规则生成样例");gen->setObjectName("statusGenerateSample");auto run=new QPushButton("解析测试");run->setObjectName("statusRunTest");for(auto b:{gen,run})b->setStyleSheet("QPushButton {background:#1579ad;color:white;padding:7px;border-radius:3px;}");test->addWidget(gen);test->addWidget(run);connect(gen,&QPushButton::clicked,this,&StatusItemDialog::generateSample);connect(run,&QPushButton::clicked,this,&StatusItemDialog::runTest);
    auto recent=new QPushButton("使用此项最近数据");recent->setToolTip("取已保存监视项最近收到的数据，不会重新发送。尚未保存或已修改来源时请手动输入样例。");test->addWidget(recent);connect(recent,&QPushButton::clicked,this,[this]{
        Rule current;QString error;if(!collect(current,error)){m_output->setPlainText(error);return;}
        if(m_context.session&&current.source==m_original.source)for(const auto &s:m_context.session->snapshot())if(s.id==m_original.id){
            if(current.source==Source::Channel&&current.channel==m_original.channel&&current.converted==m_original.converted&&current.protocolId==m_original.protocolId&&current.protocolShape==m_original.protocolShape&&(current.protocolId.isEmpty()||(current.protocolId==m_context.protocolId&&current.protocolShape==m_context.protocolShape))&&std::isfinite(s.value)){m_sample->setPlainText(QString::number(s.value,'g',15));return;}
            if(current.source==Source::Text)for(const auto &d:m_context.session->textDiagnostics())if(d.sourceId==s.textSourceId&&d.sequence==s.sequence&&d.receiveUnixNs==s.receiveUnixNs){m_sampleFormat->setCurrentIndex(0);m_sample->setPlainText(d.text);return;}
        }
        m_output->setPlainText("尚无可复用的完整实时数据，或来源已修改；请手动填写样例。");
    });
    m_output=new QPlainTextEdit;m_output->setObjectName("statusTestOutput");m_output->setReadOnly(true);test->addWidget(m_output,1);
    auto saveButton=new QPushButton("保存监视项");saveButton->setObjectName("statusSaveItem");saveButton->setToolTip("保存并关闭编辑窗口；Ctrl+S 仅保存、不关闭。不会向设备发送命令。");saveButton->setMinimumSize(210,42);saveButton->setAutoDefault(false);editLayout->addWidget(saveButton);
    saveButton->setStyleSheet("QPushButton {background:#147caf;color:white;font-weight:bold;padding:8px 20px;border:1px solid #116a96;border-radius:4px;} QPushButton:hover {background:#198fca;} QPushButton:pressed {background:#10628d;}");
    auto savedHint=new QLabel;savedHint->setObjectName("statusSavedHint");savedHint->setWordWrap(true);auto cancelButton=new QPushButton("关闭编辑");cancelButton->setAutoDefault(false);cancelButton->setObjectName("statusCloseEditor");editLayout->addWidget(cancelButton);editLayout->addWidget(savedHint);connect(cancelButton,&QPushButton::clicked,this,&QDialog::reject);
    auto save=[this,saveButton,savedHint](bool close){
        // Commit the in-place table editor before reading the model. A shortcut
        // does not move focus by itself, unlike a mouse click on Save.
        if(auto focus=QApplication::focusWidget())if(auto edit=qobject_cast<QLineEdit*>(focus))if((m_table->isAncestorOf(edit)||m_silence->isAncestorOf(edit))&&!edit->hasAcceptableInput()){QMessageBox::warning(this,"配置无效","请先补全当前单元格的数值。");return;}
        saveButton->setFocus(Qt::OtherFocusReason);for(auto spin:findChildren<QAbstractSpinBox*>())spin->interpretText();
        QString error;Rule candidate;if(!collect(candidate,error)){QMessageBox::warning(this,"配置无效",error);return;}if(m_saveHandler&&!m_saveHandler(candidate,error)){QMessageBox::warning(this,"保存失败",error);return;}m_result=candidate;m_original=candidate;savedHint->setText("已保存。Ctrl+S 只保存；点击“保存监视项”保存并关闭。");if(close)accept();
    };
    connect(saveButton,&QPushButton::clicked,this,[save]{save(true);});
    auto shortcut=new QShortcut(QKeySequence(Qt::CTRL|Qt::Key_S),this);shortcut->setObjectName("statusSaveShortcut");shortcut->setContext(Qt::WindowShortcut);shortcut->setAutoRepeat(false);connect(shortcut,&QShortcut::activated,this,[save]{save(false);});
    m_previousSource=rule.source==Source::Text?1:0;m_timeouts[m_previousSource]=rule.timeoutMs;
    m_source->setCurrentIndex(m_previousSource);m_triggerDrafts[m_previousSource]=m_eventTrigger->currentIndex();loadSilenceDraft(m_previousSource);if(!rule.event.silence.isEmpty()){m_silence->setRowCount(0);for(const auto &c:rule.event.silence){addSilence();const int row=m_silence->rowCount()-1;m_silence->item(row,1)->setText(QStringList{"固定数值","数值区间","固定文本"}.value(int(c.kind)));m_silence->item(row,0)->setCheckState(c.enabled?Qt::Checked:Qt::Unchecked);m_silence->item(row,2)->setText(c.kind==SilenceKind::Text?c.text:QString::number(c.kind==SilenceKind::Range?c.lower:c.value,'g',16));m_silence->item(row,3)->setText(c.kind==SilenceKind::Range?QString::number(c.upper,'g',16):QString());updateSilenceRow(row);}}else if(rule.event.enabled)m_silence->setRowCount(0);rebuildTable();
    connect(m_source,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{saveSilenceDraft(m_previousSource);m_triggerDrafts[m_previousSource]=m_eventTrigger->currentIndex();m_timeouts[m_previousSource]=m_timeout->value();m_previousSource=m_source->currentIndex();m_timeout->setValue(m_timeouts[m_previousSource]);m_eventTrigger->setCurrentIndex(m_triggerDrafts[m_previousSource]);loadSilenceDraft(m_previousSource);rebuildTable();});connect(m_kind,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{rebuildTable();});connect(m_mode,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{updateSource();});
    connect(m_shared,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{const auto id=m_shared->currentData().toString();for(const auto &r:m_siblings)if(!id.isEmpty()&&r.textSource.id==id){const auto &t=r.textSource;m_framing->setCurrentIndex(t.datagram);m_encoding->setCurrentText(t.encoding);m_delimiter->setCurrentText(QString::fromLatin1(t.delimiter.toHex().toUpper()));m_trim->setChecked(t.trim);m_case->setChecked(t.caseSensitive);m_sync->setChecked(t.strictSync);m_max->setValue(t.maxBytes);m_partial->setValue(t.partialTimeoutMs);break;}for(QWidget *w:QList<QWidget*>{m_framing,m_encoding,m_delimiter,m_trim,m_case,m_sync,m_max,m_partial})w->setEnabled(id.isEmpty());});
    const auto sharedIndex=m_shared->findData(rule.textSource.id);if(sharedIndex>0)m_shared->setCurrentIndex(sharedIndex);
    connect(m_sample,&QPlainTextEdit::textChanged,this,[this]{if(m_sampleFormat->currentIndex()!=2)return;QString hex=m_sample->toPlainText().toUpper();hex.remove(QRegularExpression("[^0-9A-F]"));if(hex.size()>262144)hex.truncate(262144);QString formatted;for(int i=0;i<hex.size();++i){if(i&&i%2==0)formatted+=' ';formatted+=hex[i];}if(formatted!=m_sample->toPlainText()){QSignalBlocker blocker(m_sample);auto cursor=m_sample->textCursor();const int old=cursor.position();m_sample->setPlainText(formatted);cursor=m_sample->textCursor();cursor.setPosition(qMin(old,formatted.size()));m_sample->setTextCursor(cursor);}});
    connect(m_sampleFormat,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this](int format){if(m_sample->toPlainText().isEmpty()){m_previousFormat=format;return;}QByteArray bytes;QString error;const auto encoding=m_encoding->currentText();if(m_previousFormat==2){if(!DataProtocols::ProtocolProfileValidator::parseHex(m_sample->toPlainText(),bytes))error="当前 HEX 不完整，请先补齐字节。";}else if(m_previousFormat==1)unescape(m_sample->toPlainText(),encoding,bytes,error);else{auto codec=QTextCodec::codecForName(encoding=="ASCII"?QByteArray("ISO-8859-1"):encoding.toLatin1());QTextCodec::ConverterState state(QTextCodec::IgnoreHeader);const auto text=m_sample->toPlainText();bytes=codec->fromUnicode(text.constData(),text.size(),&state);if(state.invalidChars||state.remainingChars)error="当前正文不能用所选编码表示。";}QString converted;if(error.isEmpty()){if(format==2)converted=QString::fromLatin1(bytes.toHex(' ').toUpper());else if(format==1)converted=escaped(bytes);else{int bad;if(!TextEngine::decode(bytes,encoding,converted,bad))error="这些字节不能转换为所选编码的正文。";}}if(!error.isEmpty()){QSignalBlocker block(m_sampleFormat);m_sampleFormat->setCurrentIndex(m_previousFormat);m_output->setPlainText(error);return;}m_previousFormat=format;m_sample->setPlainText(converted);});
    split->setSizes({660,360});
}
void StatusItemDialog::addSilence(){if(m_silence->rowCount()>=64)return;QSignalBlocker block(m_silence);const int row=m_silence->rowCount();m_silence->insertRow(row);for(int c=0;c<4;++c)m_silence->setItem(row,c,new QTableWidgetItem);auto enabled=m_silence->item(row,0);enabled->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable|Qt::ItemIsUserCheckable);enabled->setCheckState(Qt::Checked);m_silence->item(row,1)->setText(m_source->currentIndex()==1?"固定文本":"固定数值");m_silence->item(row,2)->setText(m_source->currentIndex()==1?QString():"0");updateSilenceRow(row);m_silence->selectRow(row);}
void StatusItemDialog::updateSilenceRow(int row){if(row<0||row>=m_silence->rowCount()||!m_silence->item(row,3)||!m_silence->item(row,2)||!m_silence->item(row,1))return;QSignalBlocker block(m_silence);const auto kind=m_silence->item(row,1)->text();auto end=m_silence->item(row,3);end->setFlags(kind=="数值区间"?(Qt::ItemIsEnabled|Qt::ItemIsSelectable|Qt::ItemIsEditable):(Qt::ItemIsEnabled|Qt::ItemIsSelectable));if(kind!="数值区间")end->setText(QString());else if(end->text().isEmpty())end->setText("1");if(kind!="固定文本"){bool ok;const auto value=QLocale::c().toDouble(m_silence->item(row,2)->text(),&ok);if(!ok||!std::isfinite(value))m_silence->item(row,2)->setText("0");}}
void StatusItemDialog::saveSilenceDraft(int source){QVector<QVariantList> rows;for(int i=0;i<m_silence->rowCount();++i)rows.append({m_silence->item(i,0)->checkState()==Qt::Checked,m_silence->item(i,1)->text(),m_silence->item(i,2)->text(),m_silence->item(i,3)->text()});m_silenceDrafts[source]=rows;}
void StatusItemDialog::loadSilenceDraft(int source){QSignalBlocker block(m_silence);if(auto old=m_silence->itemDelegateForColumn(1))old->deleteLater();m_silence->setItemDelegateForColumn(1,new ChoiceCell(source==1?QStringList{"固定文本","固定数值","数值区间"}:QStringList{"固定数值","数值区间"},m_silence));m_silence->setRowCount(0);if(m_silenceDrafts.contains(source)){for(const auto &row:m_silenceDrafts[source]){addSilence();int i=m_silence->rowCount()-1;m_silence->item(i,0)->setCheckState(row[0].toBool()?Qt::Checked:Qt::Unchecked);for(int c=1;c<4;++c)m_silence->item(i,c)->setText(row[c].toString());updateSilenceRow(i);}}else if(source==0)addSilence();m_silence->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);m_silence->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);m_silence->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch);m_silence->horizontalHeader()->setStretchLastSection(true);}
void StatusItemDialog::updateBindingUi(){
    StatusPanels::Panel p;p.bindingMode=StatusPanels::BindingMode::Follow;
    const auto id=m_context.inheritPanelBinding?m_context.panelProtocolId:m_original.protocolId;
    const auto shape=m_context.inheritPanelBinding?m_context.panelProtocolShape:m_original.protocolShape;
    if(!id.isEmpty()){p.bindingMode=StatusPanels::BindingMode::Locked;p.protocolId=id;p.protocolShape=shape;p.protocolName=m_context.panelProtocolName;}
    QString hint=m_context.panelBindingPending?"此面板的旧绑定尚未统一；保留本项原绑定。请到面板设置中统一。\n":QString("继承面板设置 · ");
    hint+=statusBindingSummary(p,m_context);QStringList names=id.isEmpty()?m_context.channelNames:QStringList();
    for(const auto &option:m_context.protocols)if(option.id==id&&option.shape==shape){names=option.channelNames;break;}
    if(id==m_context.protocolId&&shape==m_context.protocolShape&&names.isEmpty())names=m_context.channelNames;
    if(!id.isEmpty()&&!names.isEmpty()&&m_channel->currentIndex()>=names.size())hint+="\n当前通道超出所选协议的已知范围，请检查；通道编号不会自动更改。";
    m_bindingHint->setText(hint);static_cast<WrappedBindingLabel*>(m_bindingHint)->updateMinimumHeight();
    QSignalBlocker block(m_channel);for(int i=0;i<m_channel->count();++i)m_channel->setItemText(i,QString("Ch%1%2").arg(i+1).arg(i<names.size()&&!names[i].isEmpty()?" · "+names[i]:QString()));
}
void StatusItemDialog::updateSource(){
    const bool text=m_source->currentIndex()==1,event=m_mode->currentIndex()==1;const int kind=m_kind->currentIndex();
    m_numberPage->setVisible(!text);m_textPage->setVisible(text);m_bits->setVisible(!text&&kind==1);m_width->setVisible(!text&&kind!=2);
    auto nf=static_cast<QFormLayout*>(m_numberPage->layout());auto widthLabel=static_cast<QLabel*>(nf->labelForField(m_width));widthLabel->setVisible(!text&&kind!=2);widthLabel->setText(kind==0?"数值位宽":"状态字位宽");
    m_threshold->setVisible(!text&&kind==2&&!event);auto hold=m_hold->parentWidget();hold->setVisible(!text&&!event);nf->labelForField(hold)->setVisible(!text&&!event);m_eventBox->setVisible(event);
    auto tx=static_cast<QFormLayout*>(m_textPage->layout());m_every->setVisible(!event);tx->labelForField(m_every)->setVisible(!event);
    m_timeoutTitle->setText(event?"数据静默超时":"状态有效期");
    const QString timeoutTip=event?"0 关闭。事件模式按有效数值帧或合法完整文本消息计算数据活跃时间；屏蔽数据也表示来源仍活跃，但不延长事件提示。异常或断线不会伪装成成功提示。":"无新的有效匹配数据时过期，0 不自动过期。";
    m_timeoutTitle->setToolTip(timeoutTip);m_timeout->setToolTip(timeoutTip);m_timeout->parentWidget()->setToolTip(timeoutTip);
    m_hint->setText((text?QString("按表格顺序匹配；完整匹配是整条正文，前缀匹配保留其后详情。颜色列以色块展示，棋盘格表示默认/透明；双击选择颜色，“使用透明颜色”恢复主题默认，文字不会隐藏。"):QString("映射/位段只接受整数；阈值空下限/上限表示无界，默认区间左闭右开。颜色列以色块展示，棋盘格表示默认/透明；双击选择颜色，“使用透明颜色”恢复主题默认。"))+(event?"\n事件模式立即触发；屏蔽/未匹配内容不覆盖提示，单帧也可捕获。新事件显示在卡片上，历史记录最多 2000 条。":""));
    m_sampleFormat->setEnabled(text);m_chunks->setEnabled(text);
}
void StatusItemDialog::rebuildTable(){
    if(m_tableKey>=0){QVector<QVariantList> saved;for(int i=0;i<m_table->rowCount();++i){QVariantList row;for(int c=0;c<m_table->columnCount();++c){auto cell=m_table->item(i,c);row.append(QVariant(QVariantList{cell->text(),cell->data(Qt::CheckStateRole),cell->data(Qt::UserRole)}));}saved.append(row);}m_drafts[m_tableKey]=saved;}
    updateSource();for(int c=0;c<m_table->columnCount();++c)m_table->setItemDelegateForColumn(c,nullptr);m_table->clear();m_table->setRowCount(0);const bool text=m_source->currentIndex()==1,range=!text&&m_kind->currentIndex()==2;m_tableKey=text?3:m_kind->currentIndex();
    QStringList headers=text?QStringList{"匹配方式","匹配正文","状态名称","颜色","严重程度","说明","启用"}:range?QStringList{"下限（空为无界）","含下限","上限（空为无界）","含上限","状态名称","启用恢复回差","颜色","严重程度","说明"}:QStringList{"值 / 位段结果","状态名称","颜色","严重程度","说明"};
    m_table->setColumnCount(headers.size());m_table->setHorizontalHeaderLabels(headers);m_table->setItemDelegate(new QStyledItemDelegate(m_table));
    if(text){m_table->setItemDelegateForColumn(0,new ChoiceCell({"完整匹配","前缀匹配"},m_table));m_table->setItemDelegateForColumn(4,new ChoiceCell({"无","提示","警告","错误"},m_table));}
    else if(range){m_table->setItemDelegateForColumn(0,new NumericCell(false,m_table));m_table->setItemDelegateForColumn(2,new NumericCell(false,m_table));m_table->setItemDelegateForColumn(7,new ChoiceCell({"无","提示","警告","错误"},m_table));}
    else {m_table->setItemDelegateForColumn(0,new NumericCell(true,m_table));m_table->setItemDelegateForColumn(3,new ChoiceCell({"无","提示","警告","错误"},m_table));}
    m_table->setItemDelegateForColumn(text?3:range?6:2,new RuleColorCell(m_table));
    const auto &r=m_original;int rows=text&&r.source==Source::Text?r.textMatches.size():range&&r.kind==RuleKind::Threshold?r.ranges.size():!text&&!range&&r.source==Source::Channel&&r.kind!=RuleKind::Threshold?r.mappings.size():0;
    for(int i=0;i<rows;++i){addRow();auto put=[&](int col,const QString &s){m_table->item(i,col)->setText(s);};if(text){const auto &v=r.textMatches[i];put(0,v.prefix?"前缀匹配":"完整匹配");put(1,v.text);put(2,v.state);put(3,v.color);put(4,v.severity.isEmpty()?"无":v.severity);put(5,v.note);m_table->item(i,6)->setCheckState(v.enabled?Qt::Checked:Qt::Unchecked);m_table->item(i,0)->setData(Qt::UserRole,v.id);}else if(range){const auto &v=r.ranges[i];put(0,std::isfinite(v.lower)?QString::number(v.lower,'g',16):"");put(2,std::isfinite(v.upper)?QString::number(v.upper,'g',16):"");put(4,v.state);m_table->item(i,1)->setCheckState(v.includeLower?Qt::Checked:Qt::Unchecked);m_table->item(i,3)->setCheckState(v.includeUpper?Qt::Checked:Qt::Unchecked);m_table->item(i,5)->setCheckState(v.retainWithHysteresis?Qt::Checked:Qt::Unchecked);}else{const auto &v=r.mappings[i];put(0,QString::number(v.value));put(1,v.state);put(2,v.color);put(3,v.severity.isEmpty()?"无":v.severity);put(4,v.note);}}
    if(!rows){addRow();if(!text&&!range){m_table->item(0,0)->setText("0");m_table->item(0,1)->setText("状态0");addRow();m_table->item(1,0)->setText("1");m_table->item(1,1)->setText("状态1");}}
    if(range&&r.kind==RuleKind::Threshold)for(int i=0;i<rows;++i){m_table->item(i,6)->setText(r.ranges[i].color);m_table->item(i,7)->setText(r.ranges[i].severity.isEmpty()?"无":r.ranges[i].severity);m_table->item(i,8)->setText(r.ranges[i].note);}
    if(m_drafts.contains(m_tableKey)){const auto saved=m_drafts[m_tableKey];m_table->setRowCount(0);for(int i=0;i<saved.size();++i){addRow();for(int c=0;c<qMin(saved[i].size(),m_table->columnCount());++c){const auto data=saved[i][c].toList();m_table->item(i,c)->setText(data.value(0).toString());m_table->item(i,c)->setData(Qt::CheckStateRole,data.value(1));m_table->item(i,c)->setData(Qt::UserRole,data.value(2));}}}
    m_table->resizeColumnsToContents();for(int c=0;c<headers.size();++c)m_table->setColumnWidth(c,qBound(85,m_table->columnWidth(c)+16,240));
}
void StatusItemDialog::addRow(){if(m_table->rowCount()>=256)return;int row=m_table->rowCount();m_table->insertRow(row);for(int c=0;c<m_table->columnCount();++c)m_table->setItem(row,c,new QTableWidgetItem);
    const bool text=m_source->currentIndex()==1,range=!text&&m_kind->currentIndex()==2;
    auto check=[&](int c,bool value){auto item=m_table->item(row,c);item->setFlags((item->flags()|Qt::ItemIsUserCheckable)&~Qt::ItemIsEditable);item->setCheckState(value?Qt::Checked:Qt::Unchecked);};
    const int stateColumn=text?2:range?4:1;QString name;int number=row;for(;;){name="状态"+QString::number(number++);bool exists=false;for(int i=0;i<row;++i)if(m_table->item(i,stateColumn)->text()==name)exists=true;if(!exists)break;}m_table->item(row,stateColumn)->setText(name);
    if(text){m_table->item(row,0)->setText("完整匹配");m_table->item(row,0)->setData(Qt::UserRole,StatusPanels::newId());m_table->item(row,4)->setText("无");check(6,true);}else if(range){check(1,true);check(3,false);check(5,false);}else{m_table->item(row,0)->setText(QString::number(row));m_table->item(row,3)->setText("无");}}
QVector<QVariantList> StatusItemDialog::tableSnapshot()const {QVector<QVariantList> saved;for(int i=0;i<m_table->rowCount();++i){QVariantList row;for(int c=0;c<m_table->columnCount();++c){auto cell=m_table->item(i,c);row.append(QVariant(QVariantList{cell->text(),cell->data(Qt::CheckStateRole),cell->data(Qt::UserRole)}));}saved.append(row);}return saved;}
void StatusItemDialog::rememberRules(){auto &history=m_undo[m_tableKey];if(history.size()>=100)history.removeFirst();history.append(tableSnapshot());}
void StatusItemDialog::undoRules(){auto &history=m_undo[m_tableKey];if(history.isEmpty())return;const auto saved=history.takeLast();m_table->setRowCount(0);for(int i=0;i<saved.size();++i){addRow();for(int c=0;c<saved[i].size();++c){const auto data=saved[i][c].toList();auto item=m_table->item(i,c);item->setText(data.value(0).toString());item->setData(Qt::CheckStateRole,data.value(1));item->setData(Qt::UserRole,data.value(2));}}if(m_table->rowCount())m_table->selectRow(m_table->rowCount()-1);}
void StatusItemDialog::moveRow(int direction){int row=m_table->currentRow(),target=row+direction;if(row<0||target<0||target>=m_table->rowCount())return;rememberRules();for(int c=0;c<m_table->columnCount();++c){auto a=m_table->takeItem(row,c);auto b=m_table->takeItem(target,c);m_table->setItem(row,c,b);m_table->setItem(target,c,a);}m_table->selectRow(target);}
bool StatusItemDialog::collect(Rule &r,QString &error)const {
    r=m_original;r.name=m_name->text().trimmed();r.note=m_note->toPlainText();r.enabled=m_enabled->isChecked();r.timeoutMs=m_timeout->value();r.mappings.clear();r.ranges.clear();r.textMatches.clear();
    auto cell=[&](int row,int col){return m_table->item(row,col)?m_table->item(row,col)->text():QString();};
    const bool text=m_source->currentIndex()==1;r.source=text?Source::Text:Source::Channel;
    if(text){r.kind=RuleKind::TextMatch;r.holdMs=0;r.hysteresis=0;r.protocolId.clear();r.protocolShape.clear();auto &t=r.textSource;t.id=m_shared->currentData().toString();if(t.id.isEmpty())t.id=m_independentSourceId;t.encoding=m_encoding->currentText();t.datagram=m_framing->currentIndex()==1;t.trim=m_trim->isChecked();t.caseSensitive=m_case->isChecked();t.strictSync=m_sync->isChecked();t.maxBytes=m_max->value();t.partialTimeoutMs=m_partial->value();r.recordEveryMatch=m_every->isChecked();
        if(!DataProtocols::ProtocolProfileValidator::parseHex(m_delimiter->currentText(),t.delimiter)||(!t.datagram&&t.delimiter.isEmpty())){error="结束符必须是完整 HEX 字节，且串流结束符不能为空。";return false;}
        for(int i=0;i<m_table->rowCount();++i){auto id=m_table->item(i,0)->data(Qt::UserRole).toString();if(id.isEmpty())id=StatusPanels::newId();r.textMatches.append({cell(i,1),cell(i,2),cell(i,0)=="前缀匹配",id,cell(i,3),cell(i,4),cell(i,5),m_table->item(i,6)->checkState()==Qt::Checked});if(!cell(i,3).isEmpty()&&!QColor(cell(i,3)).isValid()){error="状态颜色无效，请双击颜色单元格选择。";return false;}}
    }else{r.kind=RuleKind(m_kind->currentIndex());r.channel=m_channel->currentIndex();r.converted=m_value->currentIndex()==1;r.width=m_width->currentText().toInt();r.firstBit=m_first->value();r.bitCount=m_count->value();r.holdMs=m_hold->value();r.hysteresis=m_hysteresis->value();
        if(m_context.panelBindingPending){bool existing=false;for(const auto &s:m_siblings)if(s.id==m_original.id)existing=true;if(!existing||m_original.source!=Source::Channel){error="请先到面板设置中统一旧协议绑定，再添加通道数值监视。";return false;}}r.protocolId=m_context.inheritPanelBinding?m_context.panelProtocolId:m_original.protocolId;r.protocolShape=m_context.inheritPanelBinding?m_context.panelProtocolShape:m_original.protocolShape;if(r.protocolId=="builtin.raw"){error="Raw 不输出数值通道，请在面板设置中更换协议。";return false;}
        for(int i=0;i<m_table->rowCount();++i)if(r.kind==RuleKind::Threshold){Range v;bool ok=true;if(!cell(i,0).trimmed().isEmpty())v.lower=QLocale::c().toDouble(cell(i,0),&ok);if(!ok){error="下限必须为数字或留空。";return false;}if(!cell(i,2).trimmed().isEmpty())v.upper=QLocale::c().toDouble(cell(i,2),&ok);if(!ok){error="上限必须为数字或留空。";return false;}v.includeLower=m_table->item(i,1)->checkState()==Qt::Checked;v.includeUpper=m_table->item(i,3)->checkState()==Qt::Checked;v.state=cell(i,4);v.retainWithHysteresis=m_table->item(i,5)->checkState()==Qt::Checked;r.ranges.append(v);}else{bool ok;auto n=cell(i,0).toULongLong(&ok);if(!ok||n>4294967295ULL){error="映射值必须为 0–4294967295 的整数。";return false;}if(!cell(i,2).isEmpty()&&!QColor(cell(i,2)).isValid()){error="状态颜色无效，请双击颜色单元格选择。";return false;}r.mappings.append({quint32(n),cell(i,1),cell(i,2),cell(i,3),cell(i,4)});}
    }
    if(r.source==Source::Channel&&r.kind==RuleKind::Threshold)for(int i=0;i<r.ranges.size();++i){r.ranges[i].color=cell(i,6);r.ranges[i].severity=cell(i,7);r.ranges[i].note=cell(i,8);if(!r.ranges[i].color.isEmpty()&&!QColor(r.ranges[i].color).isValid()){error="阈值状态颜色无效。";return false;}}
    r.event.enabled=m_mode->currentIndex()==1;
    if(r.event.enabled){r.event.displayMs=m_eventDuration->value();r.event.onChange=m_eventTrigger->currentIndex()==0;r.event.silence.clear();r.holdMs=0;r.hysteresis=0;
        for(int i=0;i<m_silence->rowCount();++i){SilenceCondition c;c.enabled=m_silence->item(i,0)->checkState()==Qt::Checked;const auto kind=m_silence->item(i,1)->text();const auto content=m_silence->item(i,2)->text();bool ok=true;if(kind=="固定文本"){c.kind=SilenceKind::Text;c.text=content;}else if(kind=="固定数值"){c.kind=SilenceKind::Number;c.value=QLocale::c().toDouble(content,&ok);}else if(kind=="数值区间"){c.kind=SilenceKind::Range;c.lower=QLocale::c().toDouble(content,&ok);if(ok)c.upper=QLocale::c().toDouble(m_silence->item(i,3)->text(),&ok);}else ok=false;if(!ok){error=QString("第 %1 条屏蔽条件的类型或数字无效。").arg(i+1);return false;}r.event.silence.append(c);}
    }else if(r.source!=m_original.source){r.event=EventOptions();r.event.onChange=r.source==Source::Channel;}
    return Engine::validate({r},error);
}
void StatusItemDialog::generateSample(){
    Rule r;QString error;if(!collect(r,error)){m_output->setPlainText(error);return;}
    if(r.source==Source::Channel){
        double value=0;
        if(r.kind==RuleKind::Threshold){const auto &range=r.ranges.first();
            if(std::isfinite(range.lower)&&std::isfinite(range.upper))value=range.lower/2+range.upper/2;
            else if(std::isfinite(range.lower))value=range.includeLower?range.lower:std::nextafter(range.lower,std::numeric_limits<double>::infinity());
            else if(std::isfinite(range.upper))value=range.includeUpper?range.upper:std::nextafter(range.upper,-std::numeric_limits<double>::infinity());
            if(!std::isfinite(value)||value<range.lower||value>range.upper||(value==range.lower&&!range.includeLower)||(value==range.upper&&!range.includeUpper)){m_output->setPlainText("首个区间内没有可生成的有限数值，请手动输入。");return;}
        }else {bool found=false;for(const auto &mapping:r.mappings){value=double(quint64(mapping.value)<<(r.kind==RuleKind::Bits?r.firstBit:0));if(!r.event.enabled||!silencesNumber(r.event,value)){found=true;break;}}if(!found){m_output->setPlainText("所有可自动生成的映射样例都被屏蔽，请调整屏蔽条件或手动输入。");return;}}
        if(r.event.enabled&&silencesNumber(r.event,value)){bool found=false;for(const auto &range:r.ranges){const QVector<double> candidates{1,-1,range.lower,range.upper,std::nextafter(range.lower,std::numeric_limits<double>::infinity()),std::nextafter(range.upper,-std::numeric_limits<double>::infinity()),range.lower/2+range.upper/2};for(double candidate:candidates)if(std::isfinite(candidate)&&(candidate>range.lower||(range.includeLower&&candidate==range.lower))&&(candidate<range.upper||(range.includeUpper&&candidate==range.upper))&&!silencesNumber(r.event,candidate)){value=candidate;found=true;break;}if(found)break;}if(!found){m_output->setPlainText("自动样例被屏蔽，请调整屏蔽条件或手动输入。");return;}}
        m_sample->setPlainText(QString::number(value,'g',17));return;
    }
    if(m_sampleFormat->currentIndex()==0){for(const auto &row:r.textMatches)if(row.enabled){auto value=row.text+(row.prefix?"example":"");if(r.event.enabled&&silencesText(r,value))continue;m_sample->setPlainText(value);return;}m_output->setPlainText("没有未屏蔽的自动样例，请检查规则及屏蔽条件，或手动输入。");return;}
    auto samples=generateTextSamples(r,error);if(samples.isEmpty()){m_output->setPlainText(error.isEmpty()?"请先启用至少一条匹配规则。":error);return;}
    m_sample->setPlainText(m_sampleFormat->currentIndex()==2?QString::fromLatin1(samples.first().toHex(' ').toUpper()):escaped(samples.first()));m_output->setPlainText("已生成首条启用规则的样例；前缀后的 example 可修改。");
}
void StatusItemDialog::runTest(){m_output->setStyleSheet("QPlainTextEdit {color:#d24b4b;}");Rule r;QString error;if(m_sample->toPlainText().size()>262144){m_output->setPlainText("样例过大，请使用不超过 256 Ki 个字符的测试数据。");return;}if(!collect(r,error)){m_output->setPlainText("配置错误："+error);return;}QString result;
    if(r.source==Source::Channel){bool ok;double n;if(m_sample->toPlainText().trimmed().startsWith("0x",Qt::CaseInsensitive))n=m_sample->toPlainText().trimmed().mid(2).toULongLong(&ok,16);else n=QLocale::c().toDouble(m_sample->toPlainText().trimmed(),&ok);if(!ok||!std::isfinite(n)){m_output->setPlainText("请输入有限数值，例如 5 或 0x05。");return;}Engine engine;r.protocolId.clear();r.protocolShape.clear();engine.configure({r},0,error);engine.reset(true,"",{},0);QVector<double> values(r.channel+1,qQNaN());values[r.channel]=n;engine.consume(values,values,0);if(r.holdMs)engine.consume(values,values,r.holdMs);const auto state=engine.states().first();result=qualityText(state.quality)+" · "+state.state+"\n"+state.reason;if(r.holdMs)result+="\n已按生效延迟模拟稳定输入。";if(r.event.enabled)result+=QString("\n事件触发 %1 次 · 已屏蔽 %2 条。提示与实时监视隔离。").arg(state.matches).arg(state.suppressed);
    }else{TextTestResult out;if(m_sampleFormat->currentIndex()==0)out=testTextMessage(r,m_sample->toPlainText(),error);else{QByteArray bytes;if(m_sampleFormat->currentIndex()==2){if(!DataProtocols::ProtocolProfileValidator::parseHex(m_sample->toPlainText(),bytes))error="HEX 样例必须为完整十六进制字节。";}else unescape(m_sample->toPlainText(),r.textSource.encoding,bytes,error);if(error.isEmpty()){QVector<QByteArray> chunks;const int step=r.textSource.datagram||m_chunks->currentIndex()==0?qMax(1,bytes.size()):m_chunks->currentIndex()==1?1:7;for(int i=0;i<bytes.size();i+=step)chunks.append(bytes.mid(i,step));out=testTextStream({r},chunks,r.textSource.datagram,error);}}
        if(!error.isEmpty())result="解析测试失败："+error;else{for(const auto &d:out.messages)result+=QString("[%1, %2) %3\n%4\n").arg(d.byteStart).arg(d.byteEnd).arg(d.reason,d.text);if(!out.states.isEmpty()){auto s=out.states.first();result+="结果："+qualityText(s.quality)+" · "+s.state;if(s.matchedRow>=0)result+=QString("（规则 %1）").arg(s.matchedRow+1);if(r.event.enabled)result+=QString("\n事件触发 %1 次 · 已屏蔽 %2 条。").arg(s.matches).arg(s.suppressed);if(s.quality==Quality::Waiting&&s.suppressed==0)result+="\n尚未得到完整匹配消息，请检查结束符、正文及分包设置。";}}}
    const bool failed=!error.isEmpty()||result.contains("编码错误")||result.contains("控制字符")||result.contains("数据异常")||result.contains("超过消息");m_output->setStyleSheet(failed?"QPlainTextEdit {color:#d24b4b;}":QString());m_output->setPlainText(result);
}
