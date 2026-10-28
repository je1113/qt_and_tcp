"""모의 M0609 브리지.

실제 로봇 대신 가짜 M0609를 흉내 낸다. 접속한 TCP 클라이언트(Qt 앱)에게 상태를 주기적으로 보내고,
클라이언트가 보낸 명령(movej, home, stop)을 받아 가짜 로봇을 움직인다.

프레임: [길이 헤더 4바이트][JSON 본문]  (양방향 모두 같은 형식)
    - 길이 헤더: 본문의 바이트 수. uint32, 빅엔디언(네트워크 바이트 순서)
    - 본문    : UTF-8 JSON

브리지 → 클라이언트
    {"type": "status", "state": "MOVING", "joints": [j1, ..., j6], "gripper": "OPEN", "seq": n}
        - state  : "IDLE"(정지) 또는 "MOVING"(이동 중)
        - joints : 관절 6개의 현재 각도 [deg]
        - gripper: "OPEN" 또는 "CLOSED"
        - seq    : 연결마다 0부터 1씩 증가하는 메시지 번호
    {"type": "ack", "id": n, "cmd": "movej", "ok": true}
    {"type": "ack", "id": n, "cmd": "movej", "ok": true, "duplicate": true}
    {"type": "ack", "id": n, "cmd": "movej", "ok": false, "error": "이유"}
        - 명령 하나마다 응답 하나. id는 명령의 id를 그대로 돌려준다.
        - 같은 session에서 이미 실행한 id가 다시 오면 실행하지 않고 duplicate=true로 응답한다(재전송해도 안전).
    {"type": "ack", "id": n, "cmd": "query", "ok": true, "status": {...}, "executed_ids": [...]}
        - query의 응답. 지금 상태와, 그 session에서 최근에 실행한 명령 id 목록을 돌려준다.

클라이언트 → 브리지  (모든 command에 "session": "<클라이언트가 켜질 때 만든 UUID>"를 붙인다)
    {"type": "command", "id": n, "cmd": "movej", "joints": [j1, ..., j6]}  목표 관절각으로 이동
    {"type": "command", "id": n, "cmd": "home"}                            모든 관절 0°로 이동
    {"type": "command", "id": n, "cmd": "stop"}                            그 자리에서 정지
    {"type": "command", "id": n, "cmd": "grip", "close": true}             그리퍼 닫기(false면 열기)
    {"type": "command", "id": n, "cmd": "query"}                           실제 상태 재조회
    {"type": "heartbeat"}   살아 있다는 신호. --client-timeout 초 동안 아무것도 안 오면 연결을 끊는다.

실행: python3 bridge/mock_m0609.py [--port 5000] [--period 0.5] [--client-timeout 3]

장애 주입 (기본값은 모두 0 = 장애 없음)
    --delay 0.3     브리지가 보내는 모든 메시지를 0.3초 늦게 보낸다
    --loss 0.2      메시지(양방향)를 20% 확률로 버린다
    --cut-rate 0.05 1초마다 5% 확률로 연결을 강제로 끊는다
    --seed 1        무작위 값을 고정해서 같은 장애를 다시 재현한다
"""

import argparse
import asyncio
import collections
import json
import random
import struct
import time

HEADER_SIZE = 4
MAX_BODY_SIZE = 1024 * 1024


class MockM0609:
    """목표 관절각을 받으면 일정한 속도로 그쪽을 향해 움직이는 가짜 로봇."""

    JOINT_COUNT = 6
    JOINT_LIMIT = 360.0   # [deg] 연습용으로 단순화한 값. 실제 M0609 사양이 아니다.
    SPEED = 30.0          # [deg/s] 관절마다 1초에 움직일 수 있는 최대 각도
    HOME = [0.0] * JOINT_COUNT

    def __init__(self):
        self.joints = list(self.HOME)
        self.target = list(self.HOME)
        self.gripper = "OPEN"
        self.last_update = time.monotonic()
        # 최근에 실행한 명령의 (session, id). 중복 실행 방지와 query 응답에 쓴다.
        # id는 클라이언트마다 1부터 다시 세므로, 어느 클라이언트(session)의 id인지까지 함께 기억해야 한다.
        self.executed_ids = collections.deque(maxlen=50)

    def _update(self):
        """마지막 갱신 이후 흐른 시간만큼 관절을 목표 쪽으로 움직인다."""
        now = time.monotonic()
        max_step = self.SPEED * (now - self.last_update)
        self.last_update = now

        for i in range(self.JOINT_COUNT):
            diff = self.target[i] - self.joints[i]
            if abs(diff) <= max_step:
                self.joints[i] = self.target[i]     # 이번에 도착할 수 있으면 딱 맞춰 멈춘다
            elif diff > 0:
                self.joints[i] += max_step
            else:
                self.joints[i] -= max_step

    def movej(self, joints):
        if not isinstance(joints, list) or len(joints) != self.JOINT_COUNT:
            raise ValueError(f"관절 각도 {self.JOINT_COUNT}개가 필요합니다")
        for i, j in enumerate(joints):
            if isinstance(j, bool) or not isinstance(j, (int, float)):
                raise ValueError(f"J{i + 1} 값이 숫자가 아닙니다")
            if abs(j) > self.JOINT_LIMIT:
                raise ValueError(f"J{i + 1} 각도가 범위(±{self.JOINT_LIMIT:g}°)를 벗어났습니다")

        self._update()
        self.target = [float(j) for j in joints]

    def home(self):
        self._update()
        self.target = list(self.HOME)

    def stop(self):
        self._update()
        self.target = list(self.joints)

    def grip(self, close):
        if not isinstance(close, bool):
            raise ValueError("close는 true/false여야 합니다")
        self.gripper = "CLOSED" if close else "OPEN"

    def status(self):
        self._update()
        return {
            "type": "status",
            "state": "MOVING" if self.joints != self.target else "IDLE",
            "joints": [round(j, 2) for j in self.joints],
            "gripper": self.gripper,
        }


