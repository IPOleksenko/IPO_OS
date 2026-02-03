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
Compiles the C library (lib/c) and assembler utilities (lib/asm):
```bash
make lib
```

#### `make kernel` — Build Kernel Only
Compiles kernel entry point and kernel code, links with the library:
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

#### `make apps` — Build applications
Build applications:
```bash
make apps
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

#### `make clean-apps` — Clean Applications Only
Removes only the applications build artifacts:
```bash
make clean-apps
```

### 🔧 Utility Commands

#### `make patch-config` — Update Boot Configuration
Updates the boot configuration with kernel size information:
```bash
make patch-config
```

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

- `ls [path]` — list directory contents (default: `/`).
- `cat <path>` — print a file's contents to stdout (use `--` or shell redirection if needed).
- `mkdir <path>` — create a directory at the specified path.
- `touch <path> [text|local_file]` — create a file or overwrite it with the given text; if the second argument is a path to an existing local file, that file will be imported into the image (either provide text or a local file).
- `put <src> <dest>` — copy a local file `src` into the image at path `dest` (default destination: `/`).
- `rm <path>` — remove a file or an empty directory.

### Examples

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

## 🧑‍💻 Authors

- [IPOleksenko](https://github.com/IPOleksenko) (owner) — Developer and creator of the idea.


# 📜 License

This project is licensed under the [MIT License][license].

[license]: ./LICENSE