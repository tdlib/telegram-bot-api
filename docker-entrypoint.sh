#!/bin/sh
set -e

LOGS_DIR="/var/log/telegram-bot-api"
LOG_FILENAME="telegram-bot-api.log"
WORK_DIR="/etc/telegram-bot-api"
TEMP_DIR="/tmp/telegram-bot-api"

if [ -n "${1}" ]; then
  exec "${*}"
fi

mkdir -p "${LOGS_DIR}"
mkdir -p "${WORK_DIR}"
mkdir -p "${TEMP_DIR}"

DEFAULT_ARGS="--http-port 8081 --http-stat-port=8082 --dir=${WORK_DIR} --temp-dir=${TEMP_DIR} --log=${LOGS_DIR}/${LOG_FILENAME}"
CUSTOM_ARGS=""

if [ -n "$TELEGRAM_FILTER" ]; then
  CUSTOM_ARGS="${CUSTOM_ARGS} --filter=$TELEGRAM_FILTER"
fi
if [ -n "$TELEGRAM_MAX_WEBHOOK_CONNECTIONS" ]; then
  CUSTOM_ARGS="${CUSTOM_ARGS} --max-webhook-connections=$TELEGRAM_MAX_WEBHOOK_CONNECTIONS"
fi
if [ -n "$TELEGRAM_VERBOSITY" ]; then
  CUSTOM_ARGS="${CUSTOM_ARGS} --verbosity=$TELEGRAM_VERBOSITY"
fi
if [ -n "$TELEGRAM_MAX_CONNECTIONS" ]; then
  CUSTOM_ARGS="${CUSTOM_ARGS} --max-connections=$TELEGRAM_MAX_CONNECTIONS"
fi
if [ -n "$TELEGRAM_PROXY" ]; then
  CUSTOM_ARGS="${CUSTOM_ARGS} --proxy=$TELEGRAM_PROXY"
fi
if [ -n "$TELEGRAM_LOCAL" ]; then
  CUSTOM_ARGS="${CUSTOM_ARGS} --local"
fi

COMMAND="telegram-bot-api ${DEFAULT_ARGS}${CUSTOM_ARGS}"

echo "$COMMAND"
# shellcheck disable=SC2086
exec $COMMAND
