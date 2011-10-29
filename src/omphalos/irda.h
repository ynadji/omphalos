#ifndef OMPHALOS_IRDA
#define OMPHALOS_IRDA

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/socket.h>
#include <linux/if_ether.h>
#include <linux/rtnetlink.h>

struct omphalos_iface;
struct omphalos_packet;

void handle_irda_packet(const struct omphalos_iface *,struct omphalos_packet *,
			const void *,size_t) __attribute__ ((nonnull (1,2,3)));

#ifdef __cplusplus
}
#endif

#endif