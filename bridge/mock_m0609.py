"""모의 M0609 브리지.

실제 로봇 대신 가짜 M0609를 흉내 낸다. 접속한 TCP 클라이언트(Qt 앱)에게 상태를 주기적으로 보내고,
클라이언트가 보낸 명령(movej, home, stop)을 받아 가짜 로봇을 움직인다.

프레임: [길이 헤더 4바이트][JSON 본문]  (양방향 모두 같은 형식)
    - 길이 헤더: 본문의 바이트 수. uint32, 빅엔디언(네트워크 바이트 순서)
    - 본문    : UTF-8 JSON

브리지 → 클라이언트
    {"type": "status", "state": "MOVING", "joints": [j1, ..., j6], "seq": n}
        - state : "IDLE"(정지) 또는 "MOVING"(이동 중)
        - joints: 관절 6개의 현재 각도 [deg]
        - seq   : 연결마다 0부터 1씩 증가하는 메시지 번호
    {"type": "ack", "id": n, "cmd": "movej", "ok": true}
    {"type": "ack", "id": n, "cmd": "movej", "ok": false, "error": "이유"}
        - 명령 하나마다 응답 하나. id는 명령의 id를 그대로 돌려준다.

클라이언트 → 브리지
    {"type": "command", "id": n, "cmd": "movej", "joints": [j1, ..., j6]}  목표 관절각으로 이동
    {"type": "command", "id": n, "cmd": "home"}                            모든 관절 0°로 이동
    {"type": "command", "id": n, "cmd": "stop"}                            그 자리에서 정지

실행: python3 bridge/mock_m0609.py [--port 5000] [--period 0.5]
"""

import argparse
import asyncio
import json
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
        self.last_update = time.monotonic()

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

    def status(self):
        self._update()
        return {
            "type": "status",
            "state": "MOVING" if self.joints != self.target else "IDLE",
            "joints": [round(j, 2) for j in self.joints],
        }


def handle_command(robot, msg):
    """명령 하나를 실행하고, 클라이언트에게 돌려줄 ack 메시지를 만든다."""
    cmd = msg.get("cmd")
    ack = {"type": "ack", "id": msg.get("id"), "cmd": cmd}

    try:
        if cmd == "movej":
            robot.movej(msg.get("joints"))
        elif cmd == "home":
            robot.home()
        elif cmd == "stop":
            robot.stop()
        else:
            raise ValueError(f"알 수 없는 명령: {cmd}")
    except ValueError as e:
        ack["ok"] = False
        ack["error"] = str(e)
        return ack

    ack["ok"] = True
    return ack


def send_message(writer, msg):
    """메시지 하나를 [길이 헤더][JSON]으로 보낸다. 한 번의 write로 보내서 다른 메시지와 섞이지 않는다."""
    body = json.dumps(msg, ensure_ascii=False).encode("utf-8")
    header = struct.pack(">I", len(body))   # > = 빅엔디언, I = uint32 (4바이트)
    writer.write(header + body)


async def read_message(reader):
    """메시지 하나를 받는다. readexactly가 'N바이트가 다 모일 때까지' 기다려 준다."""
    header = await reader.readexactly(HEADER_SIZE)
    (body_size,) = struct.unpack(">I", header)
    if body_size > MAX_BODY_SIZE:
        raise ValueError(f"메시지 길이 이상 ({body_size}바이트)")
    body = await reader.readexactly(body_size)
    return json.loads(body)


async def send_status_loop(writer, robot, period):
    """연결이 끊길 때까지 주기적으로 상태를 보낸다."""
    seq = 0
    while True:
        msg = robot.status()
        msg["seq"] = seq
        send_message(writer, msg)
        await writer.drain()          # 실제로 보내질 때까지 (다른 클라이언트를 막지 않고) 기다림
        await asyncio.sleep(period)
        seq = seq + 1


async def receive_command_loop(reader, writer, robot):
    """연결이 끊길 때까지 명령을 받아 실행하고 ack를 보낸다."""
    while True:
        msg = await read_message(reader)
        print(f"[명령] {msg}")
        if msg.get("type") != "command":
            continue
        send_message(writer, handle_command(robot, msg))
        await writer.drain()


async def handle_client(reader, writer, robot, period):
    """클라이언트 하나가 접속하면 실행된다. 보내기/받기 두 작업을 동시에 돌린다."""
    addr = writer.get_extra_info("peername")
    print(f"[연결] {addr}")

    tasks = [
        asyncio.create_task(send_status_loop(writer, robot, period)),
        asyncio.create_task(receive_command_loop(reader, writer, robot)),
    ]
    try:
        # 둘 중 하나라도 끝나면(대부분 연결이 끊긴 경우) 이 연결은 끝난 것이다.
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            error = task.exception()
            if isinstance(error, (asyncio.IncompleteReadError, ConnectionResetError, BrokenPipeError)):
                pass                  # 클라이언트가 끊은 경우
            elif error is not None:
                print(f"[오류] {addr}: {error!r}")
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        print(f"[해제] {addr}")
        writer.close()


async def main():
    parser = argparse.ArgumentParser(description="모의 M0609 TCP 브리지")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--period", type=float, default=0.5, help="상태 전송 주기 [초]")
    args = parser.parse_args()

    robot = MockM0609()

    server = await asyncio.start_server(
        lambda r, w: handle_client(r, w, robot, args.period),
        args.host,
        args.port,
    )
    print(f"모의 M0609 브리지 대기 중: {args.host}:{args.port} (주기 {args.period}s)")

    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n종료")
