# General TODO list, in rough order of priority/timeline

    It feels a bit high-fallutin to call this a "roadmap", so it's rather a loose collection of TODO items

* [x] Bootloader (limine)
* [x] Memory Management
* [x] Logging to in-kernel ring buffer
* [x] Terminal output of ring buffer log
* [x] Enable HPET
* [x] Enable Paging and setup page map
* [x] Enable Interrupts, setup some basic handlers
* [x] Scheduler (basic)
* [ ] Syscalls - needs asm wrapper for syscall/sysret, C dispatch logic exists
* [ ] Futex system call (do I even need this? I hope not :( )
* [x] Virtual Filesystem
* [ ] Load kernel modules (e.g drivers)
* [ ] Load user programs - ELF loading works, blocked by syscalls
* [ ] Network stack - device abstraction exists, no TCP/IP/UDP
* [ ] E1000 driver - init works, no packet TX/RX
* [ ] Web server kernel module
* [ ] Syscalls over HTTP
* [ ] JS: user authentication
* [ ] JS: windowing system
