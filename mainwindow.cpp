#include "mainwindow.h"

#include <QDebug>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpSocket>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>
#include <QtEndian>

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle("TCP Chat");
    resize(560, 640);

    // 위젯들에 this(부모)를 주면, 창이 삭제될 때 Qt가 자식들도 함께 delete 해준다.
    hostEdit = new QLineEdit("127.0.0.1", this);
    portSpin = new QSpinBox(this);
    portSpin->setRange(1, 65535);
    portSpin->setValue(5000);
    connectButton = new QPushButton("연결", this);
    disconnectButton = new QPushButton("해제", this);
    statusLabel = new QLabel(this);

    // 현재 상태: state + 관절 J1~J6 + seq를 "이름: 값" 표로 보여준다.
    // (레이아웃에 넣으면 부모가 자동으로 정해진다)
    auto *jointForm = new QFormLayout;
    robotStateLabel = new QLabel("-");
    jointForm->addRow("state", robotStateLabel);
    gripperLabel = new QLabel("-");
    jointForm->addRow("gripper", gripperLabel);
    for (int i = 0; i < 6; ++i) {
        auto *label = new QLabel("-");
        jointForm->addRow(QString("J%1").arg(i + 1), label);
        jointLabels.append(label);
    }
    seqLabel = new QLabel("-");
    jointForm->addRow("seq", seqLabel);

    auto *currentBox = new QGroupBox("현재");
    currentBox->setLayout(jointForm);

    // 명령: 목표 관절각 입력 + 버튼들
    auto *targetForm = new QFormLayout;
    const double defaultTarget[6] = {0, 0, 90, 0, 90, 0};
    for (int i = 0; i < 6; ++i) {
        auto *spin = new QDoubleSpinBox;
        spin->setRange(-360.0, 360.0);
        spin->setDecimals(2);
        spin->setSuffix("°");
        spin->setValue(defaultTarget[i]);
        targetForm->addRow(QString("J%1").arg(i + 1), spin);
        targetSpins.append(spin);
    }

    moveJButton = new QPushButton("MoveJ");
    homeButton = new QPushButton("홈");
    stopButton = new QPushButton("정지");

    auto *commandButtons = new QHBoxLayout;
    commandButtons->addWidget(moveJButton);
    commandButtons->addWidget(homeButton);
    commandButtons->addWidget(stopButton);

    pickButton = new QPushButton("피킹 시작");
    pickLabel = new QLabel("피킹: 대기");
    auto *pickRow = new QHBoxLayout;
    pickRow->addWidget(pickButton);
    pickRow->addWidget(pickLabel, 1);

    auto *commandLayout = new QVBoxLayout;
    commandLayout->addLayout(targetForm);
    commandLayout->addLayout(commandButtons);
    commandLayout->addLayout(pickRow);

    // 피스피킹 시퀀스: 집을 위치에서 물건을 집어 놓을 위치에 놓고 홈으로 돌아온다.
    // 관절 각도는 연습용으로 정한 값이다(실제 작업 위치가 아님).
    const auto movej = [](std::initializer_list<double> joints) {
        QJsonArray array;
        for (double j : joints)
            array.append(j);
        return QJsonObject{{"cmd", "movej"}, {"joints", array}};
    };
    const QJsonObject pickAbove = movej({-30, 10, 80, 0, 90, 0});
    const QJsonObject pickDown = movej({-30, 25, 85, 0, 70, 0});
    const QJsonObject placeAbove = movej({40, 10, 80, 0, 90, 0});
    const QJsonObject placeDown = movej({40, 25, 85, 0, 70, 0});
    const QJsonObject gripOpen{{"cmd", "grip"}, {"close", false}};
    const QJsonObject gripClose{{"cmd", "grip"}, {"close", true}};
    pickSteps = {
        {"그리퍼 열기", gripOpen},
        {"집을 위치 위", pickAbove},
        {"하강", pickDown},
        {"집기", gripClose},
        {"상승", pickAbove},
        {"놓을 위치 위", placeAbove},
        {"하강", placeDown},
        {"놓기", gripOpen},
        {"상승", placeAbove},
        {"홈", QJsonObject{{"cmd", "home"}}},
    };

    auto *commandBox = new QGroupBox("명령");
    commandBox->setLayout(commandLayout);

    // 로그: 연결 상태, 오류, 보낸 명령, 받은 응답을 시간과 함께 계속 쌓는다.
    logView = new QPlainTextEdit;
    logView->setReadOnly(true);
    logView->setMaximumBlockCount(1000);  // 너무 오래 켜 두어도 메모리가 계속 늘지 않게 최근 1000줄만 둔다.

    auto *logLayout = new QVBoxLayout;
    logLayout->addWidget(logView);
    auto *logBox = new QGroupBox("로그");
    logBox->setLayout(logLayout);

    auto *panel = new QHBoxLayout;
    panel->addWidget(hostEdit);
    panel->addWidget(portSpin);
    panel->addWidget(connectButton);
    panel->addWidget(disconnectButton);

    auto *middle = new QHBoxLayout;
    middle->addWidget(currentBox);
    middle->addWidget(commandBox);

    auto *root = new QVBoxLayout(this);
    root->addLayout(panel);
    root->addWidget(statusLabel);
    root->addLayout(middle);
    root->addWidget(logBox, 1);  // 1 = 남는 세로 공간은 로그가 차지한다.

    socket = new QTcpSocket(this);

    heartbeatTimer = new QTimer(this);
    heartbeatTimer->setInterval(HeartbeatIntervalMs);

    watchdogTimer = new QTimer(this);
    watchdogTimer->setSingleShot(true);
    watchdogTimer->setInterval(HeartbeatTimeoutMs);

    reconnectTimer = new QTimer(this);
    reconnectTimer->setSingleShot(true);

    ackTimer = new QTimer(this);
    ackTimer->setSingleShot(true);
    ackTimer->setInterval(AckTimeoutMs);

    // 버튼 클릭 → 우리 슬롯
    connect(connectButton, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(disconnectButton, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);
    connect(moveJButton, &QPushButton::clicked, this, &MainWindow::onMoveJClicked);
    connect(homeButton, &QPushButton::clicked, this, &MainWindow::onHomeClicked);
    connect(stopButton, &QPushButton::clicked, this, &MainWindow::onStopClicked);
    connect(pickButton, &QPushButton::clicked, this, &MainWindow::onPickClicked);
    
    // 소켓 상태 변화/오류 → 우리 슬롯
    connect(socket, &QTcpSocket::stateChanged, this, &MainWindow::onStateChanged);
    connect(socket, &QTcpSocket::errorOccurred, this, &MainWindow::onErrorOccurred);
    connect(socket, &QTcpSocket::readyRead, this, &MainWindow::onReadyRead);

    // 타이머 → 우리 슬롯
    connect(heartbeatTimer, &QTimer::timeout, this, &MainWindow::onHeartbeatTimeout);
    connect(watchdogTimer, &QTimer::timeout, this, &MainWindow::onWatchdogTimeout);
    connect(reconnectTimer, &QTimer::timeout, this, &MainWindow::startConnecting);
    connect(ackTimer, &QTimer::timeout, this, &MainWindow::onAckTimeout);

    // 시작 시점의 화면도 같은 함수로 맞춘다.
    onStateChanged(socket->state());
}

