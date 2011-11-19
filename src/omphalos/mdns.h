#ifndef OMPHALOS_MDNS
#define OMPHALOS_MDNS

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

struct l2host;
struct l3host;
struct interface;
struct omphalos_packet;

void handle_mdns_packet(struct omphalos_packet *,const void *,size_t)
			__attribute__ ((nonnull (1,2)));

int tx_mdns_ptr(struct interface *,const char *,int,const void *)
			__attribute__ ((nonnull (1,2,4)));

int mdns_sd_enumerate(int,struct interface *) __attribute__ ((nonnull (2)));
int mdns_sd_probe(int,struct interface *,unsigned,const char *) __attribute__ ((nonnull (2,4)));

#ifdef __cplusplus
}
#endif

#endif
