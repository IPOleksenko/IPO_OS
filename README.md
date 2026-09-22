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
- Cleans the build directory
- Builds the library (libc.a)
- Compiles the kernel
- Builds the bootloader
- Creates the OS image
- Build applications
- Launches QEMU with the OS

```bash
make all
```

#### `make lib` — Build Library Only
Compiles the C runtime, userspace C standard library (src/userland) and kernel library:
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

#### `make image` — Create OS Image
Combines bootloader and kernel into a single OS image:
```bash
make image
```

#### `make applications` (alias `make apps`) — Build applications
Builds userland applications from `applications/`:
```bash
make applications
# or
make apps
```

#### `make setup-tap` — Setup TAP Network Interface
Configures the host virtual TAP interface (`tap0`) for direct L2 communication with IPO_OS:
- Automatically loads the `tun` kernel module (`modprobe tun`)
- Configures `/dev/net/tun`
- Assigns host IP `192.168.7.1/24` to `tap0`
- Exposes all 65,535 TCP/UDP ports directly between host and guest without port forwarding

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

#### `make clean-applications` (alias `make clean-apps`) — Clean Applications Only
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

## **load_applications.py**

- **File:** [load_applications.py](load_applications.py) (backwards-compatible alias: [load_apps.py](load_apps.py))
- **Purpose:** loads compiled applications into the OS disk image so they are available from `/applications` at runtime.

### Usage

```bash
python3 load_applications.py
```

It scans the built app binaries in `build/applications`, copies them into the filesystem image, and makes them executable from the shell.

## **Disk Editor**

- **File:** [disk_editor.py](disk_editor.py)
- **Purpose:** A utility for viewing and editing the contents of the IPO_FS filesystem inside a disk image. Allows listing directories, reading files, creating directories, writing text files, copying files from the host into the image, and deleting entries.

### Usage and options

General syntax:

```bash
python3 disk_editor.py [-i IMAGE] [-s START_LBA] <command> [arguments]
```

- `-i, --image` — path to the disk image (default: `build/disk.img`).
- `-s, --start-lba` — LBA offset of the start of the IPO_FS partition inside the image (default: `2048`).

Supported commands:

- `format` — clear the filesystem by deleting all files and directories
- `ls [path]` — list directory contents (default: `/`).
- `cat <path>` — print a file's contents to stdout (use `--` or shell redirection if needed).
- `mkdir <path>` — create a directory at the specified path.
- `touch <path> [text|local_file]` — create a file or overwrite it with the given text; if the second argument is a path to an existing local file, that file will be imported into the image (either provide text or a local file).
- `put <src> <dest>` — copy a local file `src` into the image at path `dest` (default destination: `/`).
- `rm <path>` — remove a file or an empty directory.

### Examples

- Clear the filesystem

```bash
python3 disk_editor.py format
```

- List the image root:

```bash
python3 disk_editor.py ls /
```

- Show a text file:

```bash
python3 disk_editor.py cat /etc/example.txt
```

- Create a directory and copy a local file into it:

```bash
python3 disk_editor.py mkdir /mydir
python3 disk_editor.py put ./local.bin /mydir/remote.bin
```

- Create a file with text:

```bash
python3 disk_editor.py touch /hello.txt "Hello IPO"
```

- Remove a file:

```bash
python3 disk_editor.py rm /oldfile
```

## 🌐 Networking (TAP Mode & Direct Ports Access)

IPO_OS implements a native bare-metal TCP/IP stack (RTL8139 NIC driver, Ethernet, ARP, IPv4 with routing table and default gateway, TCP with RFC 793/RFC 9293 state machine, and a POSIX-like Socket API).

### 🚀 TAP Network Interface Setup (`setup-tap.sh`)

By default, QEMU is configured in **TAP networking mode** (`NET_MODE ?= tap` in `mk/config.mk`). In TAP mode, all 65,535 TCP/UDP ports are directly exposed and reachable between host and guest without predefined port forwarding.

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
# 1. Setup virtual TAP device on host
make setup-tap
# (or: sudo ./tools/setup-tap.sh tap0)

# 2. Launch IPO_OS
make run

# 3. Inside IPO_OS: check or change IP configuration dynamically
ifconfig
# (or set custom: ifconfig 192.168.7.100 255.255.255.0 192.168.7.1)

# 4. Inside IPO_OS: run the hosting server on any port
hosting_server 8000

# 5. Connect to server:
curl http://192.168.7.2:8000/

# 6. Inside IPO_OS: fetch data from another host
hosting_client 192.168.7.1 9000 /
```

#### Connecting across routed networks or virtual environments:
If running in an environment where the TAP network needs to be reached from other machines or interfaces:
1. Enable IP forwarding on the host machine:
   ```bash
   sudo sysctl -w net.ipv4.ip_forward=1
   ```
2. On the client machine, add a route to the IPO_OS subnet:
   ```bash
   route add 192.168.7.0 mask 255.255.255.0 <HOST_IP>
   ```
3. Connect directly to any port:
   ```bash
   curl http://192.168.7.2:8000/
   ```

## 🧑‍💻 Authors

- [IPOleksenko](https://github.com/IPOleksenko) (owner) — Developer and creator of the idea.


# 📜 License

This project is licensed under the [MIT License][license].

[license]: ./LICENSE