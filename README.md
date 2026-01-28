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

## 🧑‍💻 Authors

- [IPOleksenko](https://github.com/IPOleksenko) (owner) — Developer and creator of the idea.


# 📜 License

This project is licensed under the [MIT License][license].

[license]: ./LICENSE