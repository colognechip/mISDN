/* $Id: dsp_core.c,v 1.6 2004/01/26 22:21:30 keil Exp $
 *
 * Author       Andreas Eversberg (jolly@jolly.de)
 * Based on source code structure by
 *		Karsten Keil (keil@isdn4linux.de)
 *
 *		This file is (c) under GNU PUBLIC LICENSE
 *		For changes and modifications please read
 *		../../../Documentation/isdn/mISDN.cert
 *
 * Thanks to    Karsten Keil (great drivers)
 *              Cologne Chip (great chips)
 *
 * This module does:
 *		Real-time tone generation
 *		DTMF detection
 *		Real-time cross-connection and conferrence
 *		Compensate jitter due to system load and hardware fault.
 *		All features are done in kernel space and will be realized
 *		using hardware, if available and supported by chip set.
 */

/* STRUCTURE:
 *
 * The dsp module provides layer 2 for b-channels (64kbit). It provides
 * transparent audio forwarding with special digital signal processing:
 *
 * - (1) generation of tones
 * - (2) detection of dtmf tones
 * - (3) crossconnecting and conferences
 * - (4) echo generation for delay test
 * - (5) volume control
 * - (6) disable receive data
 *
 * Look:
 *             TX            RX
 *         ------upper layer------
 *             |             ^
 *             |             |(6)
 *             v             |
 *       +-----+-------------+-----+
 *       |(3)(4)                   |
 *       |                         |
 *       |                         |
 *       |           CMX           |
 *       |                         |
 *       |                         |
 *       |                         |
 *       |                         |
 *       |           +-------------+
 *       |           |       ^
 *       |           |       |
 *       |           |       |
 *       |+---------+|  +----+----+
 *       ||(1)      ||  |(5)      |
 *       ||         ||  |         |
 *       ||  Tones  ||  |RX Volume|
 *       ||         ||  |         |
 *       ||         ||  |         |
 *       |+----+----+|  +----+----+
 *       +-----+-----+       ^
 *             |             | 
 *             |             |
 *             v             |
 *        +----+----+   +----+----+
 *        |(5)      |   |(2)      |
 *        |         |   |         |
 *        |TX Volume|   |  DTMF   |
 *        |         |   |         |
 *        |         |   |         |
 *        +----+----+   +----+----+
 *             |             ^ 
 *             |             |
 *             v             |
 *         ------card  layer------
 *             TX            RX
 *
 * Above you can see the logical data flow. If software is used to do the
 * process, it is actually the real data flow. If hardware is used, data
 * may not flow, but hardware commands to the card, to provide the data flow
 * as shown.
 *
 * NOTE: The channel must be activated in order to make dsp work, even if
 * no data flow to the upper layer is intended. Activation can be done
 * after and before controlling the setting using PH_CONTROL requests.
 *
 * DTMF: Will be detected by hardware if possible. It is done before CMX 
 * processing.
 *
 * Tones: Will be generated via software if endless looped audio fifos are
 * not supported by hardware. Tones will override all data from CMX.
 * It is not required to join a conference to use tones at any time.
 *
 * CMX: Is transparent when not used. When it is used, it will do
 * crossconnections and conferences via software if not possible through
 * hardware. If hardware capability is available, hardware is used.
 *
 * Echo: Is generated by CMX and is used to check performane of hard and
 * software CMX.
 *
 * The CMX has special functions for conferences with one, two and more
 * members. It will allow different types of data flow. Receive and transmit
 * data to/form upper layer may be swithed on/off individually without loosing
 * features of CMX, Tones and DTMF.
 *
 * If all used features can be realized in hardware, and if transmit and/or
 * receive data ist disabled, the card may not send/receive any data at all.
 * Not receiving is usefull if only announcements are played. Not sending is
 * usefull if an answering machine records audio. Not sending and receiving is
 * usefull during most states of the call. If supported by hardware, tones
 * will be played without cpu load. Small PBXs and NT-Mode applications will
 * not need expensive hardware when processing calls.
 *
 *
 * LOCKING:
 *
 * When data is received from upper or lower layer (card), the complete dsp
 * module is locked by a global lock.  When data is ready to be transmitted
 * to a different layer, the module is unlocked. It is not allowed to hold a
 * lock outside own layer.
 * Reasons: Multiple threads must not process cmx at the same time, if threads
 * serve instances, that are connected in same conference.
 * PH_CONTROL must not change any settings, join or split conference members
 * during process of data.
 * 
 *
 * TRANSMISSION:
 *

TBD

There are three things that need to receive data from card:
 - software DTMF decoder
 - software cmx (if conference exists)
 - upper layer, if rx-data not disabled

Whenever dtmf decoder is turned on or off, software cmx changes, rx-data is disabled or enabled, or card becomes activated, then rx-data is disabled or enabled using a special command to the card.

There are three things that need to transmit data to card:
 - software tone generation (part of cmx)
 - software cmx
 - upper layer, if tx-data is written to tx-buffer

Whenever cmx is changed, or data is sent from upper layer, the transmission is triggered by an silence freame (if not already tx_pending==1). When the confirm is received from card, next frame is
sent using software cmx, if tx-data is still available, or if software tone generation is used,
or if cmx is currently using software.


 
 */

