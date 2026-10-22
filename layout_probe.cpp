#include <QApplication>
#include <QLabel>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
QPixmap makeWaveformDocumentPixmap() { QPixmap p(56,56); p.fill(Qt::transparent); return p; }
QWidget* makeEmptyWaveformsState(QWidget* parent = nullptr)
{
    auto* state = new QWidget(parent);
    state->setAccessibleName(QObject::tr("No radio waveforms installed"));
    auto* layout = new QVBoxLayout(state);
    layout->setContentsMargins(12, 18, 12, 18);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignCenter);

    auto* icon = new QLabel(state);
    icon->setObjectName(QStringLiteral("EmptyStateIcon"));
    icon->setPixmap(makeWaveformDocumentPixmap());
    icon->setAlignment(Qt::AlignCenter);
    icon->setAccessibleName(QObject::tr("Waveform empty state icon"));
    layout->addWidget(icon, 0, Qt::AlignHCenter);
    layout->addSpacing(6);

    auto* text = new QLabel(QObject::tr("No radio waveforms installed"), state);
    text->setObjectName(QStringLiteral("EmptyStateText"));
    text->setAlignment(Qt::AlignCenter);
    text->setAccessibleName(QObject::tr("No radio waveforms installed"));
    layout->addWidget(text, 0, Qt::AlignHCenter);
    layout->addSpacing(4);

    auto* subtext = new QLabel(QObject::tr("Use Install... to add waveforms to your radio."), state);
    subtext->setObjectName(QStringLiteral("EmptyStateSubtext"));
    subtext->setAlignment(Qt::AlignCenter);
    subtext->setWordWrap(true);
    subtext->setAccessibleName(QObject::tr("Install radio waveforms hint"));
    layout->addWidget(subtext, 0, Qt::AlignHCenter);

    return state;
}
QWidget* makeFixedEmptyWaveformsState(QWidget* parent = nullptr)
{
    auto* state = new QWidget(parent);
    state->setAccessibleName(QObject::tr("No radio waveforms installed"));
    auto* layout = new QVBoxLayout(state);
    layout->setContentsMargins(12, 18, 12, 18);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignCenter);

    auto* icon = new QLabel(state);
    icon->setObjectName(QStringLiteral("EmptyStateIcon"));
    icon->setPixmap(makeWaveformDocumentPixmap());
    icon->setAlignment(Qt::AlignCenter);
    icon->setAccessibleName(QObject::tr("Waveform empty state icon"));
    layout->addWidget(icon, 0, Qt::AlignHCenter);
    layout->addSpacing(6);

    auto* text = new QLabel(QObject::tr("No radio waveforms installed"), state);
    text->setObjectName(QStringLiteral("EmptyStateText"));
    text->setAlignment(Qt::AlignCenter);
    text->setAccessibleName(QObject::tr("No radio waveforms installed"));
    layout->addWidget(text, 0, Qt::AlignHCenter);
    layout->addSpacing(4);

    auto* subtext = new QLabel(QObject::tr("Use Install... to add waveforms to your radio."), state);
    subtext->setObjectName(QStringLiteral("EmptyStateSubtext"));
    subtext->setAlignment(Qt::AlignCenter);
    subtext->setWordWrap(true);
    subtext->setAccessibleName(QObject::tr("Install radio waveforms hint"));
    layout->addWidget(subtext);

    return state;
}

int main(int argc,char**argv) {
 QApplication app(argc,argv); QJsonArray results;
 for(bool fixed:{false,true}) for(int px:{12,14,16,20,24}) for(int width:{300,450,900}) {
 QWidget window; QFont font=window.font();font.setPixelSize(px);window.setFont(font);
 window.setStyleSheet("QLabel#EmptyStateText { font-size:14px; font-weight:700; } QLabel#EmptyStateIcon {min-height:56px; max-height:56px; min-width:56px; max-width:56px;}");
 auto *outer = new QVBoxLayout(&window);
 auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); outer->addWidget(scroll);
 auto *container = new QWidget; auto *layout = new QVBoxLayout(container); layout->setContentsMargins(0,0,0,0); layout->setSpacing(4); layout->addStretch(); scroll->setWidget(container);
 layout->insertWidget(0,fixed?makeFixedEmptyWaveformsState(container):makeEmptyWaveformsState(container)); window.resize(width,400); window.show();
 for(int i=0;i<3;++i)app.processEvents();
 auto *hint=window.findChild<QLabel*>("EmptyStateSubtext");
 if(px==12 && width==450)hint->grab().save(fixed?"/tmp/fb-6510/layout-after.png":"/tmp/fb-6510/layout-before.png");
 results.append(QJsonObject{{"fixed",fixed},{"fontPixels",px},{"windowWidth",width},{"width",hint->width()},{"height",hint->height()},{"requiredHeight",hint->heightForWidth(hint->width())},{"fits",hint->height()>=hint->heightForWidth(hint->width())}});
 }
 puts(QJsonDocument(results).toJson().constData());
}
