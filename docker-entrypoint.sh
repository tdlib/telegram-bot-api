#!/bin/sh
set -e


USERNAME=telegram-bot-api
GROUPNAME=telegram-bot-api

chown ${USERNAME}:${GROUPNAME} "${TELEGRAM_WORK_DIR}" "${TELEGRAM_TEMP_DIR}"

chmod 700 "${TELEGRAM_TEMP_DIR}"

if [ -n "${1}" ]; then
  exec "${*}"
fi

DEFAULT_ARGS="--http-port 8081 --dir=${TELEGRAM_WORK_DIR} --temp-dir=${TELEGRAM_TEMP_DIR}"
CUSTOM_ARGS=""

if [ -n "$TELEGRAM_STAT" ]; then
  CUSTOM_ARGS="${CUSTOM_ARGS} --http-stat-port=8082"
fi
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
if [ -n "$TELEGRAM_NO_FILE_LIMIT" ]; then
  CUSTOM_ARGS="${CUSTOM_ARGS} --no-file-limit"
fi
if [ -n "$TELEGRAM_ALLOW_USERS" ]; then
  CUSTOM_ARGS="${CUSTOM_ARGS} --allow-users"
fi
if [ -n "$TELEGRAM_ALLOW_USERS_REGISTRATION" ]; then
  CUSTOM_ARGS="${CUSTOM_ARGS} --allow-users-registration"
fi
if [ -n "$TELEGRAM_INSECURE" ]; then
  CUSTOM_ARGS="${CUSTOM_ARGS} --insecure"
fi
if [ -n "$TELEGRAM_RELATIVE" ]; then
  CUSTOM_ARGS="${CUSTOM_ARGS} --relative"
fi
if [ -n "$TELEGRAM_MAX_BATCH" ]; then
  CUSTOM_ARGS="${CUSTOM_ARGS} ---max-batch-operations=$TELEGRAM_MAX_BATCH"
fi
if [ -n "$TELEGRAM_LOGS" ]; then
  CUSTOM_ARGS="$CUSTOM_ARGS --log=${TELEGRAM_LOGS}"
else
  CUSTOM_ARGS="$CUSTOM_ARGS --log=/proc/1/fd/1"
fi

COMMAND="telegram-bot-api ${DEFAULT_ARGS}${CUSTOM_ARGS}"

echo "$COMMAND"
# shellcheck disable=SC2086
exec $COMMAND
