/*
 * Droidspaces v4 — High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspace.h"
#include <net/if.h>

/* ---------------------------------------------------------------------------
 * Host-side networking setup (before container boot)
 * ---------------------------------------------------------------------------*/

int ds_get_dns_servers(const char *custom_dns, char *out, size_t size) {
  out[0] = '\0';
  int count = 0;

  /* 0. Try custom DNS if provided */
  if (custom_dns && custom_dns[0]) {
    char buf[1024];
    safe_strncpy(buf, custom_dns, sizeof(buf));
    char *saveptr;
    char *token = strtok_r(buf, ", ", &saveptr);
    while (token && (size_t)strlen(out) < size - 32) {
      char line[128];
      snprintf(line, sizeof(line), "nameserver %s\n", token);
      strncat(out, line, size - strlen(out) - 1);
      count++;
      token = strtok_r(NULL, ", ", &saveptr);
    }
  }

  /* 1. Global stable fallbacks (defined in droidspace.h) */
  if (count == 0) {
    int n = snprintf(out, size, "nameserver %s\nnameserver %s\n",
                     DS_DNS_DEFAULT_1, DS_DNS_DEFAULT_2);
    if (n > 0 && (size_t)n < size)
      count = 2;
  }

  return count;
}

int fix_networking_host(struct ds_config *cfg) {
  ds_log("Configuring host-side networking for %s...", cfg->container_name);

  /* Enable IPv4 forwarding */
  write_file("/proc/sys/net/ipv4/ip_forward", "1");

  /* IPv6: default disabled unless explicitly enabled via --enable-ipv6 */
  if (cfg->enable_ipv6) {
    write_file("/proc/sys/net/ipv6/conf/all/disable_ipv6", "0");
    write_file("/proc/sys/net/ipv6/conf/default/disable_ipv6", "0");
    write_file("/proc/sys/net/ipv6/conf/all/forwarding", "1");
  } else {
    /* If IPv6 is not available, these writes might fail, which is fine */
    write_file("/proc/sys/net/ipv6/conf/all/disable_ipv6", "1");
    write_file("/proc/sys/net/ipv6/conf/default/disable_ipv6", "1");
  }

  /* Get DNS and store it in the config struct to be used after pivot_root */
  cfg->dns_server_content[0] = '\0';
  int count = ds_get_dns_servers(cfg->dns_servers, cfg->dns_server_content,
                                 sizeof(cfg->dns_server_content));

  if (cfg->dns_servers[0])
    ds_log("Setting up %d custom DNS servers...", count);

  /* If shared networking (Host mode) on Android, apply basic fixes/optimizations
   * but skip iptables to avoid breaking connectivity (as per previous fix). */
  if (cfg->net_mode == DS_NET_HOST && is_android()) {
    android_configure_iptables();
  }

  return 0;
}

/* ---------------------------------------------------------------------------
 * Network Namespace Configuration (NAT / Macvlan)
 * ---------------------------------------------------------------------------*/

static void generate_random_mac(char *buf) {
  /* Locally Administered Address (x2, x6, xA, xE) */
  /* We use 02:xx:xx:xx:xx:xx */
  unsigned char mac[6];
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    read(fd, mac, 6);
    close(fd);
  } else {
    /* Fallback pseudo-random */
    for (int i = 0; i < 6; i++) mac[i] = rand() % 256;
  }

  mac[0] &= 0xFE; /* Unicast */
  mac[0] |= 0x02; /* Locally Administered */

  snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int get_wlan_interface(char *buf, size_t size) {
  /* Simple heuristic: check for wlan0, then swlan0, then eth0 */
  if (access("/sys/class/net/wlan0", F_OK) == 0) {
    safe_strncpy(buf, "wlan0", size);
    return 0;
  }
  if (access("/sys/class/net/swlan0", F_OK) == 0) {
    safe_strncpy(buf, "swlan0", size);
    return 0;
  }
  if (access("/sys/class/net/eth0", F_OK) == 0) {
    safe_strncpy(buf, "eth0", size);
    return 0;
  }
  return -1;
}