#ifdef WITH_HARDWARE
alle einstellungen der hardware sollten in einer struktur hinterlegt werden.
sobald der channel aktiviert wird, wird diese struktur an den hfc-chip gesendet.
wenn sich die daten der hardware ändern, gibt es eben ein update der struktur, 
die an den hfc-chip gesendet wird (wenn aktiv).
beim PH_DEACTIVATE sollten daten zur deaktivierung der hardware gesendet werden.

die daten der struktur
        - conference number
	- other crossconnect member
	- loop
        - rx_data
	- dtmf
	- law-codec
	- tone-loop
	- volume
	- loop
#endif

const char *dsp_revision = "$Revision: 1.6 $";

#include <linux/delay.h>
#include <linux/config.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include "layer1.h"
#include "helper.h"
#include "debug.h"
#include "dsp.h"
#include "hw_lock.h"

static char DSPName[] = "DSP";
mISDNobject_t dsp_obj;
static mISDN_HWlock_t dsp_lock;

static int debug = 0;
int options = 0;

#ifdef MODULE
MODULE_AUTHOR("Andreas Eversberg");
MODULE_PARM(debug, "1i");
MODULE_PARM(options, "1i");
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif
#endif

/*
 * sending next frame to card (triggered by PH_ACTIVAT and PH_DATA_CONF)
 */
static void
sendevent(dsp_t *dsp)
{
	struct		sk_buff *nskb;

	lock_HW(&dsp_lock, 0);
	dsp->tx_pending = 0;
	if (dsp->b_active) {
		/* get data from cmx */
		nskb = dsp_cmx_send(dsp, SEND_LEN, 0);
		if (!nskb) {
			printk(KERN_ERR "%s: failed to create subsequent packet\n", __FUNCTION__);
			unlock_HW(&dsp_lock);
			return;
		}
		/* change volume if requested */
		if (dsp->tx_volume)
			dsp_change_volume(nskb, dsp->tx_volume);
		/* send subsequent requests to card */
		unlock_HW(&dsp_lock);
		if (dsp->inst.down.func(&dsp->inst.down, nskb)) {
			printk(KERN_ERR "%s: failed to send subsequent packet\n", __FUNCTION__);
			dev_kfree_skb(nskb);
		}
	} else
		unlock_HW(&dsp_lock);
}


/*
 * special message process for DL_CONTROL | REQUEST
 */
