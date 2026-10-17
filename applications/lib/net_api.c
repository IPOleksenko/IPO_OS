#include <net/net.h>
#include <syscall.h>
#include <stdio.h>

static net_if_t interface_cache;

static int net_api_call(uint32_t syscall_number, uint32_t argc, uint32_t *argv) {
    return ipo_syscall(syscall_number, argc, argv);
}

void ip_to_str(ip4_addr_t ip, char *buf, size_t size) {
    if (!buf || size == 0) {
        return;
    }
    snprintf(buf, size, "%u.%u.%u.%u",
             (unsigned int)((ip >> 24) & 0xFF),
             (unsigned int)((ip >> 16) & 0xFF),
             (unsigned int)((ip >> 8) & 0xFF),
             (unsigned int)(ip & 0xFF));
}

bool str_to_ip(const char *str, ip4_addr_t *out_ip) {
    if (!str || !out_ip) {
        return false;
    }
    unsigned int octets[4] = { 0, 0, 0, 0 };
    for (size_t part = 0; part < 4; part++) {
        if (*str < '0' || *str > '9') {
            return false;
        }
        while (*str >= '0' && *str <= '9') {
            octets[part] = octets[part] * 10u + (unsigned int)(*str++ - '0');
            if (octets[part] > 255u) {
                return false;
            }
        }
        if (part < 3) {
            if (*str++ != '.') {
                return false;
            }
        } else if (*str != '\0') {
            return false;
        }
    }
    *out_ip = IP4_ADDR(octets[0], octets[1], octets[2], octets[3]);
    return true;
}

bool net_init(const net_config_t *config) {
    if (!config) {
        return false;
    }
    return net_api_call(IPO_SYSCALL_NET_INIT, 1u,
                        (uint32_t[]){ (uint32_t)(uintptr_t)config }) != 0;
}

void net_poll(void) {
    net_api_call(IPO_SYSCALL_NET_POLL, 0u, NULL);
}

net_if_t *net_get_interface(void) {
    if (!net_api_call(IPO_SYSCALL_NET_GET_INTERFACE, 1u,
                      (uint32_t[]){ (uint32_t)(uintptr_t)&interface_cache })) {
        return NULL;
    }
    return &interface_cache;
}

void net_set_ip(ip4_addr_t ip, ip4_addr_t netmask, ip4_addr_t gateway, ip4_addr_t dns) {
    uint32_t args[4] = { ip, netmask, gateway, dns };
    net_api_call(IPO_SYSCALL_NET_SET_IP, 4u, args);
}
