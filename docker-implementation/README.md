# MyDocker Orchestration Engine

A custom, completely scratch-built container orchestration engine written in C. This project implements the core primitives of Linux containerization (namespaces, cgroups, OverlayFS) and combines them with a custom YAML parser and IPAM system to create a declarative, compose-style deployment tool.

## Technical Architecture

This engine bypasses high-level wrappers and interacts directly with the Linux kernel and filesystem to achieve true process, storage, and network isolation.

### 1. Process & Resource Isolation
* **Namespaces:** Utilizes the `clone()` syscall with `CLONE_NEWNS`, `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNET` to create strictly isolated environments.
* **Cgroups v2:** Dynamically generates control groups (`/sys/fs/cgroup/my_container_<name>`) to strictly enforce `memory.max` and `pids.max` limits on running workloads.
* **Chroot Jails:** Secures the process by pivoting the root filesystem and isolating `/proc` so internal processes cannot map host resources.

### 2. Storage & Filesystem (OverlayFS & Bind Mounts)
* **Union Filesystems:** Implements a multi-layered storage architecture using `mount -t overlay`. It pulls an immutable Alpine/Ubuntu base layer (`lowerdir`) and stacks a read-write container layer (`upperdir`) on top.
* **Persistent Volumes:** Supports `mount --bind` to punch controlled holes through the isolated filesystem, mapping host directories directly to container inodes for live-reloading and database persistence.
* **Delta Snapshots:** Includes a state-saving mechanism to archive only the `upper` directory deltas for lightweight backups.

### 3. Networking & Routing
* **Custom IPAM:** Features a lightweight IP Address Management system to dynamically allocate and lock `10.0.0.X` subnet addresses for newly spawned containers.
* **Virtual Bridging:** Wires a custom `br0` host bridge and uses `veth` pairs to connect isolated network namespaces to the host.
* **NAT Port Forwarding:** Programmatically injects `iptables` rules for **DNAT** (Destination NAT) and **Masquerading**, allowing host-bound traffic (e.g., `localhost:8080`) to seamlessly route into isolated container processes.

### 4. Declarative Orchestration
* **YAML Parsing:** Integrates `libyaml` to parse `docker-compose.yml` equivalents.
* **Concurrency:** Forks multiple independent container pipelines simultaneously and tracks their lifecycles using `waitpid()`.

## Usage & CLI Commands

> **Note:** This engine requires `sudo` as it manipulates host kernel tables, mounts, and network interfaces.

* `sudo ./engine init`
  Initializes the host environment and pulls the base OS rootfs architecture.
* `sudo ./engine compose-up`
  Parses the local `docker-compose.yml`, provisions networking, and spins up the entire stack concurrently.
* `sudo ./engine compose-down [-v]`
  Gracefully terminates the stack. Passing the `-v` flag permanently wipes the isolated storage layers.
* `sudo ./engine ps`
  Lists all active containers by probing host PID records.
* `sudo ./engine spin_up <name> [--memory] [--pids] [--dns]`
  Imperatively launches a single container with specific resource limits.
* `sudo ./engine save <name> <dir>`
  Archives the container's delta layer to a host directory.
* `sudo ./engine uninstall`
  A nuclear cleanup command that unmounts all active filesystems, drops the virtual bridge, and purges the host directory configuration.

## Running the Demo ("Victory Stack")

The repository includes a sample deployment to test the engine's capability, featuring a FastAPI backend, a web UI, and a persistent SQLite volume mount.

1. Compile the engine: `gcc main.c -lyaml -o engine`
2. Initialize the storage roots: `sudo ./engine init`
3. Launch the deployment: `sudo ./engine compose-up`
4. Access the routed container via `http://localhost:8080`.

## Platform Constraints
This software interacts intimately with the Linux kernel. It requires a Linux host (e.g., Ubuntu 22.04/24.04) and relies on features specific to `systemd` and modern kernel configurations (Cgroups v2, OverlayFS).

## The Major Architecture Upgrades

### 1. Declarative Orchestration (YAML Parsing)

* **Stage 6:** You were using imperative CLI commands (`sudo ./engine spin_up my_app --memory 50M`). You had to spin up one container at a time and manually track them.
* **Final Stage:** You implemented a custom YAML parser using `libyaml`. The engine now reads a desired state (`docker-compose.yml`) and uses `fork()` to spawn multiple independent container processes simultaneously. The parent process then uses `waitpid()` to monitor the entire stack.

### 2. The Network Layer: Port Forwarding (DNAT/SNAT)

In Stage 6, your containers had an IP address (e.g., `10.0.0.2`), but they were completely invisible to the outside world. To reach a web server inside, you had to query `10.0.0.2` directly from the host.

In the Final Stage, you introduced **Network Address Translation (NAT)** via `iptables`, creating a true gateway:

* **PREROUTING (DNAT):** Catches traffic hitting your host machine's physical network card (e.g., port 8080) and rewrites the destination packet to the container's isolated IP (e.g., `10.0.0.2:8080`).
* **OUTPUT:** Does the exact same thing, but for traffic generated locally on the host (when you type `localhost:8080` in your browser).
* **POSTROUTING (Masquerade):** Ensures that when the container sends a response back out, the source IP is rewritten to look like the host machine. Without this, the client would drop the packet, seeing a response from an unknown `10.0.0.X` address instead of the host it queried.

### 3. Persistent Bind Mounts

* **Stage 6:** You only used an `OverlayFS`. Everything written inside the container was trapped in the `upper` directory and vanished if you deleted the container.
* **Final Stage:** You implemented `mount --bind`. This directly links a directory on your host (like `/home/prashant/docker_test/victory_stack/backend`) to a directory inside the container's isolated root filesystem (like `/app`). The VFS (Virtual File System) maps them to the exact same inodes on the disk.

---