int ds_configure_network_namespace(pid_t container_pid, struct ds_config *cfg) {
  ds_log("Configuring isolated network namespace (PID %d)...", container_pid);

  char veth_host[32], veth_peer[32];
  snprintf(veth_host, sizeof(veth_host), "veth%d", container_pid);
  snprintf(veth_peer, sizeof(veth_peer), "vethc%d", container_pid);

  if (cfg->net_mode == DS_NET_NAT) {
    /* -----------------------------------------------------------------------
     * NAT Mode: veth pair + iptables MASQUERADE
     * ----------------------------------------------------------------------- */

    /* Calculate unique subnet based on PID to avoid collisions: 10.0.(PID%250 + 1).0/24 */
    /* This allows ~250 containers to coexist without bridging logic. */
    int subnet_id = (container_pid % 250) + 1;
    char host_ip[32], container_ip[32];
    snprintf(host_ip, sizeof(host_ip), "10.0.%d.1/24", subnet_id);
    snprintf(container_ip, sizeof(container_ip), "10.0.%d.2/24", subnet_id);
    char gateway_ip[32];
    snprintf(gateway_ip, sizeof(gateway_ip), "10.0.%d.1", subnet_id);

    ds_log("Mode: NAT. Subnet: 10.0.%d.0/24", subnet_id);

    /* 1. Create veth pair */
    char *args_link[] = {"ip", "link", "add", veth_host, "type", "veth", "peer", "name", veth_peer, NULL};
    if (run_command_quiet(args_link) != 0) {
      ds_error("Failed to create veth pair");
      return -1;
    }

    /* 2. Configure Host Side */
    char *args_host_up[] = {"ip", "link", "set", veth_host, "up", NULL};
    run_command_quiet(args_host_up);

    char *args_host_ip[] = {"ip", "addr", "add", host_ip, "dev", veth_host, NULL};
    if (run_command_quiet(args_host_ip) != 0) {
        ds_warn("Failed to assign host IP %s (collision?)", host_ip);
    }

    /* 3. Enable NAT (Masquerade) on Host */
    /* We masquerade traffic from this subnet going out anywhere */
    /* On Android, use standard iptables. */
    char subnet_cidr[32];
    snprintf(subnet_cidr, sizeof(subnet_cidr), "10.0.%d.0/24", subnet_id);

    char *args_nat[] = {"iptables", "-t", "nat", "-A", "POSTROUTING", "-s", subnet_cidr, "-j", "MASQUERADE", NULL};
    run_command_quiet(args_nat);

    /* Ensure forwarding is ACCEPT in filter (Android sometimes drops it) */
    char *args_fwd[] = {"iptables", "-A", "FORWARD", "-i", veth_host, "-j", "ACCEPT", NULL};
    run_command_quiet(args_fwd);
    char *args_fwd2[] = {"iptables", "-A", "FORWARD", "-o", veth_host, "-j", "ACCEPT", NULL};
    run_command_quiet(args_fwd2);

    /* 4. Move Peer to Container Namespace */
    char pid_str[16];
    snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    char *args_move[] = {"ip", "link", "set", veth_peer, "netns", pid_str, NULL};
    if (run_command_quiet(args_move) != 0) {
      ds_error("Failed to move interface to container namespace");
      return -1;
    }

    /* 5. Configure Container Side (using fork + setns) */
    /* We use a child process to enter the namespace so we don't mess up the monitor's network view */
    pid_t worker = fork();
    if (worker == 0) {
        /* Child worker */
        char ns_path[PATH_MAX];
        snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/net", container_pid);
        int fd = open(ns_path, O_RDONLY);
        if (fd < 0 || setns(fd, CLONE_NEWNET) < 0) {
             exit(1);
        }
        close(fd);

        /* Inside container namespace now */

        /* Rename veth_peer -> eth0 */
        char *cmd_rename[] = {"ip", "link", "set", veth_peer, "name", "eth0", NULL};
        run_command_quiet(cmd_rename);

        /* Set Fake MAC */
        char mac[32];
        generate_random_mac(mac);
        ds_log("Assigned Virtual MAC: %s", mac);
        char *cmd_mac[] = {"ip", "link", "set", "eth0", "address", mac, NULL};
        run_command_quiet(cmd_mac);

        /* Set IP */
        char *cmd_ip[] = {"ip", "addr", "add", container_ip, "dev", "eth0", NULL};
        run_command_quiet(cmd_ip);

        /* Set UP */
        char *cmd_up[] = {"ip", "link", "set", "eth0", "up", NULL};
        run_command_quiet(cmd_up);

        /* Loopback UP */
        char *cmd_lo[] = {"ip", "link", "set", "lo", "up", NULL};
        run_command_quiet(cmd_lo);

        /* Default Gateway */
        char *cmd_gw[] = {"ip", "route", "add", "default", "via", gateway_ip, NULL};
        run_command_quiet(cmd_gw);

        exit(0);
    }
    int status;
    waitpid(worker, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        ds_error("Failed to configure container network interface");
        return -1;
    }

  } else if (cfg->net_mode == DS_NET_MACVLAN) {
    /* -----------------------------------------------------------------------
     * Macvlan Mode: Bridge to physical interface
     * ----------------------------------------------------------------------- */
    ds_log("Mode: Macvlan (Bridge). Trying to get real LAN IP...");

    char phys_if[32];
    if (get_wlan_interface(phys_if, sizeof(phys_if)) < 0) {
        ds_error("Could not find a suitable parent interface (wlan0/eth0) for Macvlan.");
        return -1;
    }
    ds_log("Parent interface: %s", phys_if);

    /* Create macvlan link */
    /* ip link add link wlan0 name macXXX type macvlan mode bridge */
    char mac_if[32];
    snprintf(mac_if, sizeof(mac_if), "mac%d", container_pid);

    char *args_link[] = {"ip", "link", "add", "link", phys_if, "name", mac_if, "type", "macvlan", "mode", "bridge", NULL};
    if (run_command_quiet(args_link) != 0) {
        ds_error("Failed to create macvlan interface. (Driver might not support it)");
        return -1;
    }

    /* Move to container */
    char pid_str[16];
    snprintf(pid_str, sizeof(pid_str), "%d", container_pid);
    char *args_move[] = {"ip", "link", "set", mac_if, "netns", pid_str, NULL};
    run_command_quiet(args_move);

    /* Configure inside */
    pid_t worker = fork();
    if (worker == 0) {
        char ns_path[PATH_MAX];
        snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/net", container_pid);
        int fd = open(ns_path, O_RDONLY);
        if (fd < 0 || setns(fd, CLONE_NEWNET) < 0) exit(1);
        close(fd);

        char *cmd_rename[] = {"ip", "link", "set", mac_if, "name", "eth0", NULL};
        run_command_quiet(cmd_rename);

        /* Set Random MAC to ensure it's distinct */
        char mac[32];
        generate_random_mac(mac);
        ds_log("Assigned Virtual MAC: %s", mac);
        char *cmd_mac[] = {"ip", "link", "set", "eth0", "address", mac, NULL};
        run_command_quiet(cmd_mac);

        char *cmd_up[] = {"ip", "link", "set", "eth0", "up", NULL};
        run_command_quiet(cmd_up);

        char *cmd_lo[] = {"ip", "link", "set", "lo", "up", NULL};
        run_command_quiet(cmd_lo);

        /* We cannot set IP/Gateway here because we don't know the LAN config.
         * The user must run a DHCP client inside (e.g., 'udhcpc -i eth0'). */

        exit(0);
    }
    waitpid(worker, NULL, 0);
    ds_warn("Macvlan setup complete. You must run a DHCP client inside the container.");
  }

  return 0;
}

