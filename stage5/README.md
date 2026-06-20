# Stage 5: Network Isolation

## 1. The Isolation Blueprint (Main Process)

```c
int container_pid = clone(
    container_main, 
    stack + STACK_SIZE, 
    CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD, 
    NULL
);

```

* **What it does:** Adds the `CLONE_NEWNET` flag to the `clone` system call.
* **Why it is needed:** By default, a cloned process shares the host's network interfaces, routing tables, and firewall rules. `CLONE_NEWNET` provisions a completely blank network stack inside the container. When the container boots, it cannot see or access the host's internet connection or local interfaces; it is entirely offline until you manually build a path back to the outside world.

---

## 2. Host-Side Network Plumbing (`main`)

This section creates a virtual network link spanning the boundary between the host's network namespace and the container's isolated network namespace.

```c
system("ip link add veth_host type veth peer name veth_child");

```

* **What it does:** Creates a Virtual Ethernet (`veth`) pair named `veth_host` and `veth_child`.
* **Why it is needed:** Think of a `veth` pair as a virtual, bidirectional Ethernet cable. Anything sent into `veth_host` instantly pops out of `veth_child`, and vice versa. This provides the physical link layer between the host and container.

```c
snprintf(cmd, sizeof(cmd), "ip link set veth_child netns %d", container_pid);
system(cmd);

```

* **What it does:** Moves the `veth_child` interface out of the host's default network namespace and throws it directly into the network namespace of the container process (identified by `container_pid`).
* **Why it is needed:** Initially, both ends of the virtual cable sit on the host. By shifting one end into the container's namespace, you bridge the isolation wall, giving the container a dedicated interface it can control.

```c
system("ip addr add 10.0.0.1/24 dev veth_host");
system("ip link set veth_host up");

```

* **What it does:** Assigns the static IP address `10.0.0.1` to the host's end of the cable and sets the link state to active (`up`).
* **Why it is needed:** This establishes the host interface as the default gateway for the container. The container will send all traffic destined for the internet to this address.

```c
system("sysctl -w net.ipv4.ip_forward=1 > /dev/null");

```

* **What it does:** Informs the Linux kernel on the host that it is allowed to route IP packets originating from other interfaces.
* **Why it is needed:** By default, Linux drops packets that arrive on one interface (like `veth_host`) if they are meant for an external network. Enabling IP forwarding turns your host machine into a functional router.

```c
system("iptables -t nat -A POSTROUTING -s 10.0.0.0/24 -j MASQUERADE");

```

* **What it does:** Appends a Network Address Translation (NAT) rule to the host's firewall using `iptables`. It instructs the host to masquerade (rewrite) any traffic coming from the `10.0.0.0/24` subnet using the host's own public IP address.
* **Why it is needed:** External routers on the internet do not know how to send responses back to a private internal address like `10.0.0.2`. NAT replaces the source IP with the host's valid IP when traffic leaves, and maps it back when responses return, granting the container internet access.

---

## 3. On-The-Fly DNS Injection (`main`)

```c
mkdir("./rootfs/etc", 0755); 
write_file("./rootfs/etc/resolv.conf", "nameserver 8.8.8.8\n");

```

* **What it does:** Programmatically creates an `etc` folder inside your target root filesystem directory on the host and writes a default Google public DNS server to a fresh `resolv.conf` file.
* **Why it is needed:** Inside a `chroot` jail, the container cannot see the host's `/etc/resolv.conf`. Without this file inside the container's own isolated view, tools like `ping` or `apk` cannot translate human-readable domain names (like `google.com`) into numeric IP addresses, breaking domain-based network utilities.

---

## 4. Container-Side Network Setup (`container_main`)

Once the synchronization pipe unblocks the container process, the internal network configuration script executes before spawning the shell.

```c
system("ip link set lo up");

```

* **What it does:** Activates the local loopback interface (`lo`).
* **Why it is needed:** In a blank network namespace, even the loopback interface (`127.0.0.1`) is down by default. Many applications, runtimes, and local inter-process utilities fail instantly if they cannot talk to `localhost`.

```c
system("ip addr add 10.0.0.2/24 dev veth_child");
system("ip link set veth_child up");

```

* **What it does:** Configures the container's end of the virtual interface with the static IP address `10.0.0.2` on a `/24` subnet mask and activates the interface link.
* **Why it is needed:** Gives the container a valid identity on the network link so it can communicate directly with the host's gateway interface (`10.0.0.1`).

```c
system("ip route add default via 10.0.0.1");

```

* **What it does:** Alters the container's internal routing table, establishing `10.0.0.1` (the host interface) as the default gateway routing rule.
* **Why it is needed:** Without a default route, the container knows how to talk to `10.0.0.1`, but if you try to reach an outside target like `8.8.8.8`, the kernel will error out with `Network is unreachable`. This rule acts as a catch-all saying: *"If a packet is not local, forward it to the host."*

---

## 5. Graceful Host Teardown (`main`)

```c
system("ip link set veth_host down 2>/dev/null");
system("ip link del veth_host 2>/dev/null");

```

* **What it does:** Explicitly marks the host network interface down to clear out pending kernel locks, and then cleanly deletes the virtual link configuration.
* **Why it is needed:** Though the kernel automatically tears down the container's side of the cable when the namespace dies, manually shutting down and removing the host-side endpoint ensures that asynchronous timing bugs don't leave lingering, dead `veth` interfaces visible on the host OS.

```c
system("iptables -t nat -D POSTROUTING -s 10.0.0.0/24 -j MASQUERADE 2>/dev/null");

```

* **What it does:** Deletes (`-D`) the exact NAT rule that was appended during the initialization phase.
* **Why it is needed:** Firewalls persist indefinitely across processes. If you do not explicitly delete this rule, every single execution of your container engine will leave a permanent, duplicate routing artifact in the host's system configuration tables, cluttering your network stack.

---

### Quick Diagnostic Cheat Sheet

| Command | Objective | When to Run | Expected Good Outcome |
| --- | --- | --- | --- |
| `ip a` | Check interface creation/deletion | During & After | `veth_host` appears during, disappears after. |
| `sudo iptables -t nat -S` | Check NAT rules | During & After | Rule is added during, deleted after. |
| `lsns -t net` | List Network Namespaces | During | A unique network namespace ID exists for the container PID. |
| `ls /sys/fs/cgroup/` | Check Cgroup footprint | During & After | `my_container` folder exists during, is wiped after. |