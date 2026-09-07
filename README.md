
## Introduction
OAF is a parental control software based on OpenWrt. It supports popular applications across gaming, video streaming, instant messaging, such as TikTok, YouTube, Facebook. Currently, it supports hundreds of different applications.
For a detailed introduction, please visit [www.openappfilter.com](http://www.openappfilter.com).

## OpenWrt support
The `Qos` branch targets OpenWrt 25.12.5 and newer only. Older OpenWrt releases are intentionally not supported on this branch.

## Features
- DPI-based protocol identification: Supports Layer 7 protocol parsing and HTTPS domain resolution, and operates independently of DNS.
- Industry-standard architecture: Flow-based identification for high efficiency, with extremely low hardware requirements.
- Supports custom protocol signatures.
- Supports installation as a plugin on OpenWrt systems.
- Unified AppFilter time rules with three actions: `Normal`, `Block`, or `Limit`.
- Per-rule upload/download shaping: limit the exact applications, client and time window selected by the existing AppFilter rule.
- Time-aware QoS: active shaping rules follow the existing AppFilter schedule automatically.

## Unified rule configuration
Open LuCI under `Services -> OAF -> App Filter` and edit an existing time rule or create a new one. The rule dialog now contains an `Action` field:

- `Normal`: the rule does not block or shape traffic.
- `Block`: uses the existing OAF application-filter behavior.
- `Limit`: keeps the same application/client/time matching conditions but shapes traffic instead of blocking it. Configure independent upload and download rates in Kbit/s. A value of `0` means unlimited in that direction.

Example:
```
Rule: Evening video apps
Client: child phone
Schedule: Monday-Friday 18:00-22:00
Applications: YouTube, TikTok
Action: Limit
Download: 1024 Kbit/s
Upload: 256 Kbit/s
```

For legacy AppFilter rules that do not have an explicit QoS action, behavior remains `Block`. QoS classification uses the App ID already stored in conntrack state and writes a dedicated skb mark consumed by `tc`; the QoS classifier is built as a separate kernel module so the original OAF filtering module remains unchanged.

## Build dependencies
The QoS path intentionally keeps its package footprint small and uses `tc-tiny` plus the kernel scheduler support needed for HTB shaping.

## How to Compile
1. Prepare an OpenWrt 25.12.5 or newer source tree.
2. Clone the OAF source code under your OpenWrt source tree as `package/OpenAppFilter`.
3. Enable the LuCI package:
```
echo "CONFIG_PACKAGE_luci-app-oaf=y" >> .config
make defconfig
```
4. Compile the packages:
```
make package/luci-app-oaf/compile V=s
make package/open-app-filter/compile V=s
make package/oaf/compile V=s
```
Or rebuild the complete firmware:
```
make V=s
```

## Discussion Group

[https://t.me/openappfilter](https://t.me/openappfilter)

If you encounter issues during installation or usage, please report the OpenWrt version, target architecture, OAF logs and `tc qdisc show` output.

## License
- Individuals can use this software completely free of charge, and are also permitted to develop upon and redistribute it.
- If you undertake derivative development based on OAF, you must adhere to the GPL 2.0 license and retain references to the OAF repository or website information.
- If a company wishes to use the software, please contact the author for authorization.

## Star
If you find this project helpful, please give it a star.
[![Stargazers over time](https://starchart.cc/destan19/OpenAppFilter.svg?variant=adaptive)](https://github.com/destan19/OpenAppFilter)
