"""doosan-robot2 M0609 브리지.

mock_m0609.py와 같은 TCP 프로토콜(길이 헤더 + JSON, 하트비트, query, 중복 제거, 장애 주입)을 그대로 쓰고,
가짜 로봇 대신 doosan-robot2의 ROS 2 서비스를 호출한다. Qt 앱은 어느 브리지에 붙었는지 모른다.

준비 (터미널 1): doosan-robot2를 virtual 모드로 띄운다. 에뮬레이터 컨테이너는 launch가 알아서 켜고 끈다.
    source ~/ros/install/setup.bash
    ros2 launch dsr_bringup2 dsr_bringup2_rviz.launch.py mode:=virtual host:=127.0.0.1 port:=12345 model:=m0609

실행 (터미널 2):
    source ~/ros/install/setup.bash
    python3 bridge/dsr_bridge.py [--port 5000] [--ns dsr01] [--vel 30] [--acc 60]  (+ mock_m0609.py와 같은 장애 옵션)

명령 → 서비스 (<ns>=dsr01, <prefix>=dsr_controller2 가 기본값)
    movej, home  → /<ns>/<prefix>/motion/move_joint  (비동기: 움직이기 시작하면 바로 응답)
    stop         → /<ns>/<prefix>/motion/move_stop   (DR_QSTOP)
    grip         → virtual 모드에는 그리퍼가 없어서 브리지 안에서 상태만 기억한다.
    status       → /<ns>/<prefix>/aux_control/get_current_posj + .../motion/check_motion 을 주기적으로 물어본 값
"""

import asyncio
import collections
import threading
import time

import rclpy
from rclpy.node import Node
from rclpy.signals import SignalHandlerOptions

from dsr_msgs2.srv import CheckMotion, GetCurrentPosj, MoveJoint, MoveStop

from mock_m0609 import make_arg_parser, serve

DR_QSTOP = 1          # MoveStop.srv: Quick stop (Stop Category 2)
DR_STATE_IDLE = 0     # CheckMotion.srv: 움직이는 중인 모션 없음
MOVE_SYNC_ASYNC = 1   # MoveJoint.srv: 모션이 끝날 때까지 기다리지 않고 바로 응답


