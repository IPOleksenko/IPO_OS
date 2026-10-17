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
- Builds libraries, kernel, bootloader, applications, and toolchain
- Creates and populates the disk images
- Launches QEMU with the OS

```bash
make all
```

#### `make lib` — Build Library Only
Compiles the C runtime, userspace C standard library (applications/lib) and kernel library:
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
Builds the bootable OS image and IPO_FS disk images, then loads any applications already built in `build/applications`. This target does not build the applications itself; run `make applications` first when preparing a fresh image:
```bash
make applications
make image
```

#### `make applications` — Build applications
Builds userland applications from `applications/`:
```bash
make applications
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

#### `make clean-applications` — Clean Applications Only
Removes only the applications build artifacts:
```bash
make clean-applications
```

### 🔧 Utility Commands

#### `make patch-config` — Update Boot Configuration
Updates the boot configuration with kernel size information:
```bash
make patch-config
```

## 🛠️ Build & Development Tools (`tools/`)

The `tools/` directory contains host-side build automation scripts, image formatters, symbol generators, and native developer toolchain wrappers:

---

### **1. format_disk_via_os.py — In-Kernel Virtual Disk Formatter**
- **File:** [tools/format_disk_via_os.py](tools/format_disk_via_os.py)
- **Purpose:** Automatically formats `build/disk.img` natively through the IPO_OS kernel running inside headless QEMU. This guarantees that the IPO_FS superblock, root directory inode, inode table, and block bitmap are initialized with 100% compliance to the kernel's filesystem driver.
- **Invoked by:** `make image`
- **Manual Usage:**
```bash
python3 tools/format_disk_via_os.py --boot build/boot/boot.bin --kernel build/kernel/kernel.bin --disk build/disk.img
```
- **Options:**
  - `--disk <path>` — target disk image (default: `build/disk.img`).
  - `--boot <path>` — bootloader binary (default: `build/boot/boot.bin`).
  - `--kernel <path>` — compiled kernel binary (default: `build/kernel/kernel.bin`).
  - `--size-mb <int>` — disk image size in MB (default: `128`).
  - `--force` — re-format the disk even if a valid IPO_FS superblock already exists.

---

### **2. load_applications.py — Application & Toolchain Image Loader**
- **File:** [tools/load_applications.py](tools/load_applications.py)
- **Purpose:** Scans compiled binaries, libraries, and headers in `build/applications`, converts executable ELF formats for the kernel's universal binary loader, and writes them into `/applications` and system directories inside the IPO_FS disk image.
- **Invoked by:** `make image`
- **Manual Usage:**
```bash
python3 tools/load_applications.py
```
- **Options:**
  - `--image <path>` — destination disk image (default: `build/disk.img`).
  - `--start-lba <int>` — partition LBA start offset (default: `2048`).
  - `--apps-dir <path>` — path to compiled applications (default: `build/applications`).
  - `--dry-run` — simulate files to be copied without writing to disk.

---

### **3. create_live_image.py — Unified Live USB OS Image Generator**
- **File:** [tools/create_live_image.py](tools/create_live_image.py)
- **Purpose:** Combines the bootloader, kernel, and IPO_FS filesystem partition into a single, bootable 128 MB Live USB disk image (`build/IPO_OS.img`) with a valid Master Boot Record (MBR) partition table at sector 0.
- **Invoked by:** `make image`, `make run`
- **Manual Usage:**
```bash
python3 tools/create_live_image.py
```
- **Features:**
  - Generates a bootable MBR with partition 1 starting at LBA 2048.
  - Updates superblocks and syncs partition boundaries.
  - Allows burning directly to a physical flash drive: `sudo dd if=build/IPO_OS.img of=/dev/sdX bs=4M status=progress`.

---

### **4. disk_editor.py — Offline IPO_FS Inspector & File Manager**
- **File:** [tools/disk_editor.py](tools/disk_editor.py)
- **Purpose:** CLI utility and Python library for inspecting, reading, and modifying IPO_FS disk images on the host machine without running QEMU or the OS kernel.
- **Usage Syntax:**
```bash
python3 tools/disk_editor.py [-i IMAGE] [-s START_LBA] <command> [arguments]
```
- **Commands:**
  - `ls [path]` — list files and directories (default: `/`).
  - `cat <path>` — output file contents to terminal.
  - `mkdir <path>` — create a new directory.
  - `put <src_file> <dest_path>` — import host file into the disk image.
  - `touch <path> [text|file]` — create a file with text or import content.
  - `rm <path>` — delete a file or directory.
  - `format` — wipe and initialize a clean IPO_FS partition.
- **Examples:**
```bash
python3 tools/disk_editor.py ls /applications
python3 tools/disk_editor.py cat /etc/example.txt
python3 tools/disk_editor.py put ./my_prog /applications/my_prog
```

---

### **5. generate_ksyms.py — Kernel Symbol Table Generator**
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

### **6. build_font_db.py — VGA & Graphical Font Compiler**
- **File:** [tools/build_font_db.py](tools/build_font_db.py)
- **Purpose:** Compiles Cyrillic and Latin 8x16 bitmap font definitions into the binary font database `build/fonts/default.fnt`.
- **Invoked by:** `make image`
- **Manual Usage:**
```bash
python3 tools/build_font_db.py
```
- **Features:**
  - Supports CP866 / UTF-8 Cyrillic glyph rendering for the VGA text console and VBE graphical window manager (`wm`).

---

### **7. setup-tap.sh — Host TAP Network Interface Setup**
- **File:** [tools/setup-tap.sh](tools/setup-tap.sh)
- **Purpose:** Configures Linux TAP device (`tap0`) and host IP routing for full bare-metal TCP/IP communication with IPO_OS in QEMU.
- **Invoked by:** `make setup-tap`
- **Manual Usage:**
```bash
./tools/setup-tap.sh [tap_dev] [host_ip] [netmask] [guest_ip]
```
- **Defaults:** `tap_dev=tap0`, `host_ip=192.168.7.1`, `netmask=24`, `guest_ip=192.168.7.2`.
- The script uses `sudo` for operations that require elevated privileges. Run it as the same user that will launch QEMU so the TAP interface is assigned to that user.

---

### **8. binutils/ — In-OS Developer Toolchain Drivers**
- **Directory:** [tools/binutils/](tools/binutils/)
- **Files:** `gcc.c`, `ld.c`, `as.c`, `ar.c`, `nm.c`, `objdump.c`, `objcopy.c`, `readelf.c`
- **Purpose:** C source code for the native developer toolchain utilities running inside IPO_OS.
- **Invoked by:** `make toolchain`
- **Resulting Binaries:** Built and installed into `/applications/` (`gcc`, `g++`, `ld`, `as`, `ar`, `ranlib`, `objcopy`, `strip`, `objdump`, `readelf`, `nm`) to enable native compilation, assembly, and inspection directly inside IPO_OS.

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
Build the applications before creating the image so `hosting_client` and `hosting_server` are included:
```bash
# Build applications and prepare the bootable images
make applications
make image

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