#include <errno.h>
#include <ctype.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <linux/if.h>

// The wireless extensions headers are not so fantastic. This workaround comes
// to us courtesy of Jean II in iwlib.h. Ugh.
#ifndef __user
#define __user
#endif
#include <asm/types.h>
#include <wireless.h>

#include <sys/utsname.h>
#include <linux/version.h>
#include <linux/nl80211.h>
#include <ncursesw/panel.h>
#include <ncursesw/ncurses.h>
#include <omphalos/ethtool.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>
#include <gnu/libc-version.h>

#define PROGNAME "omphalos"	// FIXME
#define VERSION  "0.98-pre"	// FIXME

#define PAD_LINES 3
#define PAD_COLS (COLS - START_COL * 2)
#define START_LINE 2
#define START_COL 1

// FIXME we ought precreate the subwindows, and show/hide them rather than
// creating and destroying them every time.
struct panel_state {
	PANEL *p;
	WINDOW *w;
	int ysize;			// number of lines of *text* (not win)
	int xsize;			// maximum cols of *text* (not win)
};

#define PANEL_STATE_INITIALIZER { .p = NULL, .w = NULL, .ysize = -1, .xsize = -1, }

static struct panel_state *active;
static struct panel_state help = PANEL_STATE_INITIALIZER;
static struct panel_state details = PANEL_STATE_INITIALIZER;

// Bind one of these state structures to each interface in the callback,
// and also associate an iface with them via ifacenum (for UI actions).
typedef struct iface_state {
	int ifacenum;			// iface number
	int scrline;			// line within the containing pad
	int ysize;			// number of lines
	WINDOW *subwin;			// subwin
	PANEL *panel;			// panel
	const char *typestr;		// looked up using iface->arptype
	struct timeval lastprinted;	// last time we printed the iface
	int alarmset;			// alarm set for UI update?
	int devaction;			// 1 == down, -1 == up, 0 == nothing
	struct panel_state *detailwin;	// non-NULL if we own the detail window
	struct iface_state *next,*prev;
} iface_state;

enum {
	BORDER_COLOR = 1,		// main window
	HEADING_COLOR,
	DBORDER_COLOR,			// down interfaces
	DHEADING_COLOR,
	UBORDER_COLOR,			// up interfaces
	UHEADING_COLOR,
	PBORDER_COLOR,			// popups
	PHEADING_COLOR,
	BULKTEXT_COLOR,			// bulk text (help)
};

// FIXME granularize things, make packet handler iret-like
static pthread_mutex_t bfl = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static WINDOW *pad;
static pthread_t inputtid;
static struct utsname sysuts;
static unsigned count_interface;
static iface_state *current_iface;
static const char *glibc_version,*glibc_release;

// Status bar at the bottom of the screen. Must be reallocated upon screen
// resize and allocated based on initial screen at startup. Don't shrink
// it; widening the window again should show the full message.
static char *statusmsg;
static int statuschars;	// True size, not necessarily what's available

#define ANSITERM_COLS 80

static inline int
start_screen_update(void){
	int ret = OK;

	update_panels();
	return ret;
}

static inline int
finish_screen_update(void){
	if(doupdate() == ERR){
		return ERR;
	}
	return OK;
}

// Pass current number of columns
static int
setup_statusbar(int cols){
	if(cols < 0){
		return -1;
	}else if(cols < ANSITERM_COLS){
		cols = ANSITERM_COLS;
	}
	if(statuschars <= cols){
		const size_t s = cols + 1;
		char *sm;

		if((sm = realloc(statusmsg,s)) == NULL){
			return -1;
		}
		statuschars = s;
		if(statusmsg == NULL){
			time_t t = time(NULL);
			struct tm tm;

			if(localtime_r(&t,&tm)){
				strftime(sm,s,"launched at %T",&tm);
			}else{
				sm[0] = '\0';
			}
		}
		statusmsg = sm;
	}
	return 0;
}

static inline int
interface_sniffing_p(const interface *i){
	return (i->rfd >= 0);
}

static inline int
interface_up_p(const interface *i){
	return (i->flags & IFF_UP);
}

static inline int
interface_carrier_p(const interface *i){
	return (i->flags & IFF_LOWER_UP);
}

static inline int
interface_promisc_p(const interface *i){
	return (i->flags & IFF_PROMISC);
}

static int
iface_optstr(WINDOW *w,const char *str,int hcolor,int bcolor){
	if(wcolor_set(w,bcolor,NULL) != OK){
		return ERR;
	}
	if(waddch(w,'|') == ERR){
		return ERR;
	}
	if(wcolor_set(w,hcolor,NULL) != OK){
		return ERR;
	}
	if(waddstr(w,str) == ERR){
		return ERR;
	}
	return OK;
}

static const char *
duplexstr(unsigned dplx){
	switch(dplx){
		case DUPLEX_FULL: return "full"; break;
		case DUPLEX_HALF: return "half"; break;
		default: break;
	}
	return "";
}

static const char *
modestr(unsigned dplx){
	switch(dplx){
		case NL80211_IFTYPE_UNSPECIFIED: return "auto"; break;
		case NL80211_IFTYPE_ADHOC: return "adhoc"; break;
		case NL80211_IFTYPE_STATION: return "managed"; break;
		case NL80211_IFTYPE_AP: return "ap"; break;
		case NL80211_IFTYPE_AP_VLAN: return "apvlan"; break;
		case NL80211_IFTYPE_WDS: return "wds"; break;
		case NL80211_IFTYPE_MONITOR: return "monitor"; break;
		case NL80211_IFTYPE_MESH_POINT: return "mesh"; break;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,38)
		case NL80211_IFTYPE_P2P_CLIENT: return "p2pclient"; break;
		case NL80211_IFTYPE_P2P_GO: return "p2pgo"; break;
#endif
		default: break;
	}
	return "";
}

