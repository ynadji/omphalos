#include <assert.h>
#include <sys/socket.h>
#include <omphalos/tx.h>
#include <linux/version.h>
#include <omphalos/route.h>
#include <linux/rtnetlink.h>
#include <omphalos/netaddrs.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>

// FIXME need locking on all of this!!!

// FIXME use a multibit trie rather than ultra-lame linked list yeargh
// see varghese's 'networking algorithmics' sec 11.8
typedef struct route {
	interface *iface;
	sa_family_t family;
	struct sockaddr_storage sss,ssd,ssg;
	unsigned maskbits;
	struct route *next;
} route;

static route *ip_table4,*ip_table6;

// To do longest-match routing, we need to keep:
//  (a) a partition of the address space, pointing to the longest match for
//       each distinct section. each node stores 1 or more (in the case of
//       equal-length multipath) routes, and a pointer to:
//  (b) the next-longest match.
//
// A route may be pointed at by more than one node (the default routes will be
// eventually pointed to by any and all nodes save their own), but does not
// point to more than one node.
static route *
create_route(void){
	route *r;

	if( (r = malloc(sizeof(*r))) ){
		memset(r,0,sizeof(*r));
	}
	return r;
}

static void
free_route(route *r){
	free(r);
}

int handle_rtm_delroute(const omphalos_iface *octx,const struct nlmsghdr *nl){
	const struct rtmsg *rt = NLMSG_DATA(nl);
	struct rtattr *ra;
	int rlen;

	rlen = nl->nlmsg_len - NLMSG_LENGTH(sizeof(*rt));
	ra = (struct rtattr *)((char *)(NLMSG_DATA(nl)) + sizeof(*rt));
	while(RTA_OK(ra,rlen)){
		switch(ra->rta_type){
		case RTA_DST:{
		break;}case RTA_SRC:{
		break;}case RTA_IIF:{
		break;}case RTA_OIF:{
		break;}default:{
			octx->diagnostic("Unknown rtatype %u",ra->rta_type);
			break;
		break;}}
		ra = RTA_NEXT(ra,rlen);
	}
	if(rlen){
		octx->diagnostic("%d excess bytes on newlink message",rlen);
	}
	return 0;
}

int handle_rtm_newroute(const omphalos_iface *octx,const struct nlmsghdr *nl){
	const struct rtmsg *rt = NLMSG_DATA(nl);
	void *as,*ad,*ag,*pag,*pas;
	struct rtattr *ra;
	int rlen,iif,oif;
	size_t flen;
	route *r;

	iif = oif = -1;
	pas = pag = NULL; // pointers set only once as/ag are copied into
	if((r = create_route()) == NULL){
		return -1;
	}
	switch( (r->family = rt->rtm_family) ){
	case AF_INET:{
		flen = sizeof(uint32_t);
		as = &((struct sockaddr_in *)&r->sss)->sin_addr;
		ad = &((struct sockaddr_in *)&r->ssd)->sin_addr;
		ag = &((struct sockaddr_in *)&r->ssg)->sin_addr;
	break;}case AF_INET6:{
		flen = sizeof(uint32_t) * 4;
		as = &((struct sockaddr_in6 *)&r->sss)->sin6_addr;
		ad = &((struct sockaddr_in6 *)&r->ssd)->sin6_addr;
		ag = &((struct sockaddr_in6 *)&r->ssg)->sin6_addr;
	break;}default:{
		flen = 0;
	break;} }
	r->maskbits = rt->rtm_dst_len;
	if(flen == 0 || flen > sizeof(r->sss.__ss_padding)){
		octx->diagnostic("Unknown route family %u",rt->rtm_family);
		return -1;
	}
	memset(ag,0,flen);
	rlen = nl->nlmsg_len - NLMSG_LENGTH(sizeof(*rt));
	ra = (struct rtattr *)((char *)(NLMSG_DATA(nl)) + sizeof(*rt));
	while(RTA_OK(ra,rlen)){
		switch(ra->rta_type){
		case RTA_DST:{
			if(RTA_PAYLOAD(ra) != flen){
				octx->diagnostic("Expected %zu src bytes, got %lu",
						flen,RTA_PAYLOAD(ra));
				break;
			}
			memcpy(ad,RTA_DATA(ra),flen);
		break;}case RTA_SRC:{
			if(RTA_PAYLOAD(ra) != flen){
				octx->diagnostic("Expected %zu src bytes, got %lu",
						flen,RTA_PAYLOAD(ra));
				break;
			}
			memcpy(as,RTA_DATA(ra),flen);
			pas = as;
		break;}case RTA_IIF:{
			if(RTA_PAYLOAD(ra) != sizeof(int)){
				octx->diagnostic("Expected %zu iface bytes, got %lu",
						sizeof(int),RTA_PAYLOAD(ra));
				break;
			}
			iif = *(int *)RTA_DATA(ra);
		break;}case RTA_OIF:{
			if(RTA_PAYLOAD(ra) != sizeof(int)){
				octx->diagnostic("Expected %zu iface bytes, got %lu",
						sizeof(int),RTA_PAYLOAD(ra));
				break;
			}
			oif = *(int *)RTA_DATA(ra);
		break;}case RTA_GATEWAY:{
			if(RTA_PAYLOAD(ra) != flen){
				octx->diagnostic("Expected %zu gw bytes, got %lu",
						flen,RTA_PAYLOAD(ra));
				break;
			}
			// We get 0.0.0.0 as the gateway when there's no 'via'
			if(memcmp(ag,RTA_DATA(ra),flen)){
				memcpy(ag,RTA_DATA(ra),flen);
				pag = ag;
			}
		break;}case RTA_PRIORITY:{
		break;}case RTA_PREFSRC:{
			if(RTA_PAYLOAD(ra) != flen){
				octx->diagnostic("Expected %zu src bytes, got %lu",
						flen,RTA_PAYLOAD(ra));
				break;
			}
			memcpy(as,RTA_DATA(ra),flen);
			pas = as;
		break;}case RTA_METRICS:{
		break;}case RTA_MULTIPATH:{
		// break;}case RTA_PROTOINFO:{ // unused
		break;}case RTA_FLOW:{
		break;}case RTA_CACHEINFO:{
		// break;}case RTA_SESSION:{ // unused
		// break;}case RTA_MP_ALGO:{ // unused
		break;}case RTA_TABLE:{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
		break;}case RTA_MARK:{
#endif
		break;}default:{
			octx->diagnostic("Unknown rtatype %u",ra->rta_type);
		break;}}
		ra = RTA_NEXT(ra,rlen);
	}
	if(rlen){
		octx->diagnostic("%d excess bytes on newlink message",rlen);
	}
	if(oif > -1){
		if((r->iface = iface_by_idx(oif)) == NULL){
			octx->diagnostic("Unknown output interface %d on %s",oif,r->iface->name);
			goto err;
		}
	}else{
		// blackhole routes typically have no output interface
		return 0;
	}
	if(r->family == AF_INET){
		if(add_route4(r->iface,ad,pag,pas,r->maskbits,iif)){
			octx->diagnostic("Couldn't add route to %s",r->iface->name);
			return -1;
		}
		r->next = ip_table4;
		ip_table4 = r;
	}else if(r->family == AF_INET6){
		if(add_route6(r->iface,ad,pag,pas,r->maskbits,iif)){
			octx->diagnostic("Couldn't add route to %s",r->iface->name);
			return -1;
		}
		r->next = ip_table6;
		ip_table6 = r;
	}
	// FIXME need a route callback in octx
	/*{
		char str[INET6_ADDRSTRLEN];
		inet_ntop(rt->rtm_family,ad,str,sizeof(str));
		octx->diagnostic("[%8s] route to %s/%u %s",r->iface->name,str,r->maskbits,
			rt->rtm_type == RTN_LOCAL ? "(local)" :
			rt->rtm_type == RTN_BROADCAST ? "(broadcast)" :
			rt->rtm_type == RTN_UNREACHABLE ? "(unreachable)" :
			rt->rtm_type == RTN_ANYCAST ? "(anycast)" :
			rt->rtm_type == RTN_UNICAST ? "(unicast)" :
			rt->rtm_type == RTN_MULTICAST ? "(multicast)" :
			rt->rtm_type == RTN_BLACKHOLE ? "(blackhole)" :
			rt->rtm_type == RTN_MULTICAST ? "(multicast)" :
			"");
	}*/
	return 0;

err:
	free_route(r);
	return -1;
}

