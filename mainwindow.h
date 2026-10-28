#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QJsonObject>
#include <QVector>
#include <QWidget>

class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTcpSocket;
class QTimer;

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
    void onMoveJClicked();
    void onHomeClicked();
    void onStopClicked();
    void onHeartbeatTimeout();
    void onWatchdogTimeout();
    void onAckTimeout();
    void onPickClicked();

private:
    void startConnecting();
    void scheduleReconnect();
    void updateControls();
    void handleMessage(const QByteArray &body);
    void applyStatus(const QJsonObject &status);
    void handleQueryResult(const QJsonObject &msg);
    void confirmPendingCommand(bool ok, const QString &error = QString());
    void resendPendingCommand();
    void sendQuery();
    void sendCommand(QJsonObject command);
    void sendMessage(const QJsonObject &msg);
    void appendLog(const QString &text);

    // 피스피킹 시퀀스
    void runPickStep();
    void tryAdvancePicking();
    void abortPicking(const QString &reason);

    // 길이 헤더 크기(uint32)와, 이보다 큰 길이는 잘못된 데이터로 보는 상한.
    static constexpr int HeaderSize = 4;
    static constexpr quint32 MaxBodySize = 1024 * 1024;

    // 시간 설정 [ms]
    static constexpr int HeartbeatIntervalMs = 1000;     // 이 간격으로 브리지에 heartbeat를 보낸다.
    static constexpr int HeartbeatTimeoutMs = 3000;      // 이 시간 동안 아무것도 안 오면 연결이 죽은 것으로 본다.
    static constexpr int InitialReconnectDelayMs = 1000; // 첫 재연결 대기 시간
    static constexpr int MaxReconnectDelayMs = 8000;     // 재연결 대기 시간의 최댓값
    static constexpr int AckTimeoutMs = 2000;            // 명령을 보내고 이 시간 안에 ack가 없으면 실제 상태 재조회
    static constexpr int MaxQueryRetries = 3;            // 재조회도 응답이 없을 때 다시 물어보는 최대 횟수

    QLineEdit *hostEdit;
    QSpinBox *portSpin;
    QPushButton *connectButton;
    QPushButton *disconnectButton;
    QLabel *statusLabel;

    QLabel *robotStateLabel;
    QLabel *gripperLabel;
    QVector<QLabel *> jointLabels;
    QLabel *seqLabel;

    QVector<QDoubleSpinBox *> targetSpins;
    QPushButton *moveJButton;
    QPushButton *homeButton;
    QPushButton *stopButton;
    QPushButton *pickButton;
    QLabel *pickLabel;

    QPlainTextEdit *logView;

    QTcpSocket *socket;
    QByteArray buffer;  // 아직 메시지 하나로 완성되지 않은 수신 바이트
    int lastSeq = -1;
    int nextCommandId = 1;  // 명령마다 붙이는 번호. ack가 어느 명령에 대한 것인지 맞춰 볼 때 쓴다.
    // 앱이 켜질 때마다 새로 만드는 이름표. 앱을 다시 켜면 id가 1부터 다시 시작하므로,
    // 브리지가 "이전 실행의 1번"과 "이번 실행의 1번"을 구분할 수 있게 모든 명령에 붙인다.
    QString sessionId;

    QTimer *heartbeatTimer;  // 반복: 주기적으로 heartbeat 전송
    QTimer *watchdogTimer;   // 한 번: 데이터가 올 때마다 다시 시작. 울리면 "한동안 소식 없음"
    QTimer *reconnectTimer;  // 한 번: 울리면 재연결 시도
    QTimer *ackTimer;        // 한 번: 울리면 "ack가 안 옴"

    bool autoReconnect = false;  // [연결]을 누르면 true, [해제]를 누르면 false
    int reconnectDelayMs = InitialReconnectDelayMs;
    QString robotState;          // 마지막으로 받은 로봇 state ("IDLE"/"MOVING"). 비어 있으면 모름

    // 보냈지만 실행 여부를 아직 확정하지 못한 명령(id 포함). 비어 있으면 없음.
    // 재연결 뒤에도 남겨 두었다가, 실제 상태를 조회해서 실행됐는지 확인한다.
    QJsonObject pendingCommand;
    int pendingQueryId = 0;      // 응답을 기다리는 query의 id. 0이면 없음
    int queryRetries = 0;

    struct PickStep {
        QString name;
        QJsonObject command;
    };
    QVector<PickStep> pickSteps;
    bool picking = false;
    int pickStep = 0;            // 지금 실행 중인 단계 (0부터)
    bool stepAcked = false;      // 지금 단계의 명령이 실행된 것을 확인했는지
};