// For full safety, pass in a buffer that can hold the decimal representation
// of the largest uintmax_t plus three (one for the unit, one for the
// decimal separator, and one for the NUL byte). 'decimal' serves as a
// primitive floating point; '1' indicates no scaling (this is useful when val
// might be less than 1000, so that we can display the two digits of precision
// (if val >= 1000, the only displayed precision comes from the prefixing, so
// decimal is meaningless in that case). If omitdec is non-zero, and the decimal
// portion is all 0's, the decimal portion will not be printed.
static char *
rate(uintmax_t val,uintmax_t decimal,char *buf,size_t bsize,int omitdec){
	const char prefixes[] = "KMGTPEY";
	unsigned consumed = 0;
	uintmax_t div;

	div = 1000;
	while((val / decimal) > div && consumed < strlen(prefixes)){
		div *= 1000;
		if(UINTMAX_MAX / div < 1000){ // watch for overflow
			break;
		}
		++consumed;
	}
	if(div != 1000){
		div /= 1000;
		val /= decimal;
		if(val % div || omitdec == 0){
			snprintf(buf,bsize,"%ju.%02ju%c",val / div,(val % div) / ((div + 99) / 100),
					prefixes[consumed - 1]);
		}else{
			snprintf(buf,bsize,"%ju%c",val / div,prefixes[consumed - 1]);
		}
	}else{
		snprintf(buf,bsize,"%ju.%02ju",val / decimal,val % decimal);
	}
	return buf;
}

#define ERREXIT endwin() ; fprintf(stderr,"ncurses failure|%s|%d\n",__func__,__LINE__); abort() ; goto err
// to be called only while ncurses lock is held
static int
iface_box(WINDOW *w,const interface *i,const iface_state *is){
	int bcolor,hcolor,rows,cols;
	size_t buslen;
	int attrs;

	getmaxyx(w,rows,cols);
	assert(cols >= 0);	// for 'set but unused' warning on cols
	// FIXME shouldn't have to know IFF_UP out here
	bcolor = interface_up_p(i) ? UBORDER_COLOR : DBORDER_COLOR;
	hcolor = interface_up_p(i) ? UHEADING_COLOR : DHEADING_COLOR;
	attrs = ((is == current_iface) ? A_REVERSE : 0) | A_BOLD;
	assert(wattron(w,attrs | COLOR_PAIR(bcolor)) == OK);
	assert(box(w,0,0) == OK);
	assert(wattroff(w,A_REVERSE) == OK);
	assert(mvwprintw(w,0,START_COL,"[") != ERR);
	assert(wcolor_set(w,hcolor,NULL) == OK);
	assert(waddstr(w,i->name) != ERR);
	assert(wprintw(w," (%s",is->typestr) != ERR);
	if(strlen(i->drv.driver)){
		assert(waddch(w,' ') != ERR);
		assert(waddstr(w,i->drv.driver) != ERR);
		if(strlen(i->drv.version)){
			assert(wprintw(w," %s",i->drv.version) != ERR);
		}
		if(strlen(i->drv.fw_version)){
			assert(wprintw(w," fw %s",i->drv.fw_version) != ERR);
		}
	}
	assert(waddch(w,')') != ERR);
	assert(wcolor_set(w,bcolor,NULL) != ERR);
	assert(wprintw(w,"]") != ERR);
	assert(wattron(w,attrs) != ERR);
	assert(wattroff(w,A_REVERSE) != ERR);
	assert(mvwprintw(w,rows - 1,START_COL * 2,"[") != ERR);
	assert(wcolor_set(w,hcolor,NULL) != ERR);
	assert(wprintw(w,"mtu %d",i->mtu) != ERR);
	if(interface_up_p(i)){
		char buf[80]; // FIXME

		assert(iface_optstr(w,"up",hcolor,bcolor) != ERR);
		if(!interface_carrier_p(i)){
			assert(waddstr(w," (no carrier)") != ERR);
		}else if(i->settings_valid == SETTINGS_VALID_ETHTOOL){
			assert(wprintw(w," (%sb %s)",rate(i->settings.ethtool.speed * 1000000u,1,buf,sizeof(buf),1),
						duplexstr(i->settings.ethtool.duplex)) != ERR);
		}else if(i->settings_valid == SETTINGS_VALID_WEXT){
			assert(wprintw(w," (%sb %s)",rate(i->settings.wext.bitrate,1,buf,sizeof(buf),1),modestr(i->settings.wext.mode)) != ERR);
		}
	}else{
		assert(iface_optstr(w,"down",hcolor,bcolor) != ERR);
		// FIXME find out whether carrier is meaningful for down
		// interfaces (i've never seen a down interface with carrier)
	}
	if(interface_promisc_p(i)){
		assert(iface_optstr(w,"promisc",hcolor,bcolor) != ERR);
	}
	assert(wcolor_set(w,bcolor,NULL) != ERR);
	assert(wprintw(w,"]") != ERR);
	assert(wattroff(w,A_BOLD) != ERR);
	if( (buslen = strlen(i->drv.bus_info)) ){
		if(i->busname){
			buslen += strlen(i->busname) + 1;
			assert(mvwprintw(w,rows - 1,COLS - (buslen + 3 + START_COL),
					"%s:%s",i->busname,i->drv.bus_info) != ERR);
		}else{
			assert(mvwprintw(w,rows - 1,COLS - (buslen + 3 + START_COL),
					"%s",i->drv.bus_info) != ERR);
		}
	}
	assert(wcolor_set(w,0,NULL) != ERR);
	assert(wattroff(w,attrs) != ERR);
	return 0;
}