MainWindow::~MainWindow()
{
    // 자식들은 만든 순서대로 삭제되므로, 라벨들이 먼저 사라지고 소켓이 마지막에 삭제된다.
    // 연결된 채로 소켓이 삭제되면 stateChanged(Unconnected)가 발생하는데, 그때 이미 사라진
    // 라벨에 접근하면 프로그램이 죽는다. 그래서 창이 사라지는 중에는 소켓 시그널을 끊어 둔다.
    socket->disconnect(this);
}

void MainWindow::onConnectClicked()
{
    autoReconnect = true;
    reconnectDelayMs = InitialReconnectDelayMs;
    startConnecting();
}

void MainWindow::startConnecting()
{
    // 이전 연결에서 남은 반쪽 메시지가 새 연결의 데이터와 섞이지 않게 비운다.
    buffer.clear();
    lastSeq = -1;

    // 비동기: 여기서 바로 반환되고, 결과는 나중에 stateChanged/errorOccurred로 온다.
    socket->connectToHost(hostEdit->text(), static_cast<quint16>(portSpin->value()));
}

void MainWindow::onDisconnectClicked()
{
    // 사용자가 직접 끊은 것이므로 재연결하지 않는다. 진행 중이던 일도 모두 그만둔다.
    autoReconnect = false;
    abortPicking("연결 해제");
    pendingCommand = QJsonObject();
    if (reconnectTimer->isActive()) {
        reconnectTimer->stop();
        appendLog("재연결 취소");
    }

    if (socket->state() == QAbstractSocket::ConnectedState)
        socket->disconnectFromHost();  // 보내던 데이터를 마저 보내고 정상 종료
    else
        socket->abort();               // 연결 시도 중이면 바로 취소

    updateControls();
}