class DsrM0609:
    """MockM0609와 같은 함수(movej/home/stop/grip/status, executed_ids)를 ROS 2 서비스로 구현한다."""

    JOINT_COUNT = 6
    JOINT_LIMITS = [360.0, 360.0, 150.0, 360.0, 360.0, 360.0]  # [deg] dsr_description2/urdf/m0609.urdf
    HOME = [0.0] * JOINT_COUNT
    SERVICE_TIMEOUT = 2.0    # [s] 서비스 응답을 기다리는 최대 시간
    POLL_PERIOD = 0.1        # [s] 관절 각도/모션 상태를 물어보는 주기
    STALE_AFTER = 1.0        # [s] 이보다 오래된 값이면 상태를 "UNKNOWN"으로 보고한다
    MIN_MOVING_TIME = 0.3    # [s] 이동 명령 직후 이 시간 동안은 IDLE이 와도 믿지 않는다

    def __init__(self, ns, prefix, vel, acc, node_name):
        self.vel = vel
        self.acc = acc

        # rclpy는 기본으로 Ctrl+C(SIGINT)/SIGTERM을 가로채서 ROS만 멈춘다. 그러면 asyncio 서버는 계속 살아 있어서
        # 프로그램이 안 꺼진다. 신호 처리는 파이썬 기본(KeyboardInterrupt)에 맡기고, 종료는 shutdown()에서 한다.
        rclpy.init(signal_handler_options=SignalHandlerOptions.NO)
        self.node = Node(node_name, namespace=ns)
        self.move_joint_client = self.node.create_client(MoveJoint, f"{prefix}/motion/move_joint")
        self.move_stop_client = self.node.create_client(MoveStop, f"{prefix}/motion/move_stop")
        self.posj_client = self.node.create_client(GetCurrentPosj, f"{prefix}/aux_control/get_current_posj")
        self.check_motion_client = self.node.create_client(CheckMotion, f"{prefix}/motion/check_motion")

        # ROS 스레드(폴링 결과 저장)와 asyncio 스레드(status 읽기, 명령)가 같이 쓰는 값은 lock으로 보호한다.
        self.lock = threading.Lock()
        self.joints = list(self.HOME)
        self.joints_time = 0.0       # 관절 각도를 마지막으로 받은 시각
        self.motion_idle = None      # 마지막으로 받은 check_motion 결과 (None = 모름)
        self.motion_time = 0.0
        self.motion_cmd_time = 0.0   # 마지막 이동 명령을 보낸 시각
        self.gripper = "OPEN"
        self.executed_ids = collections.deque(maxlen=50)

        self.polling = False
        self.poll_started = 0.0
        self.node.create_timer(self.POLL_PERIOD, self._poll)

        # ROS는 자기만의 이벤트 루프(spin)가 필요하다. asyncio(TCP)와 섞이지 않게 따로 스레드에서 돌린다.
        self.spin_thread = threading.Thread(target=rclpy.spin, args=(self.node,), daemon=True)
        self.spin_thread.start()

    def wait_ready(self, timeout):
        """필요한 서비스가 모두 뜰 때까지 기다린다. 뜨지 않은 서비스 이름 목록을 돌려준다."""
        missing = []
        for client in (self.move_joint_client, self.move_stop_client, self.posj_client, self.check_motion_client):
            if not client.wait_for_service(timeout_sec=timeout):
                missing.append(client.srv_name)
        return missing

    def shutdown(self):
        if rclpy.ok():
            rclpy.shutdown()   # spin 스레드가 빠져나온다
        self.spin_thread.join(timeout=2.0)
        self.node.destroy_node()

    # ---------- 상태 폴링 (ROS 스레드에서 실행) ----------

    def _poll(self):
        now = time.monotonic()
        if self.polling:
            if now - self.poll_started < self.STALE_AFTER:
                return  # 이전 질문의 답이 아직 안 왔다. 겹쳐서 묻지 않는다.
            # 답이 끝내 안 오는 질문이 있다(예: 서비스와 연결되기 전에 보낸 요청은 그냥 사라진다).
            # 계속 기다리면 폴링이 영원히 멈추므로, 그 질문은 버리고 새로 묻는다.
        if not (self.posj_client.service_is_ready() and self.check_motion_client.service_is_ready()):
            return  # 아직 서비스와 연결되지 않았다. 지금 보내면 요청이 사라진다.
        self.polling = True
        self.poll_started = now
        asked_at = now

        posj_future = self.posj_client.call_async(GetCurrentPosj.Request())
        motion_future = self.check_motion_client.call_async(CheckMotion.Request())

        def on_done(_):
            if not (posj_future.done() and motion_future.done()):
                return
            posj = posj_future.result()
            motion = motion_future.result()
            if asked_at != self.poll_started:
                return  # 이미 버린 질문의 늦은 답이다. 새 질문의 결과와 섞지 않는다.
            with self.lock:
                if posj is not None and posj.success:
                    self.joints = list(posj.pos)
                    self.joints_time = asked_at
                if motion is not None and motion.success:
                    self.motion_idle = (motion.status == DR_STATE_IDLE)
                    self.motion_time = asked_at
            self.polling = False

        posj_future.add_done_callback(on_done)
        motion_future.add_done_callback(on_done)

    # ---------- 서비스 호출 (asyncio 스레드에서 실행) ----------

    def _call(self, client, request):
        """서비스를 부르고 응답을 기다린다. 실패하면 ValueError → handle_command가 ok=false ack로 바꿔 준다."""
        if not client.service_is_ready():
            raise ValueError(f"{client.srv_name} 서비스가 없습니다")
        future = client.call_async(request)
        deadline = time.monotonic() + self.SERVICE_TIMEOUT
        while not future.done():
            if time.monotonic() > deadline:
                raise ValueError(f"{client.srv_name} 응답 없음")
            time.sleep(0.005)
        response = future.result()
        if response is None or not response.success:
            raise ValueError(f"{client.srv_name} 실패")
        return response

    def _move_joint(self, joints):
        request = MoveJoint.Request()
        request.pos = [float(j) for j in joints]
        request.vel = float(self.vel)
        request.acc = float(self.acc)
        request.sync_type = MOVE_SYNC_ASYNC
        self._call(self.move_joint_client, request)
        with self.lock:
            self.motion_cmd_time = time.monotonic()

    def movej(self, joints):
        if not isinstance(joints, list) or len(joints) != self.JOINT_COUNT:
            raise ValueError(f"관절 각도 {self.JOINT_COUNT}개가 필요합니다")
        for i, j in enumerate(joints):
            if isinstance(j, bool) or not isinstance(j, (int, float)):
                raise ValueError(f"J{i + 1} 값이 숫자가 아닙니다")
            if abs(j) > self.JOINT_LIMITS[i]:
                raise ValueError(f"J{i + 1} 각도가 범위(±{self.JOINT_LIMITS[i]:g}°)를 벗어났습니다")
        self._move_joint(joints)

    def home(self):
        self._move_joint(self.HOME)

    def stop(self):
        request = MoveStop.Request()
        request.stop_mode = DR_QSTOP
        self._call(self.move_stop_client, request)

    def grip(self, close):
        if not isinstance(close, bool):
            raise ValueError("close는 true/false여야 합니다")
        self.gripper = "CLOSED" if close else "OPEN"

    def status(self):
        now = time.monotonic()
        with self.lock:
            joints = list(self.joints)
            if (self.motion_idle is None or now - self.motion_time > self.STALE_AFTER
                    or now - self.joints_time > self.STALE_AFTER):
                state = "UNKNOWN"   # 컨트롤러와 연락이 끊겼다. Qt는 IDLE이 아니면 이동 명령을 잠근다.
            elif not self.motion_idle:
                state = "MOVING"
            elif (self.motion_time < self.motion_cmd_time + self.MIN_MOVING_TIME):
                # 이동 명령 직후의 IDLE은 명령 전에 물어본 답일 수 있다. 도착한 걸로 착각하지 않게 아직 MOVING.
                state = "MOVING"
            else:
                state = "IDLE"

        return {
            "type": "status",
            "state": state,
            "joints": [round(j, 2) for j in joints],
            "gripper": self.gripper,
        }