// to be called only while ncurses lock is held
static int
draw_main_window(WINDOW *w,const char *name,const char *ver){
	int rows,cols;

	getmaxyx(w,rows,cols);
	if(setup_statusbar(cols)){
		ERREXIT;
	}
	if(wcolor_set(w,BORDER_COLOR,NULL) != OK){
		ERREXIT;
	}
	if(box(w,0,0) != OK){
		ERREXIT;
	}
	if(mvwprintw(w,0,2,"[") < 0){
		ERREXIT;
	}
	if(wattron(w,A_BOLD | COLOR_PAIR(HEADING_COLOR)) != OK){
		ERREXIT;
	}
	if(wprintw(w,"%s %s on %s %s (libc %s-%s)",name,ver,sysuts.sysname,
				sysuts.release,glibc_version,glibc_release) < 0){
		ERREXIT;
	}
	if(wattroff(w,A_BOLD | COLOR_PAIR(HEADING_COLOR)) != OK){
		ERREXIT;
	}
	if(wcolor_set(w,BORDER_COLOR,NULL) != OK){
		ERREXIT;
	}
	if(wprintw(w,"]") < 0){
		ERREXIT;
	}
	if(wattron(w,A_BOLD | COLOR_PAIR(HEADING_COLOR)) != OK){
		ERREXIT;
	}
	// addstr() doesn't interpret format strings, so this is safe. It will
	// fail, however, if the string can't fit on the window, which will for
	// instance happen if there's an embedded newline.
	mvwaddstr(w,rows - 1,START_COL * 2,statusmsg);
	if(wattroff(w,A_BOLD | COLOR_PAIR(BORDER_COLOR)) != OK){
		ERREXIT;
	}
	if(wcolor_set(w,0,NULL) != OK){
		ERREXIT;
	}
	if(start_screen_update() == ERR){
		ERREXIT;
	}
	if(finish_screen_update() == ERR){
		ERREXIT;
	}
	return 0;

err:
	return -1;
}

static int
wvstatus_locked(WINDOW *w,const char *fmt,va_list va){
	if(fmt == NULL){
		statusmsg[0] = '\0';
	}else{
		vsnprintf(statusmsg,statuschars,fmt,va);
	}
	return draw_main_window(w,PROGNAME,VERSION);
}

// NULL fmt clears the status bar
static int
wvstatus(WINDOW *w,const char *fmt,va_list va){
	int ret;

	pthread_mutex_lock(&bfl);
	ret = wvstatus_locked(w,fmt,va);
	pthread_mutex_unlock(&bfl);
	return ret;
}

// NULL fmt clears the status bar
static int
wstatus_locked(WINDOW *w,const char *fmt,...){
	va_list va;
	int ret;

	va_start(va,fmt);
	ret = wvstatus_locked(w,fmt,va);
	va_end(va);
	return ret;
}

// NULL fmt clears the status bar
static int
wstatus(WINDOW *w,const char *fmt,...){
	va_list va;
	int ret;

	va_start(va,fmt);
	ret = wvstatus(w,fmt,va);
	va_end(va);
	return ret;
}

static const interface *
get_current_iface(void){
	if(current_iface){
		return iface_by_idx(current_iface->ifacenum);
	}
	return NULL;
}

static void
toggle_promisc_locked(const omphalos_iface *octx,WINDOW *w){
	const interface *i = get_current_iface();

	if(i){
		if(interface_promisc_p(i)){
			wstatus_locked(w,"Disabling promiscuity on %s",i->name);
			disable_promiscuity(octx,i);
		}else{
			wstatus_locked(w,"Enabling promiscuity on %s",i->name);
			enable_promiscuity(octx,i);
		}
	}
}

static void
sniff_interface_locked(const omphalos_iface *octx,WINDOW *w){
	const interface *i = get_current_iface();

	if(i){
		if(!interface_sniffing_p(i)){
			if(!interface_up_p(i)){
				wstatus_locked(w,"Bringing up %s...",i->name);
				up_interface(octx,i);
			}
		}else{
			// FIXME send request to stop sniffing
		}
	}
}

static void
down_interface_locked(const omphalos_iface *octx,WINDOW *w){
	const interface *i = get_current_iface();

	if(i){
		if(interface_up_p(i)){
			wstatus_locked(w,"Bringing down %s...",i->name);
			current_iface->devaction = 1;
			down_interface(octx,i);
		}
	}
}

static void
transfer_details_window(iface_state *from,iface_state *to){
	if(from->detailwin){
		to->detailwin = from->detailwin;
		from->detailwin = NULL;
	}
}

static void
hide_panel_locked(WINDOW *w,struct panel_state *ps){
	if(ps){
		hide_panel(ps->p);
		del_panel(ps->p);
		ps->p = NULL;
		delwin(ps->w);
		ps->w = NULL;
		ps->xsize = ps->ysize = -1;
		start_screen_update();
		draw_main_window(w,PROGNAME,VERSION);
		finish_screen_update();
	}
}

