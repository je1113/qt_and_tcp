"""모의 M0609 브리지.

실제 로봇 대신 가짜 M0609의 상태를 만들어, 접속한 TCP 클라이언트(Qt 앱)에게 주기적으로 보낸다.

프로토콜: 한 줄에 JSON 하나 (UTF-8, '\\n'으로 끝남)
    {"type": "status", "state": "MOVING", "joints": [j1, ..., j6]}
    - state : "IDLE" 또는 "MOVING"
    - joints: 관절 6개의 각도 [deg]

실행: python3 bridge/mock_m0609.py [--port 5000] [--period 0.5]
"""

import argparse
import asyncio
import json
import math
import time


class MockM0609:
    """관절 6개가 사인파로 천천히 움직이는 가짜 로봇."""

    JOINT_COUNT = 6

    def __init__(self):
        self.start_time = time.monotonic()

    def status(self):
        t = time.monotonic() - self.start_time

        # 관절마다 진폭과 속도를 조금씩 다르게 해서, 값이 움직이는 게 눈에 보이게 한다.
        joints = [
            round(30.0 * math.sin(t * 0.5 + i), 2)
            for i in range(self.JOINT_COUNT)
        ]

        return {
            "type": "status",
            "state": "MOVING",
            "joints": joints,
        }


async def handle_client(reader, writer, robot, period):
    """클라이언트 하나가 접속하면 실행된다. 연결이 끊길 때까지 상태를 보낸다."""
    addr = writer.get_extra_info("peername")
    print(f"[연결] {addr}")

    try:
        seq = 0
        while True:
            msg = robot.status()
            msg["seq"] = seq
            line = json.dumps(msg) + "\n"
            writer.write(line.encode("utf-8"))
            await writer.drain()          # 실제로 보내질 때까지 (다른 클라이언트를 막지 않고) 기다림
            await asyncio.sleep(period)
            seq = seq + 1
    except (ConnectionResetError, BrokenPipeError):
        pass                              # 클라이언트가 먼저 끊은 경우
    finally:
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
