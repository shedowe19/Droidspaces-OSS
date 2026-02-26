package com.droidspaces.app.util

/**
 * Builds droidspaces commands dynamically based on container configuration.
 * Constructs start/stop/restart commands with all necessary flags.
 * All values are properly quoted to handle spaces and special characters.
 */
object ContainerCommandBuilder {
    private const val DROIDSPACES_BINARY_PATH = Constants.DROIDSPACES_BINARY_PATH

    /**
     * Quote a value for use in shell commands.
     * Escapes single quotes and wraps in single quotes for safety.
     */
    fun quote(value: String): String {
        // Replace single quotes with '\'' (end quote, escaped quote, start quote)
        return "'${value.replace("'", "'\\''")}'"
    }

    /**
     * Build start command for a container based on its configuration.
     */
    fun buildStartCommand(container: ContainerInfo): String {
        val parts = mutableListOf<String>()

        // Binary path
        parts.add(DROIDSPACES_BINARY_PATH)

        // Container name (quoted to handle spaces)
        parts.add("--name=${quote(container.name)}")

        // Rootfs path (quoted to handle spaces)
        // Use --rootfs-img for sparse images, --rootfs for directories
        if (container.useSparseImage) {
            parts.add("--rootfs-img=${quote(container.rootfsPath)}")
        } else {
        parts.add("--rootfs=${quote(container.rootfsPath)}")
        }

        // Hostname (only if not empty and different from name, quoted to handle spaces)
        if (container.hostname.isNotEmpty() && container.hostname != container.name) {
            parts.add("--hostname=${quote(container.hostname)}")
        }

        // DNS servers (quoted to handle the comma-separated string safely)
        if (container.dnsServers.isNotEmpty()) {
            parts.add("--dns=${quote(container.dnsServers)}")
        }

        // Feature flags
        if (container.enableIPv6) {
            parts.add("--enable-ipv6")
        }

        if (container.enableAndroidStorage) {
            parts.add("--enable-android-storage")
        }

        if (container.enableHwAccess) {
            parts.add("--hw-access")
        }

        if (container.enableSensors) {
            parts.add("--sensors")
        }

        if (container.networkMode != "host") {
            parts.add("--network-mode=${container.networkMode}")
        }

        if (container.selinuxPermissive) {
            parts.add("--selinux-permissive")
        }

        if (container.volatileMode) {
            parts.add("-V")
        }

        if (container.bindMounts.isNotEmpty()) {
            val bindString = container.bindMounts.joinToString(",") { "${it.src}:${it.dest}" }
            parts.add("-B")
            parts.add(quote(bindString))
        }

        // Command
        parts.add("start")

        return parts.joinToString(" ")
    }

    /**
     * Build stop command for a container.
     */
    fun buildStopCommand(container: ContainerInfo): String {
        return "$DROIDSPACES_BINARY_PATH --name=${quote(container.name)} stop"
    }

    /**
     * Build restart command for a container.
     * Built directly (not via string-replace) to avoid corruption if name/path contains "start".
     */
    fun buildRestartCommand(container: ContainerInfo): String {
        val parts = mutableListOf<String>()

        parts.add(DROIDSPACES_BINARY_PATH)
        parts.add("--name=${quote(container.name)}")

        if (container.useSparseImage) {
            parts.add("--rootfs-img=${quote(container.rootfsPath)}")
        } else {
            parts.add("--rootfs=${quote(container.rootfsPath)}")
        }

        if (container.hostname.isNotEmpty() && container.hostname != container.name) {
            parts.add("--hostname=${quote(container.hostname)}")
        }

        if (container.dnsServers.isNotEmpty()) {
            parts.add("--dns=${quote(container.dnsServers)}")
        }

        if (container.enableIPv6) parts.add("--enable-ipv6")
        if (container.enableAndroidStorage) parts.add("--enable-android-storage")
        if (container.enableHwAccess) parts.add("--hw-access")
        if (container.enableSensors) parts.add("--sensors")
        if (container.networkMode != "host") parts.add("--network-mode=${container.networkMode}")
        if (container.selinuxPermissive) parts.add("--selinux-permissive")
        if (container.volatileMode) parts.add("-V")

        if (container.bindMounts.isNotEmpty()) {
            val bindString = container.bindMounts.joinToString(",") { "${it.src}:${it.dest}" }
            parts.add("-B")
            parts.add(quote(bindString))
        }

        parts.add("restart")

        return parts.joinToString(" ")
    }

    /**
     * Build status command for a container.
     */
    fun buildStatusCommand(container: ContainerInfo): String {
        return "$DROIDSPACES_BINARY_PATH --name=${quote(container.name)} status"
    }
}