// Determine how to send a packet to a layer 3 address.
// FIXME this whole functions is just incredibly godawful
int get_router(int fam,const void *addr,struct routepath *rp){
	uint128_t maskaddr;
	size_t len;
	route *rt;

	// FIXME we will want an actual cross-interface routing table rather
	// than iterating over all interfaces, eek
	if(fam == AF_INET){
		rt = ip_table4;
		len = 4;
		maskaddr[0] = *(const uint32_t *)addr;
	}else if(fam == AF_INET6){
		rt = ip_table6;
		len = 16;
		maskaddr = *(const uint128_t *)addr;
	}else{
		return -1;
	}
	while(rt){
		uint128_t tmp; // FIXME so lame
		unsigned z;

		for(z = 0 ; z < len / 4 ; ++z){
			if(rt->maskbits > 32 * z){
				if(rt->maskbits >= 32 * (z + 1)){
					tmp[z] = maskaddr[z];
				}else{
					uint32_t mask = ~0U << (32 - rt->maskbits % 32);

					tmp[z] = maskaddr[z] & mask;
				}
			}else{
				tmp[z] = 0;
			}
		}
		if(memcmp(&rt->ssd,&tmp,len) == 0){
			break;
		}
		rt = rt->next;
	}
	if(rt == NULL){
		return -1;
	}
	rp->i = rt->iface;
	// FIXME we have the interface. find the l3
	rp->l3 = NULL;
	rp->l2 = l3_getlastl2(rp->l3);
	assert(rp->i && rp->l2 && rp->l3); // FIXME
	return 0;
}

// Call get_router() on the address, acquire a TX frame from the discovered
// interface, Initialize it with proper L2 and L3 data.
int get_routed_frame(const omphalos_iface *octx,int fam,const void *addr,
			struct routepath *rp,void **frame,size_t *flen,size_t *hlen){
	if(get_router(fam,addr,rp)){
		return -1;
	}
	if((*frame = get_tx_frame(octx,rp->i,flen)) == NULL){
		return -1;
	}
	assert(hlen); // FIXME set up the l2/l3 headers
	return -1;
}

void free_routes(void){
	route *rt;

	while( (rt = ip_table4) ){
		ip_table4 = rt->next;
		free_route(rt);
	}
	while( (rt = ip_table6) ){
		ip_table6 = rt->next;
		free_route(rt);
	}
}