// Can leak resources on failure -- caller must free window/panel on error
static int
new_display_panel(struct panel_state *ps,int rows,int cols,int srow,int scol,
						const wchar_t *hstr){
	const wchar_t crightstr[] = L"copyright © 2011 nick black";
	const int crightlen = wcslen(crightstr);

	if(cols < crightlen + START_COL * 2){
		return ERR;
	}
	assert((ps->w = newwin(rows,cols,srow,scol)) != NULL);
	assert((ps->p = new_panel(ps->w)) != NULL);
	assert(wattron(ps->w,A_BOLD) != ERR);
	assert(wcolor_set(ps->w,PBORDER_COLOR,NULL) == OK);
	assert(box(ps->w,0,0) == OK);
	assert(wattroff(ps->w,A_BOLD) != ERR);
	assert(wcolor_set(ps->w,PHEADING_COLOR,NULL) == OK);
	assert(mvwaddwstr(ps->w,0,START_COL * 2,hstr) != ERR);
	assert(mvwaddwstr(ps->w,rows - 1,cols - (crightlen + START_COL * 2),crightstr) != ERR);
	assert(wcolor_set(ps->w,BULKTEXT_COLOR,NULL) == OK);
	return OK;
}

static int
offload_details(WINDOW *w,const interface *i,int row,int col,const char *name,
						unsigned val){
	int r;

	r = iface_offloaded_p(i,val);
	return mvwprintw(w,row,col,"%s: %c",name,r > 0 ? 'y' : 'n');
}

#define DETAILROWS 6

// FIXME need to support scrolling through the output
static int
iface_details(WINDOW *hw,const interface *i,int row,int col,int rows,int cols){
	int z;

	if((z = rows - 1) > DETAILROWS){
		z = DETAILROWS;
	}
	switch(z){ // Intentional fallthroughs all the way to 0
	case DETAILROWS:{
		// FIXME: display percentage of truncations that were recovered
		assert(mvwprintw(hw,row + z,col,"mform: %-15ju\tnoprot: %-15ju\ttruncs: %-15ju",
					i->malformed,i->noprotocol,i->truncated) != ERR);
		--z;
	}case 4:{
		assert(mvwprintw(hw,row + z,col,"bytes: %-15ju\tframes: %-15ju\tdrops: %-15ju",
					i->bytes,i->frames,i->drops) != ERR);
		--z;
	}case 3:{
		assert(offload_details(hw,i,row + z,col,"TSO",TCP_SEG_OFFLOAD) != ERR);
		assert(offload_details(hw,i,row + z,col + 7,"S/G",ETH_SCATTER_GATHER) != ERR);
		assert(offload_details(hw,i,row + z,col + 15,"UTO",UDP_LARGETX_OFFLOAD) != ERR);
		assert(offload_details(hw,i,row + z,col + 23,"GSO",GEN_SEG_OFFLOAD) != ERR);
		assert(offload_details(hw,i,row + z,col + 31,"GRO",GEN_LARGERX_OFFLOAD) != ERR);
		assert(offload_details(hw,i,row + z,col + 39,"TCsm",TX_CSUM_OFFLOAD) != ERR);
		assert(offload_details(hw,i,row + z,col + 47,"RCsm",RX_CSUM_OFFLOAD) != ERR);
		--z;
	}case 2:{
		assert(mvwprintw(hw,row + z,col,"TXfd: %d\tfsize: %u\tfnum: %-6u\tbsize: %u\tbnum: %u",
					i->fd,i->ttpr.tp_frame_size,i->ttpr.tp_frame_nr,
					i->ttpr.tp_block_size,i->ttpr.tp_block_nr) != ERR);
		--z;
	}case 1:{
		assert(mvwprintw(hw,row + z,col,"RXfd: %d\tfsize: %u\tfnum: %-6u\tbsize: %u\tbnum: %u",
					i->rfd,i->rtpr.tp_frame_size,i->rtpr.tp_frame_nr,
					i->rtpr.tp_block_size,i->rtpr.tp_block_nr) != ERR);
		--z;
	}case 0:{
		char *mac;

		if((mac = hwaddrstr(i)) == NULL){
			return ERR;
		}
		assert(mvwprintw(hw,row + z,col,"%s\t%s\ttxr: %-10zu\trxr: %-10zu\tmtu: %d",
					i->name,mac,i->ts,i->rs,i->mtu) != ERR);
		free(mac);
		--z;
		break;
	}default:{
		return ERR;
	} }
	// FIXME it's not sufficient to just pad these out; we need do so for all
	// lines, or clear the detail window each time
	if(i->topinfo.devname){
		assert(mvwprintw(hw,row,col,"%-*s",cols - 2,i->topinfo.devname) != ERR);
	}else{ // FIXME
		assert(mvwprintw(hw,row,col,"%-*s",cols - 2,"Unknown device") != ERR);
	}
	return OK;
}

