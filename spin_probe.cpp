#include <QApplication>
#include <QSpinBox>
#include <QLineEdit>
#include <QStyleFactory>
#include <QStyle>
#include <QTest>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QFontMetrics>
#include <QDebug>

// The port field's production configuration, with theme colors resolved to
// arbitrary opaque colors: no radio, model, settings, or transport involved.
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QJsonArray results;
    int unexpected = 0;
    for (const QString& styleName : {QStringLiteral("Fusion"), QStringLiteral("Windows")}) {
        app.setStyle(QStyleFactory::create(styleName));
        for (bool fixed : {false, true}) {
            for (const QString& port : {QStringLiteral("1"), QStringLiteral("9999"),
                                       QStringLiteral("10000"), QStringLiteral("12345"),
                                       QStringLiteral("65535")}) {
                QSpinBox spin;
                spin.setRange(1, 65535);
                spin.setValue(10000);
                if (!fixed) {
                    spin.setButtonSymbols(QAbstractSpinBox::NoButtons);
                }
                spin.setFixedWidth(52);
                spin.setAlignment(Qt::AlignRight);
                QString sheet = QStringLiteral(
                    "QSpinBox { background: #172736; border: 1px solid #172736; "
                    "border-radius: 3px; padding: 2px 4px; color: white; font-size: 10px; }");
                if (fixed) {
                    sheet += QStringLiteral(
                        "QSpinBox::up-button, QSpinBox::down-button { width: 0; }");
                }
                spin.setStyleSheet(sheet);
                spin.show();
                app.processEvents();
                QLineEdit* edit = spin.findChild<QLineEdit*>();
                QTest::mouseClick(edit, Qt::LeftButton);
                QTest::keySequence(edit, QKeySequence::SelectAll);
                QTest::keyClicks(edit, port);
                QTest::keyClick(edit, Qt::Key_Tab);
                app.processEvents();
                const int textWidth = edit->fontMetrics().horizontalAdvance(port);
                const bool fits = edit->width() >= textWidth + 4;
                const bool accepted = spin.value() == port.toInt();
                if (!accepted || fits != fixed) {
                    ++unexpected;
                }
                results.append(QJsonObject{{"style", styleName}, {"fixed", fixed},
                    {"port", port}, {"editorWidth", edit->width()},
                    {"textWidth", textWidth}, {"fits", fits}, {"accepted", accepted}});
            }
        }
    }
    QFile output(QStringLiteral("/tmp/fb-6519/keyboard-matrix.json"));
    if (!output.open(QIODevice::WriteOnly)) {
        return 2;
    }
    output.write(QJsonDocument(results).toJson());
    qInfo() << results.size() << "keyboard cases; unexpected results:" << unexpected;
    return unexpected == 0 ? 0 : 1;
}
