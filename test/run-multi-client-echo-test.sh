#! /bin/bash

set -eu

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
PID_DIR="${SCRIPT_DIR}/.pids"
BUILD_DIR="${SCRIPT_DIR}/../build"
HELPER="${BUILD_DIR}/mqtt-forward-test-helper"
CLIENT_PORT=12390
ECHO_PORT=12391
CLIENTS=8
TIMEOUT_MS=15000
UNSECURE=""
echo_pid=""
SERVER_LOG="${PID_DIR}/mqtt-forward-server.log"
CLIENT_LOG="${PID_DIR}/mqtt-forward-client.log"

wait_for_broker_connection() {
	local log_file=$1
	local name=$2
	local timeout_s=20
	local elapsed=0

	while [[ "${elapsed}" -lt "${timeout_s}" ]]; do
		if [[ -f "${log_file}" ]] &&
		   grep -q "Successfully connected to broker!" "${log_file}"; then
			return 0
		fi

		sleep 1
		elapsed=$((elapsed + 1))
	done

	echo "${name} did not connect to the broker within ${timeout_s} seconds"
	if [[ -f "${log_file}" ]] ; then
		cat "${log_file}"
	fi

	return 1
}

cleanup() {
	if [[ -n "${echo_pid}" ]] && kill -0 "${echo_pid}" 2>/dev/null; then
		kill "${echo_pid}" 2>/dev/null || true
		wait "${echo_pid}" 2>/dev/null || true
	fi

	"${SCRIPT_DIR}"/teardown.sh >/dev/null 2>&1 || true
}

while [[ "$#" -gt 0 ]]; do
	case "$1" in
		-u|--unsecure)
			UNSECURE="--unsecure"
			shift
			;;
		-n|--clients)
			CLIENTS="$2"
			shift 2
			;;
		*)
			echo "Unknown option: $1"
			exit 1
			;;
	esac
done

if [[ ! -x "${HELPER}" ]] ; then
	echo "Missing helper binary: ${HELPER}. Build the project first."
	exit 1
fi

trap cleanup EXIT

mkdir -p "${PID_DIR}"
rm -f "${PID_DIR}"/*.pid

echo "Starting Mosquitto test broker"
"${SCRIPT_DIR}"/setup-mqtt-docker.sh ${UNSECURE}
"${SCRIPT_DIR}"/start-mqtt-docker.sh

echo "Starting local echo service on port ${ECHO_PORT}"
"${HELPER}" echo-server --port "${ECHO_PORT}" &
echo_pid="$!"

"${HELPER}" multi-client \
	--host 127.0.0.1 \
	--port "${ECHO_PORT}" \
	--clients 1 \
	--payload-size 64 \
	--timeout-ms 2000 >/dev/null

echo "Starting server-side mqtt-forward"
"${SCRIPT_DIR}"/start-mqtt-forward.sh -s ${UNSECURE} --port "${ECHO_PORT}" \
	>"${SERVER_LOG}" 2>&1 &
echo "$!" > "${PID_DIR}/mqtt-forward-server.pid"

echo "Starting client-side mqtt-forward"
"${SCRIPT_DIR}"/start-mqtt-forward.sh ${UNSECURE} --port "${CLIENT_PORT}" \
	>"${CLIENT_LOG}" 2>&1 &
echo "$!" > "${PID_DIR}/mqtt-forward-client.pid"

wait_for_broker_connection "${SERVER_LOG}" "Server-side mqtt-forward"
wait_for_broker_connection "${CLIENT_LOG}" "Client-side mqtt-forward"

echo "Running ${CLIENTS} simultaneous echo clients through mqtt-forward"
"${HELPER}" multi-client \
	--host 127.0.0.1 \
	--port "${CLIENT_PORT}" \
	--clients "${CLIENTS}" \
	--payload-size 1024 \
	--timeout-ms "${TIMEOUT_MS}"

echo "Multi-client echo test passed"
