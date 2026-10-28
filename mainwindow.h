#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QVector>
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
    ~MainWindow() override;

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onStateChanged(QAbstractSocket::SocketState state);
    void onErrorOccurred(QAbstractSocket::SocketError error);
    void onReadyRead();

private:
    void handleMessage(const QByteArray &body);

    // 길이 헤더 크기(uint32)와, 이보다 큰 길이는 잘못된 데이터로 보는 상한.
    static constexpr int HeaderSize = 4;
    static constexpr quint32 MaxBodySize = 1024 * 1024;

    QLineEdit *hostEdit;
    QSpinBox *portSpin;
    QPushButton *connectButton;
    QPushButton *disconnectButton;
    QLabel *statusLabel;
    QVector<QLabel *> jointLabels;
    QLabel *seqLabel;

    QTcpSocket *socket;
    QByteArray buffer;  // 아직 메시지 하나로 완성되지 않은 수신 바이트
    int lastSeq = -1;
};