def handle_command(robot, msg):
    """명령 하나를 실행하고, 클라이언트에게 돌려줄 ack 메시지를 만든다."""
    cmd = msg.get("cmd")
    cmd_id = msg.get("id")
    session = msg.get("session")
    ack = {"type": "ack", "id": cmd_id, "cmd": cmd}

    if cmd == "query":
        # 읽기만 하는 명령이라 몇 번을 다시 보내도 안전하다. 실행 기록에도 남기지 않는다.
        ack["ok"] = True
        ack["status"] = robot.status()
        ack["executed_ids"] = [i for (s, i) in robot.executed_ids if s == session]
        return ack

    if (session, cmd_id) in robot.executed_ids:
        # 같은 명령이 다시 왔다(클라이언트가 ack를 못 받아서 재전송). 두 번 실행하지 않는다.
        ack["ok"] = True
        ack["duplicate"] = True
        return ack

    try:
        if cmd == "movej":
            robot.movej(msg.get("joints"))
        elif cmd == "home":
            robot.home()
        elif cmd == "stop":
            robot.stop()
        elif cmd == "grip":
            robot.grip(msg.get("close"))
        else:
            raise ValueError(f"알 수 없는 명령: {cmd}")
    except ValueError as e:
        ack["ok"] = False
        ack["error"] = str(e)
        return ack

    robot.executed_ids.append((session, cmd_id))
    ack["ok"] = True
    return ack


class Faults:
    """장애 주입 설정. 연결마다 하나씩 만들지 않고 모든 연결이 같이 쓴다."""

    def __init__(self, delay, loss, cut_rate, seed):
        self.delay = delay
        self.loss = loss
        self.cut_rate = cut_rate
        self.rng = random.Random(seed)

    def lose(self):
        return self.loss > 0 and self.rng.random() < self.loss

    def cut(self, elapsed):
        """elapsed초 동안 끊김이 일어났는지. cut_rate는 '1초당 확률'이다."""
        return self.cut_rate > 0 and self.rng.random() < self.cut_rate * elapsed


class Link:
    """연결 하나. 메시지를 보낼 때 장애(유실, 지연)를 적용한다."""

    def __init__(self, writer, faults, addr):
        self.writer = writer
        self.faults = faults
        self.addr = addr

    def send(self, msg):
        if self.faults.lose():
            if msg.get("type") != "status":   # status는 자주 오니 유실 로그는 생략
                print(f"[장애] {self.addr}: 보낼 {msg.get('type')} #{msg.get('id')} 유실")
            return

        if self.faults.delay > 0:
            # 지연: 지금 보내지 않고 delay초 뒤에 보내도록 예약한다. 순서는 그대로 유지된다.
            asyncio.get_running_loop().call_later(self.faults.delay, self._write, msg)
        else:
            self._write(msg)

    def _write(self, msg):
        if not self.writer.is_closing():
            send_message(self.writer, msg)

    def cut(self):
        print(f"[장애] {self.addr}: 연결 강제 끊김")
        self.writer.transport.abort()


def send_message(writer, msg):
    """메시지 하나를 [길이 헤더][JSON]으로 보낸다. 한 번의 write로 보내서 다른 메시지와 섞이지 않는다."""
    body = json.dumps(msg, ensure_ascii=False).encode("utf-8")
    header = struct.pack(">I", len(body))   # > = 빅엔디언, I = uint32 (4바이트)
    writer.write(header + body)


