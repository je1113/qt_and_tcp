#include "mainwindow.h"

#include <QDebug>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpSocket>
#include <QVBoxLayout>
#include <QtEndian>

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle("TCP Chat");
    resize(400, 280);

    // 위젯들에 this(부모)를 주면, 창이 삭제될 때 Qt가 자식들도 함께 delete 해준다.
    hostEdit = new QLineEdit("127.0.0.1", this);
    portSpin = new QSpinBox(this);
    portSpin->setRange(1, 65535);
    portSpin->setValue(5000);
    connectButton = new QPushButton("연결", this);
    disconnectButton = new QPushButton("해제", this);
    statusLabel = new QLabel(this);

    // 관절 J1~J6 + seq를 "이름: 값" 표로 보여준다. (레이아웃에 넣으면 부모가 자동으로 정해진다)
    auto *jointForm = new QFormLayout;
    for (int i = 0; i < 6; ++i) {
        auto *label = new QLabel("-");
        jointForm->addRow(QString("J%1").arg(i + 1), label);
        jointLabels.append(label);
    }
    seqLabel = new QLabel("-");
    jointForm->addRow("seq", seqLabel);

    auto *panel = new QHBoxLayout;
    panel->addWidget(hostEdit);
    panel->addWidget(portSpin);
    panel->addWidget(connectButton);
    panel->addWidget(disconnectButton);

    auto *root = new QVBoxLayout(this);
    root->addLayout(panel);
    root->addWidget(statusLabel);
    root->addLayout(jointForm);
    root->addStretch();

    socket = new QTcpSocket(this);

    // 버튼 클릭 → 우리 슬롯
    connect(connectButton, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(disconnectButton, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);

    // 소켓 상태 변화/오류 → 우리 슬롯
    connect(socket, &QTcpSocket::stateChanged, this, &MainWindow::onStateChanged);
    connect(socket, &QTcpSocket::errorOccurred, this, &MainWindow::onErrorOccurred);
    connect(socket, &QTcpSocket::readyRead, this, &MainWindow::onReadyRead);

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
    // 이전 연결에서 남은 반쪽 메시지가 새 연결의 데이터와 섞이지 않게 비운다.
    buffer.clear();
    lastSeq = -1;

    // 비동기: 여기서 바로 반환되고, 결과는 나중에 stateChanged/errorOccurred로 온다.
    socket->connectToHost(hostEdit->text(), static_cast<quint16>(portSpin->value()));
}

void MainWindow::onDisconnectClicked()
{
    socket->disconnectFromHost();
}

void MainWindow::onStateChanged(QAbstractSocket::SocketState state)
{
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

    // 연결이 안 된 상태에서만 주소를 바꾸고 [연결]을 누를 수 있게 한다.
    const bool idle = (state == QAbstractSocket::UnconnectedState);
    hostEdit->setEnabled(idle);
    portSpin->setEnabled(idle);
    connectButton->setEnabled(idle);
    disconnectButton->setEnabled(state == QAbstractSocket::ConnectedState);
}

void MainWindow::onErrorOccurred(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);

    statusLabel->setText("오류: " + socket->errorString());
}

void MainWindow::onReadyRead()
{
    // readyRead는 "메시지 하나"가 아니라 "바이트가 좀 왔다"는 뜻이다.
    // 일단 버퍼에 모아 두고, 완성된 메시지가 있는 만큼 꺼낸다.
    buffer.append(socket->readAll());

    while (buffer.size() >= HeaderSize) {
        const quint32 bodySize = qFromBigEndian<quint32>(buffer.constData());

        if (bodySize > MaxBodySize) {
            statusLabel->setText(QString("오류: 메시지 길이 이상 (%1바이트)").arg(bodySize));
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
    if (msg["type"].toString() != "status")
        return;

    const QJsonArray joints = msg["joints"].toArray();
    for (int i = 0; i < jointLabels.size() && i < joints.size(); ++i)
        jointLabels[i]->setText(QString("%1°").arg(joints[i].toDouble(), 0, 'f', 2));

    const int seq = msg["seq"].toInt();
    seqLabel->setText(QString::number(seq));

    if (seq != lastSeq + 1){
        qWarning() << "seq 누락:" << lastSeq + 1 << "받음" << seq;
    }
    lastSeq = seq;
}
