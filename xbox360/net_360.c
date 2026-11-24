// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * UDP over XAM's NetDll_* exports as the system caller (net_360.h).
 *
 * The 17559 XAM, read offline (research/dumps/xam_815f0000.bin; XEX export table at
 * 0x81A73D34, image base 0x815F0000, 2890 exports from ordinal 1; ordinal 714 resolves to the
 * 0x816C5508 xinput_360.c already uses, so the table is read right):
 *
 *   ord  export (Xenia's name)        17559 address  stub
 *     1  NetDll_WSAStartup            0x81742490     -> 0x81742010 (startup, WSA)
 *     2  NetDll_WSACleanup            0x81742440     -> 0x81742250 (cleanup, WSA)
 *     3  NetDll_socket                0x81741AD8     -> 0x81747878
 *     4  NetDll_closesocket           0x81741AF8
 *     6  NetDll_ioctlsocket           0x81741B28
 *     7  NetDll_setsockopt            0x81741B48     -> 0x81744DD8
 *    11  NetDll_bind                  0x81741BD8
 *    20  NetDll_recvfrom              0x81741D30
 *    24  NetDll_sendto                0x81741E28
 *    27  NetDll_WSAGetLastError       0x8173FE70     (no caller argument)
 *    51  NetDll_XNetStartup           0x81742470     -> 0x81742010 (startup, XNet)
 *    52  NetDll_XNetCleanup           0x81742408     -> 0x81742250 (cleanup, XNet)
 *    73  NetDll_XNetGetTitleXnAddr    0x81741280
 *    75  NetDll_XNetGetEthernetLinkStatus 0x817423D0
 * The argument counts the stubs shift match Xenia's table for every one of these (socket 4,
 * setsockopt 6, sendto / recvfrom 7, select 6, ...).
 *
 * The startup implementation (0x81742010): caller 1..2 only (else WSAEINVAL 10022). The first
 * startup of any caller allocates XAM's network context (0x1C98 bytes, tag "NETD"); a caller
 * whose XNet and WSA counts are both 0 gets its own per-caller state set up (with the
 * XNetStartupParams given, if any); then the caller's XNet count (+0x1B40 + 64 x caller) or
 * WSA count (+0x1B44 + 64 x caller) goes up by one and, for WSA, WSADATA is filled (version,
 * 0x0202 ...). Cleanup takes the same count down. So our counts never touch a title's, and a
 * balanced startup / cleanup never tears down what XAM, xbdm or HaloLAN started.
 *
 * socket (0x81747878): WSANOTINITIALISED unless the caller's WSA count is non-zero (an XNet
 * start alone isn't enough); AF_INET or 0; SOCK_STREAM / SOCK_DGRAM; IPPROTO 0 / 6 / 17 /
 * 254 (VDP). If the caller's state was started with the bypass-security flag, the socket gets
 * flags 0x6000 at once.
 *
 * setsockopt (0x81744DD8), SOL_SOCKET (0xFFFF):
 *   0x5801 (insecure): refused on a VDP socket; for caller 1 (a title) only with executable
 *     privilege 6 (insecure sockets: what DashLaunch's sockpatch grants) or an override in the
 *     context, else WSAEACCES (10013); for caller 2 no check at all. Sets socket flag
 *     0x20000000; turning it back off once set is refused.
 *   SO_BROADCAST (0x20): only on a datagram socket that isn't VDP.
 *   SO_REUSEADDR (4), SO_RCVBUF (0x1002), 0x5802, 0x5803 also handled.
 *
 * HaloLAN.xex (0x91A20000) and xbdm.xex (0x91000000) on the console, read from memory over
 * xbdm: every NetDll call they make passes caller 2. HaloLAN: XNetStartup(2, {13, flags 1
 * (bypass security), 0, 0, 32, 32, 0...}), WSAStartupEx (ord 36, version 2.2), a UDP socket,
 * 0x5801 = 1, SO_RCVBUF 128 KB, bind to a random port from 20000. xbdm: UDP socket, 0x5801,
 * SO_REUSEADDR and SO_BROADCAST, no startup of its own (XAM's system caller is up already).
 * JRPC2.xex imports no NetDll export.
 */
#include <xecore/xboxkrnl.h>

#include "../core/platform.h"
#include "net_360.h"

#define CALLER_SYSAPP 2
#define AF_INET 2
#define SOCK_DGRAM 2
#define IPPROTO_UDP 17
#define SOL_SOCKET 0xFFFF
#define SO_REUSEADDR 0x0004
#define SO_BROADCAST 0x0020
#define SO_RCVBUF 0x1002
#define SO_INSECURE 0x5801
#define FIONBIO 0x8004667E
#ifndef INVALID_SOCKET
#define INVALID_SOCKET 0xFFFFFFFFu
#endif

/* the exports, as XAM declares them (caller first; all 32-bit arguments, at most 7) */
static int (*pWSAStartup)(u32 caller, u16 version, void *wsadata);
static int (*pWSACleanup)(u32 caller);
static u32 (*pSocket)(u32 caller, int af, int type, int proto);
static int (*pClosesocket)(u32 caller, u32 s);
static int (*pIoctlsocket)(u32 caller, u32 s, u32 cmd, u32 *arg);
static int (*pSetsockopt)(u32 caller, u32 s, int level, int name, const void *val, int len);
static int (*pBind)(u32 caller, u32 s, const void *addr, int len);
static int (*pRecvfrom)(u32 caller, u32 s, void *buf, int len, int flags, void *from,
			int *fromlen);
static int (*pSendto)(u32 caller, u32 s, const void *buf, int len, int flags, const void *to,
		      int tolen);
static int (*pWSAGetLastError)(void);
static int (*pXNetStartup)(u32 caller, const void *params);
static int (*pXNetCleanup)(u32 caller);
static u32 (*pXNetGetTitleXnAddr)(u32 caller, void *xnaddr);
static u32 (*pXNetGetEthernetLinkStatus)(u32 caller);

struct xn_export {
	u32 ordinal, addr, code[4];
	void **fn;
};

/* each export's address and first four words, from the 17559 XAM dump */
static const struct xn_export EXPORTS[] = {
	{ 1, 0x81742490, { 0x3d6081ac, 0x7c872378, 0x7ca82b78, 0x388b7c04 }, (void **)&pWSAStartup },
	{ 2, 0x81742440, { 0x3d6081ac, 0x38a00001, 0x388b7c04, 0x4bfffe04 }, (void **)&pWSACleanup },
	{ 3, 0x81741ad8, { 0x3d6081ac, 0x7cc73378, 0x7ca62b78, 0x7c852378 }, (void **)&pSocket },
	{ 4, 0x81741af8, { 0x3d6081ac, 0x7c852378, 0x7c641b78, 0x806b7c04 }, (void **)&pClosesocket },
	{ 6, 0x81741b28, { 0x3d6081ac, 0x7cc73378, 0x7ca62b78, 0x7c852378 }, (void **)&pIoctlsocket },
	{ 7, 0x81741b48, { 0x7d094378, 0x3d6081ac, 0x7ce83b78, 0x7cc73378 }, (void **)&pSetsockopt },
	{ 11, 0x81741bd8, { 0x3d6081ac, 0x7cc73378, 0x7ca62b78, 0x7c852378 }, (void **)&pBind },
	{ 20, 0x81741d30, { 0x7d2a4b78, 0x7d094378, 0x3d6081ac, 0x7ce83b78 }, (void **)&pRecvfrom },
	{ 24, 0x81741e28, { 0x7d2a4b78, 0x7d094378, 0x3d6081ac, 0x7ce83b78 }, (void **)&pSendto },
	{ 27, 0x8173fe70, { 0x48002a68, 0x00000000, 0x48002a68, 0x00000000 }, (void **)&pWSAGetLastError },
	{ 51, 0x81742470, { 0x3d6081ac, 0x7c852378, 0x39200000, 0x388b7c04 }, (void **)&pXNetStartup },
	{ 52, 0x81742408, { 0x3d6081ac, 0x38a00000, 0x388b7c04, 0x4bfffe3c }, (void **)&pXNetCleanup },
	{ 73, 0x81741280, { 0x3d6081ac, 0x806b7c04, 0x2b030000, 0x409a000c }, (void **)&pXNetGetTitleXnAddr },
	{ 75, 0x817423d0, { 0x7d8802a6, 0x9181fff8, 0x9421ffa0, 0x4800618d }, (void **)&pXNetGetEthernetLinkStatus },
};

static int g_init = -1;       /* xn_init's result, once */
static u32 g_init_detail;
static u32 g_started;         /* XN_START_* that succeeded (xn_cleanup undoes them) */
static u8 g_wsadata[400] __attribute__((aligned(8))); /* WSADATA: XAM writes up to +0x18F */

/* XNetStartupParams as HaloLAN passes them: size 13, XNET_STARTUP_BYPASS_SECURITY, default
 * socket counts, 32 KB default receive and send buffers */
static const u8 XNET_PARAMS[13] = { 13, 1, 0, 0, 32, 32 };

struct sockaddr_in360 { /* big-endian, so host order is network order */
	u16 family, port;
	u32 addr;
	u8 zero[8];
};

int xn_init(void)
{
	HANDLE xam;

	if (g_init >= 0)
		return g_init;
	if (XexGetModuleHandle("xam.xex", &xam) < 0 || !xam) {
		g_init = XN_E_NO_XAM;
		return g_init;
	}
	for (u32 i = 0; i < ARRAY_SIZE(EXPORTS); i++) {
		const struct xn_export *e = &EXPORTS[i];
		void *p = 0;

		XexGetProcedureAddress(xam, e->ordinal, &p);
		if (!p) {
			g_init_detail = e->ordinal;
			g_init = XN_E_ORDINAL;
			return g_init;
		}
		if ((u32)p != e->addr || memcmp(p, e->code, sizeof(e->code))) {
			g_init_detail = (u32)p;
			g_init = XN_E_CODE;
			xa_log("net: xam ordinal %u at %08x is not the 17559 code read offline: no "
			       "network\n", e->ordinal, (u32)p);
			return g_init;
		}
		*e->fn = p;
	}
	g_init = 0;
	return 0;
}

u32 xn_init_detail(void)
{
	return g_init_detail;
}

static u32 last_error(void)
{
	int e = pWSAGetLastError();

	return e ? (u32)e : 0xFFFFFFFFu; /* failed without an error code: still a failure */
}

u32 xn_startup(u32 mode, u32 *xnet, u32 *wsa)
{
	u32 err = 0;

	if (g_init)
		return XN_ENOTINIT;
	if ((mode & XN_START_XNET) && !(g_started & XN_START_XNET)) {
		int r = pXNetStartup(CALLER_SYSAPP, XNET_PARAMS);

		*xnet = (u32)r;
		if (r == 0)
			g_started |= XN_START_XNET;
		else
			err = (u32)r;
	}
	if ((mode & XN_START_WSA) && !(g_started & XN_START_WSA)) {
		int r = pWSAStartup(CALLER_SYSAPP, 0x0202, g_wsadata);

		*wsa = (u32)r;
		if (r == 0)
			g_started |= XN_START_WSA;
		else if (!err)
			err = (u32)r;
	}
	return err;
}

void xn_cleanup(void)
{
	if (g_init)
		return;
	if (g_started & XN_START_WSA)
		pWSACleanup(CALLER_SYSAPP);
	if (g_started & XN_START_XNET)
		pXNetCleanup(CALLER_SYSAPP);
	g_started = 0;
}

static int set_int(u32 s, int name, u32 value)
{
	return pSetsockopt(CALLER_SYSAPP, s, SOL_SOCKET, name, &value, 4);
}

u32 xn_open_udp(u16 port, u32 opts, u32 rcvbuf, u32 *sock, u32 *step)
{
	struct sockaddr_in360 a = { AF_INET, port, 0, { 0 } };
	u32 s, one = 1, err;

	*sock = INVALID_SOCKET;
	*step = XN_STEP_NONE;
	if (g_init)
		return XN_ENOTINIT;
	s = pSocket(CALLER_SYSAPP, AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) {
		*step = XN_STEP_SOCKET;
		return last_error();
	}
	/* in HaloLAN's and xbdm's order: insecure first, then the rest */
	if ((opts & XN_OPT_INSECURE) && set_int(s, SO_INSECURE, 1)) {
		*step = XN_STEP_INSECURE;
		goto fail;
	}
	if ((opts & XN_OPT_REUSEADDR) && set_int(s, SO_REUSEADDR, 1)) {
		*step = XN_STEP_REUSE;
		goto fail;
	}
	if ((opts & XN_OPT_BROADCAST) && set_int(s, SO_BROADCAST, 1)) {
		*step = XN_STEP_BROADCAST;
		goto fail;
	}
	if (rcvbuf && set_int(s, SO_RCVBUF, rcvbuf)) {
		*step = XN_STEP_RCVBUF;
		goto fail;
	}
	if (pIoctlsocket(CALLER_SYSAPP, s, FIONBIO, &one)) {
		*step = XN_STEP_NBIO;
		goto fail;
	}
	if (pBind(CALLER_SYSAPP, s, &a, sizeof(a))) {
		*step = XN_STEP_BIND;
		goto fail;
	}
	*sock = s;
	return 0;
fail:
	err = last_error();
	pClosesocket(CALLER_SYSAPP, s);
	return err;
}

void xn_close(u32 sock)
{
	if (!g_init && sock != INVALID_SOCKET)
		pClosesocket(CALLER_SYSAPP, sock);
}

u32 xn_send_to(u32 sock, u32 ip, u16 port, const void *buf, int len)
{
	struct sockaddr_in360 a = { AF_INET, port, ip, { 0 } };
	int n;

	if (g_init)
		return XN_ENOTINIT;
	n = pSendto(CALLER_SYSAPP, sock, buf, len, 0, &a, sizeof(a));
	if (n == len)
		return 0;
	return n < 0 ? last_error() : 0xFFFFFFFEu; /* short write: can't happen for UDP */
}

u32 xn_recv(u32 sock, void *buf, int cap, int *len, u32 *ip, u16 *port)
{
	struct sockaddr_in360 a = { 0, 0, 0, { 0 } };
	int alen = sizeof(a), n;

	if (g_init)
		return XN_ENOTINIT;
	n = pRecvfrom(CALLER_SYSAPP, sock, buf, cap, 0, &a, &alen);
	if (n < 0)
		return last_error();
	*len = n;
	*ip = a.addr;
	*port = a.port;
	return 0;
}

u32 xn_xnaddr(u8 mac[6], u32 *ip)
{
	/* XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20] */
	u8 x[36] __attribute__((aligned(4)));
	u32 r;

	memset(x, 0, sizeof(x));
	if (g_init)
		return 1;
	r = pXNetGetTitleXnAddr(CALLER_SYSAPP, x);
	memcpy(ip, x, 4);
	memcpy(mac, x + 10, 6);
	return r;
}

u32 xn_link(void)
{
	return g_init ? 0 : pXNetGetEthernetLinkStatus(CALLER_SYSAPP);
}
