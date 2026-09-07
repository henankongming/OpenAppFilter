

## Introduction
OAF is a parental control software based on OpenWrt. It supports popular applications across gaming, video streaming, instant messaging, such as TikTok, YouTube, Facebook. Currently, it supports hundreds of different applications.
For a detailed introduction, please visit [www.openappfilter.com](http://www.openappfilter.com).

## OpenWrt support
The `Qos` branch targets OpenWrt 25.12.5 and newer only. Older OpenWrt releases are intentionally not supported on this branch. OpenWrt 25.12 uses `apk` as its package manager and the QoS implementation uses the 25.12 networking stack.

## Features
- DPI-based protocol identification: Supports Layer 7 protocol parsing and HTTPS domain resolution, and operates independently of DNS.
- Industry-standard architecture: Flow-based identification for high efficiency, with extremely low hardware requirements.
- Supports custom protocol signatures: Offers a high degree of flexibility and customization.
- Supports installation as a plugin on OpenWrt systems: Compatible with OpenWrt 25.12.5+ devices.
- Per-application QoS profiles: Attach a bandwidth profile to an existing AppFilter rule and reuse its application, client and time-window conditions.
- Separate upload/download shaping: Configure independent Kbit/s values for each QoS profile.
- Time-aware QoS: Active QoS classifiers are refreshed automatically as the existing AppFilter schedule changes.

## QoS configuration
Enable `Application QoS` in LuCI under `Services -> OAF -> QoS`. The recommended workflow is to first create an ordinary AppFilter rule containing the desired applications, client and time windows, then attach a QoS profile to that rule.

Example:
```
AppFilter rule: Evening video apps
Schedule: Monday-Friday 18:00-22:00
Applications: YouTube, TikTok
Client: child phone

QoS profile:
Download: 1024 Kbit/s
Upload: 256 Kbit/s
```

The QoS layer classifies packets from the AppFilter application id stored in conntrack state and uses a dedicated skb mark range for `tc`. `tc-tiny` and `kmod-sched-core` are used to keep the dependency footprint small.

## How to Compile
1. Prepare a set of OpenWrt 25.12.5 or newer source code that has already been successfully compiled into firmware.
2. Clone the OAF source code. Navigate to the root directory of your OpenWrt source code and execute:
```
git clone https://github.com/destan19/OpenAppFilter.git package/OpenAppFilter
```
3. Enable the LuCI package:
```
echo "CONFIG_PACKAGE_luci-app-oaf=y" >> .config
make defconfig
```
This automatically selects the OAF service and kernel modules.
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

[https://t.me/openappfilter](https://t.me/openappfilter) (Telegram)

If you encounter issues during installation or usage, you can join the group for discussion.

## License
- Individuals can use this software completely free of charge, and are also permitted to develop upon and redistribute it.
- If you undertake derivative development based on OAF, you must adhere to the GPL 2.0 license and retain references to the OAF repository or website information.
- If a company wishes to use the software, please contact the author for authorization.

## Star
If you find this project helpful, please give it a star.
[![Stargazers over time](https://starchart.cc/destan19/OpenAppFilter.svg?variant=adaptive)](https://starchart.cc/destan19/OpenAppFilter)