void MainWindow::scheduleReconnect()
{
    statusLabel->setText(QString("재연결 대기 (%1초 후)").arg(reconnectDelayMs / 1000.0));
    appendLog(statusLabel->text());
    reconnectTimer->start(reconnectDelayMs);
    reconnectDelayMs = reconnectDelayMs * 2;
    reconnectDelayMs = qMin(reconnectDelayMs, MaxReconnectDelayMs);
}

void MainWindow::onStateChanged(QAbstractSocket::SocketState state)
{
    const QString before = statusLabel->text();

    switch (state) {
    case QAbstractSocket::UnconnectedState:
        statusLabel->setText("연결 안 됨");
        break;
    case QAbstractSocket::HostLookupState:
    case QAbstractSocket::ConnectingState:
        statusLabel->setText("연결 중...");
        break;
    case QAbstractSocket::ConnectedState:
        statusLabel->setText("연결됨");
        break;
    case QAbstractSocket::ClosingState:
        statusLabel->setText("해제 중...");
        break;
    default:
        break;
    }
    // HostLookup → Connecting처럼 문구가 같은 상태가 연달아 오면 로그에 한 번만 남긴다.
    if (statusLabel->text() != before)
        appendLog(statusLabel->text());

    if (state == QAbstractSocket::ConnectedState) {
        reconnectDelayMs = InitialReconnectDelayMs;  // 연결에 성공했으니 다음 재연결은 처음 간격부터
        heartbeatTimer->start();
        watchdogTimer->start();

        // 끊기기 전에 보낸 명령의 결과를 모른다. 추측하지 말고 로봇에게 실제 상태를 물어본다.
        if (!pendingCommand.isEmpty()) {
            appendLog(QString("재연결: #%1 %2의 실행 여부 확인")
                          .arg(pendingCommand["id"].toInt()).arg(pendingCommand["cmd"].toString()));
            queryRetries = 0;
            sendQuery();
        }
    } else if (state == QAbstractSocket::UnconnectedState) {
        heartbeatTimer->stop();
        watchdogTimer->stop();
        ackTimer->stop();
        pendingQueryId = 0;  // 이 연결로 보낸 query의 응답은 이제 오지 않는다.
        robotState.clear();  // 연결이 없으면 로봇 상태도 알 수 없다.
        // pendingCommand와 피킹 진행 상태는 지우지 않는다. 재연결 뒤 실제 상태를 보고 이어 간다.

        if (autoReconnect)
            scheduleReconnect();
    }

    updateControls();
}

void MainWindow::updateControls()
{
    // 버튼 활성화는 여러 조건(소켓 상태, 재연결 모드, 로봇 상태, ack 대기)에 따라 정해진다.
    // 조건이 바뀌는 곳에서는 이 함수만 부르면 되도록, 계산은 전부 여기서 한다.
    const QAbstractSocket::SocketState state = socket->state();
    const bool connected = (state == QAbstractSocket::ConnectedState);

    // 재연결 모드가 꺼져 있고 완전히 끊긴 상태에서만 주소를 바꾸고 [연결]을 누를 수 있다.
    const bool canConnect = !autoReconnect && state == QAbstractSocket::UnconnectedState;
    hostEdit->setEnabled(canConnect);
    portSpin->setEnabled(canConnect);
    connectButton->setEnabled(canConnect);

    // 재연결 모드 중이면(연결됨 / 연결 시도 중 / 재연결 대기 중) [해제]로 멈출 수 있다.
    disconnectButton->setEnabled(autoReconnect);

    // 이동 명령: 연결됨 + 로봇이 멈춰 있음 + 결과를 모르는 명령 없음 + 피킹 중 아님
    const bool canMove = connected && robotState == "IDLE" && pendingCommand.isEmpty()
                         && pendingQueryId == 0 && !picking;
    moveJButton->setEnabled(canMove);
    homeButton->setEnabled(canMove);
    pickButton->setEnabled(canMove);
    for (QDoubleSpinBox *spin : targetSpins)
        spin->setEnabled(canMove);

    // 정지: 안전을 위해 연결만 되어 있으면 언제나 누를 수 있다.
    stopButton->setEnabled(connected);
}

