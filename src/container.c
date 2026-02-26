/*
 * Droidspaces v3 — High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspace.h"

/* ---------------------------------------------------------------------------
 * Cleanup
 * ---------------------------------------------------------------------------*/

/* Build a restart marker path from a container name.
 * Returns the path in 'buf'. Safe against format-truncation. */
static void restart_marker_path(const char *name, char *buf, size_t size) {
  snprintf(buf, size, "%s/%s.restart", get_pids_dir(), name);
}

static void cleanup_container_resources(struct ds_config *cfg, pid_t pid,
                                        int skip_unmount, int force_cleanup) {
  /* Flush filesystem buffers (skip if force cleanup — sync can hang on
   * zombie-held fs) */
  if (!force_cleanup)
    sync();

  if (is_android() && !skip_unmount && count_running_containers(NULL, 0) == 0)
    android_optimizations(0);

  /* 1. Cleanup firmware path (skip when force — accessing zombie rootfs hangs)
   */
  if (!force_cleanup) {
    if (cfg->rootfs_path[0]) {
      firmware_path_remove_rootfs(cfg->rootfs_path);
    } else if (pid > 0) {
      char rootfs[PATH_MAX];
      char root_link[PATH_MAX];
      snprintf(root_link, sizeof(root_link), "/proc/%d/root", pid);
      ssize_t rlen = readlink(root_link, rootfs, sizeof(rootfs) - 1);
      if (rlen > 0) {
        rootfs[rlen] = '\0'; /* readlink does NOT null-terminate */
        firmware_path_remove_rootfs(rootfs);
      }
    }
  }

  /* 2. Resolve global PID file path */
  char global_pidfile[PATH_MAX];
  resolve_pidfile_from_name(cfg->container_name, global_pidfile,
                            sizeof(global_pidfile));

  /* 3. Handle Volatile Overlay Cleanup (upper/work/merged)
   * This MUST happen before unmounting the lower rootfs image.
   * When force_cleanup, use detach+force unmount to avoid hangs. */
  if (cfg->volatile_mode) {
    if (force_cleanup) {
      /* Force path: skip sync, just detach everything */
      char merged[PATH_MAX + 32];
      snprintf(merged, sizeof(merged), "%s/merged", cfg->volatile_dir);
      umount2(merged, MNT_DETACH | MNT_FORCE);
      umount2(cfg->volatile_dir, MNT_DETACH | MNT_FORCE);
      /* Best-effort directory removal */
      remove_recursive(cfg->volatile_dir);
      cfg->volatile_dir[0] = '\0';
    } else {
      cleanup_volatile_overlay(cfg);
    }
  }

  /* 4. Handle rootfs image unmount */
  char mount_point[PATH_MAX] = "";
  if (read_mount_path(cfg->pidfile, mount_point, sizeof(mount_point)) <= 0) {
    /* Fallback: use cfg->img_mount_point if .mount sidecar is gone */
    if (cfg->img_mount_point[0]) {
      safe_strncpy(mount_point, cfg->img_mount_point, sizeof(mount_point));
    }
  }

  if (mount_point[0] && !skip_unmount) {
    if (force_cleanup) {
      /* Force path: detach+force unmount, no sync, no retry loops */
      umount2(mount_point, MNT_DETACH | MNT_FORCE);
      rmdir(mount_point); /* best-effort */
    } else {
      /* Explicitly call unmount wrapper. It handles its own logging. */
      unmount_rootfs_img(mount_point, 0);
    }
  }

  /* 5. Remove tracking info and unlink PID files.
   * For restart (skip_unmount), preserve the .mount sidecar and pidfiles
   * so start_rootfs() can detect the existing mount and reuse it. */
  if (!skip_unmount) {
    remove_mount_path(cfg->pidfile);
    if (cfg->pidfile[0])
      unlink(cfg->pidfile);
    if (global_pidfile[0] && strcmp(cfg->pidfile, global_pidfile) != 0)
      unlink(global_pidfile);

    /* Also clean up any stale restart marker (edge case: restart was
     * attempted but the new start never consumed the marker). */
    if (cfg->container_name[0]) {
      char marker[PATH_MAX];
      restart_marker_path(cfg->container_name, marker, sizeof(marker));
      unlink(marker); /* ignore errors — may not exist */
    }
  }
}