static int
display_details_locked(WINDOW *mainw,struct panel_state *ps,iface_state *is){
	// The NULL doesn't count as a row
	int rows,cols,startrow;

	memset(ps,0,sizeof(*ps));
	getmaxyx(mainw,rows,cols);
	// Space for the status bar + gap, bottom bar + gap,
	// and top bar + gap
	startrow = rows - (START_LINE * 3 + DETAILROWS + 1);
	if(rows <= startrow){
		ERREXIT;
	}
	rows -= startrow + START_LINE;
	cols -= START_COL * 2;
	if(new_display_panel(ps,rows,cols,startrow,START_COL,
				L"press 'v' to dismiss details")){
		ERREXIT;
	}
	if(is){
		if(iface_details(ps->w,get_current_iface(),
					START_LINE,START_COL,
					rows - START_LINE * 2,
					cols - START_COL * 2)){
			ERREXIT;
		}
		is->detailwin = ps;
	}
	assert(start_screen_update() != ERR);
	assert(finish_screen_update() != ERR);
	ps->ysize = rows - START_LINE * 2;
	ps->xsize = cols - START_COL * 2;
	return 0;

err:
	if(ps->p){
		hide_panel(ps->p);
		del_panel(ps->p);
	}
	if(ps->w){
		delwin(ps->w);
	}
	memset(ps,0,sizeof(*ps));
	return -1;
}

static const wchar_t *helps[] = {
	L"'k'/'↑' (up arrow): previous interface",
	L"'j'/'↓' (down arrow): next interface",
	L"'^L' (ctrl + 'L'): redraw the screen",
	L"'P': preferences",
	L"       configure persistent or temporary program settings",
	L"'n': network configuration",
	L"       configure addresses, routes, bridges, and wireless",
	L"'a': attack configuration",
	L"       configure source quenching, assassinations, and deauthentication",
	L"'J': hijack configuration",
	L"       configure fake APs, rogue DHCP/DNS, and ARP MitM",
	L"'D': defense configuration",
	L"       define authoritative configurations to enforce",
	L"'S': secrets database",
	L"       export pilfered passwords, cookies, and identifying data",
	L"'c': crypto configuration",
	L"       configure algorithm stepdown, WEP/WPA cracking, SSL MitM",
	L"'r': reset interface's stats",
	L"'R': reset all interfaces' stats",
	L"'p': toggle promiscuity",
	L"'s': toggle sniffing, bringing up interface if down",
	L"'d': bring down device",
	L"'v': view detailed interface info/statistics",
	L"'m': change device MAC",
	L"'u': change device MTU",
	L"'h': toggle this help display",
	L"'q': quit",
	NULL
};

// FIXME need to support scrolling through the list
static int
helpstrs(WINDOW *hw,int row,int col,int rows){
	const wchar_t *hs;
	int z;

	for(z = 0 ; (hs = helps[z]) && z < rows ; ++z){
		if(mvwaddwstr(hw,row + z,col,hs) == ERR){
			return ERR;
		}
	}
	return OK;
}

static int
display_help_locked(WINDOW *mainw,struct panel_state *ps){
	// The NULL doesn't count as a row
	const int helprows = sizeof(helps) / sizeof(*helps) - 1;
	int rows,cols,startrow;

	memset(ps,0,sizeof(*ps));
	getmaxyx(mainw,rows,cols);
	// Optimally, we get space for the status bar + gap, bottom bar + gap,
	// and top bar + gap. We might get less.
	startrow = rows - (START_LINE * 3 + helprows);
	// Need to support scrolling for this to work! FIXME
	if(startrow <= START_LINE + PAD_LINES + 1){
		startrow = START_LINE + PAD_LINES + 1;
	}
	// We get all the rows from startrow to the last two.
	assert(startrow + START_LINE < rows);
	rows -= startrow + START_LINE;
	cols -= START_COL * 2;
	if(new_display_panel(ps,rows,cols,startrow,START_COL,
				L"press 'h' to dismiss help")){
		ERREXIT;
	}
	if(helpstrs(ps->w,START_LINE,START_COL,rows - START_LINE * 2)){
		ERREXIT;
	}
	if(start_screen_update() == ERR){
		ERREXIT;
	}
	if(finish_screen_update() == ERR){
		ERREXIT;
	}
	ps->ysize = rows - START_LINE * 2;
	ps->xsize = cols - START_COL * 2;
	return 0;

err:
	if(ps->p){
		hide_panel(ps->p);
		del_panel(ps->p);
	}
	if(ps->w){
		delwin(ps->w);
	}
	memset(ps,0,sizeof(*ps));
	return -1;
}

// FIXME eliminate all callers!
static void
unimplemented(WINDOW *w,const void *v){
	wstatus_locked(w,"Sorry bro; that ain't implemented yet (%p)!",v);
}

static void
configure_prefs(WINDOW *w){
	unimplemented(w,NULL);
}

static void
configure_network(WINDOW *w){
	unimplemented(w,NULL);
}

static void
configure_attacks(WINDOW *w){
	unimplemented(w,NULL);
}

static void
configure_hijacks(WINDOW *w){
	unimplemented(w,NULL);
}

static void
configure_defence(WINDOW *w){
	unimplemented(w,NULL);
}

static void
configure_secrets(WINDOW *w){
	unimplemented(w,NULL);
}

static void
configure_crypto(WINDOW *w){
	unimplemented(w,NULL);
}

static void
change_mac(WINDOW *w){
	unimplemented(w,NULL);
}

static void
change_mtu(WINDOW *w){
	unimplemented(w,NULL);
}

static void
reset_interface_stats(WINDOW *w,const interface *i){
	unimplemented(w,i);
}

static void
reset_all_interface_stats(WINDOW *w){
	iface_state *is;

	if( (is = current_iface) ){
		do{
			const interface *i = get_current_iface(); // FIXME get_iface(is);

			reset_interface_stats(w,i);
		}while((is = is->next) != current_iface);
	}
}