/* ---------------------------------------------------------------------------
 * Rootfs-side networking setup (inside container, after pivot_root)
 * ---------------------------------------------------------------------------*/

int fix_networking_rootfs(struct ds_config *cfg) {
  /* 1. Hostname */
  if (cfg->hostname[0]) {
    if (sethostname(cfg->hostname, strlen(cfg->hostname)) < 0) {
      ds_warn("Failed to set hostname to %s: %s", cfg->hostname,
              strerror(errno));
    }
    /* Persist to /etc/hostname */
    char hn_buf[256 + 2];
    snprintf(hn_buf, sizeof(hn_buf), "%.256s\n", cfg->hostname);
    write_file("/etc/hostname", hn_buf);
  }

  /* 2. /etc/hosts */
  char hosts_content[1024];
  const char *hostname = (cfg->hostname[0]) ? cfg->hostname : "localhost";
  snprintf(hosts_content, sizeof(hosts_content),
           "127.0.0.1\tlocalhost\n"
           "127.0.1.1\t%s\n"
           "::1\t\tlocalhost ip6-localhost ip6-loopback\n"
           "ff02::1\t\tip6-allnodes\n"
           "ff02::2\t\tip6-allrouters\n",
           hostname);
  write_file("/etc/hosts", hosts_content);

  /* 3. resolv.conf (from in-memory config passed via cfg struct) */
  mkdir("/run/resolvconf", 0755);
  if (cfg->dns_server_content[0]) {
    write_file("/run/resolvconf/resolv.conf", cfg->dns_server_content);
  } else {
    /* Fallback if DNS content is empty */
    char dns_fallback[256];
    snprintf(dns_fallback, sizeof(dns_fallback),
             "nameserver %s\nnameserver %s\n", DS_DNS_DEFAULT_1,
             DS_DNS_DEFAULT_2);
    write_file("/run/resolvconf/resolv.conf", dns_fallback);
  }

  /* Link /etc/resolv.conf */
  unlink("/etc/resolv.conf");
  if (symlink("/run/resolvconf/resolv.conf", "/etc/resolv.conf") < 0) { /* ignore */ }

  /* 4. Android Network Groups */
  if (is_android()) {
    /* If /etc/group exists, ensure aid_inet and other groups are present
     * so the user can actually use the network. */
    const char *etc_group = "/etc/group";
    if (access(etc_group, F_OK) == 0) {
      if (!grep_file(etc_group, "aid_inet")) {
        FILE *fg = fopen(etc_group, "a");
        if (fg) {
          fprintf(
              fg,
              "aid_inet:x:3003:\naid_net_raw:x:3004:\naid_net_admin:x:3005:\n");
          fclose(fg);
        }
      }
    }

    /* Add root to groups if usermod exists */
    if (access("/usr/sbin/usermod", X_OK) == 0 ||
        access("/sbin/usermod", X_OK) == 0) {
      /* Performance skip: check if root is already in aid_inet */
      if (!grep_file("/etc/group", "aid_inet:x:3003:root") &&
          !grep_file("/etc/group", "aid_inet:*:3003:root")) {
        char *args[] = {"usermod", "-a", "-G", "aid_inet,aid_net_raw",
                        "root",    NULL};
        run_command_quiet(args);
      }
    }
  }

  return 0;
}

/* ---------------------------------------------------------------------------
 * Runtime introspection
 * ---------------------------------------------------------------------------*/

int detect_ipv6_in_container(pid_t pid) {
  char path[PATH_MAX];
  build_proc_root_path(pid, "/proc/sys/net/ipv6/conf/all/disable_ipv6", path,
                       sizeof(path));

  char buf[16];
  if (read_file(path, buf, sizeof(buf)) < 0)
    return -1;

  /* 0 means enabled, 1 means disabled */
  return (buf[0] == '0') ? 1 : 0;
}
