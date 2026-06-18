#!/bin/sh
LOCKDIR="/var/run/fancts.lockdir"
PIDFILE="/var/run/fancts.pid"
LOGTAG="Airpifanctrl"

if ! mkdir "$LOCKDIR" 2>/dev/null; then
  oldpid="$(cat "$PIDFILE" 2>/dev/null)"
  if [ -n "$oldpid" ] && kill -0 "$oldpid" 2>/dev/null; then
    logger -t "$LOGTAG" "fancts already running pid=$oldpid"
    exit 0
  fi
  rm -rf "$LOCKDIR"
  mkdir "$LOCKDIR" 2>/dev/null || exit 0
fi

echo $$ > "$PIDFILE"

cleanup() {
  rm -rf "$LOCKDIR"
  rm -f "$PIDFILE"
  exit 0
}
trap cleanup INT TERM EXIT

get_cpu_temp() {
  for f in /sys/class/thermal/thermal_zone*/temp; do
    [ -f "$f" ] || continue
    raw="$(cat "$f" 2>/dev/null)"
    case "$raw" in
      ''|*[!0-9]*) continue ;;
    esac
    if [ "$raw" -gt 1000 ]; then
      t=$((raw / 1000))
    else
      t="$raw"
    fi
    if [ "$t" -ge 20 ] && [ "$t" -le 120 ]; then
      echo "$t"
      return 0
    fi
  done
  echo ""
  return 1
}

get_modem_temp() {
  v="$(/usr/bin/airpi-modem-temp-raw 2>/dev/null)"
  case "$v" in
    ''|*[!0-9]*) echo ""; return 1 ;;
  esac
  t=$((v / 10))
  if [ "$t" -ge 20 ] && [ "$t" -le 120 ]; then
    echo "$t"
    return 0
  fi
  echo ""
  return 1
}

get_temp() {
  mode="$(cat /etc/fanvallv.conf 2>/dev/null)"
  case "$mode" in
    *模组温度*)
      t="$(get_modem_temp)"
      case "$t" in
        ''|*[!0-9]*)
          logger -t "$LOGTAG" "modem temp invalid, fallback to CPU temp"
          get_cpu_temp
          ;;
        *)
          echo "$t"
          ;;
      esac
      ;;
    *)
      get_cpu_temp
      ;;
  esac
}

logger -t "$LOGTAG" "smart fancts started, temp_source=$(cat /etc/fanvallv.conf 2>/dev/null)"
last_duty=192
bad_count=0

while true; do
  mode="$(cat /etc/fanvall 2>/dev/null)"
  [ "$mode" = "9" ] || {
    logger -t "$LOGTAG" "smart fancts exit, fanvall=$mode"
    cleanup
  }

  temp="$(get_temp)"
  case "$temp" in
    ''|*[!0-9]*)
      bad_count=$((bad_count + 1))
      duty="$last_duty"
      if [ "$bad_count" -ge 2 ] && [ "$duty" -lt 192 ]; then
        duty=192
      fi
      /usr/bin/fan-write-duty "$duty"
      logger -t "$LOGTAG" "smart temp invalid, keep duty=${duty}, bad_count=${bad_count}"
      sleep 10
      continue
      ;;
  esac

  bad_count=0
  if [ "$temp" -lt 45 ]; then
    duty=64
  elif [ "$temp" -lt 55 ]; then
    duty=128
  elif [ "$temp" -lt 65 ]; then
    duty=192
  else
    duty=255
  fi

  last_duty="$duty"
  /usr/bin/fan-write-duty "$duty"
  logger -t "$LOGTAG" "smart temp=${temp}C duty=${duty}"
  sleep 10
done