async def read_message(reader, timeout):
    """메시지 하나를 받는다. readexactly가 'N바이트가 다 모일 때까지' 기다려 준다.

    timeout초 안에 다음 메시지가 시작되지 않으면 TimeoutError. (timeout이 0이면 무한정 기다림)
    """
    header = await asyncio.wait_for(reader.readexactly(HEADER_SIZE), timeout or None)
    (body_size,) = struct.unpack(">I", header)
    if body_size > MAX_BODY_SIZE:
        raise ValueError(f"메시지 길이 이상 ({body_size}바이트)")
    body = await reader.readexactly(body_size)
    return json.loads(body)


async def send_status_loop(link, robot, period):
    """연결이 끊길 때까지 주기적으로 상태를 보낸다."""
    seq = 0
    while True:
        msg = robot.status()
        msg["seq"] = seq
        link.send(msg)
        await link.writer.drain()     # 실제로 보내질 때까지 (다른 클라이언트를 막지 않고) 기다림
        await asyncio.sleep(period)
        seq = seq + 1

        if link.faults.cut(period):
            link.cut()
            return


async def receive_command_loop(reader, link, robot, client_timeout):
    """연결이 끊길 때까지 명령을 받아 실행하고 ack를 보낸다."""
    while True:
        msg = await read_message(reader, client_timeout)
        if msg.get("type") != "command":
            continue                  # heartbeat 등은 '살아 있다'는 것만 알려 주면 충분하다

        if link.faults.lose():
            print(f"[장애] {link.addr}: 받은 명령 #{msg.get('id')} {msg.get('cmd')} 유실 (실행 안 함)")
            continue

        print(f"[명령] {msg}")
        link.send(handle_command(robot, msg))
        await link.writer.drain()


async def handle_client(reader, writer, robot, faults, period, client_timeout):
    """클라이언트 하나가 접속하면 실행된다. 보내기/받기 두 작업을 동시에 돌린다."""
    addr = writer.get_extra_info("peername")
    print(f"[연결] {addr}")
    link = Link(writer, faults, addr)

    tasks = [
        asyncio.create_task(send_status_loop(link, robot, period)),
        asyncio.create_task(receive_command_loop(reader, link, robot, client_timeout)),
    ]
    try:
        # 둘 중 하나라도 끝나면(대부분 연결이 끊긴 경우) 이 연결은 끝난 것이다.
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            error = task.exception()
            if isinstance(error, (asyncio.IncompleteReadError, ConnectionResetError, BrokenPipeError)):
                pass                  # 클라이언트가 끊은 경우
            elif isinstance(error, TimeoutError):
                print(f"[하트비트 없음] {addr}: {client_timeout}초 동안 수신 없음")
            elif error is not None:
                print(f"[오류] {addr}: {error!r}")
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        print(f"[해제] {addr}")
        writer.close()


def make_arg_parser(description):
    """브리지 공통 옵션. 다른 브리지(dsr_bridge.py)도 이 옵션을 그대로 쓴다."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--period", type=float, default=0.5, help="상태 전송 주기 [초]")
    parser.add_argument("--client-timeout", type=float, default=3.0,
                        help="이 시간[초] 동안 클라이언트에게서 아무것도 안 오면 끊는다. 0 = 끄기")
    parser.add_argument("--delay", type=float, default=0.0, help="[장애] 보내는 메시지 지연 [초]")
    parser.add_argument("--loss", type=float, default=0.0, help="[장애] 메시지 유실 확률 (0~1)")
    parser.add_argument("--cut-rate", type=float, default=0.0, help="[장애] 1초당 연결 강제 끊김 확률 (0~1)")
    parser.add_argument("--seed", type=int, default=None, help="[장애] 무작위 시드 (같은 값이면 같은 장애)")
    return parser


async def serve(robot, args, name):
    """TCP 서버를 연다. robot은 movej/home/stop/grip/status와 executed_ids를 가진 객체면 된다."""
    faults = Faults(args.delay, args.loss, args.cut_rate, args.seed)

    server = await asyncio.start_server(
        lambda r, w: handle_client(r, w, robot, faults, args.period, args.client_timeout),
        args.host,
        args.port,
    )
    print(f"{name} 브리지 대기 중: {args.host}:{args.port} (주기 {args.period}s)")
    if args.delay or args.loss or args.cut_rate:
        print(f"[장애 주입] 지연 {args.delay}s, 유실 {args.loss:.0%}, 끊김 {args.cut_rate:.0%}/s, 시드 {args.seed}")

    async with server:
        await server.serve_forever()


async def main():
    args = make_arg_parser("모의 M0609 TCP 브리지").parse_args()
    await serve(MockM0609(), args, "모의 M0609")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n종료")
