# Cottage Architecture

## Overview

Cottage is an experimental x86_64 operating system where the primary user interface is a web browser. The kernel
includes an HTTP server that allows browser-based applications to invoke kernel functionality directly. Server-side
applications make traditional syscalls; browser-side JavaScript communicates with the kernel via HTTP/WebSocket.

This web-native approach eliminates the traditional terminal emulator or GUI toolkit layers, replacing them with
standard web technologies (HTML/CSS/JavaScript).

## Execution Model

Applications have two components:

1. **Browser-side**: HTML, CSS, and JavaScript that runs in the user's browser. This handles UI and user interaction.

2. **Server-side** (optional): Native binaries that run in ring 3 userspace on the server. These make syscalls to the
   kernel like traditional Unix applications. Useful for background tasks, workers, or operations that must continue
   when the browser is disconnected.

The server-side model is analogous to SSH-accessible Linux: authenticated users can run binaries with their own
privileges. This is the traditional Unix execution model, not a sandboxed or interpreted environment.

## I/O Model

The browser acts as a pseudo-TTY (pTTY) for terminal applications. This follows the Unix remote terminal model, which
was designed from the start for network-attached terminals.

- stdin/stdout/stderr are forwarded between server-side processes and the browser via WebSocket
- The WebSocket connection may be multiplexed (single connection, multiple pTTYs) or simplex (one connection per pTTY)
  — to be decided
- CLI tools like `ls` or `cat` work naturally: they write to stdout, the kernel forwards to the browser's terminal

## Network Architecture

Two networking paths are supported:

1. **Browser ↔ Kernel ↔ App**: The browser connects to the kernel's web server via WebSocket. The kernel mediates
   communication with server-side applications. This is the primary path for interactive use.

2. **App ↔ External Network**: Server-side applications can create their own sockets for external connections (e.g.,
   federation, fetching remote content, accepting inbound connections while browser is disconnected).

The socket API for server-side applications follows BSD conventions:
- `socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `send()`, `recv()`, `close()`
- Implemented as kernel syscalls
- Familiar, well-documented, widely understood

## Security Model

*To be designed.*

Key considerations:
- Authentication mechanism for browser → kernel connection
- User accounts and privilege separation
- Superuser privileges for system modification vs. normal user privileges for running applications

## Open Questions

- **WebSocket multiplexing**: Single multiplexed connection vs. one connection per pTTY?
- **Window manager**: Single terminal in browser, or embedded window manager with multiple pTTY windows?
- **Kernel routing**: How does the kernel's web server route/proxy requests to userspace applications?
- **Process lifecycle**: How are server-side processes spawned, monitored, and cleaned up?
- **IPC mechanism**: How do browser-side and server-side components of the same application communicate?