static int
dsp_control_req(dsp_t *dsp, mISDN_head_t *hh, struct sk_buff *skb)
{
	int ret = 0;
	int cont;
	unsigned char *data;
	int len;

	if (skb->len < sizeof(int)) {
		printk(KERN_ERR "%s: PH_CONTROL message too short\n", __FUNCTION__);
	}
	cont = *((int *)skb->data);
	len = skb->len - sizeof(int);
	data = skb->data + sizeof(int);

	switch (cont) {
		case DTMF_TONE_START: /* turn on DTMF */
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: start dtmf\n", __FUNCTION__);
			dsp_dtmf_goertzel_init(dsp);
			/* checking for hardware capability */
#ifdef WITH_HARDWARE
			if (...............) {
				dsp->dtmf.hardware = 2;
				dsp->dtmf.software = 0;
			} else {
#endif
				dsp->dtmf.hardware = 0;
				dsp->dtmf.software = 1;
#ifdef WITH_HARDWARE
			}
auch hier muss der chip davon erfahren
auch beim TONE_STOP muss der chip was erfahren.
bedenke activate un deactivate. 
#endif
			break;
		case DTMF_TONE_STOP: /* turn off DTMF */
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: stop dtmf\n", __FUNCTION__);
			dsp->dtmf.hardware = 0;
			dsp->dtmf.software = 0;
			break;
		case CMX_CONF_JOIN: /* join / update conference */
			if (len != sizeof(int)) {
				ret = -EINVAL;
				break;
			}
			dsp->conf_id = *((int *)data);
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: join conference %d\n", __FUNCTION__, dsp->conf_id);
			ret = dsp_cmx(dsp);
			if (debug & DEBUG_DSP_CMX)
				dsp_cmx_debug(dsp);
			break;
		case CMX_CONF_SPLIT: /* remove from conference */
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: release conference\n", __FUNCTION__);
			dsp->conf_id = 0;
			ret = dsp_cmx(dsp);
			if (debug & DEBUG_DSP_CMX)
				dsp_cmx_debug(dsp);
			break;
		case TONE_PATT_ON: /* play tone */
			if (len != sizeof(int)) {
				ret = -EINVAL;
				break;
			}
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: turn tone 0x%x on\n", __FUNCTION__, *((int *)skb->data));
			ret = dsp_tone(dsp, *((int *)data));
			if (!ret && dsp->conf_id)
				ret = dsp_cmx(dsp);
			if (ret)
				dsp_tone(dsp, 0);
			if (!dsp->tone.tone)
				goto tone_off;
			break;
		case TONE_PATT_OFF: /* stop tone */
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: turn tone off\n", __FUNCTION__);
			dsp_tone(dsp, 0);
			if (dsp->conf_id)
				ret = dsp_cmx(dsp);
			/* reset tx buffers (user space data) */
			tone_off:
			dsp->R_tx = dsp->W_tx = 0;
			break;
		case VOL_CHANGE_TX: /* change volume */
			if (len != sizeof(int)) {
				ret = -EINVAL;
				break;
			}
			dsp->tx_volume = *((int *)data);
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: change tx volume to %d\n", __FUNCTION__, dsp->tx_volume);
			if (dsp->conf_id)
				ret = dsp_cmx(dsp);
			break;
		case VOL_CHANGE_RX: /* change volume */
			if (len != sizeof(int)) {
				ret = -EINVAL;
				break;
			}
			dsp->rx_volume = *((int *)data);
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: change rx volume to %d\n", __FUNCTION__, dsp->tx_volume);
			if (dsp->conf_id)
				ret = dsp_cmx(dsp);
			break;
		case CMX_ECHO_ON: /* enable echo */
			dsp->echo = 1;
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: enable cmx-echo\n", __FUNCTION__);
			if (dsp->conf_id)
				ret = dsp_cmx(dsp);
			if (debug & DEBUG_DSP_CMX)
				dsp_cmx_debug(dsp);
			break;
		case CMX_ECHO_OFF: /* disable echo */
			dsp->echo = 0;
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: disable cmx-echo\n", __FUNCTION__);
			if (dsp->conf_id)
				ret = dsp_cmx(dsp);
			if (debug & DEBUG_DSP_CMX)
				dsp_cmx_debug(dsp);
			break;
		case CMX_RECEIVE_ON: /* enable receive to user space */
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: enable receive to user space\n", __FUNCTION__);
			dsp->rx_disabled = 0;
			if (dsp->conf_id)
				ret = dsp_cmx(dsp);
			break;
		case CMX_RECEIVE_OFF: /* disable receive to user space */
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: disable receive to user space\n", __FUNCTION__);
			dsp->rx_disabled = 1;
			if (dsp->conf_id)
				ret = dsp_cmx(dsp);
			break;
		case CMX_MIX_ON: /* enable mixing of transmit data with conference members */
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: enable mixing of tx-data with conf mebers\n", __FUNCTION__);
			dsp->tx_mix = 1;
			if (dsp->conf_id)
				ret = dsp_cmx(dsp);
			if (debug & DEBUG_DSP_CMX)
				dsp_cmx_debug(dsp);
			break;
		case CMX_MIX_OFF: /* disable mixing of transmit data with conference members */
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: disable mixing of tx-data with conf mebers\n", __FUNCTION__);
			dsp->tx_mix = 0;
			if (dsp->conf_id)
				ret = dsp_cmx(dsp);
			if (debug & DEBUG_DSP_CMX)
				dsp_cmx_debug(dsp);
			break;
		default:
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: ctrl req %x unhandled\n", __FUNCTION__, cont);
			ret = -EINVAL;
	}
	return(ret);
}