int is_valid_container_pid(pid_t pid) {
  char path[PATH_MAX];
  char buf[256];

  /* Primary marker: /run/droidspaces must exist inside the container.
   * This is the one authoritative marker written by droidspaces on boot.
   * We do NOT require /run/systemd/container — Alpine/runit/openrc never
   * write that file, causing scan to be blind to non-systemd distros. */
  if (build_proc_root_path(pid, DS_DROIDSPACES_MARKER, path, sizeof(path)) < 0)
    return 0;
  if (access(path, F_OK) != 0)
    return 0;

  /* Secondary check: cmdline must contain "init" (any init system).
   * Accepts: /sbin/init, /bin/init, /usr/bin/runit-init, /bin/openrc-init */
  snprintf(path, sizeof(path), DS_PROC_CMDLINE_FMT, pid);
  if (read_file(path, buf, sizeof(buf)) < 0)
    return 0;
  if (!strstr(buf, "init"))
    return 0;

  return 1;
}

/* ---------------------------------------------------------------------------
 * Introspection
 * ---------------------------------------------------------------------------*/

int check_status(struct ds_config *cfg, pid_t *pid_out) {
  if (auto_resolve_pidfile(cfg) < 0) {
    ds_error("Could not resolve PID file. Use --name or --pidfile.");
    return -1;
  }

  pid_t pid = 0;
  if (is_container_running(cfg, &pid)) {
    if (pid_out)
      *pid_out = pid;
    return 0;
  }

  ds_error("Container '%s' is not running or invalid.", cfg->container_name);
  return -1;
}

/* ---------------------------------------------------------------------------
 * Start
 * ---------------------------------------------------------------------------*/