void MainWindow::onErrorOccurred(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);

    statusLabel->setText("오류: " + socket->errorString());
    appendLog(statusLabel->text());
}

void MainWindow::onReadyRead()
{
    // readyRead는 "메시지 하나"가 아니라 "바이트가 좀 왔다"는 뜻이다.
    // 일단 버퍼에 모아 두고, 완성된 메시지가 있는 만큼 꺼낸다.
    buffer.append(socket->readAll());

    // 뭐라도 왔으면 상대는 살아 있다. watchdog을 처음부터 다시 센다.
    watchdogTimer->start();

    while (buffer.size() >= HeaderSize) {
        const quint32 bodySize = qFromBigEndian<quint32>(buffer.constData());

        if (bodySize > MaxBodySize) {
            statusLabel->setText(QString("오류: 메시지 길이 이상 (%1바이트)").arg(bodySize));
            appendLog(statusLabel->text());
            socket->abort();  // 데이터가 어긋났으므로 이후 내용은 믿을 수 없다. 즉시 끊는다.
            return;
        }

        if (buffer.size() < HeaderSize + static_cast<int>(bodySize))
            break;  // 본문이 아직 덜 왔다. 다음 readyRead를 기다린다.

        const QByteArray body = buffer.mid(HeaderSize, bodySize);
        buffer.remove(0, HeaderSize + bodySize);
        handleMessage(body);
    }
}

void MainWindow::handleMessage(const QByteArray &body)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "JSON 파싱 실패:" << parseError.errorString();
        return;
    }

    const QJsonObject msg = doc.object();
    const QString type = msg["type"].toString();

    if (type == "ack") {
        const int id = msg["id"].toInt();
        const QString cmd = msg["cmd"].toString();
        const bool ok = msg["ok"].toBool();

        QString head = QString("← #%1 %2").arg(id).arg(cmd);
        if (msg["duplicate"].toBool())
            head += " (중복: 이미 실행됨)";
        if (ok)
            appendLog(head + " OK");
        else
            appendLog(head + " 실패: " + msg["error"].toString());

        if (id == pendingQueryId) {
            pendingQueryId = 0;
            ackTimer->stop();
            handleQueryResult(msg);
        } else if (!pendingCommand.isEmpty() && id == pendingCommand["id"].toInt()) {
            confirmPendingCommand(ok, msg["error"].toString());
        }
        // 그 밖의 ack(정지, 이미 결과를 확정한 명령의 늦은 ack 등)는 로그만 남긴다.
        return;
    }

    if (type != "status")
        return;

    applyStatus(msg);

    const int seq = msg["seq"].toInt();
    seqLabel->setText(QString::number(seq));

    if (seq != lastSeq + 1){
        qWarning() << "seq 누락:" << lastSeq + 1 << "받음" << seq;
    }
    lastSeq = seq;
}

void MainWindow::onMoveJClicked()
{
    QJsonArray joints;
    for (QDoubleSpinBox *spin : targetSpins)
        joints.append(spin->value());

    sendCommand(QJsonObject{{"cmd", "movej"}, {"joints", joints}});
}

void MainWindow::onHomeClicked()
{
    sendCommand(QJsonObject{{"cmd", "home"}});
}

void MainWindow::onStopClicked()
{
    // 정지는 다른 모든 것보다 우선한다. 피킹을 멈추고, 결과를 기다리던 명령도 더 이상 신경 쓰지 않는다.
    // (그 명령이 먼저 실행됐더라도 TCP는 순서를 지키므로 정지가 뒤에 실행되어 멈춘다.)
    abortPicking("정지 버튼");
    pendingCommand = QJsonObject();
    pendingQueryId = 0;
    ackTimer->stop();

    sendCommand(QJsonObject{{"cmd", "stop"}});
}