/*
 * messages from upper layers
 */
static int
dsp_from_up(mISDNif_t *hif, struct sk_buff *skb)
{
	dsp_t			*dsp;
	mISDN_head_t		*hh;
	int			ret = 0;

	if (!hif || !hif->fdata || !skb)
		return(-EINVAL);
	dsp = hif->fdata;
	if (!dsp->inst.down.func)
		return(-ENXIO);

	hh = mISDN_HEAD_P(skb);
	switch(hh->prim) {
		case DL_DATA | RESPONSE:
		case PH_DATA | RESPONSE:
			/* ignore response */
			break;
		case DL_DATA | REQUEST:
		case PH_DATA | REQUEST:
			lock_HW(&dsp_lock, 0);
			/* send data to tx-buffer (if no tone is played) */
			if (!dsp->tone.tone)
				dsp_cmx_transmit(dsp, skb);
			unlock_HW(&dsp_lock);
			break;
		case PH_CONTROL | REQUEST:
			lock_HW(&dsp_lock, 0);
			ret = dsp_control_req(dsp, hh, skb);
			unlock_HW(&dsp_lock);
			break;
		case DL_ESTABLISH | REQUEST:
		case PH_ACTIVATE | REQUEST:
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: activating b_channel %s\n", __FUNCTION__, dsp->inst.name);
			lock_HW(&dsp_lock, 0);
			dsp->tx_pending = 0;
			if (dsp->dtmf.hardware || dsp->dtmf.software)
				dsp_dtmf_goertzel_init(dsp);
			unlock_HW(&dsp_lock);
			hh->prim = PH_ACTIVATE | REQUEST;
			return(dsp->inst.down.func(&dsp->inst.down, skb));
		case DL_RELEASE | REQUEST:
		case PH_DEACTIVATE | REQUEST:
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: releasing b_channel %s\n", __FUNCTION__, dsp->inst.name);
			lock_HW(&dsp_lock, 0);
			dsp->tx_pending = 0;
			unlock_HW(&dsp_lock);
			hh->prim = PH_DEACTIVATE | REQUEST;
			return(dsp->inst.down.func(&dsp->inst.down, skb));
		default:
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: msg %x unhandled %s\n", __FUNCTION__, hh->prim, dsp->inst.name);
			ret = -EINVAL;
			break;
	}
	if (!ret)
		dev_kfree_skb(skb);
	return(ret);
}


/*
 * messages from lower layers
 */
