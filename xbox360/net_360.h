// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Plain UDP sockets from a resident system DLL (docs/PLAN.md 4.1, "Transport"), over XAM's
 * NetDll_* exports as the system caller (XNCALLER_SYSAPP = 2), so a title's own XNetCleanup /
 * WSACleanup never closes them. team_360.c uses only this; no ordinals live there.
 *
 * What the 17559 XAM does (read from research/dumps/xam_815f0000.bin; details in net_360.c):
 *   - every NetDll_* export is a stub that moves the caller type into r4 and XAM's network
 *     context into r3, then branches to the implementation;
 *   - XNetStartup / WSAStartup accept caller 1 (title) or 2 (system) only, and keep a separate
 *     XNet and WSA start count per caller; socket() fails with WSANOTINITIALISED (10093)
 *     unless the caller's WSA count is non-zero;
 *   - setsockopt(SOL_SOCKET, 0x5801, 1) marks a socket insecure (plain UDP, no XNet
 *     encapsulation); only a title caller is checked for the insecure-sockets privilege;
 *   - SO_BROADCAST is accepted on a datagram socket.
 * HaloLAN.xex and xbdm.xex on the console do exactly this with caller 2 (their import tables
 * and call sites, read from memory): XNetStartup + WSAStartupEx, UDP, 0x5801, and xbdm also
 * SO_REUSEADDR and SO_BROADCAST.
 *
 * Calls here block for no longer than XAM does (the sockets are non-blocking): never from a
 * DPC or the driver thread; team_360.c's and bridge_360.c's net threads only.
 *
 * Every function returns 0 or the raw error: the NetDll return code where it is one
 * (startup), else WSAGetLastError() (a WSA code, e.g. 10035 WSAEWOULDBLOCK).
 */
#ifndef NET_360_H
#define NET_360_H

#include "../core/compat.h"

#define XN_WOULDBLOCK 10035 /* WSAEWOULDBLOCK: nothing to receive */
#define XN_ENOTINIT 10093   /* WSANOTINITIALISED */
#define XN_BROADCAST 0xFFFFFFFFu

/* xn_init's refusals (negative, not WSA codes) */
#define XN_E_NO_XAM 1       /* xam.xex has no module handle */
#define XN_E_ORDINAL 2      /* an ordinal didn't resolve */
#define XN_E_CODE 3         /* an export isn't the 17559 code read offline */

/* xn_startup's mode bits (the spike's question "does it need XNetStartup or WSAStartup
 * first?"): HaloLAN does both, XNet first */
#define XN_START_XNET 1     /* NetDll_XNetStartup(2, HaloLAN's parameters) */
#define XN_START_WSA 2      /* NetDll_WSAStartup(2, 2.2) */

/* xn_open_udp's option bits, and the failed step it reports */
#define XN_OPT_INSECURE 1   /* setsockopt(SOL_SOCKET, 0x5801, 1) */
#define XN_OPT_BROADCAST 2  /* setsockopt(SOL_SOCKET, SO_BROADCAST, 1) */
#define XN_OPT_REUSEADDR 4  /* setsockopt(SOL_SOCKET, SO_REUSEADDR, 1) */
enum { XN_STEP_NONE, XN_STEP_SOCKET, XN_STEP_INSECURE, XN_STEP_BROADCAST, XN_STEP_REUSE,
       XN_STEP_RCVBUF, XN_STEP_NBIO, XN_STEP_BIND };

/* once, before anything else: resolves the exports and checks each against the 17559 code.
 * 0 = usable; else XN_E_* (nothing else here may be called then). Any thread. */
int xn_init(void);
/* the address the failed check was about (an export's address, or the ordinal), for status */
u32 xn_init_detail(void);

/* starts the system caller's XNet and/or WSA layer (mode: XN_START_* bits) for one user;
 * *xnet and *wsa get each call's raw return (0 = ok, also when another user had started it;
 * untouched if not called). Returns 0 if everything that was asked for started. Balanced by
 * xn_cleanup_as, which undoes what this user started unless another user still holds it (so
 * team voice closing never stops the bridge's socket, and the other way round). */
#define XN_USER_TEAM 0      /* team_360.c (xn_startup / xn_cleanup) */
#define XN_USER_BRIDGE 1    /* bridge_360.c */
#define XN_USERS 2
u32 xn_startup_as(u32 user, u32 mode, u32 *xnet, u32 *wsa);
void xn_cleanup_as(u32 user);
u32 xn_startup(u32 mode, u32 *xnet, u32 *wsa); /* as XN_USER_TEAM */
void xn_cleanup(void);

/* a non-blocking UDP socket bound to INADDR_ANY:port (host order), with the XN_OPT_* options
 * and a receive buffer of rcvbuf bytes (0 = XAM's default). On failure *step says which call
 * failed (XN_STEP_*), the socket is closed and the raw error is returned. */
u32 xn_open_udp(u16 port, u32 opts, u32 rcvbuf, u32 *sock, u32 *step);
void xn_close(u32 sock);

/* one datagram to ip:port (host order; ip XN_BROADCAST = 255.255.255.255) */
u32 xn_send_to(u32 sock, u32 ip, u16 port, const void *buf, int len);
#define xn_send_broadcast(sock, port, buf, len) xn_send_to((sock), XN_BROADCAST, (port), (buf), (len))

/* the next datagram, if there is one: 0 and *len, *ip, *port (host order); XN_WOULDBLOCK when
 * nothing is queued; any other value is an error */
u32 xn_recv(u32 sock, void *buf, int cap, int *len, u32 *ip, u16 *port);

/* this console's Ethernet MAC and IP (XNetGetTitleXnAddr, system caller); returns XAM's
 * XNET_GET_XNADDR_* flags (0 = still pending, 1 = none) */
u32 xn_xnaddr(u8 mac[6], u32 *ip);
/* XNetGetEthernetLinkStatus: XNET_ETHERNET_LINK_* flags (1 = active) */
u32 xn_link(void);

#endif
