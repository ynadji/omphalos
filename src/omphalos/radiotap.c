#include <omphalos/radiotap.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>
//#include <net/ieee80211_radiotap.h>

void handle_radiotap_packet(const omphalos_iface *octx,interface *iface,
				const void *frame,size_t len){
	octx->diagnostic("FIXME radiotap %p/%zu (%s)\n",frame,len,iface->name);
	if(octx->packet_read){
		octx->packet_read(iface,iface->opaque);
	}
}