static int
dsp_from_down(mISDNif_t *hif,  struct sk_buff *skb)
{
	dsp_t		*dsp;
	mISDN_head_t	*hh;
	int		ret = 0;
	unsigned char	*digits;
	int 		cont;
	struct		sk_buff *nskb;

	if (!hif || !hif->fdata || !skb)
		return(-EINVAL);
	dsp = hif->fdata;
	if (!dsp->inst.up.func)
		return(-ENXIO);

	hh = mISDN_HEAD_P(skb);
	switch(hh->prim)
	{
		case PH_DATA | CONFIRM:
		case DL_DATA | CONFIRM:
			lock_HW(&dsp_lock, 0);
			dsp->tx_pending = 1;
			schedule_work(&dsp->sendwork);
			unlock_HW(&dsp_lock);
			break;
		case PH_DATA | INDICATION:
		case DL_DATA | INDICATION:
			lock_HW(&dsp_lock, 0);
			/* check if dtmf soft decoding is turned on */
			if (dsp->dtmf.software) {
				digits = dsp_dtmf_goertzel_decode(dsp, skb->data, skb->len, (options&DSP_OPT_ULAW)?1:0);
				if (digits) while(*digits) {
					if (dsp->debug & DEBUG_DSP_DTMF)
						printk(KERN_DEBUG "%s: sending software decoded digit(%c) to upper layer %s\n", __FUNCTION__, *digits, dsp->inst.name);
					cont = DTMF_TONE_VAL | *digits;
					nskb = create_link_skb(PH_CONTROL | INDICATION, 0, sizeof(int), &cont, 0);
					unlock_HW(&dsp_lock);
					if (!nskb)
						break;
					if (dsp->inst.up.func(&dsp->inst.up, nskb))
						dev_kfree_skb(nskb);
					lock_HW(&dsp_lock, 0);
					digits++;
				}
			}
			/* change volume if requested */
			if (dsp->rx_volume)
				dsp_change_volume(skb, dsp->rx_volume);
			/* process data from card at cmx */
			dsp_cmx_receive(dsp, skb);
			if (dsp->rx_disabled) {
				/* if receive is not allowed */
				unlock_HW(&dsp_lock);
				break;
			}
			unlock_HW(&dsp_lock);
			hh->prim = DL_DATA | INDICATION;
			return(dsp->inst.up.func(&dsp->inst.up, skb));
		case PH_CONTROL | INDICATION:
			lock_HW(&dsp_lock, 0);
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: PH_CONTROL received: %x %s\n", __FUNCTION__, hh->dinfo, dsp->inst.name);
			switch (hh->dinfo) {
				case HW_HFC_COEFF: /* getting coefficients */
				if (dsp->dtmf.hardware == 2)
				if (dsp->inst.up.func) {
					digits = dsp_dtmf_goertzel_decode(dsp, skb->data, skb->len, 2);
					if (digits) while(*digits) {
						int k;
						struct sk_buff *nskb;
						if (dsp->debug & DEBUG_DSP_DTMF)
							printk(KERN_DEBUG "%s: now sending software decoded digit(%c) to upper layer %s\n", __FUNCTION__, *digits, dsp->inst.name);
						k = *digits | DTMF_TONE_VAL;
						nskb = create_link_skb(PH_CONTROL | INDICATION, 0, sizeof(int), &k, 0);
						unlock_HW(&dsp_lock);
						if (!nskb)
							break;
						if (dsp->inst.up.func(&dsp->inst.up, nskb))
							dev_kfree_skb(nskb);
						lock_HW(&dsp_lock, 0);
						digits++;
					}
				}
				break;

				default:
				if (dsp->debug & DEBUG_DSP_CORE)
					printk(KERN_DEBUG "%s: ctrl ind %x unhandled %s\n", __FUNCTION__, hh->dinfo, dsp->inst.name);
				ret = -EINVAL;
			}
			unlock_HW(&dsp_lock);
			break;
		case PH_ACTIVATE | CONFIRM:
			lock_HW(&dsp_lock, 0);
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: b_channel is now active %s\n", __FUNCTION__, dsp->inst.name);
			/* bchannel now active */
			dsp->b_active = 1;
			dsp->W_tx = dsp->R_tx = 0; /* clear TX buffer */
			dsp->W_rx = dsp->R_rx = 0; /* clear RX buffer */
			if (dsp->conf)
				dsp->W_rx = dsp->R_rx = dsp->conf->W_max;
			memset(dsp->rx_buff, 0, sizeof(dsp->rx_buff));
			if (dsp->conf_id)
				ret = dsp_cmx(dsp);
			/* now trigger transmission */
			dsp->tx_pending = 1;
			schedule_work(&dsp->sendwork);
			unlock_HW(&dsp_lock);
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: done with activation, sending confirm to user space. %s\n", __FUNCTION__, dsp->inst.name);
			/* send activation to upper layer */
			hh->prim = DL_ESTABLISH | CONFIRM;
			return(dsp->inst.up.func(&dsp->inst.up, skb));
		case PH_DEACTIVATE | CONFIRM:
			lock_HW(&dsp_lock, 0);
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: b_channel is now inactive %s\n", __FUNCTION__, dsp->inst.name);
			/* bchannel now inactive */
			dsp->b_active = 0;
			if (dsp->conf_id)
				dsp_cmx(dsp);
			unlock_HW(&dsp_lock);
			hh->prim = DL_RELEASE | CONFIRM;
			return(dsp->inst.up.func(&dsp->inst.up, skb));
		default:
			if (dsp->debug & DEBUG_DSP_CORE)
				printk(KERN_DEBUG "%s: msg %x unhandled %s\n", __FUNCTION__, hh->prim, dsp->inst.name);
			ret = -EINVAL;
	}
	if (!ret)
		dev_kfree_skb(skb);
	return(ret);
}


