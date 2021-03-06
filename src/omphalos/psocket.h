#ifndef OMPHALOS_PSOCKET
#define OMPHALOS_PSOCKET

#ifdef __cplusplus
extern "C" {
#endif

#include <linux/if_packet.h>

struct interface;
struct tpacket_req;

// Open a packet socket. Requires superuser or network admin capabilities.
int packet_socket(unsigned);

// Returns the size of the map, or 0 if the operation fails (in this case,
// map will be set to MAP_FAILED).
size_t mmap_rx_psocket(int,int,unsigned,void **,struct tpacket_req *);
size_t mmap_tx_psocket(int,int,unsigned,void **,struct tpacket_req *);

int handle_ring_packet(struct interface *,int,void *);

// map and size ought have been returned by mmap_*_psocket().
int unmap_psocket(void *,size_t);

// Calculate the relative address of the next frame, respecting blocks.
static inline
ssize_t inclen(unsigned *idx,const struct tpacket_req *treq){
	ssize_t inc = treq->tp_frame_size; // advance at least this much
	unsigned fperb = treq->tp_block_size / treq->tp_frame_size;

	++*idx;
	if(*idx == treq->tp_frame_nr){
		inc -= fperb * inc;
		if(treq->tp_block_nr > 1){
			inc -= (treq->tp_block_nr - 1) * treq->tp_block_size;
		}
		*idx = 0;
	}else if(*idx % fperb == 0){
		inc += treq->tp_block_size - fperb * inc;
	}
	return inc;
}

#ifdef __cplusplus
}
#endif

#endif
