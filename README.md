# OpenAppFilter (OAF)

OpenAppFilter (OAF) is an OpenWrt application-awareness and parental-control suite. It identifies supported application traffic with DPI and domain-aware matching, then provides LuCI-managed policies for application filtering, device access control, usage records, whitelists, and per-rule traffic shaping.

For product information and application-signature updates, visit [openappfilter.com](http://www.openappfilter.com). Community discussion: [Telegram](https://t.me/openappfilter).

## Highlights

- **Application identification** — flow-based Layer-7 DPI, including HTTPS domain-aware matching, without relying solely on DNS.
- **Application rules** — scope a policy by selected application(s), all users or one MAC-addressed user, and one or more weekly time ranges.
- **Application blocking** — normal application rules block matching traffic during their active time windows; optional QUIC filtering is available for applications that require it.
- **Per-rule bandwidth limits** — set upload and download limits in Kbps on an application rule. `0` means unlimited. Rate rules are shaped rather than dropped.
- **Traffic shaping implementation** — upload is shaped by HTB on WAN egress; download is redirected from WAN ingress to the dedicated `oaf-ifb0` IFB with `act_mirred`, then shaped by HTB.
- **MAC filtering and blacklist support** — control network access by device, schedule, duration, or traffic allowance.
- **Usage and audit views** — LuCI pages expose clients, recognized applications, records, and dashboard information.
- **Whitelists and custom signatures** — exempt devices from relevant rules and extend the identification database with custom features.

## Components

This repository contains three OpenWrt packages:

| Package | Purpose |
| --- | --- |
| `luci-app-oaf` | LuCI frontend, menus, views, JavaScript, and translations. |
| `open-app-filter` | `oafd` userspace service, UCI/ubus API, rule manager, defaults, and init script. |
| `oaf` | Kernel netfilter/DPI module. |

The LuCI package depends on the service and kernel packages, so selecting `luci-app-oaf` in an OpenWrt build enables the complete stack.

## Build for OpenWrt

1. Start with an OpenWrt source tree that can already build an image for your target.
2. Clone this repository into the package tree:

   ```sh
   git clone https://github.com/destan19/OpenAppFilter.git package/OpenAppFilter
   ```

3. Enable OAF. In `make menuconfig`, select **LuCI → Applications → luci-app-oaf**. Or run:

   ```sh
   echo 'CONFIG_PACKAGE_luci-app-oaf=y' >> .config
   make defconfig
   ```

4. Build just the packages while developing:

   ```sh
   make package/luci-app-oaf/compile V=s
   make package/open-app-filter/compile V=s
   make package/oaf/compile V=s
   ```

   Or build a complete firmware image:

   ```sh
   make V=s
   ```

The application-filter service declares the `tc` utility and the HTB, IFB, and `act_mirred` kernel dependencies required for bandwidth shaping.

## Installation and service control

Install the generated `.ipk` packages that match the target architecture, then start and enable the service:

```sh
/etc/init.d/appfilter enable
/etc/init.d/appfilter start
```

Useful operational commands:

```sh
/etc/init.d/appfilter restart
logread | grep -E 'oafd|rule_manager'
```

The service initializes missing default UCI configuration files under `/etc/config/`, loads the `oaf` kernel module, starts `oafd`, and starts the rule manager. On stop, it removes OAF's IFB device and its traffic-control setup.

## Configure application rules

Open **Services → OAF → App Filter Rules** in LuCI and create or edit a rule:

1. Enter a rule name and enable it when ready.
2. Choose **All Users** or **Single User** and select the user when needed.
3. Add one or more weekday/time ranges.
4. Select one or more applications. Enable **Filter QUIC** only where appropriate.
5. Optionally set **Upload Limit (Kbps)** and **Download Limit (Kbps)**.

The corresponding `/etc/config/appfilter` rule uses the existing rule scope fields plus two optional rate fields:

```uci
config rule
        option id '1700000000'
        option name 'Evening video limit'
        option mode '2'
        option user_mac 'AA:BB:CC:DD:EE:FF'
        option enabled '1'
        option upload_kbps '1000'
        option download_kbps '5000'
        list app_id '1001'
        list time_rule '1,2,3,4,5,18:00,22:00'
```

- `upload_kbps` and `download_kbps` accept non-negative integers from `0` to `10000000`.
- An omitted rate field is treated as `0`, preserving compatibility with existing rules.
- A nonzero rate makes the matching rule a **shaping rule**; it does not block matching traffic.
- When multiple active rate rules match the same flow, OAF installs the more restrictive rule first; the lowest nonzero configured rate wins, with rule ID used as a deterministic tie-breaker.
- Rate limits only exist while the rule is enabled and inside a matching time range. Editing, disabling, deleting, restarting the service, or leaving the time window removes or replaces the relevant OAF traffic-control state.

### Traffic-control resources reserved by OAF

OAF reserves a narrow, package-specific range to avoid fwmark/class collisions:

- packet marks: `0x4f000001`–`0x4f000040`
- HTB class minors: `1:101`–`1:140`
- IFB device: `oaf-ifb0`

Because Linux has one root qdisc per interface, do not run another tool that replaces the WAN root qdisc at the same time as OAF rate rules. If another QoS system owns the WAN root qdisc, integrate the OAF mark range into that QoS policy instead of enabling both independent root-qdisc managers.

## Verify rate-limit lifecycle on a target

The repository includes a target-side verification script. It is currently intended to be run from a source checkout (it is not installed by the package Makefile):

```sh
sh open-app-filter/tests/verify_rate_limit_lifecycle.sh
```

It creates two active rules with different application IDs and rates, checks WAN/IFB HTB classes, edits one rate, deletes both rules, and restarts the service to check cleanup. Run it only on a test router: it writes temporary `appfilter` UCI sections and restarts OAF.

To inspect the active shaping state manually:

```sh
WAN="$(ubus call network.interface.wan status | jsonfilter -e '@.l3_device')"
tc qdisc show dev "$WAN"
tc class show dev "$WAN"
tc filter show dev "$WAN" parent 1:
tc class show dev oaf-ifb0
```

## License

- Individuals may use, modify, and redistribute this software free of charge.
- Derivative development must comply with GPL-2.0 and retain references to the OAF repository or website.
- Commercial use requires authorization from the author.

## Support the project

If OAF is useful to you, please star the repository.

[![Stargazers over time](https://starchart.cc/destan19/OpenAppFilter.svg?variant=adaptive)](https://starchart.cc/destan19/OpenAppFilter)
