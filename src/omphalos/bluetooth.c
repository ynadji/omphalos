#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <omphalos/omphalos.h>
#include <omphalos/bluetooth.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>


static struct {
	struct hci_dev_list_req list;
	struct hci_dev_req devlist[HCI_MAX_DEV];
} devreq;

int discover_bluetooth(const omphalos_iface *octx){
	int sd;

	if((sd = socket(AF_BLUETOOTH,SOCK_RAW,BTPROTO_HCI)) < 0){
		if(errno == ENOTSUP){
			octx->diagnostic("No IEEE 802.15 (Bluetooth) support");
			return 0;
		}
		octx->diagnostic("Couldn't get Bluetooth socket (%s?)\n",strerror(errno));
		return -1;
	}
	devreq.list.dev_num = sizeof(devreq.devlist) / sizeof(*devreq.devlist);
	if(ioctl(sd,HCIGETDEVLIST,&devreq)){
		octx->diagnostic("Failure listing IEEE 802.15 (Bluetooth) devices (%s?)",strerror(errno));
		close(sd);
		return -1;
	}
	if(devreq.list.dev_num){
		// FIXME found bluetooth devices
	}
	close(sd);
	return 0;
}
