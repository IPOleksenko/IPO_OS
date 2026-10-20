#!/bin/bash
set -e

TAP_DEV="${1:-tap0}"
HOST_IP="${2:-192.168.7.1}"
NETMASK="${3:-24}"
GUEST_IP="${4:-192.168.7.2}"

echo "[TAP] Setting up virtual interface: ${TAP_DEV} with Host IP ${HOST_IP}/${NETMASK}..."

# Ensure tun kernel module is loaded
if ! lsmod | grep -q "^tun"; then
    echo "[TAP] Loading tun kernel module (modprobe tun)..."
    sudo modprobe tun 2>/dev/null || true
fi

# Ensure /dev/net/tun exists
if [ ! -c /dev/net/tun ]; then
    echo "[TAP] Creating /dev/net/tun device node..."
    sudo mkdir -p /dev/net
    sudo mknod /dev/net/tun c 10 200
    sudo chmod 666 /dev/net/tun
fi

# Check if interface already exists
if ip link show "${TAP_DEV}" >/dev/null 2>&1; then
    echo "[TAP] Interface ${TAP_DEV} already exists."
else
    echo "[TAP] Creating TAP interface ${TAP_DEV} for user ${USER}..."
    sudo ip tuntap add dev "${TAP_DEV}" mode tap user "${USER}"
fi

# Assign IP if not already set
if ! ip addr show dev "${TAP_DEV}" | grep -q "${HOST_IP}"; then
    echo "[TAP] Assigning IP ${HOST_IP}/${NETMASK} to ${TAP_DEV}..."
    sudo ip addr add "${HOST_IP}/${NETMASK}" dev "${TAP_DEV}"
fi

# Bring up interface
sudo ip link set dev "${TAP_DEV}" up

# Ensure host routes guest IP directly into TAP interface
sudo ip route replace "${GUEST_IP}/32" dev "${TAP_DEV}" 2>/dev/null || true

# Enable IP forwarding and NAT so the guest can reach external networks.
DEFAULT_IF=$(ip route show default 2>/dev/null | awk '/default/ {print $5}' | head -n1)
if [ -n "$DEFAULT_IF" ]; then
    NAT_READY=1
    if ! sudo sysctl -w net.ipv4.ip_forward=1 >/dev/null; then
        echo "[TAP] Could not enable host IP forwarding."
        NAT_READY=0
    fi

    if command -v iptables >/dev/null 2>&1; then
        if ! sudo iptables -C FORWARD -i "${TAP_DEV}" -o "${DEFAULT_IF}" -j ACCEPT 2>/dev/null &&
           ! sudo iptables -I FORWARD 1 -i "${TAP_DEV}" -o "${DEFAULT_IF}" -j ACCEPT; then
            echo "[TAP] Could not add the outbound forwarding rule."
            NAT_READY=0
        fi
        if ! sudo iptables -C FORWARD -i "${DEFAULT_IF}" -o "${TAP_DEV}" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null &&
           ! sudo iptables -I FORWARD 1 -i "${DEFAULT_IF}" -o "${TAP_DEV}" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT; then
            echo "[TAP] Could not add the return forwarding rule."
            NAT_READY=0
        fi
        if ! sudo iptables -t nat -C POSTROUTING -s 192.168.7.0/24 -o "${DEFAULT_IF}" -j MASQUERADE 2>/dev/null &&
           ! sudo iptables -t nat -A POSTROUTING -s 192.168.7.0/24 -o "${DEFAULT_IF}" -j MASQUERADE; then
            echo "[TAP] Could not add the NAT masquerade rule."
            NAT_READY=0
        fi
    else
        echo "[TAP] iptables is unavailable; TAP guest Internet access will not work."
        NAT_READY=0
    fi
    if [ "$NAT_READY" -ne 1 ]; then
        echo "[TAP] Internet NAT setup failed; QEMU user networking can be used instead."
        exit 1
    fi
else
    echo "[TAP] No host default route found; TAP guest Internet access cannot be configured."
    exit 1
fi

echo "[TAP] ✓ Interface ${TAP_DEV} is UP and active."
echo "[TAP] Host IP           : ${HOST_IP}"
echo "[TAP] Guest (IPO_OS) IP : ${GUEST_IP}"
echo "[TAP] All 65535 ports are directly reachable at http://${GUEST_IP}:<PORT>/"
