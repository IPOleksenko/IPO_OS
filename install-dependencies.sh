#!/bin/bash

# ============================
# Update and install required packages
# ============================

echo "Updating package lists..."
sudo apt update || { echo "Failed to update package lists"; exit 1; }

# Check and install only missing packages
REQUIRED_PACKAGES=("build-essential" "bison" "flex" "libgmp3-dev" "libmpc-dev" "libmpfr-dev" "texinfo" "qemu-system-x86" "xorriso" "grub-pc-bin" "python3" "python3-pip" "python3-venv")
for PACKAGE in "${REQUIRED_PACKAGES[@]}"; do
    if dpkg -s $PACKAGE &> /dev/null; then
        echo "$PACKAGE is already installed. Skipping..."
    else
        echo "Installing $PACKAGE..."
        sudo apt install -y $PACKAGE || { echo "Failed to install $PACKAGE"; exit 1; }
    fi
done

# ============================
# Set up cross-compiler
# ============================

echo "Setting up cross-compiler for $TARGET..."

CROSS_DIR=~/opt/cross
BINUTILS_VERSION=2.40
GCC_VERSION=13.2.0
TARGET=i386-elf

mkdir -p $CROSS_DIR || { echo "Failed to create cross-compiler directory"; exit 1; }

# Download and build binutils
if [ ! -f "$CROSS_DIR/bin/$TARGET-as" ]; then
    echo "Downloading and building binutils..."
    cd /tmp || { echo "Failed to navigate to /tmp"; exit 1; }
    wget https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.gz || { echo "Failed to download binutils"; exit 1; }
    tar -xvf binutils-$BINUTILS_VERSION.tar.gz
    mkdir binutils-build && cd binutils-build
    ../binutils-$BINUTILS_VERSION/configure --target=$TARGET --prefix=$CROSS_DIR --with-sysroot --disable-nls --disable-werror || { echo "Failed to configure binutils"; exit 1; }
    make || { echo "Failed to build binutils"; exit 1; }
    make install || { echo "Failed to install binutils"; exit 1; }
else
    echo "Binutils already installed. Skipping..."
fi

# Download and build GCC
if [ ! -f "$CROSS_DIR/bin/$TARGET-gcc" ]; then
    echo "Downloading and building GCC..."
    cd /tmp || { echo "Failed to navigate to /tmp"; exit 1; }
    wget https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.gz || { echo "Failed to download GCC"; exit 1; }
    tar -xvf gcc-$GCC_VERSION.tar.gz
    mkdir gcc-build && cd gcc-build
    ../gcc-$GCC_VERSION/configure --target=$TARGET --prefix=$CROSS_DIR --disable-nls --enable-languages=c,c++ --without-headers || { echo "Failed to configure GCC"; exit 1; }
    make all-gcc || { echo "Failed to build GCC"; exit 1; }
    make all-target-libgcc || { echo "Failed to build target libgcc"; exit 1; }
    make install-gcc || { echo "Failed to install GCC"; exit 1; }
    make install-target-libgcc || { echo "Failed to install target libgcc"; exit 1; }
else
    echo "GCC already installed. Skipping..."
fi

# ============================
# Set environment variables
# ============================

echo "Setting up environment variables..."
echo "export PATH=$CROSS_DIR/bin:\$PATH" >> ~/.bashrc
export PATH=$CROSS_DIR/bin:$PATH

# ============================
# Verify installation
# ============================

echo "Verifying installation..."
if command -v $TARGET-gcc &> /dev/null && command -v $TARGET-as &> /dev/null; then
    echo "Cross-compiler setup completed successfully!"
else
    echo "Cross-compiler setup failed!"
    exit 1;
fi

echo "Installation complete."