int start_rootfs(struct ds_config *cfg) {
  /* 0. Early restart detection: check for existing mount BEFORE name
   *    resolution or workspace setup, using the restart marker and
   *    .mount sidecar to detect a preserved mount from stop(skip_unmount). */
  int restart_reuse = 0;
  if (cfg->container_name[0] && cfg->rootfs_img_path[0]) {
    /* Build restart marker path */
    char restart_marker[PATH_MAX];
    restart_marker_path(cfg->container_name, restart_marker,
                        sizeof(restart_marker));

    if (access(restart_marker, F_OK) == 0) {
      /* Restart marker present — try to reuse existing mount */
      unlink(restart_marker); /* consume the marker */

      /* Resolve pidfile so we can read .mount sidecar */
      if (cfg->pidfile[0] == '\0')
        resolve_pidfile_from_name(cfg->container_name, cfg->pidfile,
                                  sizeof(cfg->pidfile));

      char existing_mount[PATH_MAX];
      if (cfg->pidfile[0] &&
          read_mount_path(cfg->pidfile, existing_mount,
                          sizeof(existing_mount)) > 0 &&
          is_mountpoint(existing_mount)) {
        ds_log("Reusing existing mount at %s (restart)", existing_mount);
        safe_strncpy(cfg->rootfs_path, existing_mount,
                     sizeof(cfg->rootfs_path));
        cfg->is_img_mount = 1;
        safe_strncpy(cfg->img_mount_point, cfg->rootfs_path,
                     sizeof(cfg->img_mount_point));
        restart_reuse = 1;
      } else {
        ds_warn("Restart marker found but mount not active, doing fresh mount");
      }
    }
  }

  /* 1. Preparation */
  ensure_workspace();

  if (cfg->selinux_permissive)
    android_set_selinux_permissive();
  if (cfg->android_storage && !is_android())
    ds_warn("--enable-android-storage is only supported on Android hosts. "
            "Skipping.");

  /* 1b. Resolve container name (needed for descriptive mount points) */
  if (cfg->container_name[0] == '\0') {
    if (cfg->rootfs_img_path[0]) {
      ds_error("--name is mandatory when using a rootfs image.");
      return -1;
    }

    if (generate_container_name(cfg->rootfs_path, cfg->container_name,
                                sizeof(cfg->container_name)) < 0)
      return -1;
  }

  if (!restart_reuse) {
    /* Find an available name (only needed for fresh starts) */
    char final_name[256];
    if (find_available_name(cfg->container_name, final_name,
                            sizeof(final_name)) < 0)
      ds_die("Too many containers running with similar names");
    safe_strncpy(cfg->container_name, final_name, sizeof(cfg->container_name));
  }

  /* If no hostname specified, default to container name */
  if (cfg->hostname[0] == '\0') {
    safe_strncpy(cfg->hostname, cfg->container_name, sizeof(cfg->hostname));
  }

  /* 2. Mount rootfs image if provided (using the resolved name) */
  if (cfg->rootfs_img_path[0] && !restart_reuse) {
    if (mount_rootfs_img(cfg->rootfs_img_path, cfg->rootfs_path,
                         sizeof(cfg->rootfs_path), cfg->volatile_mode,
                         cfg->container_name) < 0)
      return -1;
    cfg->is_img_mount = 1;
    safe_strncpy(cfg->img_mount_point, cfg->rootfs_path,
                 sizeof(cfg->img_mount_point));
  }

  /* 3. Early pre-flight for volatile mode (before any host changes) */
  if (check_volatile_mode(cfg) < 0) {
    if (cfg->is_img_mount)
      unmount_rootfs_img(cfg->img_mount_point, cfg->foreground);
    return -1;
  }

  generate_uuid(cfg->uuid, sizeof(cfg->uuid));

  /* Pre-populate volatile_dir for monitor cleanup (actual overlay setup
   * happens inside internal_boot's isolated mount namespace) */
  if (cfg->volatile_mode) {
    snprintf(cfg->volatile_dir, sizeof(cfg->volatile_dir),
             "%s/" DS_VOLATILE_SUBDIR "/%s", get_workspace_dir(),
             cfg->container_name);
  }

  /* Write UUID sync file for boot sequence
   * Skip in volatile mode: rootfs.img is mounted RO, and UUID
   * is already in cfg (survives fork). */
  if (!cfg->volatile_mode) {
    char uuid_sync[PATH_MAX];
    snprintf(uuid_sync, sizeof(uuid_sync), "%.4070s/.droidspaces-uuid",
             cfg->rootfs_path);
    write_file(uuid_sync, cfg->uuid);
  }

  /* 2. Parent-side PTY allocation (LXC Model) */
  /* CRITICAL: Before forking, verify /sbin/init exists in the rootfs */
  char init_path[PATH_MAX];
  char rootfs_norm[PATH_MAX];
  safe_strncpy(rootfs_norm, cfg->rootfs_path, sizeof(rootfs_norm));
  size_t rlen = strlen(rootfs_norm);
  if (rlen > 0 && rootfs_norm[rlen - 1] == '/')
    rootfs_norm[rlen - 1] = '\0';

  snprintf(init_path, sizeof(init_path), "%.4080s/sbin/init", rootfs_norm);
  struct stat st;
  if (lstat(init_path, &st) != 0) {
    ds_error("Init binary not found: %s", init_path);
    ds_error(
        "Please ensure the rootfs path is correct and contains /sbin/init.");
    if (cfg->is_img_mount)
      unmount_rootfs_img(cfg->img_mount_point, cfg->foreground);
    return -1;
  }

  /*
   * Robust Check: If it's a symlink, we MUST assume it's valid.
   * Absolute symlinks (e.g. /sbin/init -> /lib/systemd/systemd) will appear
   * "broken" from the host's perspective, but will resolve correctly inside
   * the container after pivot_root.
   */
  if (!S_ISLNK(st.st_mode) && access(init_path, X_OK) != 0) {
    ds_error("Init binary is not executable: %s", init_path);
    ds_error("Ensure it has executable permissions.");
    if (cfg->is_img_mount)
      unmount_rootfs_img(cfg->img_mount_point, cfg->foreground);
    return -1;
  }

  cfg->tty_count = DS_MAX_TTYS;
  if (ds_terminal_create(&cfg->console) < 0)
    ds_die("Failed to allocate console PTY");

  /* Propagate the host terminal's window size to the console PTY master
   * so the slave (which becomes /dev/console) has correct dimensions
   * from the very start of boot. This prevents misaligned output during
   * the window between PTY creation and the console_monitor_loop startup.
   * Without this, 'sudo poweroff' output is misaligned for the first
   * ~10 lines because sudo resets/queries the terminal size and finds
   * a {0,0} winsize on the PTY slave. */
  if (isatty(STDIN_FILENO)) {
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
      ioctl(cfg->console.master, TIOCSWINSZ, &ws);
  }

  for (int i = 0; i < cfg->tty_count; i++) {
    if (ds_terminal_create(&cfg->ttys[i]) < 0)
      break;
  }

  /* 3. Resolve target PID file names early so monitor inherits them */
  char global_pidfile[PATH_MAX];
  resolve_pidfile_from_name(cfg->container_name, global_pidfile,
                            sizeof(global_pidfile));

  /* If no pidfile specified, or we want to use the global one */
  if (!cfg->pidfile[0]) {
    safe_strncpy(cfg->pidfile, global_pidfile, sizeof(cfg->pidfile));
  }

  /* 4. Pipe for synchronization */
  int sync_pipe[2]; /* Init -> Monitor (sends PID) */
  if (pipe(sync_pipe) < 0)
    ds_die("pipe failed: %s", strerror(errno));

  int monitor_pipe[2]; /* Monitor -> Init (sends "Network Ready" signal) */
  if (pipe(monitor_pipe) < 0)
    ds_die("pipe failed: %s", strerror(errno));

  /* 5. Configure host-side networking (NAT, ip_forward, DNS) BEFORE fork.
   * This eliminates the race condition where the child boots and reads
   * DNS before the parent has written it. */
  fix_networking_host(cfg);
  android_optimizations(1);

  /* 4. Fork Monitor Process */
  pid_t monitor_pid = fork();
  if (monitor_pid < 0)
    ds_die("fork failed: %s", strerror(errno));

  if (monitor_pid == 0) {
    /* MONITOR PROCESS */
    close(sync_pipe[0]);
    close(monitor_pipe[0]); /* Write end only */

    if (setsid() < 0 && errno != EPERM) {
      /* Fatal only if it's not EPERM (which means already leader) */
      ds_error("setsid failed: %s", strerror(errno));
      exit(EXIT_FAILURE);
    }
    prctl(PR_SET_NAME, "[ds-monitor]", 0, 0, 0);

    /* Unshare namespaces - Monitor enters new UTS, IPC, and optionally Cgroup
     * namespaces immediately. PID namespace unshare means only CHILDREN of the
     * monitor will be in the new PID NS. Node: we no longer unshare MNT here so
     * monitor can cleanup host mounts.
     * Note: We intentionally do NOT unshare CLONE_NEWNET in the monitor.
     * The monitor must remain in the host network namespace to configure
     * veth pairs and NAT rules. The child (init) will unshare netns itself. */
    int ns_flags = CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID;

    /* Adaptive Cgroup Namespace (introduced in Linux 4.6) */
    if (access("/proc/self/ns/cgroup", F_OK) == 0) {
      /* To get isolation from a cgroup namespace, we must be in a sub-cgroup
       * BEFORE we unshare. If we are in the root '/', the namespace root will
       * be the host's root, providing zero isolation.
       * We use a container-specific path to avoid conflicts. */
      if (access("/sys/fs/cgroup/cgroup.procs", F_OK) == 0) {
        char cg_path[PATH_MAX];
        snprintf(cg_path, sizeof(cg_path), "/sys/fs/cgroup/droidspaces/%s",
                 cfg->container_name);
        mkdir_p(cg_path, 0755);

        char cg_procs[PATH_MAX];
        safe_strncpy(cg_procs, cg_path, sizeof(cg_procs));
        strncat(cg_procs, "/cgroup.procs",
                sizeof(cg_procs) - strlen(cg_procs) - 1);
        FILE *f = fopen(cg_procs, "we");
        if (f) {
          fprintf(f, "%d\n", getpid());
          fclose(f);
        }
      }
      ns_flags |= CLONE_NEWCGROUP;
    }

    if (unshare(ns_flags) < 0)
      ds_die("unshare failed: %s", strerror(errno));

    /* Fork Container Init (PID 1 inside) */
    pid_t init_pid = fork();
    if (init_pid < 0)
      exit(EXIT_FAILURE);

    if (init_pid == 0) {
      /* CONTAINER INIT */
      close(sync_pipe[1]); /* Write end only (via dup/exec logic or direct usage?) Wait, sync_pipe is Init->Monitor. So Init writes to [1]. Monitor reads from [0]. */
      /* Actually, logic above says:
         Monitor: close(sync_pipe[0]) -> This is wrong?
         Let's re-read:
         Parent (Main) reads from sync_pipe[0].
         Monitor writes to sync_pipe[1]? No.
         Monitor FORKS Init.
         Init writes to sync_pipe[1].
         Monitor waits for Init? No, Monitor waits for Init to exit.
         Parent waits for Monitor to send PID?

         Wait, existing logic:
         - Main creates pipe.
         - Main forks Monitor.
         - Monitor forks Init.
         - Init writes PID to pipe.
         - Main reads PID from pipe.

         So Sync Pipe connects Init -> Main directly?
         Let's trace:
         Main: pipe(sync_pipe). forks Monitor.
         Monitor: close(sync_pipe[0]). forks Init.
         Init: write(sync_pipe[1], pid).
         Main: read(sync_pipe[0], pid).

         So Sync Pipe bypasses Monitor for PID delivery.

         BUT, for Network Sync, we need Monitor <-> Init.
         So `monitor_pipe` is correct.
       */

      close(monitor_pipe[1]); /* Read end only */

      /* Unshare Network Namespace if requested */
      if (cfg->net_mode != DS_NET_HOST) {
        ds_log("INIT: Unsharing network namespace...");
        if (unshare(CLONE_NEWNET) < 0) {
             ds_error("Failed to unshare network namespace: %s", strerror(errno));
             exit(EXIT_FAILURE);
        }
        ds_log("INIT: Unshare success.");
      }

      /* Notify Main (and implicitly Monitor via timing?) that we are alive/unshared */
      /* Actually, Main reads this. Monitor doesn't see it. */
      ds_log("INIT: Writing PID %d to parent...", getpid());
      if (write(sync_pipe[1], &init_pid, sizeof(pid_t)) < 0) { /* ignore */ }
      close(sync_pipe[1]);

      /* Wait for Monitor to configure network */
      if (cfg->net_mode != DS_NET_HOST) {
          char buf;
          ds_log("INIT: Waiting for monitor configuration...");
          if (read(monitor_pipe[0], &buf, 1) != 1) {
              ds_error("Failed to sync with monitor (network setup)");
              exit(EXIT_FAILURE);
          }
          ds_log("INIT: Network configured.");
      }
      close(monitor_pipe[0]);

      /* internal_boot will handle its own stdfds. */
      exit(internal_boot(cfg));
    }

    /* MONITOR CONTINUES */
    /* Write child PID to sync pipe? No, Init did that. */
    /* Monitor doesn't use sync_pipe. */
    close(sync_pipe[1]);

    /* Monitor needs to know Init PID. */
    /* init_pid is known here. */

    /* Configure network namespace if requested (from Monitor context) */
    if (cfg->net_mode != DS_NET_HOST) {
      /* Wait a tiny bit for Init to unshare?
         Init writes to sync_pipe then waits on monitor_pipe.
         So Init is definitely blocked or running.
         We can proceed. */

      if (ds_configure_network_namespace(init_pid, cfg) < 0) {
        ds_error("Failed to configure network namespace. Killing container.");
        kill(init_pid, SIGKILL);
        exit(EXIT_FAILURE);
      }

      /* Signal Init to proceed */
      if (write(monitor_pipe[1], "1", 1) < 0) { /* ignore */ }
    }
    close(monitor_pipe[1]);

    /* Ensure monitor is not sitting inside any mount point */
    if (chdir("/") < 0) { /* ignore */ }

    /* Stdio handling for monitor in background mode */
    if (!cfg->foreground) {
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        dup2(devnull, 0);
        dup2(devnull, 1);
        dup2(devnull, 2);
        close(devnull);
      }
    }

    /* Wait for child to exit */
    int status;
    while (waitpid(init_pid, &status, 0) < 0 && errno == EINTR)
      ;

    /* Check for restart marker — if present, skip cleanup so the
     * restart command can reuse the existing mount. */
    char restart_marker[PATH_MAX];
    restart_marker_path(cfg->container_name, restart_marker,
                        sizeof(restart_marker));
    if (access(restart_marker, F_OK) == 0) {
      ds_log("Restart marker found, skipping monitor cleanup");
    } else {
      /* Normal exit or crash — full cleanup */
      cleanup_container_resources(cfg, init_pid, 0, 0);
    }

    exit(WEXITSTATUS(status));
  }

  /* PARENT PROCESS */
  close(sync_pipe[1]);
  close(monitor_pipe[0]);
  close(monitor_pipe[1]);

  /* Wait for Init (via Monitor's fork) to send child PID */
  if (read(sync_pipe[0], &cfg->container_pid, sizeof(pid_t)) != sizeof(pid_t)) {
    ds_error("Monitor failed to send container PID.");
    return -1;
  }
  close(sync_pipe[0]);

  ds_log("Container started with PID %d (Monitor: %d)", cfg->container_pid,
         monitor_pid);

  /* 5b. Android: Remount /data with suid for directory-based containers.
   * This is required for sudo/su to work if the rootfs is on /data. */
  if (is_android() && !cfg->rootfs_img_path[0])
    android_remount_data_suid();

  if (cfg->hw_access)
    ds_log("Hardware access enabled: using host devtmpfs...");
  else
    ds_log("Hardware access disabled: using isolated tmpfs /dev...");

  /* Log volatile mode before boot message */
  if (cfg->volatile_mode)
    ds_log("Entering volatile mode (OverlayFS)...");

  /* Log bind mounts before boot message */
  if (cfg->bind_count > 0)
    ds_log("Setting up %d custom bind mount(s)...", cfg->bind_count);

  ds_log("Booting '%s' (init: /sbin/init)...", cfg->container_name);

  /* 6. Save PID file */
  char pid_str[32];
  snprintf(pid_str, sizeof(pid_str), "%d", cfg->container_pid);

  /* Always save to global Pids directory (for --name lookups) */
  if (write_file_atomic(global_pidfile, pid_str) < 0) {
    ds_error("Failed to write PID file: %s", global_pidfile);
  }

  /* Also save to user-specified --pidfile if different */
  if (cfg->pidfile[0] && strcmp(cfg->pidfile, global_pidfile) != 0) {
    if (write_file_atomic(cfg->pidfile, pid_str) < 0) {
      ds_error("Failed to write PID file: %s", cfg->pidfile);
    }
  }

  if (cfg->is_img_mount)
    save_mount_path(cfg->pidfile, cfg->img_mount_point);

  /* 6. Foreground or background finish */
  if (cfg->foreground) {
    /* Visual separation before container output */
    printf("\n");
    int ret = console_monitor_loop(cfg->console.master, monitor_pid,
                                   cfg->container_pid);
    return ret;
  } else {
    /* Wait for container to finish pivot_root before showing info.
     * The boot sequence writes /run/droidspaces after pivot_root,
     * so we poll for it via /proc/<pid>/root/run/droidspaces. */
    char marker[PATH_MAX];
    snprintf(marker, sizeof(marker), "/proc/%d/root/run/droidspaces",
             cfg->container_pid);
    int booted = 0;
    for (int i = 0; i < 50; i++) { /* 5 seconds max */
      if (access(marker, F_OK) == 0) {
        booted = 1;
        break;
      }
      /* If the container PID is already dead, stop polling */
      if (kill(cfg->container_pid, 0) < 0 && errno == ESRCH)
        break;
      usleep(100000); /* 100ms */
    }

    if (!booted) {
      ds_error("Container failed to boot correctly.");
      /* If pid is still alive, we might want to kill it, but monitor usually
       * handles this. Let's just return error so parent doesn't report
       * success.
       */
      return -1;
    }

    show_info(cfg, 1);
    ds_log("Container '%s' is running in background.", cfg->container_name);
    if (is_android()) {
      ds_log("Use 'su -c \"%s --name='%s' enter\"' to connect.", cfg->prog_name,
             cfg->container_name);
    } else {
      ds_log("Use 'sudo %s --name='%s' enter' to connect.", cfg->prog_name,
             cfg->container_name);
    }
  }

  return 0;
}