void MainWindow::sendCommand(QJsonObject command)
{
    // 공통 필드는 여기서 채운다. 버튼마다 type/id를 따로 쓰지 않아도 된다.
    command["type"] = "command";
    command["id"] = nextCommandId++;  // 지금 값을 쓰고 나서 1 늘린다.

    sendMessage(command);

    // 정지는 언제든 누를 수 있어야 하므로 잠금 대상에서 뺀다.
    // 나머지는 실행 여부가 확정될 때까지 기억해 두고 버튼을 잠근다.
    if (command["cmd"].toString() != "stop") {
        pendingCommand = command;
        ackTimer->start();
        updateControls();
    }

    // 로그에는 type을 빼고 사람이 보기 쉬운 부분만 남긴다.
    QJsonObject shown = command;
    shown.remove("type");
    shown.remove("id");
    appendLog(QString("→ #%1 %2").arg(command["id"].toInt())
                  .arg(QString::fromUtf8(QJsonDocument(shown).toJson(QJsonDocument::Compact))));
}

void MainWindow::sendMessage(const QJsonObject &msg)
{
    if (socket->state() != QAbstractSocket::ConnectedState)
        return;

    // 받을 때와 같은 형식: [길이 헤더 4바이트, 빅엔디언][JSON 본문]
    const QByteArray body = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    QByteArray header(HeaderSize, '\0');
    qToBigEndian<quint32>(static_cast<quint32>(body.size()), header.data());

    // 헤더와 본문을 한 번에 write한다. write도 비동기라, 실제 전송은 이벤트 루프가 알아서 한다.
    socket->write(header + body);
}

void MainWindow::appendLog(const QString &text)
{
    logView->appendPlainText(QTime::currentTime().toString("hh:mm:ss.zzz") + "  " + text);
}

void MainWindow::onHeartbeatTimeout()
{
    // 명령이 아니므로 id도 없고 로그에도 남기지 않는다. "나 살아 있어"라는 신호일 뿐이다.
    sendMessage(QJsonObject{{"type", "heartbeat"}});
}

void MainWindow::onWatchdogTimeout()
{
    appendLog(QString("하트비트 끊김: %1초 동안 수신 없음").arg(HeartbeatTimeoutMs / 1000));
    socket->abort();  // 상대가 응답이 없으니 정상 종료 절차 없이 바로 끊는다. → Unconnected → 재연결
}

void MainWindow::onAckTimeout()
{
    if (pendingQueryId != 0) {
        // 상태를 물어본 query마저 응답이 없다. query는 읽기만 하므로 다시 물어봐도 안전하다.
        if (++queryRetries > MaxQueryRetries) {
            appendLog(QString("⚠ 재조회 %1회 실패 → 상태를 알 수 없어 중단").arg(MaxQueryRetries));
            abortPicking("상태 확인 실패");
            pendingCommand = QJsonObject();
            pendingQueryId = 0;
            updateControls();
            return;
        }
        appendLog(QString("⚠ #%1 query 응답 없음 → 다시 조회 (%2/%3)")
                      .arg(pendingQueryId).arg(queryRetries).arg(MaxQueryRetries));
        sendQuery();
        return;
    }

    if (pendingCommand.isEmpty())
        return;

    // ack가 안 왔다. 명령이 사라진 건지 ack만 사라진 건지 여기서는 알 수 없다.
    // 그냥 다시 보내면 두 번 실행될 수도 있으니, 먼저 로봇의 실제 상태를 물어본다.
    appendLog(QString("⚠ #%1 %2 응답 없음 (%3초) → 실제 상태 재조회")
                  .arg(pendingCommand["id"].toInt()).arg(pendingCommand["cmd"].toString())
                  .arg(AckTimeoutMs / 1000));
    queryRetries = 0;
    sendQuery();
}

void MainWindow::sendQuery()
{
    pendingQueryId = nextCommandId++;
    sendMessage(QJsonObject{{"type", "command"}, {"id", pendingQueryId}, {"cmd", "query"}});
    ackTimer->start();
    appendLog(QString("→ #%1 query (실제 상태 재조회)").arg(pendingQueryId));
    updateControls();
}