static void
reset_current_interface_stats(WINDOW *w){
	const interface *i;

	if( (i = get_current_iface()) ){
		reset_interface_stats(w,i);
	}
}

static void
use_next_iface_locked(void){
	if(current_iface && current_iface->next != current_iface){
		const iface_state *is = current_iface;
		interface *i = iface_by_idx(is->ifacenum);

		transfer_details_window(current_iface,current_iface->next);
		current_iface = current_iface->next;
		iface_box(is->subwin,i,is);
		is = current_iface;
		i = iface_by_idx(is->ifacenum);
		iface_box(is->subwin,i,is);
		if(details.w){
			iface_details(details.w,i,START_LINE,START_COL,details.ysize,details.xsize);
		}
		start_screen_update();
		finish_screen_update();
	}
}

static void
use_prev_iface_locked(void){
	if(current_iface && current_iface->prev != current_iface){
		const iface_state *is = current_iface;
		interface *i = iface_by_idx(is->ifacenum);

		transfer_details_window(current_iface,current_iface->prev);
		current_iface = current_iface->prev;
		iface_box(is->subwin,i,is);
		is = current_iface;
		i = iface_by_idx(is->ifacenum);
		iface_box(is->subwin,i,is);
		if(details.w){
			iface_details(details.w,i,START_LINE,START_COL,details.ysize,details.xsize);
		}
		start_screen_update();
		finish_screen_update();
	}
}

struct ncurses_input_marshal {
	WINDOW *w;
	PANEL *p;
	const omphalos_iface *octx;
};

