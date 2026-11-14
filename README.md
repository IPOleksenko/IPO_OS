# IPO_OS

## ⚙️ Compilation Commands

### 📦 Install Dependencies
Install the build tools and dependencies:
```bash
./install-dependencies.sh
```

### 🔨 Build Commands

#### `make all` — Full Build and Run
Executes the complete build pipeline in order, then runs the OS in QEMU:
- Cleans previous build outputs
- Builds kernel library (`libk.a`), kernel binary, and bootloader
- Prepares OS and virtual disk images
- Launches QEMU with the OS

```bash
make all
```

#### `make lib` — Build Kernel Library
Compiles the kernel subsystem library (`build/lib/libk.a`):
```bash
make lib
```

#### `make kernel` — Build Kernel Only
Compiles kernel, drivers, filesystems, networking, graphics and arch HAL:
```bash
make kernel
```

#### `make boot` — Build Bootloader
Builds the kernel prerequisite used to calculate boot configuration, then assembles the bootloader:
```bash
make boot
```

#### `make image` — Build OS Binary
Compiles the bootable OS binary (`build/IPO_OS.bin`):
```bash
make image
```

#### `make disk` — Install OS onto Virtual Disk
Ensures the 512 MiB raw virtual disk (`build/disk/disk.img`) exists and installs `build/IPO_OS.bin` onto its boot area:
```bash
make disk
```

#### `make setup-tap` — Setup TAP Network Interface
Configures the host virtual TAP interface (`tap0`) for direct L2 communication and NAT internet access with IPO_OS:
- Attempts to load the `tun` kernel module (`modprobe tun`) and ensure `/dev/net/tun` exists
- Assigns host IP `192.168.7.1/24` to `tap0` and adds a host route for `guest_ip/32`
- Enables IP forwarding and `iptables` NAT (`MASQUERADE` for `192.168.7.0/24`)
- Starts a local `dnsmasq` DHCP server on `tap0` (offering `guest_ip`, default `192.168.7.2`, gateway `192.168.7.1`, DNS `8.8.8.8, 1.1.1.1`) if `dnsmasq` is installed

```bash
make setup-tap
```

#### `make run` — Launch in QEMU
Runs `build/disk/disk.img` in QEMU (`2048` MiB RAM, audio enabled via `AUDIODEV`, serial `stdio`; configured in `mk/config.mk`):
```bash
make run
```

You can also run with explicit paths to a full BIOS ROM image or Boot ROM image (`BOOTROM` takes precedence if both are specified):
```bash
# Launch with a full BIOS ROM image:
make run FIRMWARE=path/to/firmware.bin

# Launch with explicit Boot ROM:
make run BOOTROM=path/to/bootrom.bin
```

#### `make` — Same as `make all`
Default target (DEFAULT_GOAL is set to `all`):
```bash
make
```

### 🧹 Cleanup Commands

#### `make clean` — Remove Build Outputs (Preserves Virtual Disk)
Removes build outputs inside `build/` (`IPO_OS.bin`, `boot/`, `kernel/`, `lib/`) while **preserving `build/disk/`** (`disk.img`):
```bash
make clean
```

#### `make clean-disk` — Remove Virtual Disk
Deletes `build/disk/` including `build/disk/disk.img`:
```bash
make clean-disk
```

#### `make clean-lib` — Clean Library Only
Removes only the library build artifacts (`build/lib`):
```bash
make clean-lib
```

### 🔧 Utility Commands

#### `make patch-config` — Update Boot Configuration
Updates the boot configuration with kernel size information:
```bash
make patch-config
```

## 🛠️ Build & Development Tools (`tools/`)

The `tools/` directory contains host-side build automation scripts, symbol generators, and network configuration utilities:

---

### **1. generate_ksyms.py — Kernel Symbol Table Generator**
- **File:** [tools/generate_ksyms.py](tools/generate_ksyms.py)
- **Purpose:** Extracts defined symbols from `build/kernel/kernel.elf` using `nm` and outputs a sorted symbol table in `build/kernel/kernel_syms_autogen.c`.
- **Invoked by:** `make kernel`
- **Manual Usage:**
```bash
python3 tools/generate_ksyms.py -e build/kernel/kernel.elf -o build/kernel/kernel_syms_autogen.c
```
- **Features:**
  - Provides the symbol database for the dynamic driver subsystem (`kmod`).
  - Powers the OS shell's runtime `symbols` command to list generated symbol-table entries and addresses.

