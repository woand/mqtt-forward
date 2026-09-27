#!/bin/bash
# scripts/ensure.sh - 保证 mqtt-forward 隧道三件套都在运行，缺哪个补哪个。
# 幂等，可反复执行；每个服务都带 while true 守护循环，崩溃自动拉起。
#
# 三件套: sshdog(SSH server, 默认 :2222)
#         + socat 中继(默认 127.0.0.1:1884 -> broker，见 mqtt-relay.sh)
#         + mqtt-forward --server
#
# 用法:
#   ./scripts/ensure.sh           # 检查一次，补齐缺失的服务
#   ./scripts/ensure.sh --watch   # 自己进后台常驻，每 30 秒检查一次
#
# 环境变量（都有默认值）:
#   SSHDOG_DIR      部署目录，日志和二进制都在这里 (默认 /home/hatch/workspace/sshdog)
#   SSH_PORT        sshdog 监听端口 (默认 2222)
#   RELAY_ADDR      中继监听地址 (默认 127.0.0.1:1884)；留空则跳过中继检查
#   MQTT_HOST       mqtt-forward 连接的 broker host (默认 127.0.0.1)
#   MQTT_PORT       mqtt-forward 连接的 broker port (默认 1884)
#   SERVER_SIDE_ID  服务端 ID；也可写在 $SSHDOG_DIR/SERVER_ID.txt 里（二选一，必须提供）
#   WATCH_INTERVAL  --watch 检查间隔秒数 (默认 30)
#
# 注意: 无 cron 的环境（如容器）重启后需手动重跑一次 --watch。
set -u

SSHDOG_DIR="${SSHDOG_DIR:-/home/hatch/workspace/sshdog}"
SSH_PORT="${SSH_PORT:-2222}"
RELAY_ADDR="${RELAY_ADDR:-127.0.0.1:1884}"
MQTT_HOST="${MQTT_HOST:-127.0.0.1}"
MQTT_PORT="${MQTT_PORT:-1884}"
WATCH_INTERVAL="${WATCH_INTERVAL:-30}"
SERVER_SIDE_ID="${SERVER_SIDE_ID:-$(cat "$SSHDOG_DIR/SERVER_ID.txt" 2>/dev/null || true)}"

cd "$SSHDOG_DIR" || { echo "SSHDOG_DIR 不存在: $SSHDOG_DIR"; exit 1; }
export LD_LIBRARY_PATH="$SSHDOG_DIR/lib:${LD_LIBRARY_PATH:-}"

log() { echo "$(date '+%F %T') ensure: $*"; }

# ---- 检查 ----
up_sshdog() { ss -tln 2>/dev/null | grep -q ":$SSH_PORT "; }
up_relay()  { [ -n "$RELAY_ADDR" ] && ss -tln 2>/dev/null | grep -q "$RELAY_ADDR"; }
up_mqtt()   { pgrep -ax mqtt-forward 2>/dev/null | grep -q -- '--server'; }

# ---- 启动（setsid 脱离终端，while true 守护）----
start_sshdog() {
  setsid bash -c "cd '$SSHDOG_DIR' && while true; do ./sshdog_kxb >>sshdog.log 2>&1; echo \"\$(date): sshdog exited, restarting\" >>sshdog.log; sleep 2; done" >/dev/null 2>&1 </dev/null &
}
start_relay() {
  [ -x "$SSHDOG_DIR/mqtt-relay.sh" ] || { log "无 mqtt-relay.sh，跳过中继启动"; return 0; }
  setsid bash -c "cd '$SSHDOG_DIR' && while true; do ./mqtt-relay.sh >>relay.log 2>&1; echo \"\$(date): relay exited, restarting\" >>relay.log; sleep 2; done" >/dev/null 2>&1 </dev/null &
}
start_mqtt() {
  [ -n "$SERVER_SIDE_ID" ] || { log "错误: 未提供 SERVER_SIDE_ID（环境变量或 $SSHDOG_DIR/SERVER_ID.txt）"; return 1; }
  setsid bash -c "cd '$SSHDOG_DIR' && export LD_LIBRARY_PATH='$SSHDOG_DIR/lib:\$LD_LIBRARY_PATH' && while true; do ./mqtt-forward/build/mqtt-forward --server --port $SSH_PORT --mqtt-host $MQTT_HOST --mqtt-port $MQTT_PORT --server-side-id '$SERVER_SIDE_ID' -b >>mqtt-forward.log 2>&1; echo \"\$(date): mqtt-forward exited, restarting\" >>mqtt-forward.log; sleep 3; done" >/dev/null 2>&1 </dev/null &
}

ensure_once() {
  local changed=0
  if up_sshdog; then log "sshdog(:$SSH_PORT) 运行中"; else log "sshdog 未运行，启动"; start_sshdog; changed=1; fi
  if [ -z "$RELAY_ADDR" ]; then
    log "RELAY_ADDR 为空，跳过中继检查"
  elif up_relay; then log "relay($RELAY_ADDR) 运行中"; else log "relay 未运行，启动"; start_relay; changed=1; fi
  if up_mqtt; then log "mqtt-forward(server) 运行中"; else log "mqtt-forward 未运行，启动"; start_mqtt || return 1; changed=1; fi
  [ "$changed" = 1 ] && sleep 6
  if up_sshdog && { [ -z "$RELAY_ADDR" ] || up_relay; } && up_mqtt; then
    log "全部正常"
  else
    log "警告: 仍有服务未起来，请看 $SSHDOG_DIR/*.log"
    return 1
  fi
}

# ---- --watch: 自己进后台常驻 ----
if [ "${1:-}" = "--watch" ] && [ -z "${ENSURE_DAEMON:-}" ]; then
  ENSURE_DAEMON=1 setsid "$0" --watch >>"$SSHDOG_DIR/ensure.log" 2>&1 </dev/null &
  echo "watchdog 已在后台启动，日志: $SSHDOG_DIR/ensure.log"
  echo "（无 cron 的环境）重启后需重新执行一次: $0 --watch"
  exit 0
fi

if [ "${1:-}" = "--watch" ]; then
  log "watchdog 启动，每 ${WATCH_INTERVAL}s 检查一次"
  while true; do ensure_once >>"$SSHDOG_DIR/ensure.log" 2>&1 || true; sleep "$WATCH_INTERVAL"; done
  exit 0
fi

ensure_once
