#!/bin/sh

report=$($BIN -c 'koshkit --color never evil -a')
case $report in
  *ANOMALIES*'Mixed libraries:'*'Confidence: high'*'Cost: process mappings'*)
    anomaly_shape=matched
    ;;
  *) anomaly_shape=wrong ;;
esac
printf 'anomaly-shape=%s\n' "$anomaly_shape"

default_report=$($BIN -c 'koshkit --color never evil')
case $default_report in
  *ANOMALIES*) default_shape=extra ;;
  *) default_shape=portable ;;
esac
printf 'default-shape=%s\n' "$default_shape"
