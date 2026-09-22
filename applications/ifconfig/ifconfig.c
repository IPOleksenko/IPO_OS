/*
 * ifconfig.c - Network Interface Configuration Utility for IPO_OS
 *
 * Displays or modifies network interface IP address, netmask,
 * gateway, DNS, and MAC address dynamically at runtime.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include <net/net.h>

static void print_usage(void) {
    printf("Usage: ifconfig [options | <ip> [netmask] [gateway] [dns]]\n\n");
    printf("Display or configure network interface settings at runtime.\n\n");
    printf("Arguments:\n");
    printf("  (none)                    Display current network configuration\n");
    printf("  <ip>                      Set IPv4 address (e.g. 192.168.7.2)\n");
    printf("  <ip> <netmask> <gateway>  Set IP, netmask, and default gateway\n");
    printf("  -h, --help                Display this help\n\n");
    printf("Examples:\n");
    printf("  ifconfig\n");
    printf("  ifconfig 192.168.7.2\n");
    printf("  ifconfig 192.168.7.100 255.255.255.0 192.168.7.1\n");
}

int main(int argc, char **argv) {
    net_init();
    net_if_t *netif = net_get_interface();
    if (!netif) {
        printf("ifconfig: no network interface available\n");
        return 1;
    }

    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage();
            return 0;
        }

        ip4_addr_t new_ip = 0;
        if (!str_to_ip(argv[1], &new_ip)) {
            printf("ifconfig: invalid IP address format '%s'\n", argv[1]);
            return 1;
        }

        ip4_addr_t netmask = 0;
        if (argc >= 3) {
            if (!str_to_ip(argv[2], &netmask)) {
                printf("ifconfig: invalid netmask '%s'\n", argv[2]);
                return 1;
            }
        }

        ip4_addr_t gateway = 0;
        if (argc >= 4) {
            if (!str_to_ip(argv[3], &gateway)) {
                printf("ifconfig: invalid gateway '%s'\n", argv[3]);
                return 1;
            }
        }

        ip4_addr_t dns = 0;
        if (argc >= 5) {
            if (!str_to_ip(argv[4], &dns)) {
                printf("ifconfig: invalid DNS server '%s'\n", argv[4]);
                return 1;
            }
        }

        net_set_ip(new_ip, netmask, gateway, dns);
        printf("[ifconfig] Network settings updated successfully.\n");
    }

    /* Print current configuration */
    char ip_str[32], mask_str[32], gw_str[32], dns_str[32];
    ip_to_str(netif->ip, ip_str, sizeof(ip_str));
    ip_to_str(netif->netmask, mask_str, sizeof(mask_str));
    ip_to_str(netif->gateway, gw_str, sizeof(gw_str));
    ip_to_str(netif->dns, dns_str, sizeof(dns_str));

    printf("eth0: flags=<%s> mtu 1500\n", netif->link_up ? "UP,BROADCAST,RUNNING" : "DOWN");
    printf("      inet %s  netmask %s  broadcast 255.255.255.255\n", ip_str, mask_str);
    printf("      gateway %s  dns %s\n", gw_str, dns_str);
    printf("      ether %02x:%02x:%02x:%02x:%02x:%02x\n",
           netif->mac.mac[0], netif->mac.mac[1], netif->mac.mac[2],
           netif->mac.mac[3], netif->mac.mac[4], netif->mac.mac[5]);

    return 0;
}

