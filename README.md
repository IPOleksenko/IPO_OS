# IPO_OS

## ⚙️ Compilation Commands

### 📦 Install Dependencies
Install all necessary build tools and dependencies:
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

#### `make boot` — Build Bootloader Only
Assembles the bootloader with kernel configuration:
```bash
make boot
```

#### `make image` — Create OS Images
Builds the bootable OS image (`build/IPO_OS.img`) and ensures virtual disks (`build/disk.img`, `build/disk.iso`) exist:
```bash
make image
```

#### `make setup-tap` — Setup TAP Network Interface
Configures the host virtual TAP interface (`tap0`) for direct L2 communication with IPO_OS:
- Automatically loads the `tun` kernel module (`modprobe tun`)
- Configures `/dev/net/tun`
- Assigns host IP `192.168.7.1/24` to `tap0`
- Provides direct host/guest networking without QEMU port forwarding; a port is reachable only when an application is listening and the host firewall permits the connection

```bash
make setup-tap
```

#### `make run` — Launch in QEMU
Runs the OS image in QEMU emulator:
```bash
make run
```

#### `make` — Same as `make all`
Default target (DEFAULT_GOAL is set to `all`):
```bash
make
```

### 🧹 Cleanup Commands

#### `make clean` — Full Cleanup
Removes the entire build directory:
```bash
make clean
```

#### `make clean-lib` — Clean Library Only
Removes only the library build artifacts:
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
- **Purpose:** Extracts public symbols (functions, global variables, syscall dispatchers) from `build/kernel/kernel.elf` using `nm`, formats them into sorted lookup arrays, and outputs `build/kernel/kernel_syms_autogen.c`.
- **Invoked by:** `make kernel`
- **Manual Usage:**
```bash
python3 tools/generate_ksyms.py -e build/kernel/kernel.elf -o build/kernel/kernel_syms_autogen.c
```
- **Features:**
  - Provides the symbol database for the dynamic driver subsystem (`kmod`).
  - Powers the OS shell's runtime `symbols` command to view all active kernel routines and memory addresses.

---

### **2. setup-tap.sh — Virtual TAP Network Interface Setup**
- **File:** [tools/setup-tap.sh](tools/setup-tap.sh)
- **Purpose:** Automatically provisions the host `tap0` virtual interface with IP `192.168.7.1/24` and loads the Linux `tun` kernel driver for direct networking with IPO_OS inside QEMU.
- **Invoked by:** `make setup-tap`, `make run`
- **Manual Usage:**
```bash
./tools/setup-tap.sh [tap_dev] [host_ip] [netmask] [guest_ip]
```
- **Defaults:** `tap_dev=tap0`, `host_ip=192.168.7.1`, `netmask=24`, `guest_ip=192.168.7.2`.

---

## 🌐 Networking (TAP Mode & Direct Port Access)

IPO_OS implements a native bare-metal TCP/IP stack (RTL8139 NIC driver, Ethernet, ARP, IPv4 with routing table and default gateway, TCP with RFC 793/RFC 9293 state machine, and a POSIX-like Socket API).

### 🚀 TAP Network Interface Setup (`setup-tap.sh`)

By default, QEMU is configured in **TAP networking mode** (`NET_MODE ?= tap` in `mk/config.mk`). TAP connects the host and guest directly, without QEMU port-forwarding rules. A service is reachable only if it is listening on the requested port and the host firewall permits the connection.

#### Script: `tools/setup-tap.sh`
```bash
./tools/setup-tap.sh [tap_dev] [host_ip] [netmask] [guest_ip]
```
- Parameters:
  - `tap_dev` — virtual TAP interface name (default: `tap0`)
  - `host_ip` — IP assigned to host interface (default: `192.168.7.1`)
  - `netmask` — subnet CIDR prefix (default: `24` -> `255.255.255.0`)
  - `guest_ip` — default target IP for IPO_OS (default: `192.168.7.2`)

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

#### Manually test `hosting_client`
On the host, start a small HTTP server bound to the TAP host address:
```bash
mkdir -p /tmp/hosting-client-test
printf 'Hello from host\n' > /tmp/hosting-client-test/index.html
python3 -m http.server 9000 --bind 192.168.7.1 --directory /tmp/hosting-client-test
```

In the IPO_OS console, fetch the test page:
```text
hosting_client 192.168.7.1 9000 /
hosting_client 192.168.7.1 9000 /index.html
```
The response should contain `200 OK` and `Hello from host`.

#### Manually test `hosting_server`
In the IPO_OS console, start the server:
```text
hosting_server 8000
```

On the host, replace `192.168.7.2` with the guest IP reported by `ifconfig`, then try:
```bash
curl -i http://192.168.7.2:8000/
curl -I http://192.168.7.2:8000/
curl -i -X OPTIONS http://192.168.7.2:8000/
curl -i http://192.168.7.2:8000/no-such-file

# Upload, read back, and delete a test file
printf 'Upload test\n' > /tmp/hosting-upload.txt
curl -i -T /tmp/hosting-upload.txt http://192.168.7.2:8000/manual-test.txt
curl -i http://192.168.7.2:8000/manual-test.txt
curl -i -X DELETE http://192.168.7.2:8000/manual-test.txt
```
The first request should return the directory listing; the missing path should return `404`. The upload, read-back, and delete commands exercise the server's `PUT`, `GET`, and `DELETE` handlers. Stop the server in IPO_OS with `q`, `Esc`, or `Ctrl+C`.

#### QEMU user networking
TAP is the recommended mode for direct host/guest testing. To use QEMU user networking instead, launch with:
```bash
make run NET_MODE=user
```
In this mode, QEMU forwards only host TCP ports `8000` and `8080` to the guest by default. For example, if `hosting_server` is listening on guest port `8000`, access it from the host at `http://localhost:8000/`. Other guest ports require adding corresponding `hostfwd` rules in `mk/config.mk`.

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