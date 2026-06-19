'use strict';

import * as fs from 'fs';
import * as rtnl from 'rtnl';
import * as sysnet from 'hnat.sysnet';
import { log, merge, read_pipe } from 'hnat.utils.common';

const RX_PPD_NAME = "rxppd";

function trimnl(s) {
    return s ? replace(s, /[\r\n]+$/, "") : "";
}

function readfile(p) {
    let v = fs.readfile(p);
    return trimnl(v);
}

function is_gmac(name) {
    return name && match(name, /^eth[0-9]+$/);
}

function is_ext(name) {
    return name && match(name, /^(usb|wwan|eth)/);
}

function get_default_dev() {
    let d = read_pipe("ip -4 route show default 2>/dev/null | sed -n 's/.* dev \\([^ ]*\\).*/\\1/p' | head -n1");
    if (!d)
        d = read_pipe("ip -6 route show default 2>/dev/null | sed -n 's/.* dev \\([^ ]*\\).*/\\1/p' | head -n1");
    return d;
}

function carrier_up(dev) {
    if (!dev)
        return false;
    let v = read_pipe(`cat /sys/class/net/${dev}/carrier 2>/dev/null`);
    return v == "1";
}

function is_eth_anchor_down(dev) {
    return dev && match(dev, /^eth/) && !carrier_up(dev);
}

function cur_state() {
    return {
        ppd:  trimnl(readfile("/sys/kernel/debug/hnat/hnat_ppd_if")),
        wan:  trimnl(readfile("/sys/kernel/debug/hnat/hnat_wan_if")),
        lan:  trimnl(readfile("/sys/kernel/debug/hnat/hnat_lan_if")),
        lan2: trimnl(readfile("/sys/kernel/debug/hnat/hnat_lan2_if")),
        hook: trimnl(readfile("/sys/kernel/debug/hnat/hook_toggle"))
    };
}

function hook_toggle() {
    return trimnl(readfile("/sys/kernel/debug/hnat/hook_toggle")) == "enabled";
}

function write_debug(path, val) {
    let f = fs.open(path, "w");
    if (f) {
        f.write(val);
        f.close();
    }
}

function main() {
    let logger = log();

    # 逻辑 LAN
    let br_dev = "br-lan";

    # 当前 bridge 成员
    let br_members = sysnet.br_members(br_dev) || [];

    # 当前默认路由出口
    let actual_wan_dev = get_default_dev();
    let actual_wan_is_gmac = actual_wan_dev && is_gmac(actual_wan_dev);
    let external_default_wan = actual_wan_dev && !actual_wan_is_gmac;

    # HNAT anchor：Airpi 当前固定策略
    let ppd_name = "eth0";
    let lan_name = "eth0";
    let wan_name = "eth1";
    let lan2_name = "";

    # ext 设备列表：从 bridge 成员 + 默认出口综合判断
    let ext_candidates = [];
    if (actual_wan_dev && is_ext(actual_wan_dev) && !is_gmac(actual_wan_dev))
        push(ext_candidates, actual_wan_dev);

    for (let d in br_members) {
        if (is_ext(d) && !is_gmac(d) && index(ext_candidates, d) < 0)
            push(ext_candidates, d);
    }

    let ext_devs = ext_candidates;
    let pending_external_wan = !actual_wan_dev && length(ext_devs) > 0;
    let hnat_wan_mismatch = external_default_wan && wan_name && actual_wan_dev != wan_name;
    let eth_anchor_down = is_eth_anchor_down(ppd_name) || is_eth_anchor_down(lan_name);

    let rxppd_safe = true;
    if ((external_default_wan || pending_external_wan) &&
        (pending_external_wan || hnat_wan_mismatch || eth_anchor_down)) {
        rxppd_safe = false;
        logger.warn(sprintf(
            "rxppd unsafe: actual_wan=%s, hnat_wan=%s, ppd=%s, lan=%s, pending=%s, mismatch=%s, eth_anchor_down=%s",
            actual_wan_dev || "-",
            wan_name || "-",
            ppd_name || "-",
            lan_name || "-",
            pending_external_wan ? "1" : "0",
            hnat_wan_mismatch ? "1" : "0",
            eth_anchor_down ? "1" : "0"
        ));
    }
    else {
        logger.info(sprintf(
            "rxppd policy: actual_wan=%s, external_default_wan=%s, hnat_wan=%s, ppd=%s, lan=%s, safe=%s",
            actual_wan_dev || "-",
            external_default_wan ? "1" : "0",
            wan_name || "-",
            ppd_name || "-",
            lan_name || "-",
            rxppd_safe ? "1" : "0"
        ));
    }

    # 先维持 HNAT anchor 为 eth0 / eth1，不改成 br-lan
    let st = cur_state();
    let hnat_changed = false;

    if (ppd_name && st.ppd != ppd_name) {
        write_debug("/sys/kernel/debug/hnat/hnat_ppd_if", ppd_name);
        hnat_changed = true;
    }
    if (wan_name && st.wan != wan_name) {
        write_debug("/sys/kernel/debug/hnat/hnat_wan_if", wan_name);
        hnat_changed = true;
    }
    if (lan_name && st.lan != lan_name) {
        write_debug("/sys/kernel/debug/hnat/hnat_lan_if", lan_name);
        hnat_changed = true;
    }
    if (lan2_name && st.lan2 != lan2_name) {
        write_debug("/sys/kernel/debug/hnat/hnat_lan2_if", lan2_name);
        hnat_changed = true;
    }

    let need_hook_toggle = hnat_changed && hook_toggle();

    if (need_hook_toggle)
        write_debug("/sys/kernel/debug/hnat/hook_toggle", "0");

    # rxppd 只做 attach / cleanup，不因为它自己变化去重置 hook_toggle
    if (length(ext_devs) > 0 && rxppd_safe) {
        if (br_dev) {
            logger.info(sprintf("ext devices: %J, enable %s on %s", ext_devs, RX_PPD_NAME, br_dev));
            system(sprintf("ip link show %s >/dev/null 2>&1 || ip link add %s type dummy", RX_PPD_NAME, RX_PPD_NAME));
            system(sprintf("ip link set %s up", RX_PPD_NAME));
            if (index(sysnet.br_members(br_dev) || [], RX_PPD_NAME) < 0)
                system(sprintf("ip link set %s master %s", RX_PPD_NAME, br_dev));
        }
    }
    else {
        if (sysnet.dev_exist(RX_PPD_NAME)) {
            if (br_dev && index(sysnet.br_members(br_dev) || [], RX_PPD_NAME) >= 0) {
                logger.info(sprintf("rxppd cleanup: detach %s from %s", RX_PPD_NAME, br_dev));
                system(sprintf("ip link set %s nomaster", RX_PPD_NAME));
            }
            logger.info(sprintf("rxppd cleanup: down/delete %s", RX_PPD_NAME));
            system(sprintf("ip link set %s down || true", RX_PPD_NAME));
            system(sprintf("ip link delete %s || true", RX_PPD_NAME));
        }
    }

    if (need_hook_toggle)
        write_debug("/sys/kernel/debug/hnat/hook_toggle", "1");
}

main();
