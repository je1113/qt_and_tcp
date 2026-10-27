#pragma once

#include <QAbstractSocket>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTcpSocket;

class MainWindow : public QWidget
{
    Q_OBJECT  // 시그널/슬롯을 쓰는 클래스에 필요. moc가 이 매크로를 보고 코드를 생성한다.

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onStateChanged(QAbstractSocket::SocketState state);
    void onErrorOccurred(QAbstractSocket::SocketError error);

private:
    QLineEdit *hostEdit;
    QSpinBox *portSpin;
    QPushButton *connectButton;
    QPushButton *disconnectButton;
    QLabel *statusLabel;

    QTcpSocket *socket;
};