static void *
ncurses_input_thread(void *unsafe_marsh){
	struct ncurses_input_marshal *nim = unsafe_marsh;
	const omphalos_iface *octx = nim->octx;
	WINDOW *w = nim->w;
	int ch;

	active = NULL; // No subpanels initially
	memset(&help,0,sizeof(help));
	memset(&details,0,sizeof(details));
	while((ch = getch()) != 'q' && ch != 'Q'){
	switch(ch){
		case KEY_UP: case 'k':
			pthread_mutex_lock(&bfl);
				use_prev_iface_locked();
			pthread_mutex_unlock(&bfl);
			break;
		case KEY_DOWN: case 'j':
			pthread_mutex_lock(&bfl);
				use_next_iface_locked();
			pthread_mutex_unlock(&bfl);
			break;
		case 12: // Ctrl-L FIXME
			pthread_mutex_lock(&bfl);
				redrawwin(w);
				if(active){
					redrawwin(active->w);
				}
				start_screen_update();
				finish_screen_update();
			pthread_mutex_unlock(&bfl);
			break;
		case 'P':
			pthread_mutex_lock(&bfl);
				configure_prefs(w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'n':
			pthread_mutex_lock(&bfl);
				configure_network(w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'a':
			pthread_mutex_lock(&bfl);
				configure_attacks(w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'J':
			pthread_mutex_lock(&bfl);
				configure_hijacks(w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'D':
			pthread_mutex_lock(&bfl);
				configure_defence(w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'S':
			pthread_mutex_lock(&bfl);
				configure_secrets(w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'c':
			pthread_mutex_lock(&bfl);
				configure_crypto(w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'R':
			pthread_mutex_lock(&bfl);
				reset_all_interface_stats(w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'r':
			pthread_mutex_lock(&bfl);
				reset_current_interface_stats(w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'p':
			pthread_mutex_lock(&bfl);
				toggle_promisc_locked(octx,w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'd':
			pthread_mutex_lock(&bfl);
				down_interface_locked(octx,w);
			pthread_mutex_unlock(&bfl);
			break;
		case 's':
			pthread_mutex_lock(&bfl);
				sniff_interface_locked(octx,w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'u':
			pthread_mutex_lock(&bfl);
				change_mtu(w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'm':
			pthread_mutex_lock(&bfl);
				change_mac(w);
			pthread_mutex_unlock(&bfl);
			break;
		case 'v':{
			pthread_mutex_lock(&bfl);
			if(details.w){
				hide_panel_locked(w,&details);
				active = NULL;
			}else{
				hide_panel_locked(w,active);
				active = (display_details_locked(w,&details,current_iface) == OK)
					? &details : NULL;
			}
			pthread_mutex_unlock(&bfl);
			break;
		}case 'h':{
			pthread_mutex_lock(&bfl);
			if(help.w){
				if(current_iface){
					current_iface->detailwin = NULL;
				}
				hide_panel_locked(w,&help);
				active = NULL;
			}else{
				hide_panel_locked(w,active);
				active = (display_help_locked(w,&help) == OK)
					? &help : NULL;
			}
			pthread_mutex_unlock(&bfl);
			break;
		}default:{
			const char *hstr = !help.w ? " ('h' for help)" : "";
			if(isprint(ch)){
				wstatus(w,"unknown command '%c'%s",ch,hstr);
			}else{
				wstatus(w,"unknown scancode %d%s",ch,hstr);
			}
			break;
		}
	}
	}
	wstatus(w,"%s","shutting down");
	// we can't use raise() here, as that sends the signal only
	// to ourselves, and we have it masked.
	kill(getpid(),SIGINT);
	pthread_exit(NULL);
}

// Cleanup which ought be performed even if we had a failure elsewhere, or
// indeed never started.
static int
mandatory_cleanup(WINDOW **w,PANEL **p){
	int ret = 0;

	pthread_mutex_lock(&bfl);
	if(*p){
		if(del_panel(*p) == ERR){
			ret = -4;
		}
		*p = NULL;
	}
	if(*w){
		if(delwin(*w) != OK){
			ret = -1;
		}
		*w = NULL;
	}
	if(stdscr){
		if(delwin(stdscr) != OK){
			ret = -2;
		}
		stdscr = NULL;
	}
	if(endwin() != OK){
		ret = -3;
	}
	pthread_mutex_unlock(&bfl);
	switch(ret){
	case -4: fprintf(stderr,"Couldn't destroy main panel\n"); break;
	case -3: fprintf(stderr,"Couldn't end main window\n"); break;
	case -2: fprintf(stderr,"Couldn't delete main window\n"); break;
	case -1: fprintf(stderr,"Couldn't delete main pad\n"); break;
	case 0: break;
	default: fprintf(stderr,"Couldn't cleanup ncurses\n"); break;
	}
	return ret;
}

static WINDOW *
ncurses_setup(const omphalos_iface *octx,PANEL **panel){
	struct ncurses_input_marshal *nim;
	const char *errstr = NULL;
	WINDOW *w = NULL;
	PANEL *p = NULL;

	if(initscr() == NULL){
		fprintf(stderr,"Couldn't initialize ncurses\n");
		return NULL;
	}
	if(cbreak() != OK){
		errstr = "Couldn't disable input buffering\n";
		goto err;
	}
	if(noecho() != OK){
		errstr = "Couldn't disable input echoing\n";
		goto err;
	}
	if(intrflush(stdscr,TRUE) != OK){
		errstr = "Couldn't set flush-on-interrupt\n";
		goto err;
	}
	if(nonl() != OK){
		errstr = "Couldn't disable nl translation\n";
		goto err;
	}
	if(start_color() != OK){
		errstr = "Couldn't initialize ncurses color\n";
		goto err;
	}
	if(use_default_colors()){
		errstr = "Couldn't initialize ncurses colordefs\n";
		goto err;
	}
	w = stdscr;
	/*if((w = newpad(LINES,COLS)) == NULL){
		errstr = "Couldn't initialize main pad\n";
		goto err;
	}
	*/
	if((p = new_panel(stdscr)) == NULL){
		errstr = "Couldn't initialize main panel\n";
		goto err;
	}
	keypad(stdscr,TRUE);
	if(init_pair(BORDER_COLOR,COLOR_GREEN,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(HEADING_COLOR,COLOR_YELLOW,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(DBORDER_COLOR,COLOR_WHITE,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(DHEADING_COLOR,COLOR_WHITE,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(UBORDER_COLOR,COLOR_YELLOW,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(UHEADING_COLOR,COLOR_GREEN,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(PBORDER_COLOR,COLOR_CYAN,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(PHEADING_COLOR,COLOR_RED,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(init_pair(BULKTEXT_COLOR,COLOR_WHITE,-1) != OK){
		errstr = "Couldn't initialize ncurses colorpair\n";
		goto err;
	}
	if(curs_set(0) == ERR){
		errstr = "Couldn't disable cursor\n";
		goto err;
	}
	if(setup_statusbar(COLS)){
		errstr = "Couldn't setup status bar\n";
		goto err;
	}
	if(draw_main_window(w,PROGNAME,VERSION)){
		errstr = "Couldn't use ncurses\n";
		goto err;
	}
	if((nim = malloc(sizeof(*nim))) == NULL){
		goto err;
	}
	nim->octx = octx;
	nim->w = w;
	nim->p = p;
	if(pthread_create(&inputtid,NULL,ncurses_input_thread,nim)){
		errstr = "Couldn't create UI thread\n";
		free(nim);
		goto err;
	}
	// FIXME install SIGWINCH() handler...
	*panel = p;
	return w;

err:
	mandatory_cleanup(&w,&p);
	fprintf(stderr,"%s",errstr);
	return NULL;
}

static inline unsigned long
timerusec(const struct timeval *tv){
	return tv->tv_sec * 1000000 + tv->tv_usec;
}

static int
print_iface_state(const interface *i,const iface_state *is){
	unsigned long usecexist;
	struct timeval tdiff;
	char buf[80]; // FIXME

	timersub(&i->lastseen,&i->firstseen,&tdiff);
	usecexist = timerusec(&tdiff);
	assert(mvwprintw(is->subwin,1,1 + START_COL * 2,"%sb/s\t%15ju pkts",
				rate(i->bytes * CHAR_BIT * 1000000 * 100 / usecexist,100,buf,sizeof(buf),0),i->frames) != ERR);
	assert(start_screen_update() != ERR);
	assert(finish_screen_update() != ERR);
	return 0;
}

static inline void
packet_cb_locked(const interface *i,iface_state *is){
	if(is){
		struct timeval tdiff;
		unsigned long udiff;

		timersub(&i->lastseen,&is->lastprinted,&tdiff);
		udiff = timerusec(&tdiff);
		if(udiff < 16667){ // At most one update every 1/60s
			if(!is->alarmset){
				// FIXME register the alarm
				is->alarmset = 1;
			}
			return;
		}
		is->lastprinted = i->lastseen;
		if(is->detailwin){
			iface_details(is->detailwin->w,i,START_LINE,START_COL,
					is->detailwin->ysize,
					is->detailwin->xsize);
		}
		print_iface_state(i,is);
	}
}

static void
packet_callback(const interface *i,void *unsafe){
	pthread_mutex_lock(&bfl);
	packet_cb_locked(i,unsafe);
	pthread_mutex_unlock(&bfl);
}

static inline unsigned
lines_for_interface(const interface *i){
	if(i->flags & IFF_UP){
		return PAD_LINES;
	}else{
		return PAD_LINES - 1;
	}
}

static inline void *
interface_cb_locked(const interface *i,int inum,iface_state *ret){
	if(ret == NULL){
		const char *tstr;

		if( (tstr = lookup_arptype(i->arptype,NULL)) ){
			if( (ret = malloc(sizeof(iface_state))) ){
				ret->ysize = lines_for_interface(i);
				ret->alarmset = ret->devaction = 0;
				ret->typestr = tstr;
				ret->lastprinted.tv_sec = ret->lastprinted.tv_usec = 0;
				ret->ifacenum = inum;
				if((ret->prev = current_iface) == NULL){
					current_iface = ret->prev = ret->next = ret;
					ret->scrline = START_LINE;
					if(details.w){
						ret->detailwin = &details;
					}
				}else{
					// The order on screen must match the list order, so splice it onto
					// the end. We might be anywhere, so use absolute coords (scrline).
					while(ret->prev->next->scrline > ret->prev->scrline){
						ret->prev = ret->prev->next;
					}
					ret->scrline = lines_for_interface(iface_by_idx(ret->prev->ifacenum)) + ret->prev->scrline  + 1;
					ret->next = ret->prev->next;
					ret->next->prev = ret;
					ret->prev->next = ret;
					ret->detailwin = NULL;
				}
				if( (ret->subwin = derwin(pad,ret->ysize,PAD_COLS,ret->scrline,START_COL)) &&
						(ret->panel = new_panel(ret->subwin)) ){
					++count_interface;
				}else{
					delwin(ret->subwin);
					if(current_iface == ret){
						current_iface = NULL;
					}else{
						ret->next->prev = ret->prev;
						ret->prev->next = ret->next;
					}
					free(ret);
					ret = NULL;
				}
			}
		}
	}
	if(ret){
		if(ret->detailwin){
			iface_details(ret->detailwin->w,i,START_LINE,START_COL,
					ret->detailwin->ysize,
					ret->detailwin->xsize);
		}
		iface_box(ret->subwin,i,ret);
		if(i->flags & IFF_UP){
			print_iface_state(i,ret);
			if(ret->devaction < 0){
				wstatus_locked(pad,"");
				ret->devaction = 0;
			}
			// FIXME expand it
		}else if(ret->devaction > 0){
			wstatus_locked(pad,"");
			ret->devaction = 0;
			// FIXME collapse it
			start_screen_update();
			finish_screen_update();
		}
	}
	return ret;
}

static void *
interface_callback(const interface *i,int inum,void *unsafe){
	void *r;

	pthread_mutex_lock(&bfl);
	r = interface_cb_locked(i,inum,unsafe);
	pthread_mutex_unlock(&bfl);
	return r;
}

static inline void
interface_removed_locked(iface_state *is){
	if(is){
		werase(is->subwin);
		del_panel(is->panel);
		delwin(is->subwin);
		if(is->next != is){
			is->next->prev = is->prev;
			is->prev->next = is->next;
			if(is == current_iface){
				current_iface = is->prev;
			}
			// If we owned the details window, give it to the new
			// current_iface.
			transfer_details_window(is,current_iface);
			if(is->detailwin){
				current_iface->detailwin = is->detailwin;
			}
		}else{
			// If we owned the details window, destroy it FIXME
			current_iface = NULL;
		}
		free(is);
		// FIXME need move other ifaces up
		start_screen_update();
		finish_screen_update();
	}
}

static void
interface_removed_callback(const interface *i __attribute__ ((unused)),void *unsafe){
	pthread_mutex_lock(&bfl);
	interface_removed_locked(unsafe);
	pthread_mutex_unlock(&bfl);
}

static void
diag_callback(const char *fmt,...){
	va_list va;

	va_start(va,fmt);
	wvstatus(pad,fmt,va);
	va_end(va);
}

int main(int argc,char * const *argv){
	omphalos_ctx pctx;
	PANEL *panel;

	if(setlocale(LC_ALL,"") == NULL){
		fprintf(stderr,"Couldn't initialize locale (%s?)\n",strerror(errno));
		return EXIT_FAILURE;
	}
	if(uname(&sysuts)){
		fprintf(stderr,"Coudln't get OS info (%s?)\n",strerror(errno));
		return EXIT_FAILURE;
	}
	glibc_version = gnu_get_libc_version();
	glibc_release = gnu_get_libc_release();
	if(omphalos_setup(argc,argv,&pctx)){
		return EXIT_FAILURE;
	}
	pctx.iface.packet_read = packet_callback;
	pctx.iface.iface_event = interface_callback;
	pctx.iface.iface_removed = interface_removed_callback;
	pctx.iface.diagnostic = diag_callback;
	if((pad = ncurses_setup(&pctx.iface,&panel)) == NULL){
		return EXIT_FAILURE;
	}
	if(omphalos_init(&pctx)){
		int err = errno;

		mandatory_cleanup(&pad,&panel);
		fprintf(stderr,"Error in omphalos_init() (%s?)\n",strerror(err));
		return EXIT_FAILURE;
	}
	omphalos_cleanup(&pctx);
	if(mandatory_cleanup(&pad,&panel)){
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