/*
 * desroy DSP instances
 */
static void
release_dsp(dsp_t *dsp)
{
	mISDNinstance_t	*inst = &dsp->inst;
	conference_t *conf;

#ifdef HAS_WORKQUEUE
	if (dsp->sendwork.pending)
		printk(KERN_ERR "%s: pending sendwork: %lx %s\n", __FUNCTION__, dsp->sendwork.pending, dsp->inst.name);
#else
	if (dsp->sendwork.sync)
		printk(KERN_ERR "%s: pending sendwork: %lx %s\n", __FUNCTION__, dsp->sendwork.sync, dsp->inst.name);
#endif
	if (debug & DEBUG_DSP_MGR)
		printk(KERN_DEBUG "%s: removing conferences %s\n", __FUNCTION__, dsp->inst.name);
	conf = dsp->conf;
	if (conf) {
		dsp_cmx_del_conf_member(dsp);
		if (!conf->mlist) {
			dsp_cmx_del_conf(conf);
		}
	}

	if (debug & DEBUG_DSP_MGR)
		printk(KERN_DEBUG "%s: disconnecting instances %s\n", __FUNCTION__, dsp->inst.name);
	if (inst->up.peer) {
		inst->up.peer->obj->ctrl(inst->up.peer,
			MGR_DISCONNECT | REQUEST, &inst->up);
	}
	if (inst->down.peer) {
		inst->down.peer->obj->ctrl(inst->down.peer,
			MGR_DISCONNECT | REQUEST, &inst->down);
	}

	if (debug & DEBUG_DSP_MGR)
		printk(KERN_DEBUG "%s: remove & destroy object %s\n", __FUNCTION__, dsp->inst.name);
	REMOVE_FROM_LISTBASE(dsp, ((dsp_t *)dsp_obj.ilist));
	dsp_obj.ctrl(inst, MGR_UNREGLAYER | REQUEST, NULL);
	vfree(dsp);

	if (debug & DEBUG_DSP_MGR)
		printk(KERN_DEBUG "%s: dsp instance released\n", __FUNCTION__);
}