---

### **2. setup-tap.sh — Virtual TAP Network Interface Setup**
- **File:** [tools/setup-tap.sh](tools/setup-tap.sh)
- **Purpose:** Automatically provisions the host `tap0` virtual interface with IP `192.168.7.1/24`, configures IP forwarding/NAT, and starts a local `dnsmasq` DHCP server (when available) for direct networking with IPO_OS inside QEMU.
- **Invoked by:** `make setup-tap`, `make run`
- **Manual Usage:**
```bash
./tools/setup-tap.sh [tap_dev] [host_ip] [netmask] [guest_ip]
```
- **Defaults:** `tap_dev=tap0`, `host_ip=192.168.7.1`, `netmask=24`, `guest_ip=192.168.7.2`.
- Guest Internet NAT is configured only for `192.168.7.0/24`; changing the subnet requires updating the NAT rule in `tools/setup-tap.sh`.

---

## 🌐 Networking (TAP Mode & Direct Port Access)

IPO_OS implements a native bare-metal TCP/IP stack (`src/net/`: PCI Ethernet NIC driver, loopback, Ethernet, ARP, IPv4 with routing table and default gateway, ICMP, UDP, TCP, DHCP client, DNS resolver, and a POSIX-like socket API).

### 🚀 TAP Network Interface Setup (`setup-tap.sh`)

By default, QEMU is configured in **TAP networking mode** (`NET_MODE ?= tap` in `mk/config.mk`). TAP connects the host and guest directly, without QEMU port-forwarding rules. If TAP setup fails during `make run`, the target retries with QEMU user networking (`NET_MODE=user`). A service is reachable only if it is listening on the requested port and the host firewall permits the connection.

#### Script: `tools/setup-tap.sh`
```bash
./tools/setup-tap.sh [tap_dev] [host_ip] [netmask] [guest_ip]
```
- Parameters:
  - `tap_dev` — virtual TAP interface name (default: `tap0`)
  - `host_ip` — IP assigned to host interface (default: `192.168.7.1`)
  - `netmask` — subnet CIDR prefix (default: `24` -> `255.255.255.0`)
  - `guest_ip` — default DHCP/route target IP for IPO_OS (default: `192.168.7.2`)

#### Quick Start:
```bash
# Setup virtual TAP device on host
make setup-tap

# Launch IPO_OS using TAP networking (the default)
make run NET_MODE=tap
```

In the IPO_OS console, check the guest IP address:
```text
ifconfig
```
Use the address reported by `ifconfig` in the commands below. With the default TAP setup it is typically `192.168.7.2`.

#### HTTP client/server applications
IPO_OS provides TCP/IP and socket support, while userland HTTP client/server programs are not included in this repository and must be supplied on the virtual disk separately.

#### QEMU user networking
TAP is the default mode for direct host/guest testing. To use QEMU user networking instead, launch with:
```bash
make run NET_MODE=user
```
In this mode, QEMU creates virtual subnet `192.168.7.0/24` (host `192.168.7.1`, DNS `192.168.7.3`, DHCP start `192.168.7.2`) and forwards host TCP ports `8000` and `8080` to the guest by default. A forwarded port is accessible only when a guest application is listening on that port. Other guest ports require adding corresponding `hostfwd` rules in `mk/config.mk`.

#### Connecting across routed networks or virtual environments:
If running in an environment where the TAP network needs to be reached from other machines or interfaces:
1. Enable IP forwarding on the host machine:
   ```bash
   sudo sysctl -w net.ipv4.ip_forward=1
   ```
2. On the client machine, add a route to the IPO_OS subnet:
   ```bash
   sudo ip route add 192.168.7.0/24 via <HOST_IP>
   ```
   Replace `<HOST_IP>` with the host's IP address reachable from the client machine (not its TAP address). Configure the host firewall to allow forwarding from the client network to `tap0`; enabling IP forwarding alone may not permit inbound connections.
3. Connect directly to any port:
   ```bash
   curl http://192.168.7.2:8000/
   ```

## 🧑‍💻 Authors

- [IPOleksenko](https://github.com/IPOleksenko) (owner) — Developer and creator of the idea.


# 📜 License

This project is licensed under the [MIT License][license].

[license]: ./LICENSE