#!/bin/sh
# Run on an OpenWrt target after installing appfilter.  Uses two active app rules
# and verifies tc's OAF-reserved mark/class lifecycle.
set -eu
WAN="$(ubus call network.interface.wan status | jsonfilter -e '@.l3_device')"
[ -n "$WAN" ]
uci -q delete appfilter.rate_test_a; uci -q delete appfilter.rate_test_b
uci set appfilter.rate_test_a='rule'; uci set appfilter.rate_test_a.id='900001'; uci set appfilter.rate_test_a.name='rate-test-a'; uci set appfilter.rate_test_a.mode='1'; uci set appfilter.rate_test_a.enabled='1'; uci add_list appfilter.rate_test_a.app_id='1001'; uci add_list appfilter.rate_test_a.time_rule='0,1,2,3,4,5,6,00:00,23:59'; uci set appfilter.rate_test_a.upload_kbps='1000'; uci set appfilter.rate_test_a.download_kbps='2000'
uci set appfilter.rate_test_b='rule'; uci set appfilter.rate_test_b.id='900002'; uci set appfilter.rate_test_b.name='rate-test-b'; uci set appfilter.rate_test_b.mode='1'; uci set appfilter.rate_test_b.enabled='1'; uci add_list appfilter.rate_test_b.app_id='1002'; uci add_list appfilter.rate_test_b.time_rule='0,1,2,3,4,5,6,00:00,23:59'; uci set appfilter.rate_test_b.upload_kbps='3000'; uci set appfilter.rate_test_b.download_kbps='4000'
uci commit appfilter; echo 1 > /tmp/appfilter_rules_state; sleep 15
tc class show dev "$WAN" | grep -q 'rate 1000Kbit'
tc class show dev oaf-ifb0 | grep -q 'rate 2000Kbit'
# Editing must replace immediately on the next manager pass.
uci set appfilter.rate_test_a.upload_kbps='1500'; uci commit appfilter; echo 1 > /tmp/appfilter_rules_state; sleep 15
tc class show dev "$WAN" | grep -q 'rate 1500Kbit'
# Outside the interval removes the rule's class; deletion/restart leave no OAF IFB.
uci delete appfilter.rate_test_a; uci delete appfilter.rate_test_b; uci commit appfilter; echo 1 > /tmp/appfilter_rules_state; sleep 15
! tc class show dev "$WAN" | grep -q '1:10[1-9]'
/etc/init.d/appfilter restart; sleep 3
! ip link show oaf-ifb0 >/dev/null 2>&1
echo 'rate-limit lifecycle verification passed'