/*
 * create new DSP instances
 */
static int
new_dsp(mISDNstack_t *st, mISDN_pid_t *pid) 
{
	dsp_t *ndsp;
	int err = 0;

	if (debug & DEBUG_DSP_MGR)
		printk(KERN_DEBUG "%s: creating new dsp instance\n", __FUNCTION__);

	if (!st || !pid)
		return(-EINVAL);
	if (!(ndsp = vmalloc(sizeof(dsp_t)))) {
		printk(KERN_ERR "%s: vmalloc dsp_t failed\n", __FUNCTION__);
		return(-ENOMEM);
	}
	memset(ndsp, 0, sizeof(dsp_t));
	memcpy(&ndsp->inst.pid, pid, sizeof(mISDN_pid_t));
	ndsp->inst.obj = &dsp_obj;
	ndsp->inst.data = ndsp;
	if (!mISDN_SetHandledPID(&dsp_obj, &ndsp->inst.pid)) {
		int_error();
		err = -ENOPROTOOPT;
		free_mem:
		vfree(ndsp);
		return(err);
	}
	sprintf(ndsp->inst.name, "DSP_S%x/C%x",
		(st->id&0xff), (st->id&0xff00)>>8);
	ndsp->debug = debug;
	ndsp->inst.up.owner = &ndsp->inst;
	ndsp->inst.down.owner = &ndsp->inst;
// jolly patch start
	/* set frame size to start */
	ndsp->largest = SEND_LEN << 1;
// jolly patch stop
#ifdef WITH_HARDWARE
	if (!(options & DSP_OPT_NOHARDWARE)) {
		/* check if b-channel is on hfc_chip (0 = no chip) */
		hier muss noch herausgefunden werden, wie ich erfrage, ob und
		welcher hfc-chip verwendet wird (0 = kein chip)
		ndsp->hfc_id = ;
	}
#endif
	INIT_WORK(&ndsp->sendwork, (void *)(void *)sendevent, ndsp);
	APPEND_TO_LIST(ndsp, ((dsp_t *)dsp_obj.ilist));
	err = dsp_obj.ctrl(st, MGR_REGLAYER | INDICATION, &ndsp->inst);
	if (err) {
		printk(KERN_ERR "%s: failed to register layer %s\n", __FUNCTION__, ndsp->inst.name);
		REMOVE_FROM_LISTBASE(ndsp, ((dsp_t *)dsp_obj.ilist));
		goto free_mem;
	} else {
		if (debug & DEBUG_DSP_MGR)
			printk(KERN_DEBUG "%s: dsp instance created %s\n", __FUNCTION__, ndsp->inst.name);
	}
	return(err);
}


/*
 * manager for DSP instances
 */