void MainWindow::handleQueryResult(const QJsonObject &msg)
{
    // 조회한 김에 화면도 실제 상태로 맞춘다. (status가 유실돼도 최신 상태를 알 수 있다)
    applyStatus(msg["status"].toObject());

    if (pendingCommand.isEmpty())
        return;  // 그사이 늦은 ack가 와서 이미 확정됐다.

    const QJsonArray executedIds = msg["executed_ids"].toArray();
    const int id = pendingCommand["id"].toInt();

    if (executedIds.contains(id)){
        appendLog(QString("✔ #%1 실행된 것 확인 (ack만 유실) → 진행").arg(id));
        confirmPendingCommand(true);
    }else{
        appendLog(QString("✘ #%1 실행 안 된 것 확인 (명령 유실) → 같은 id로 재전송").arg(id));
        resendPendingCommand();
    }
}

void MainWindow::confirmPendingCommand(bool ok, const QString &error)
{
    const QString cmd = pendingCommand["cmd"].toString();
    pendingCommand = QJsonObject();
    pendingQueryId = 0;  // 이미 확정됐으니 진행 중인 재조회의 결과는 필요 없다.
    ackTimer->stop();

    // 명령이 받아들여졌으면 다음 status가 오기 전이라도 "움직이는 중"으로 본다.
    // 그러지 않으면 확정과 다음 status 사이(최대 0.5초) 동안 버튼이 풀리거나, 피킹이 도착 전에 다음 단계로 넘어간다.
    if (ok)
        robotState = "MOVING";

    if (picking) {
        if (ok)
            stepAcked = true;
        else
            abortPicking(QString("%1 실패: %2").arg(cmd, error));
    }

    updateControls();
    tryAdvancePicking();
}

void MainWindow::resendPendingCommand()
{
    // 같은 id로 다시 보낸다. 혹시 앞의 것이 늦게라도 실행됐다면 브리지가 중복으로 보고 무시한다.
    sendMessage(pendingCommand);
    ackTimer->start();
    appendLog(QString("↻ #%1 %2 재전송").arg(pendingCommand["id"].toInt()).arg(pendingCommand["cmd"].toString()));
}

void MainWindow::applyStatus(const QJsonObject &status)
{
    robotState = status["state"].toString();
    robotStateLabel->setText(robotState);
    gripperLabel->setText(status["gripper"].toString());

    const QJsonArray joints = status["joints"].toArray();
    for (int i = 0; i < jointLabels.size() && i < joints.size(); ++i)
        jointLabels[i]->setText(QString("%1°").arg(joints[i].toDouble(), 0, 'f', 2));

    updateControls();
    tryAdvancePicking();
}

void MainWindow::onPickClicked()
{
    picking = true;
    pickStep = 0;
    appendLog("■ 피킹 시작");
    runPickStep();
}

void MainWindow::runPickStep()
{
    const PickStep &step = pickSteps[pickStep];
    stepAcked = false;
    pickLabel->setText(QString("피킹: %1/%2 %3").arg(pickStep + 1).arg(pickSteps.size()).arg(step.name));
    appendLog("▶ " + pickLabel->text());
    sendCommand(step.command);
}

void MainWindow::tryAdvancePicking()
{
    // 다음 단계로 넘어가는 조건: 지금 단계 명령이 실행된 게 확인됐고, 로봇이 도착해서 멈춰 있음
    if (!picking || !stepAcked || robotState != "IDLE" || !pendingCommand.isEmpty())
        return;

    ++pickStep;
    if (pickStep == pickSteps.size()) {
        picking = false;
        pickLabel->setText("피킹: 완료");
        appendLog("■ 피킹 완료");
        updateControls();
        return;
    }
    runPickStep();
}

void MainWindow::abortPicking(const QString &reason)
{
    if (!picking)
        return;
    picking = false;
    pickLabel->setText(QString("피킹: 중단 (%1/%2)").arg(pickStep + 1).arg(pickSteps.size()));
    appendLog("■ 피킹 중단: " + reason);
    updateControls();
}