async def main():
    parser = make_arg_parser("doosan-robot2 M0609 TCP 브리지")
    parser.add_argument("--ns", default="dsr01", help="doosan-robot2 launch의 name (서비스 이름 앞부분)")
    parser.add_argument("--prefix", default="dsr_controller2", help="컨트롤러 서비스 이름 앞부분")
    parser.add_argument("--vel", type=float, default=30.0, help="movej 속도 [deg/s]")
    parser.add_argument("--acc", type=float, default=60.0, help="movej 가속도 [deg/s^2]")
    args = parser.parse_args()

    # 노드 이름이 겹치면 ROS가 헷갈려 한다. 브리지를 여러 개 띄워도 되게 포트를 이름에 넣는다.
    robot = DsrM0609(args.ns, args.prefix, args.vel, args.acc, node_name=f"qt_tcp_bridge_{args.port}")
    print(f"doosan-robot2 서비스 기다리는 중... (/{args.ns}/{args.prefix}/motion/move_joint 등)")
    missing = robot.wait_ready(timeout=10.0)
    if missing:
        print("서비스를 찾지 못했습니다:", ", ".join(missing))
        print("doosan-robot2를 먼저 띄웠는지, --ns/--prefix가 맞는지(ros2 service list) 확인하세요.")
        robot.shutdown()
        return

    try:
        await serve(robot, args, "doosan-robot2 M0609")
    finally:
        robot.shutdown()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n종료")