static int
dsp_manager(void *data, u_int prim, void *arg) {
	mISDNinstance_t *inst = data;
	dsp_t *dspl = (dsp_t *)dsp_obj.ilist;
	int ret = 0;

	if (debug & DEBUG_DSP_MGR)
		printk(KERN_DEBUG "%s: data:%p prim:%x arg:%p\n", __FUNCTION__, data, prim, arg);
	if (!data)
		return(-EINVAL);
	while(dspl) {
		if (&dspl->inst == inst)
			break;
		dspl = dspl->next;
	}

	switch(prim) {
	    case MGR_NEWLAYER | REQUEST:
		lock_HW(&dsp_lock, 0);
		ret = new_dsp(data, arg);
		unlock_HW(&dsp_lock);
		break;
	    case MGR_CONNECT | REQUEST:
		if (!dspl) {
			printk(KERN_WARNING "%s: given instance(%p) not in ilist.\n", __FUNCTION__, data);
			ret = -EINVAL;
			break;
		}
		ret = mISDN_ConnectIF(inst, arg);
		break;
	    case MGR_SETIF | REQUEST:
	    case MGR_SETIF | INDICATION:
		if (!dspl) {
			printk(KERN_WARNING "%s: given instance(%p) not in ilist.\n", __FUNCTION__, data);
			ret = -EINVAL;
			break;
		}
		ret = mISDN_SetIF(inst, arg, prim, dsp_from_up, dsp_from_down, dspl);
		break;
	    case MGR_DISCONNECT | REQUEST:
	    case MGR_DISCONNECT | INDICATION:
		if (!dspl) {
			printk(KERN_WARNING "%s: given instance(%p) not in ilist.\n", __FUNCTION__, data);
			ret = -EINVAL;
			break;
		}
		ret = mISDN_DisConnectIF(inst, arg);
		break;
	    case MGR_UNREGLAYER | REQUEST:
	    case MGR_RELEASE | INDICATION:
		if (!dspl) {
			printk(KERN_WARNING "%s: given instance(%p) not in ilist.\n", __FUNCTION__, data);
			ret = -EINVAL;
			break;
		}
		if (debug & DEBUG_DSP_MGR)
			printk(KERN_DEBUG "%s: release_dsp id %x\n", __FUNCTION__, dspl->inst.st->id);

		lock_HW(&dsp_lock, 0);
	    	release_dsp(dspl);
		unlock_HW(&dsp_lock);
	    	break;
	    default:
		printk(KERN_WARNING "%s: prim %x not handled\n", __FUNCTION__, prim);
		ret = -EINVAL;
		break;
	}
	return(ret);
}


/*
 * initialize DSP object
 */
static int dsp_init(void)
{
	int err;

	/* display revision */
	printk(KERN_INFO "mISDN_dsp: Audio DSP  Rev. %s (debug=0x%x)\n", mISDN_getrev(dsp_revision), debug);

	/* fill mISDN object (dsp_obj) */
	memset(&dsp_obj, 0, sizeof(dsp_obj));
#ifdef MODULE
	dsp_obj.owner = THIS_MODULE;
#endif
	dsp_obj.name = DSPName;
	dsp_obj.BPROTO.protocol[3] = ISDN_PID_L3_B_DSP;
	dsp_obj.own_ctrl = dsp_manager;
	dsp_obj.prev = NULL;
	dsp_obj.next = NULL;
	dsp_obj.ilist = NULL;

	/* initialize audio tables */
	silence = (options&DSP_OPT_ULAW)?0xff:0x2a;
	dsp_audio_law_to_s32 = (options&DSP_OPT_ULAW)?dsp_audio_ulaw_to_s32:dsp_audio_alaw_to_s32;
	dsp_audio_generate_s2law_table();
	dsp_audio_generate_mix_table();
	if (options & DSP_OPT_ULAW)
		dsp_audio_generate_ulaw_samples();
	dsp_audio_generate_volume_changes();

	/* init global lock */
	lock_HW_init(&dsp_lock);

	/* register object */
	if ((err = mISDN_register(&dsp_obj))) {
		printk(KERN_ERR "mISDN_dsp: Can't register %s error(%d)\n", DSPName, err);
		return(err);
	}

	return(0);
}


/*
 * cleanup DSP object during module removal
 */
static void dsp_cleanup(void)
{
	int err;

	if (debug & DEBUG_DSP_MGR)
		printk(KERN_DEBUG "%s: removing module\n", __FUNCTION__);

	if ((err = mISDN_unregister(&dsp_obj))) {
		printk(KERN_ERR "mISDN_dsp: Can't unregister Audio DSP error(%d)\n", 
			err);
	}
	if(dsp_obj.ilist) {
		printk(KERN_WARNING "mISDN_dsp: Audio DSP object inst list not empty.\n");
		while(dsp_obj.ilist)
			release_dsp(dsp_obj.ilist);
	}
	if (Conf_list) {
		printk(KERN_ERR "mISDN_dsp: Conference list not empty. Not all memory freed.\n");
	}
}

#ifdef MODULE
module_init(dsp_init);
module_exit(dsp_cleanup);
#endif

