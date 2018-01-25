/* Copyright (c) 2016, Marvell International Ltd.
 * All rights reserved.
 *
 * SPDX-License-Identifier:	 BSD-3-Clause
 */

#include <odp_posix_extensions.h>

#include <odp_musdk_internal.h>
#include <odp_packet_socket.h>
#include <odp_debug_internal.h>
#include <odp/helper/eth.h>
#include <odp/helper/ip.h>

#include <odp/api/ticketlock.h>
#include <odp_pool_internal.h>
#include <odp_packet_io_ring_internal.h>
#include <odp_classification_inlines.h>
#include <odp_classification_internal.h>

/* MUSDK PP2 public interfaces */
#include <drivers/mv_pp2.h>
#include <drivers/mv_pp2_hif.h>
#include <drivers/mv_pp2_bpool.h>
#include <drivers/mv_pp2_ppio.h>

#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <linux/ethtool.h>

#ifdef ODP_MVNMP_GUEST_MODE
#include <nmp_guest_utils.h>
#endif /* ODP_MVNMP_GUEST_MODE */

#define USE_LPBK_SW_RECYLCE

/* prefetch=2, tested to be optimal both for
   mvpp2_recv() & mvpp2_send() prefetch operations */
#define MVPP2_PREFETCH_SHIFT		2

/*#define USE_HW_BUFF_RECYLCE*/
#define MAX_NUM_PACKPROCS		1
#define PP2_SYSFS_RSS_PATH		"/sys/devices/platform/pp2/rss"
#define PP2_SYSFS_RSS_NUM_TABLES_FILE	"num_rss_tables"
#define PP2_MAX_BUF_STR_LEN		256
#define MAX_BUFFER_GET_RETRIES		10000

#define MV_DSA_MODE_BIT			(0x1ULL << 62)
#define MV_EXT_DSA_MODE_BIT		(0x1ULL << 63)

/* Macro for checking if a number is a power of 2 */
#define POWER_OF_2(_n)	(!((_n) & ((_n) - 1)))
#define NEXT_POWER_OF_2(_num, _new_num) \
do {					\
	if (POWER_OF_2(_num))		\
		_new_num = (_num);	\
	else {				\
		uint64_t tmp = (_num);	\
		_new_num = 1;		\
		while (tmp) {		\
			_new_num <<= 1;	\
			tmp >>= 1;	\
		}			\
	}				\
} while (0)

typedef struct port_desc {
	const char	*name;
	int		 pp_id;
	int		 ppio_id;
} port_desc_t;

/* Per thread unique ID used during run-time BM and HIF
 * resource indexing
 */
struct thd_info {
	int			 id;
	struct pp2_hif		*hif;
};

struct link_info {
	int speed;
	int duplex;
};

#ifdef ODP_MVNMP_GUEST_MODE
extern char *guest_prb_str;
struct pp2_info pp2_info;
#endif /* ODP_MVNMP_GUEST_MODE */

static uint32_t	used_bpools = MVPP2_BPOOL_RSRV;
static u16	used_hifs = MVPP2_HIF_RSRV;

/* Global lock used for control containers and other accesses */
static odp_ticketlock_t thrs_lock;
/* Per thread unique ID used during run-time BM and HIF
 * resource indexing
 */
static __thread int pp2_thr_id;
static struct thd_info	thds[MVPP2_TOTAL_NUM_HIFS] = {};

/* Get HIF object ID for this thread */
static inline int get_thr_id(void)
{
	return pp2_thr_id;
}

/* Reserve HIF or BM object ID for this thread */
static inline int thread_rsv_id(void)
{
	pp2_thr_id = odp_thread_id();
	return 0;
}

static int find_free_hif(void)
{
	int i;

	for (i = 0; i < MVPP2_TOTAL_NUM_HIFS; i++) {
		if (!((1 << i) & used_hifs)) {
			used_hifs |= (1 << i);
			break;
		}
	}

	if (i == MVPP2_TOTAL_NUM_HIFS) {
		ODP_ERR("no free HIF found!\n");
		return -1;
	}

	return i;
}

static inline struct pp2_hif* get_hif(int thread_id)
{
	return thds[thread_id].hif;
}

static int find_port_info(port_desc_t *port_desc)
{
	char		 name[20];
	u8		 pp, ppio;
	int		 err;

	if (!port_desc->name) {
		ODP_ERR("No port name given!\n");
		return -1;
	}

	memset(name, 0, sizeof(name));
	snprintf(name, sizeof(name), "%s", port_desc->name);
	err = pp2_netdev_get_ppio_info(name, &pp, &ppio);
	if (err) {
		ODP_ERR("PP2 Port %s not found!\n", port_desc->name);
		return err;
	}

	port_desc->ppio_id = ppio;
	port_desc->pp_id = pp;

	return 0;
}

static int find_free_bpool(void)
{
	int	i;

	odp_ticketlock_lock(&thrs_lock);
	for (i = 0; i < MVPP2_TOTAL_NUM_BPOOLS; i++) {
		if (!((uint64_t)(1 << i) & used_bpools)) {
			used_bpools |= (uint64_t)(1 << i);
			break;
		}
	}
	odp_ticketlock_unlock(&thrs_lock);
	if (i == MVPP2_TOTAL_NUM_BPOOLS)
		return -1;
	return i;
}

static int get_link_info(char *ifname, struct link_info *info)
{
	int rc, fd;
	struct ifreq ifr;
	struct ethtool_cmd get_cmd;

	if (!ifname)
		return -1;

	ifr.ifr_data = (void *)&get_cmd;

	/* "Get settings" */
	get_cmd.cmd = ETHTOOL_GSET;
	strcpy(ifr.ifr_name, ifname);
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		ODP_ERR("can't open socket: errno %d", errno);
		return -EFAULT;
	}

	rc = ioctl(fd, SIOCETHTOOL, (char *)&ifr);
	if (rc == -1) {
		ODP_ERR("ioctl request failed: errno %d\n", errno);
		close(fd);
		return -EFAULT;
	}
	close(fd);

	info->speed  = get_cmd.speed;
	info->duplex = get_cmd.duplex;
	return 0;
}

static void release_bpool(int bpool)
{
	odp_ticketlock_lock(&thrs_lock);
	used_bpools &= ~(uint64_t)(1 << bpool);
	odp_ticketlock_unlock(&thrs_lock);
}

#ifndef USE_HW_BUFF_RECYLCE
static inline void mvpp2_free_sent_buffers(struct pp2_hif *hif,
					   struct mvpp2_tx_shadow_q *shadow_q)
{
	struct buff_release_entry *entry;
	pktio_entry_t *pktio_entry;
	odp_pktio_t pktio;
	odp_packet_t pkt;
	u16 i, num_conf = 0;
#ifdef USE_LPBK_SW_RECYLCE
	u16 num_bufs = 0, skip_bufs = 0;
#endif

	num_conf = shadow_q->num_to_release;
	shadow_q->num_to_release = 0;

#ifndef USE_LPBK_SW_RECYLCE
	for (i = 0; i < num_conf; i++) {
		entry = &shadow_q->ent[shadow_q->read_ind];
		if (unlikely(!entry->buff.cookie && !entry->buff.addr)) {
			ODP_ERR("Shadow memory @%d: cookie(%lx), pa(%lx)!\n",
				shadow_q->read_ind,
				(u64)entry->buff.cookie,
				(u64)entry->buff.addr);
			shadow_q->read_ind++;
			shadow_q->size--;
			if (shadow_q->read_ind == SHADOW_Q_MAX_SIZE)
				shadow_q->read_ind = 0;
			continue;
		}
		shadow_q->read_ind++;
		shadow_q->size--;
		if (shadow_q->read_ind == SHADOW_Q_MAX_SIZE)
			shadow_q->read_ind = 0;

		if (likely(entry->bpool)) {
			pp2_bpool_put_buff(hif, entry->bpool, &entry->buff);
		} else {
			pkt = (odp_packet_t)entry->buff.cookie;
			odp_packet_free(pkt);
		}
	}
#else
	for (i = 0; i < num_conf; i++) {
		entry = &shadow_q->ent[shadow_q->read_ind + num_bufs];
		if (unlikely(!entry->buff.addr)) {
			ODP_ERR("Shadow memory @%d: cookie(%lx), pa(%lx)!\n",
				shadow_q->read_ind,
				(u64)entry->buff.cookie,
				(u64)entry->buff.addr);
			skip_bufs = 1;
			goto skip_buf;
		}

		if (unlikely(!entry->bpool)) {
			pkt = (odp_packet_t)entry->buff.cookie;
			odp_packet_free(pkt);
			skip_bufs = 1;
			goto skip_buf;
		}

		pktio = shadow_q->input_pktio[shadow_q->read_ind + num_bufs];
		pktio_entry = get_pktio_entry(pktio);
		if (unlikely(pktio_entry &&
			     pktio_entry->s.state == PKTIO_STATE_FREE)) {
			/* In case input pktio is in 'free' state, it means it
			 * was already closed and this buffer should be return
			 * to the ODP-POOL instead of the HW-Pool
			 */
			pkt = (odp_packet_t)
				((uintptr_t)entry->buff.cookie);
			odp_packet_hdr(pkt)->buf_hdr.ext_buf_free_cb = NULL;
			odp_packet_free(pkt);
			skip_bufs = 1;
			goto skip_buf;
		}

		num_bufs++;
		if (unlikely(shadow_q->read_ind + num_bufs == SHADOW_Q_MAX_SIZE))
			goto skip_buf;
		continue;
skip_buf:
		if (num_bufs)
			pp2_bpool_put_buffs(hif,
					    &shadow_q->ent[shadow_q->read_ind],
					    &num_bufs);
		num_bufs += skip_bufs;
		shadow_q->read_ind = (shadow_q->read_ind + num_bufs) & SHADOW_Q_MAX_SIZE_MASK;
		shadow_q->size -= num_bufs;
		num_bufs = 0;
		skip_bufs = 0;
	}
	if (num_bufs) {
		pp2_bpool_put_buffs(hif,
				    &shadow_q->ent[shadow_q->read_ind],
				    &num_bufs);
		shadow_q->read_ind = (shadow_q->read_ind + num_bufs) & SHADOW_Q_MAX_SIZE_MASK;
		shadow_q->size -= num_bufs;
	}
#endif /* USE_LPBK_SW_RECYLCE */
}

static inline
void mvpp2_check_n_free_sent_buffers(struct pp2_ppio *ppio,
				     struct pp2_hif *hif,
				     struct mvpp2_tx_shadow_q *shadow_q,
				     u8 tc)
{
	u16 num_conf = 0;

	pp2_ppio_get_num_outq_done(ppio, hif, tc, &num_conf);

	shadow_q->num_to_release += num_conf;

	if (odp_likely(shadow_q->num_to_release < BUFFER_RELEASE_BURST_SIZE))
		return;

	mvpp2_free_sent_buffers(hif, shadow_q);
}

#endif /* USE_HW_BUFF_RECYLCE */

static int mvpp2_sysfs_param_get(char *file)
{
	FILE *fp;
	char buf[PP2_MAX_BUF_STR_LEN];
	u32 param = 0, scanned;
	char *buf_p;

	fp = fopen(file, "r");
	if (!fp) {
		ODP_ERR("error opening file %s\n", file);
		return -1;
	}

	buf_p = fgets(buf, sizeof(buf), fp);
	if (!buf_p) {
		ODP_ERR("fgets error trying to read sysfs\n");
		fclose(fp);
		return -1;
	}

	scanned = sscanf(buf, "%d\n", &param);
	if (scanned != 1) {
		ODP_ERR("Invalid number of parameters read %s\n", buf);
		fclose(fp);
		return -1;
	}

	fclose(fp);
	return param;
}

static int mvpp2_rss_type_get(int hash_enable, odp_pktin_hash_proto_t hash_proto)
{
	/* TODO: once MUSDK API allows to configure hash per proto, need to change this
	 * function accordingly */
	if (hash_enable) {
		if (hash_proto.proto.ipv4 ||
		    hash_proto.proto.ipv6)
			return PP2_PPIO_HASH_T_2_TUPLE;

		if (hash_proto.proto.ipv4_udp ||
		    hash_proto.proto.ipv6_udp ||
		    hash_proto.proto.ipv4_tcp ||
		    hash_proto.proto.ipv6_tcp)
			return PP2_PPIO_HASH_T_5_TUPLE;
	}

	return PP2_PPIO_HASH_T_NONE;
}

static int mvpp2_free_buf(odp_buffer_t buf)
{
	odp_packet_t pkt = _odp_packet_from_buffer(buf);
	odp_packet_hdr_t *pkt_hdr;
	struct pp2_hif	*hif = get_hif(get_thr_id());
	pktio_entry_t *pktio_entry;
	struct mvpp2_bufs_stockpile *bufs_stockpile;
	int err = 0;

	if (unlikely(!hif)) {
		ODP_ERR("mvpp2_free_buf: invalid hif object for thread-%d!\n", get_thr_id());
		return -1;
	}

	pkt_hdr = odp_packet_hdr(pkt);

	if (unlikely(!pkt_hdr)) {
		ODP_ERR("mvpp2_free_buf: invalid pkt!\n");
		return -1;
	}

	if (unlikely(!pkt_hdr->input)) {
		ODP_ERR("mvpp2_free_buf: invalid input! frame_len: %d\n", pkt_hdr->frame_len);
		return -1;
	}

	pktio_entry = get_pktio_entry(pkt_hdr->input);
	if (unlikely(pktio_entry &&
		     pktio_entry->s.state == PKTIO_STATE_FREE)) {
		/* In case input pktio is in 'free' state, it means it was
		 * already closed and this buffer was saved in other pktio's
		 * tx queue. Therefor the buffer should be return to the
		 * ODP-POOL instead of the HW-Pool. this can be achevied by
		 * returning a non-zero return code.
		 */
		ODP_DBG("mvpp2_free_buf: pktio was closed! "
			"return the pkt to odp-pool\n");
		return 1;
	}
	pkt_hdr->input = NULL;

	bufs_stockpile =
		&pktio_entry->s.pkt_mvpp2.bufs_stockpile_array[get_thr_id()];
	bufs_stockpile->ent[bufs_stockpile->size].bpool =
		pktio_entry->s.pkt_mvpp2.bpool;
	bufs_stockpile->ent[bufs_stockpile->size].buff.cookie = (u64)pkt;
	bufs_stockpile->ent[bufs_stockpile->size++].buff.addr =
		mv_sys_dma_mem_virt2phys(odp_packet_head(pkt));
	if (bufs_stockpile->size == BUFFER_RELEASE_BURST_SIZE) {
		err = pp2_bpool_put_buffs(hif,
					  bufs_stockpile->ent,
					  &bufs_stockpile->size);
		bufs_stockpile->size = 0;
	}
	return err;
}

static int fill_bpool(odp_pool_t	 pool,
		      struct pp2_bpool	*bpool,
		      struct pp2_hif	*hif,
		      int		 num,
		      int		 alloc_len)
{
	int			 i, err = 0;
	odp_packet_hdr_t	*pkt_hdr;
#ifndef USE_LPBK_SW_RECYLCE
	odp_packet_t		 pkt;
	struct pp2_buff_inf	 buff_inf;
#else
	odp_packet_t		 *pkt;
	struct buff_release_entry buff_array[MVPP2_TXQ_SIZE];
	int j = 0, err2 = 0;
	u16 final_num, num_bufs;
#endif

#ifndef USE_LPBK_SW_RECYLCE
	for (i = 0; i < num; i++) {
		pkt = odp_packet_alloc(pool, alloc_len);
		if (pkt == ODP_PACKET_INVALID) {
			ODP_ERR("Allocated invalid pkt; skipping!\n");
			continue;
		}

		if (!odp_packet_head(pkt)) {
			ODP_ERR("Allocated invalid pkt (no buffer)!\n");
			continue;
		}
		pkt_hdr = odp_packet_hdr(pkt);
		if (pkt_hdr->buf_hdr.ext_buf_free_cb) {
			ODP_ERR("pkt(%p)  ext_buf_free_cb was set; skipping\n", pkt);
			continue;
		}
		pkt_hdr->buf_hdr.ext_buf_free_cb = mvpp2_free_buf;

		buff_inf.cookie = (u64)pkt;
		buff_inf.addr   =
			mv_sys_dma_mem_virt2phys(odp_packet_head(pkt));
		err = pp2_bpool_put_buff(hif, bpool, &buff_inf);
		if (err != 0)
			return err;
	}
#else
	pkt = malloc(num * sizeof(odp_packet_t));

	if ((final_num = packet_alloc_multi(pool, alloc_len, pkt, num)) != num)
		ODP_ERR("Allocated %d packets instead of %d!\n", final_num, num);
	i = 0;
	while ((i < final_num) && (pkt[i] == ODP_PACKET_INVALID)) {
		ODP_ERR("Allocated invalid pkt, pkt_num %d out of %d; skipping!\n", i, final_num);
		i++;
	}
	if (unlikely(i == final_num)) {
		err = -1;
		goto err;
	}

	for (; i < final_num; i++) {
		if (pkt[i] == ODP_PACKET_INVALID) {
			ODP_ERR("Allocated invalid pkt; skipping!\n");
			continue;
		}

		if (!odp_packet_head(pkt[i])) {
			ODP_ERR("Allocated invalid pkt (no buffer)!\n");
			continue;
		}

		pkt_hdr = odp_packet_hdr(pkt[i]);
		if (pkt_hdr->buf_hdr.ext_buf_free_cb) {
			ODP_ERR("pkt(%p)  ext_buf_free_cb was set; skipping\n", pkt[i]);
			continue;
		}
		pkt_hdr->buf_hdr.ext_buf_free_cb = mvpp2_free_buf;

		buff_array[j].bpool = bpool;
		buff_array[j].buff.cookie = (u64)pkt[i];
		buff_array[j].buff.addr =
			mv_sys_dma_mem_virt2phys(odp_packet_head(pkt[i]));
		j++;
		if (j == MVPP2_TXQ_SIZE) {
			num_bufs = j;
			err2 = pp2_bpool_put_buffs(hif, buff_array, &num_bufs);
			j = 0;
		}
	}
	num_bufs = j;
	err2 = pp2_bpool_put_buffs(hif, buff_array, &num_bufs);
err:
	free(pkt);
	if (err2)
		return err2;
	return err;

#endif

	return 0;
}

static void flush_bpool(struct pp2_bpool *bpool, struct pp2_hif *hif)
{
	u32 i, buf_num, err = 0;
	struct pp2_buff_inf buff;
	odp_packet_t pkt;
	odp_packet_hdr_t *pkt_hdr;

	pp2_bpool_get_num_buffs(bpool, &buf_num);
	for (i = 0; i < buf_num; i++) {
		err = 0;
		while (pp2_bpool_get_buff(hif, bpool, &buff)) {
			err++;
			if (err == MAX_BUFFER_GET_RETRIES)
				break;
		}

		if (err) {
			if (err == MAX_BUFFER_GET_RETRIES) {
				ODP_ERR("flush_pool: p2_id=%d, pool_id=%d: Got NULL buf (%d of %d)\n",
					bpool->pp2_id, bpool->id, i, buf_num);
				continue;
			}
			ODP_DBG("flush_pool: p2_id=%d, pool_id=%d: Got buf (%d of %d) after %d retries\n",
				bpool->pp2_id, bpool->id, i, buf_num, err);
		}
		pkt = (odp_packet_t)buff.cookie;
		pkt_hdr = odp_packet_hdr(pkt);
		pkt_hdr->buf_hdr.ext_buf_free_cb = NULL;
		odp_packet_free(pkt);
	}
}

static int mvpp2_init_global(void)
{
	struct pp2_init_params	pp2_params;
	int			err;
	char			file[PP2_MAX_BUF_STR_LEN];
	int			num_rss_tables;

	/* Master thread. Init locks */
	odp_ticketlock_init(&thrs_lock);

	memset(&pp2_params, 0, sizeof(pp2_params));
	/* TODO: the following lines should be dynamic! */
	pp2_params.hif_reserved_map = MVPP2_HIF_RSRV;
	pp2_params.bm_pool_reserved_map = MVPP2_BPOOL_RSRV;

	sprintf(file, "%s/%s", PP2_SYSFS_RSS_PATH, PP2_SYSFS_RSS_NUM_TABLES_FILE);
	num_rss_tables = mvpp2_sysfs_param_get(file);
	pp2_params.rss_tbl_reserved_map = (1 << num_rss_tables) - 1;

#ifdef ODP_MVNMP_GUEST_MODE
	guest_util_get_relations_info(guest_prb_str, &pp2_info);
	if (pp2_info.num_ports) {
		/* PP2 was configured on master. need to skip HW and get
		 * relevant pools mask
		 */
		/* TODO - create pools mask */
		pp2_params.skip_hw_init = 1;
	}
#endif /* ODP_MVNMP_GUEST_MODE */

	err = pp2_init(&pp2_params);
	if (err != 0) {
		ODP_ERR("PP2 init failed (%d)!\n", err);
		return -1;
	}

	return 0;
}

static int mvpp2_term_global(void)
{
	pp2_deinit();
	return 0;
}

static int mvpp2_init_local(void)
{
	struct pp2_hif_params		hif_params;
	char				name[15];
	int				hif_id, err, thread_id;

	/* Egress worker thread. Provide an unique ID for resource use */
	thread_rsv_id();

	thread_id = get_thr_id();

	hif_id = find_free_hif();
	if (hif_id < 0) {
		ODP_ERR("No available HIFs for this thread "
			"(used_hifs: 0x%X)!!!\n", used_hifs);
		return -1;
	}
	memset(name, 0, sizeof(name));
	snprintf(name, sizeof(name), "hif-%d", hif_id);
	memset(&hif_params, 0, sizeof(hif_params));
	hif_params.match = name;
	hif_params.out_size = MVPP2_TXQ_SIZE;

	err = pp2_hif_init(&hif_params, &thds[thread_id].hif);
	if (err != 0 || !thds[thread_id].hif) {
		ODP_ERR("HIF init failed!\n");
		return -1;
	}

	return 0;
}

/**
 * Initialize capability values
 *
 * @param pktio_entry    Packet IO entry
 */
static void init_capability(pktio_entry_t *pktio_entry)
{
	odp_pktio_capability_t *capa = &pktio_entry->s.pkt_mvpp2.capa;

	memset(capa, 0, sizeof(odp_pktio_capability_t));

	capa->max_input_queues = MVPP2_MAX_NUM_RX_QS_PER_PORT;
	capa->max_output_queues = MVPP2_MAX_NUM_TX_TCS_PER_PORT;
	capa->loop_supported = true;
	capa->set_op.op.promisc_mode = true;
	odp_pktio_config_init(&capa->config);

	/* L3, L4 checksum offload on TX */
	capa->config.pktout.bit.ipv4_chksum = 1;
	capa->config.pktout.bit.udp_chksum = 1;
	capa->config.pktout.bit.tcp_chksum = 1;
	/* TODO - need to check if HW perfrom checksum generation for this type
	* capa->config.pktout.bit.sctp_chksum = 1;
	*/

	/* L3, L4 checksum offload on RX */
	capa->config.pktin.bit.ipv4_chksum = 1;
	capa->config.pktin.bit.udp_chksum = 1;
	capa->config.pktin.bit.tcp_chksum = 1;
	/* HW alwyas generate checksum error for non UDP/TCP frames
	* capa->config.pktin.bit.sctp_chksum = 1;
	*/
	capa->config.pktin.bit.drop_ipv4_err = 1;
	/* TODO - probably need to parse it in SW to support it
	* capa->config.pktin.bit.drop_ipv6_err = 1;
	*/
	capa->config.pktin.bit.drop_udp_err = 1;
	capa->config.pktin.bit.drop_tcp_err = 1;
	/* TODO - need to check if HW perfrom checksum validation for this type.
	* if so, in SW need to identify it by looking at ip-protocol.
	* capa->config.pktin.bit.drop_sctp_err = 1;
	*/

	/* DSA mode capability
	* Marvell proprietary. Use upper two bits in odp_pktout_queue_param_t
	* (not in use by ODP) to indicate MUSDK pktio DSA awareness capability
	*/
	capa->config.pktout.all_bits |= (uint64_t)MV_DSA_MODE_BIT;
	capa->config.pktout.all_bits |= (uint64_t)MV_EXT_DSA_MODE_BIT;
}

static int mvpp2_open(odp_pktio_t pktio ODP_UNUSED,
		      pktio_entry_t *pktio_entry,
		      const char *devname,
		      odp_pool_t pool)
{
#ifdef ODP_MVNMP_GUEST_MODE
	struct pp2_ppio_capabilities	ppio_capa;
	struct pp2_bpool_capabilities	bpool_capa;
	int				buf_num;
	u32				i, max_num_buffs = 0, max_buf_len = 0;
	struct pp2_bpool		*bpool;
#else
	struct pp2_bpool_params		bpool_params;
	int				pool_id;
#endif /* ODP_MVNMP_GUEST_MODE */
	port_desc_t			port_desc;
	char				name[15];
	int				err;

	if (strlen(devname) > sizeof(name) - 1) {
		ODP_ERR("Port name (%s) too long!\n", devname);
		return -1;
	}

	/* Set port name to pktio_entry */
	snprintf(pktio_entry->s.name, sizeof(pktio_entry->s.name), "%s", devname);

	memset(&port_desc, 0, sizeof(port_desc));
	port_desc.name = pktio_entry->s.name;
	err = find_port_info(&port_desc);
	if (err != 0) {
		ODP_ERR("Port info not found!\n");
		return -1;
	}

	/* Init pktio entry */
	memset(&pktio_entry->s.pkt_mvpp2, 0, sizeof(pkt_mvpp2_t));
	pktio_entry->s.pkt_mvpp2.mtu = MVPP2_DFLT_MTU;

	/* Associate this pool with this pktio */
	pktio_entry->s.pkt_mvpp2.pool = pool;

	init_capability(pktio_entry);

#ifdef ODP_MVNMP_GUEST_MODE
	/* TODO - create ppio-match and find if exist in pp2_info
	 * currently assumes only one ppio exist and use its match */

	for (i = 0; i < pp2_info.port_info[0].num_bpools; i++) {
		struct pp2_ppio_bpool_info *bpool_info =
			&pp2_info.port_info[0].bpool_info[i];

		err = pp2_bpool_probe(bpool_info->bpool_name,
				      guest_prb_str,
				      &bpool);
		if (err) {
			ODP_ERR("pp2_bpool_probe failed for %s\n",
				bpool_info->bpool_name);
			return err;
		}

		err = pp2_bpool_get_capabilities(bpool, &bpool_capa);
		if (err) {
			ODP_ERR("pp2_bpool_get_capabilities failed for %s\n",
				bpool_info->bpool_name);
			return err;
		}
		ODP_PRINT("pp2-bpool %s was probed\n", bpool_info->bpool_name);
		/* check pool's buf size and use the biggest one */
		if (bpool_capa.buff_len > max_buf_len) {
			max_buf_len = bpool_capa.buff_len;
			max_num_buffs = bpool_capa.max_num_buffs;
			pktio_entry->s.pkt_mvpp2.bpool = bpool;
		}
	}
	err = pp2_ppio_probe(pp2_info.port_info[0].ppio_name, guest_prb_str,
			     &pktio_entry->s.pkt_mvpp2.ppio);
	if (err) {
		ODP_ERR("pp2_ppio_probe failed for %s\n",
			pp2_info.port_info[0].ppio_name);
		return err;
	}

	err = pp2_ppio_get_capabilities(pktio_entry->s.pkt_mvpp2.ppio,
					&ppio_capa);
	if (err) {
		ODP_ERR("pp2_ppio_get_capabilities failed for %s\n",
			pp2_info.port_info[0].ppio_name);
		return err;
	}

	pool_t *poole = pool_entry_from_hdl(pool);

	if (poole->data_size < max_buf_len) {
		ODP_ERR("pool buffer's size is too small!\n");
		return -1;
	}

	buf_num = MIN((poole->num / ODP_CONFIG_PKTIO_ENTRIES), max_num_buffs);

	/* Allocate maximum sized packets */
	/* Allocate 'buf_num' of the SW pool into the HW pool;
	 * i.e. allow only several ports sharing the same SW pool
	 */
	err = fill_bpool(pktio_entry->s.pkt_mvpp2.pool,
			 pktio_entry->s.pkt_mvpp2.bpool, get_hif(get_thr_id()),
			 buf_num, pktio_entry->s.pkt_mvpp2.mtu);
	if (err != 0) {
		ODP_ERR("can't fill port's pool with buffs!\n");
		return -1;
	}
#else
	/* Allocate a dedicated pool for this port */
	pool_id = find_free_bpool();
	if (pool_id < 0) {
		ODP_ERR("free pool not found!\n");
		return -1;
	}

	memset(name, 0, sizeof(name));
	snprintf(name, sizeof(name), "pool-%d:%d", port_desc.pp_id, pool_id);
	memset(&bpool_params, 0, sizeof(bpool_params));
	bpool_params.match = name;
	/* TODO: is this correct? */
	bpool_params.buff_len = pktio_entry->s.pkt_mvpp2.mtu;
	NEXT_POWER_OF_2(bpool_params.buff_len, bpool_params.buff_len);
	err = pp2_bpool_init(&bpool_params, &pktio_entry->s.pkt_mvpp2.bpool);
	if (err != 0) {
		ODP_ERR("BPool init failed!\n");
		return -1;
	}
	if (!pktio_entry->s.pkt_mvpp2.bpool) {
		ODP_ERR("BPool init failed!\n");
		return -1;
	}
	pktio_entry->s.pkt_mvpp2.bpool_id = pool_id;

	pktio_entry->s.pkt_mvpp2.sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (pktio_entry->s.pkt_mvpp2.sockfd == -1) {
		ODP_ERR("Cannot get device control socket\n");
		return -1;
	}

	/* TODO: temporary until we have ppio_get_mac_addr() */
	err = mac_addr_get_fd(pktio_entry->s.pkt_mvpp2.sockfd, devname, pktio_entry->s.pkt_mvpp2.if_mac);
	if (err != 0) {
		ODP_ERR("Cannot get device MAC address (%d)!\n", err);
		return -1;
	}
#endif /* ODP_MVNMP_GUEST_MODE */

	ODP_DBG("port '%s' is opened\n", devname);

	/* Set default num queues - will be updated at config */
	pktio_entry->s.num_in_queue = 0;
	pktio_entry->s.num_out_queue = 0;

	memset(pktio_entry->s.pkt_mvpp2.shadow_qs, 0, sizeof(pktio_entry->s.pkt_mvpp2.shadow_qs));

	return 0;
}

static int mvpp2_close(pktio_entry_t *pktio_entry)
{
	int i, tc = 0;
	struct pp2_hif *hif = thds[get_thr_id()].hif;
	struct mvpp2_tx_shadow_q *shadow_q;
	struct mvpp2_bufs_stockpile *bufs_stockpile;
	pkt_mvpp2_t *mvpp2 = &pktio_entry->s.pkt_mvpp2;

	mvpp2_deinit_cls(pktio_entry);

	if (mvpp2->ppio) {
		for (i = 0; i < MVPP2_TOTAL_NUM_HIFS; i++) {
			shadow_q = &mvpp2->shadow_qs[i][tc];
			shadow_q->num_to_release = shadow_q->size;
			mvpp2_free_sent_buffers(hif, shadow_q);
			bufs_stockpile = &mvpp2->bufs_stockpile_array[i];
			if (bufs_stockpile->size)
				pp2_bpool_put_buffs(hif,
						    bufs_stockpile->ent,
						    &bufs_stockpile->size);
		}
#ifdef ODP_MVNMP_GUEST_MODE
		/* Deinit the PP2 port */
		pp2_ppio_remove(mvpp2->ppio);
#else
		/* Deinit the PP2 port */
		pp2_ppio_deinit(mvpp2->ppio);
#endif /* ODP_MVNMP_GUEST_MODE */
	}
	flush_bpool(mvpp2->bpool, hif);
#ifdef ODP_MVNMP_GUEST_MODE
	pp2_bpool_remove(mvpp2->bpool);
#else
	pp2_bpool_deinit(mvpp2->bpool);
	release_bpool(mvpp2->bpool_id);
#endif /* ODP_MVNMP_GUEST_MODE */

	ODP_DBG("port '%s' was closed\n", pktio_entry->s.name);
	return 0;
}

static int mvpp2_start(pktio_entry_t *pktio_entry)
{
#ifndef ODP_MVNMP_GUEST_MODE
	char				name[15];
	port_desc_t			port_desc;
	int				i, j, err;
	struct pp2_ppio_params		port_params;
	struct pp2_ppio_inq_params
		inq_params[MVPP2_MAX_NUM_RX_QS_PER_PORT];
	struct pp2_ppio_tc_params	*tcs_params;
	struct odp_pktio_config_t *config = &pktio_entry->s.config;
	odp_pool_t pool;
	int  buf_num, rx_queue_size;
	struct pp2_hif	*hif = get_hif(get_thr_id());
	struct link_info info;
#endif /* !ODP_MVNMP_GUEST_MODE */

	if (!pktio_entry->s.num_in_queue && !pktio_entry->s.num_out_queue) {
		ODP_ERR("No input and output queues configured!\n");
		return -1;
	}

#ifndef ODP_MVNMP_GUEST_MODE
	if (!pktio_entry->s.pkt_mvpp2.ppio) {
		port_desc.name = pktio_entry->s.name;
		err = find_port_info(&port_desc);
		if (err != 0) {
			ODP_ERR("Port info not found!\n");
			return -1;
		}

		memset(name, 0, sizeof(name));
		snprintf(name, sizeof(name), "ppio-%d:%d", port_desc.pp_id, port_desc.ppio_id);
		memset(&port_params, 0, sizeof(port_params));
		port_params.match = name;
		port_params.type = PP2_PPIO_T_NIC;
		port_params.maintain_stats = true;
		if (config->pktout.all_bits & MV_DSA_MODE_BIT)
			port_params.eth_start_hdr = PP2_PPIO_HDR_ETH_DSA;
		else if (config->pktout.all_bits & MV_EXT_DSA_MODE_BIT)
			port_params.eth_start_hdr = PP2_PPIO_HDR_ETH_EXT_DSA;
		else
			port_params.eth_start_hdr = PP2_PPIO_HDR_ETH;

		port_params.inqs_params.hash_type =
				pktio_entry->s.pkt_mvpp2.hash_type;

		ODP_DBG("config.pktio %lx, eth_start_hdr %d\n",
			config->pktout.all_bits,
			port_params.eth_start_hdr);
		ODP_DBG("hash_type %d\n", port_params.inqs_params.hash_type);

		err = get_link_info(pktio_entry->s.name, &info);
		if (err != 0) {
			ODP_ERR("Can't get parameters from link %s!\n",
				pktio_entry->s.name);
			return -1;
		}

		if (info.speed == 10000)
			rx_queue_size = MVPP2_RXQ_SIZE_10G;
		else
			rx_queue_size = MVPP2_RXQ_SIZE_1G;

		port_params.inqs_params.num_tcs =
			MVPP2_MAX_NUM_RX_HASH_TCS_PER_PORT;
		if (pktio_entry->s.cls_enabled)
			port_params.inqs_params.num_tcs =
				MVPP2_MAX_NUM_RX_TCS_PER_PORT;
		for (i = 0; i < port_params.inqs_params.num_tcs; i++) {
			tcs_params = &port_params.inqs_params.tcs_params[i];
			tcs_params->pkt_offset = MVPP2_PACKET_OFFSET;
			tcs_params->num_in_qs = pktio_entry->s.num_in_queue;
			if (pktio_entry->s.cls_enabled)
				tcs_params->num_in_qs = 1;
			memset(inq_params, 0, sizeof(inq_params));
			for (j = 0; j < tcs_params->num_in_qs; j++) {
				inq_params[j].size = rx_queue_size;
				inq_params[j].mem = NULL;
				inq_params[j].tc_pools_mem_id_index = 0;
			}
			tcs_params->inqs_params = inq_params;
			tcs_params->pools[0][0] =
				pktio_entry->s.pkt_mvpp2.bpool;
		}
		port_params.outqs_params.num_outqs =
			MVPP2_MAX_NUM_TX_TCS_PER_PORT;
		for (i = 0; i < port_params.outqs_params.num_outqs; i++) {
			port_params.outqs_params.outqs_params[i].size = MVPP2_TXQ_SIZE;
			port_params.outqs_params.outqs_params[i].weight = 1;
		}
		err = pp2_ppio_init(&port_params, &pktio_entry->s.pkt_mvpp2.ppio);
		if (err != 0) {
			ODP_ERR("PP-IO init failed!\n");
			return -1;
		}
		if (!pktio_entry->s.pkt_mvpp2.ppio) {
			ODP_ERR("PP-IO init failed!\n");
			return -1;
		}

		pool = pktio_entry->s.pkt_mvpp2.pool;
		pool_t *poole = pool_entry_from_hdl(pool);

		if (!pktio_entry->s.num_in_queue)
			buf_num = poole->num / ODP_CONFIG_PKTIO_ENTRIES;
		else
			buf_num = MIN((poole->num /
				      ODP_CONFIG_PKTIO_ENTRIES),
				      (pktio_entry->s.num_in_queue *
				      rx_queue_size));

		/* Allocate maximum sized packets */
		/* Allocate 'buf_num' of the SW pool into the HW pool;
		* i.e. allow only several ports sharing the same SW pool
		*/
		err = fill_bpool(pktio_entry->s.pkt_mvpp2.pool,
				 pktio_entry->s.pkt_mvpp2.bpool, hif,
				 buf_num, pktio_entry->s.pkt_mvpp2.mtu);
		if (err != 0) {
			ODP_ERR("can't fill port pool with buffs!\n");
			return -1;
		}

		pktio_entry->s.ops->stats_reset(pktio_entry);

		ODP_PRINT("PktIO PP2 has %d RxTCs and %d TxTCs\n",
			  port_params.inqs_params.num_tcs,
			  port_params.outqs_params.num_outqs);
		ODP_PRINT("\t mapped to %d RxQs and %d TxQs!!!\n",
			  pktio_entry->s.pkt_mvpp2.num_inqs,
			  pktio_entry->s.num_out_queue);

		mvpp2_init_cls(pktio_entry);
		mvpp2_update_qos(pktio_entry);
	}

	pp2_ppio_set_loopback(pktio_entry->s.pkt_mvpp2.ppio, pktio_entry->s.config.enable_loop);
	pp2_ppio_enable(pktio_entry->s.pkt_mvpp2.ppio);
#endif /* !ODP_MVNMP_GUEST_MODE */

	ODP_DBG("port '%s' is ready\n", pktio_entry->s.name);
	return 0;
}

static int mvpp2_stop(pktio_entry_t *pktio_entry)
{
	/* Set the PP2 port in standby-mode.
	 * Ingress and egress disabled
	 */
	pp2_ppio_disable(pktio_entry->s.pkt_mvpp2.ppio);
	ODP_DBG("port '%s' was stopped\n", pktio_entry->s.name);

	return 0;
}

static int mvpp2_capability(pktio_entry_t *pktio_entry,
			    odp_pktio_capability_t *capa)
{
	*capa = pktio_entry->s.pkt_mvpp2.capa;
	return 0;
}

static int mvpp2_input_queues_config(pktio_entry_t *pktio_entry,
				     const odp_pktin_queue_param_t *param)
{
	u8 i, max_num_hwrx_qs_per_inq;

#ifndef ODP_MVNMP_GUEST_MODE
	if (pktio_entry->s.pkt_mvpp2.ppio) {
		ODP_ERR("Port already initialized, "
			"configuration cannot be changed\n");
		return -ENOTSUP;
	}
#endif /* !ODP_MVNMP_GUEST_MODE */

	if ((param->classifier_enable == 1) && (param->hash_enable == 1)) {
		ODP_ERR("Either classifier or hash may be enabled\n");
		return -1;
	}

	pktio_entry->s.pkt_mvpp2.num_inqs = pktio_entry->s.num_in_queue;
	pktio_entry->s.cls_enabled = param->classifier_enable;
	pktio_entry->s.pkt_mvpp2.hash_type =
		mvpp2_rss_type_get(param->hash_enable, param->hash_proto);

	/* each logical queue is mapped to one phy queue */
	max_num_hwrx_qs_per_inq = 1;
	for (i = 0; i < pktio_entry->s.pkt_mvpp2.num_inqs; i++) {
		struct inq_info	*inq = &pktio_entry->s.pkt_mvpp2.inqs[i];

		if (param->classifier_enable) {
			/* Assumes classification is enabled
			 * (TCs are bigger than 1), and RSS is disabled
			 */
			inq->first_tc = i;
			inq->first_qid = 0;
		} else {
			/* Assumes classification is disabled (TC's is only 1),
			 * and RSS may be enabled or not
			 */
			inq->first_tc = 0;
			inq->first_qid = (i * max_num_hwrx_qs_per_inq);
		}
		inq->num_tcs = 1;
		inq->next_qid = inq->first_qid;
		inq->num_qids =	max_num_hwrx_qs_per_inq;
		ODP_DBG("inqs[%d] first tc %d, num_tc %d, "
			"first_qid %d, num_qids %d\n", i,
		       inq->first_tc,
		       inq->num_tcs,
		       inq->first_qid,
		       inq->num_qids);

		/* Scheduler synchronizes input queue polls. Only single thread
		* at a time polls a queue
		*/
		if (pktio_entry->s.param.in_mode == ODP_PKTIN_MODE_SCHED)
			inq->lockless = 1;
		else
			inq->lockless =
			(param->op_mode == ODP_PKTIO_OP_MT_UNSAFE);
		if (!inq->lockless)
			odp_ticketlock_init(&inq->lock);
	}

	return 0;
}

static int mvpp2_output_queues_config(pktio_entry_t *pktio_entry,
				      const odp_pktout_queue_param_t *param)
{
	u32 max_num_hwrx_qs, num_txq = param->num_queues;

	ODP_ASSERT(num_txq == pktio_entry->s.num_out_queue);

#ifndef ODP_MVNMP_GUEST_MODE
	if (pktio_entry->s.pkt_mvpp2.ppio) {
		ODP_ERR("Port already initialized, configuration cannot be changed\n");
		return -ENOTSUP;
	}
#endif /* !ODP_MVNMP_GUEST_MODE */

	/* TODO: only support now RSS; no support for QoS; how to translate rxq_id to tc/qid???? */
	max_num_hwrx_qs = MVPP2_MAX_NUM_TX_TCS_PER_PORT;
	if (pktio_entry->s.num_out_queue > max_num_hwrx_qs) {
		ODP_ERR("Too many Out-Queues mapped (%d vs %d)!\n",
			pktio_entry->s.num_out_queue,
			max_num_hwrx_qs);
		return -1;
	}

	return 0;
}

static int mvpp2_stats(pktio_entry_t *pktio_entry,
		       odp_pktio_stats_t *stats)
{
	struct pp2_ppio_statistics ppio_stats;
	int err;

	if (!pktio_entry->s.pkt_mvpp2.ppio) {
		memset(stats, 0, sizeof(odp_pktio_stats_t));
		return 0;
	}

	err = pp2_ppio_get_statistics(pktio_entry->s.pkt_mvpp2.ppio,
				      &ppio_stats,
				      false);
	if (err)
		return -1;
	stats->in_octets = ppio_stats.rx_bytes;
	stats->in_ucast_pkts = ppio_stats.rx_unicast_packets;
	stats->in_discards = ppio_stats.rx_fullq_dropped +
			     ppio_stats.rx_bm_dropped +
			     ppio_stats.rx_early_dropped +
			     ppio_stats.rx_fifo_dropped +
			     ppio_stats.rx_cls_dropped;
	stats->in_errors = ppio_stats.rx_errors +
			   pktio_entry->s.stats.in_errors;
	stats->in_unknown_protos = 0;
	stats->out_octets = ppio_stats.tx_bytes;
	stats->out_ucast_pkts = ppio_stats.tx_unicast_packets;
	stats->out_discards = 0;
	stats->out_errors = ppio_stats.tx_errors;

	return 0;
}

static int mvpp2_stats_reset(pktio_entry_t *pktio_entry)
{
	if (pktio_entry->s.pkt_mvpp2.ppio)
		pp2_ppio_get_statistics(pktio_entry->s.pkt_mvpp2.ppio,
					NULL,
					true);
	/* Some HW counters needs to be updated with SW counters.
	* For that we have the statistics structure as part
	* of the PKTIO structure.
	* Currently only in_errors is being updated in receive function */
	pktio_entry->s.stats.in_errors = 0;

	return 0;
}

static uint32_t mvpp2_mtu_get(pktio_entry_t *pktio_entry)
{
	return pktio_entry->s.pkt_mvpp2.mtu;
}

static int mvpp2_mac_get(pktio_entry_t *pktio_entry,
			 void *mac_addr)
{
	memcpy(mac_addr, pktio_entry->s.pkt_mvpp2.if_mac, ETH_ALEN);
	return ETH_ALEN;
}

static int mvpp2_promisc_mode_set(pktio_entry_t *pktio_entry,  int enable)
{
	int err;

	if (!pktio_entry->s.pkt_mvpp2.ppio) {
		err = promisc_mode_set_fd(pktio_entry->s.pkt_mvpp2.sockfd,
					  pktio_entry->s.name,
					  enable);
	} else {
		err = pp2_ppio_set_promisc(pktio_entry->s.pkt_mvpp2.ppio,
					   enable);
		if (err)
			err = -1;
	}

	return err;
}

static int mvpp2_promisc_mode_get(pktio_entry_t *pktio_entry)
{
	int err, enable = 0;

	if (!pktio_entry->s.pkt_mvpp2.ppio) {
		enable = promisc_mode_get_fd(pktio_entry->s.pkt_mvpp2.sockfd, pktio_entry->s.name);
	} else {
		err = pp2_ppio_get_promisc(pktio_entry->s.pkt_mvpp2.ppio,
					   &enable);
		if (err)
			enable = -1;
	}

	return enable;
}

static int mvpp2_link_status(pktio_entry_t *pktio_entry)
{
	/* Returns false (zero) if link is down or true(one) if link is up */
	int err, link_up = 0;

	if (!pktio_entry->s.pkt_mvpp2.ppio)
		return 0;

	err = pp2_ppio_get_link_state(pktio_entry->s.pkt_mvpp2.ppio, &link_up);
	if (err)
		link_up = -1;

	return link_up;
}

static inline uint8_t ipv6_get_next_hdr(const uint8_t *parseptr,
					uint32_t offset)
{
	const _odp_ipv6hdr_t *ipv6 = (const _odp_ipv6hdr_t *)parseptr;
	const _odp_ipv6hdr_ext_t *ipv6ext;

	/* Skip past IPv6 header */
	offset   += sizeof(_odp_ipv6hdr_t);
	parseptr += sizeof(_odp_ipv6hdr_t);

	/* Skip past any IPv6 extension headers */
	if (ipv6->next_hdr == _ODP_IPPROTO_HOPOPTS ||
	    ipv6->next_hdr == _ODP_IPPROTO_ROUTE) {
		do  {
			ipv6ext = (const _odp_ipv6hdr_ext_t *)parseptr;
			uint16_t extlen = 8 + ipv6ext->ext_len * 8;

			offset   += extlen;
			parseptr += extlen;
		} while ((ipv6ext->next_hdr == _ODP_IPPROTO_HOPOPTS ||
			  ipv6ext->next_hdr == _ODP_IPPROTO_ROUTE));
		return ipv6ext->next_hdr;
	}

	return ipv6->next_hdr;
}

static inline void parse_l2(odp_packet_hdr_t *pkt_hdr,
			    struct pp2_ppio_desc *desc)
{
	enum pp2_inq_vlan_tag tag;
	enum pp2_inq_l2_cast_type cast;

	pkt_hdr->p.input_flags.eth = 1;
	pkt_hdr->p.input_flags.l2 = 1;

	pp2_ppio_inq_desc_get_vlan_tag(desc, &tag);
	pkt_hdr->p.input_flags.vlan = (tag != PP2_INQ_VLAN_TAG_NONE);
	pkt_hdr->p.input_flags.vlan_qinq = (tag == PP2_INQ_VLAN_TAG_DOUBLE);
	pp2_ppio_inq_desc_get_l2_cast_info(desc, &cast);
	pkt_hdr->p.input_flags.eth_mcast = (cast == PP2_INQ_L2_MULTICAST);
	pkt_hdr->p.input_flags.eth_bcast = (cast == PP2_INQ_L2_BROADCAST);
}

static inline void parse_l3(odp_packet_hdr_t *pkt_hdr,
			    enum pp2_inq_l3_type type,
			    u8 offset,
			    struct pp2_ppio_desc *desc)
{
	enum pp2_inq_l3_cast_type cast;

	if (odp_unlikely(type == PP2_INQ_L3_TYPE_NA))
		return;

	pkt_hdr->p.l3_offset = offset;
	pkt_hdr->p.input_flags.l3 = 1;
	pkt_hdr->p.input_flags.ipv4 =
		(type <= PP2_INQ_L3_TYPE_IPV4_TTL_ZERO);
	pkt_hdr->p.input_flags.ipopt =
		((type == PP2_INQ_L3_TYPE_IPV4_OK) ||
		 (type == PP2_INQ_L3_TYPE_IPV6_EXT));
	pkt_hdr->p.input_flags.ipv6 =
		((type == PP2_INQ_L3_TYPE_IPV6_NO_EXT) ||
		 (type == PP2_INQ_L3_TYPE_IPV6_EXT));
	pkt_hdr->p.input_flags.arp =
		(type == PP2_INQ_L3_TYPE_ARP);
	pkt_hdr->p.input_flags.ipfrag = pp2_ppio_inq_desc_get_ip_isfrag(desc);

	pp2_ppio_inq_desc_get_l3_cast_info(desc, &cast);
	pkt_hdr->p.input_flags.ip_mcast = (cast == PP2_INQ_L3_MULTICAST);
	pkt_hdr->p.input_flags.ip_bcast = (cast == PP2_INQ_L3_BROADCAST);
}

static inline void parse_other_l4_protocol(odp_packet_hdr_t *pkt_hdr)
{
	uint32_t len;
	uint8_t proto = _ODP_IPPROTO_INVALID;
	const uint8_t *ip_frame;
	const _odp_ipv4hdr_t *ipv4;

	ip_frame = odp_packet_offset(packet_handle(pkt_hdr),
				     pkt_hdr->p.l3_offset,
				     &len,
				     NULL);
	if (pkt_hdr->p.input_flags.ipv4) {
		ipv4 = (const _odp_ipv4hdr_t *)ip_frame;
		proto = ipv4->proto;
	} else if (pkt_hdr->p.input_flags.ipv6) {
		proto = ipv6_get_next_hdr(ip_frame, pkt_hdr->p.l3_offset);
	}

	/* Parse Layer 4 headers */
	switch (proto) {
	case _ODP_IPPROTO_ICMPV4:
	case _ODP_IPPROTO_ICMPV6:
		pkt_hdr->p.input_flags.icmp = 1;
		break;

	case _ODP_IPPROTO_AH:
		pkt_hdr->p.input_flags.ipsec = 1;
		pkt_hdr->p.input_flags.ipsec_ah = 1;
		break;

	case _ODP_IPPROTO_ESP:
		pkt_hdr->p.input_flags.ipsec = 1;
		pkt_hdr->p.input_flags.ipsec_esp = 1;
		break;

	case _ODP_IPPROTO_SCTP:
		pkt_hdr->p.input_flags.sctp = 1;
		break;
	default:
		pkt_hdr->p.input_flags.l4 = 0;
		break;
	}
}

static inline void parse_l4(odp_packet_hdr_t *pkt_hdr,
			    enum pp2_inq_l4_type type,
			    u8 offset)
{
	pkt_hdr->p.l4_offset = offset;
	pkt_hdr->p.input_flags.l4 = 1;
	if (odp_likely((type != PP2_INQ_L4_TYPE_OTHER) &&
		       (type != PP2_INQ_L4_TYPE_NA))) {
		pkt_hdr->p.input_flags.tcp =
			(type == PP2_INQ_L4_TYPE_TCP);
		pkt_hdr->p.input_flags.udp =
			(type == PP2_INQ_L4_TYPE_UDP);
	} else
		/* Need to perform SW parsing */
		parse_other_l4_protocol(pkt_hdr);
}

inline void mvpp2_activate_free_sent_buffers(pktio_entry_t *pktio_entry)
{
	struct pp2_hif *hif = get_hif(get_thr_id());
	struct mvpp2_tx_shadow_q *shadow_q;
	pkt_mvpp2_t *pkt_mvpp2 = &pktio_entry->s.pkt_mvpp2;

	shadow_q = &pkt_mvpp2->shadow_qs[get_thr_id()][0];
	if (shadow_q->size)
		mvpp2_check_n_free_sent_buffers(pkt_mvpp2->ppio,
						hif,
						shadow_q,
						0);
}

static int mvpp2_recv(pktio_entry_t *pktio_entry,
		      int rxq_id,
		      odp_packet_t pkt_table[],
		      int num_pkts)
{
	odp_packet_hdr_t	*pkt_hdr;
	odp_packet_t		 pkt;
	struct pp2_ppio_desc	 descs[MVPP2_MAX_RX_BURST_SIZE];
	u16			 i, j, num, total_got, len;
	enum pp2_inq_l3_type	 l3_type;
	enum pp2_inq_l4_type	 l4_type;
	u8			 l3_offset, l4_offset;
	u8			 tc, qid, num_qids, last_qid;
	pkt_mvpp2_t		*mvpp2 = &pktio_entry->s.pkt_mvpp2;
	enum pp2_inq_desc_status desc_err;

	total_got = 0;
	if (num_pkts > (MVPP2_MAX_RX_BURST_SIZE * MVPP2_MAX_NUM_QS_PER_RX_TC))
		num_pkts = MVPP2_MAX_RX_BURST_SIZE * MVPP2_MAX_NUM_QS_PER_RX_TC;

	if (!mvpp2->inqs[rxq_id].lockless)
		odp_ticketlock_lock(&mvpp2->inqs[rxq_id].lock);

	/* TODO: only support now RSS; no support for QoS; how to translate rxq_id to tc/qid???? */
	tc = mvpp2->inqs[rxq_id].first_tc;
	qid = mvpp2->inqs[rxq_id].next_qid;
	num_qids = mvpp2->inqs[rxq_id].num_qids;
	last_qid = mvpp2->inqs[rxq_id].first_qid + num_qids - 1;
	for (i = 0; (i < num_qids) && (total_got != num_pkts); i++) {
		num = num_pkts - total_got;
		if (num > MVPP2_MAX_RX_BURST_SIZE)
			num = MVPP2_MAX_RX_BURST_SIZE;
		pp2_ppio_recv(mvpp2->ppio, tc, qid, descs, &num);
		for (j = 0; j < num; j++) {
			if ((num - j) > MVPP2_PREFETCH_SHIFT) {
				struct pp2_ppio_desc *pref_desc;
				u64 pref_addr;
				odp_packet_hdr_t *pref_pkt_hdr;

				pref_desc = &descs[j + MVPP2_PREFETCH_SHIFT];
				pref_addr =
					pp2_ppio_inq_desc_get_cookie(pref_desc);
				pref_pkt_hdr =
					odp_packet_hdr((odp_packet_t)pref_addr);
				odp_prefetch(pref_pkt_hdr);
				odp_prefetch(&pref_pkt_hdr->p);
			}
			pkt_table[total_got] = (odp_packet_t)
				pp2_ppio_inq_desc_get_cookie(&descs[j]);
			len = pp2_ppio_inq_desc_get_pkt_len(&descs[j]);

			pkt = pkt_table[total_got];
			pkt_hdr = odp_packet_hdr(pkt);

			packet_init(pkt_hdr, len);
			pkt_hdr->input = pktio_entry->s.handle;

			pp2_ppio_inq_desc_get_l3_info(&descs[j], &l3_type, &l3_offset);
			pp2_ppio_inq_desc_get_l4_info(&descs[j], &l4_type, &l4_offset);

			desc_err = pp2_ppio_inq_desc_get_l2_pkt_error(&descs[j]);
			if (odp_unlikely(desc_err != PP2_DESC_ERR_OK)) {
				/* Always drop L2 errors.
				* Counter MIB already updated */
				ODP_DBG("Drop packet with L2 error: %d", desc_err);
				odp_packet_free(pkt);
				continue;
			}

			desc_err = pp2_ppio_inq_desc_get_l3_pkt_error(&descs[j]);
			if (odp_unlikely(desc_err == PP2_DESC_ERR_IPV4_HDR)) {
				pkt_hdr->p.error_flags.ip_err = 1;
				if (odp_unlikely(pktio_entry->s.config.pktin.bit.ipv4_chksum == 0)) {
					/* Need to parse IPv4. if the error is actually from checksum than need to unset
					* the error flag. */
					pkt_hdr->p.l3_offset = l3_offset;
					if (odp_likely(!odph_ipv4_csum_valid(pkt)))
						pkt_hdr->p.error_flags.ip_err = 0;
				}
				if (odp_likely(pktio_entry->s.config.pktin.bit.drop_ipv4_err &&
					       pkt_hdr->p.error_flags.ip_err)) {
					ODP_DBG("Drop packet with L3 error: %d", desc_err);
					odp_packet_free(pkt);
					/* Need to update in_errors counter */
					pktio_entry->s.stats.in_errors++;
					continue;
				}
			}

			desc_err = pp2_ppio_inq_desc_get_l4_pkt_error(&descs[j]);
			if (odp_unlikely(desc_err == PP2_DESC_ERR_L4_CHECKSUM)) {
				pkt_hdr->p.error_flags.udp_err = ((l4_type == PP2_INQ_L4_TYPE_UDP) &&
					(pktio_entry->s.config.pktin.bit.udp_chksum));
				pkt_hdr->p.error_flags.tcp_err = ((l4_type == PP2_INQ_L4_TYPE_TCP) &&
					(pktio_entry->s.config.pktin.bit.tcp_chksum));
				if (odp_unlikely((pkt_hdr->p.error_flags.udp_err &&
						  pktio_entry->s.config.pktin.bit.drop_udp_err) ||
					(pkt_hdr->p.error_flags.tcp_err &&
					 pktio_entry->s.config.pktin.bit.drop_tcp_err))) {
					ODP_DBG("Drop packet with L4 error: %d", desc_err);
					odp_packet_free(pkt);
					/* Need to update in_errors counter */
					pktio_entry->s.stats.in_errors++;
					continue;
				}
			}

			/* Detect jumbo frames */
			if (len > _ODP_ETH_LEN_MAX)
				pkt_hdr->p.input_flags.jumbo = 1;

			parse_l2(pkt_hdr, &descs[i]);
			parse_l3(pkt_hdr, l3_type, l3_offset, &descs[i]);
			parse_l4(pkt_hdr, l4_type, l4_offset);

			if (pktio_entry->s.cls_enabled) {
				int err;

				err = mvpp2_cls_select_cos(pktio_entry,
							   &pkt,
							   tc);
				if (err)
					continue;
				pkt_table[total_got] = pkt;
			}

			total_got++;
		}
		if (!pktio_entry->s.cls_enabled &&
		    odp_unlikely(qid++ == last_qid))
			qid = mvpp2->inqs[rxq_id].first_qid;
	}
	mvpp2->inqs[rxq_id].next_qid = qid;
	if (!mvpp2->inqs[rxq_id].lockless)
		odp_ticketlock_unlock(&mvpp2->inqs[rxq_id].lock);

	if (odp_unlikely(!total_got))
		activate_free_sent_buffers();

	return total_got;
}

static inline int
mrvl_prepare_proto_info(odp_pktout_config_opt_t config_flags,
			_odp_packet_input_flags_t packet_input_flags,
			enum pp2_outq_l3_type *l3_type,
			enum pp2_outq_l4_type *l4_type,
			int *gen_l3_cksum,
			int *gen_l4_cksum)
{
	if (packet_input_flags.ipv4) {
		*l3_type = PP2_OUTQ_L3_TYPE_IPV4;
		*gen_l3_cksum = config_flags.bit.ipv4_chksum;
	} else if (packet_input_flags.ipv6) {
		*l3_type = PP2_OUTQ_L3_TYPE_IPV6;
		/* no checksum for ipv6 header */
		*gen_l3_cksum = 0;
	} else {
		/* if something different then stop processing */
		return -1;
	}

	if (packet_input_flags.tcp) {
		*l4_type = PP2_OUTQ_L4_TYPE_TCP;
		*gen_l4_cksum = config_flags.bit.tcp_chksum;
	} else if (packet_input_flags.udp) {
		*l4_type = PP2_OUTQ_L4_TYPE_UDP;
		*gen_l4_cksum = config_flags.bit.udp_chksum;
	} else {
		*l4_type = PP2_OUTQ_L4_TYPE_OTHER;
		/* no checksum for other type */
		*gen_l4_cksum = 0;
	}

	return 0;
}

/* An implementation for enqueuing packets */
static int mvpp2_send(pktio_entry_t *pktio_entry,
		      int txq_id,
		      const odp_packet_t pkt_table[],
		      int num_pkts)
{
	odp_packet_t		 pkt;
	odp_packet_hdr_t	*pkt_hdr;
	struct pp2_hif		*hif = get_hif(get_thr_id());
#ifndef USE_HW_BUFF_RECYLCE
	struct mvpp2_tx_shadow_q *shadow_q;
	u16			 shadow_q_free_size;
#endif /* !USE_HW_BUFF_RECYLCE */
	struct pp2_ppio_desc	 descs[MVPP2_MAX_TX_BURST_SIZE];
	dma_addr_t		 pa;
	u16			 i, num, len, idx = 0;
	u8			 tc;
	int			 ret, sent = 0;
	pkt_mvpp2_t		*pkt_mvpp2 = &pktio_entry->s.pkt_mvpp2;
	pktio_entry_t		*input_entry;
	struct odp_pktio_config_t *config = &pktio_entry->s.config;
	int gen_l3_cksum, gen_l4_cksum;
	enum pp2_outq_l3_type l3_type;
	enum pp2_outq_l4_type l4_type;

	/* TODO: only support now RSS; no support for QoS; how to translate txq_id to tc/hif???? */
	tc = 0;
	NOTUSED(txq_id);

#ifndef USE_HW_BUFF_RECYLCE
	shadow_q = &pkt_mvpp2->shadow_qs[get_thr_id()][tc];
	if (shadow_q->size)
		mvpp2_check_n_free_sent_buffers(pkt_mvpp2->ppio,
						hif,
						shadow_q,
						tc);

	shadow_q_free_size = SHADOW_Q_MAX_SIZE - shadow_q->size - 1;
	if (odp_unlikely(num_pkts > shadow_q_free_size)) {
		ODP_DBG("No room in shadow queue for %d packets!!! %d packets will be sent.\n",
			num_pkts, shadow_q_free_size);
		num_pkts = shadow_q_free_size;
	}
#endif /* !USE_HW_BUFF_RECYLCE */

	for (i = 0; i < num_pkts; i++) {
		if ((num_pkts - i) > MVPP2_PREFETCH_SHIFT) {
			odp_packet_t pref_pkt;
			odp_packet_hdr_t *pref_pkt_hdr;

			pref_pkt = pkt_table[i + MVPP2_PREFETCH_SHIFT];
			pref_pkt_hdr = odp_packet_hdr(pref_pkt);
			odp_prefetch(pref_pkt_hdr);
			odp_prefetch(&pref_pkt_hdr->p);
		}
		pkt = pkt_table[i];
		len = odp_packet_len(pkt);
		pkt_hdr = odp_packet_hdr(pkt);
		if (odp_unlikely(
			(pkt_hdr->p.l3_offset != ODP_PACKET_OFFSET_INVALID) &&
			((len - pkt_hdr->p.l3_offset) >
			 pktio_entry->s.pkt_mvpp2.mtu))) {
			if (i == 0) {
				__odp_errno = EMSGSIZE;
				return -1;
			}
			break;
		}
		pa = mv_sys_dma_mem_virt2phys((void *)((uintptr_t)odp_packet_head(pkt)));
		pp2_ppio_outq_desc_reset(&descs[idx]);
		pp2_ppio_outq_desc_set_phys_addr(&descs[idx], pa);
		pp2_ppio_outq_desc_set_pkt_offset(&descs[idx], odp_packet_headroom(pkt));
		pp2_ppio_outq_desc_set_pkt_len(&descs[idx], len);

		/*
		 * in case unsupported input_flags were passed
		 * do not update descriptor offload information
		 */

		ret = mrvl_prepare_proto_info(config->pktout,
					      pkt_hdr->p.input_flags,
					      &l3_type, &l4_type, &gen_l3_cksum,
					      &gen_l4_cksum);
		if (odp_likely(!ret)) {
			pp2_ppio_outq_desc_set_proto_info(&descs[idx],
							  l3_type,
							  l4_type,
							  pkt_hdr->p.l3_offset,
							  pkt_hdr->p.l4_offset,
							  gen_l3_cksum,
							  gen_l4_cksum);
		}

#ifdef USE_HW_BUFF_RECYLCE
		pp2_ppio_outq_desc_set_cookie(&descs[idx], (u64)pkt;
		pp2_ppio_outq_desc_set_pool(&descs[idx], pktio_entry->s.pkt_mvpp2.bpool);
#else
		shadow_q->ent[shadow_q->write_ind].buff.cookie = (u64)pkt;
		shadow_q->ent[shadow_q->write_ind].buff.addr = pa;

		input_entry = get_pktio_entry(pkt_hdr->input);
		if (odp_likely(input_entry &&
			       input_entry->s.ops == &mvpp2_pktio_ops)) {
			shadow_q->ent[shadow_q->write_ind].bpool =
				input_entry->s.pkt_mvpp2.bpool;
			shadow_q->input_pktio[shadow_q->write_ind] =
				pkt_hdr->input;
		} else {
			shadow_q->ent[shadow_q->write_ind].bpool = NULL;
		}

		shadow_q->write_ind = (shadow_q->write_ind + 1) & SHADOW_Q_MAX_SIZE_MASK;
		shadow_q->size++;
#endif /* USE_HW_BUFF_RECYLCE */
		idx++;
		if (odp_unlikely(idx == MVPP2_MAX_TX_BURST_SIZE)) {
			num = idx;
			pp2_ppio_send(pkt_mvpp2->ppio, hif, tc, descs, &num);
			sent += num;
			/* In case not all frames were send we need to decrease
			 * the write_ind
			 */
			if (odp_unlikely(idx != num)) {
				idx -= num;
				shadow_q->write_ind =
						(SHADOW_Q_MAX_SIZE +
						shadow_q->write_ind - idx) &
						SHADOW_Q_MAX_SIZE_MASK;
				shadow_q->size -= idx;
				return sent;
			}
			idx = 0;
		}
	}
	num = idx;
	pp2_ppio_send(pkt_mvpp2->ppio, hif, tc, descs, &num);
	sent += num;

	/* In case not all frames were send we need to decrease the write_ind */
	if (odp_unlikely(idx != num)) {
		idx -= num;
		shadow_q->write_ind =
			(SHADOW_Q_MAX_SIZE + shadow_q->write_ind - idx) &
			SHADOW_Q_MAX_SIZE_MASK;
		shadow_q->size -= idx;
	}

	return sent;
}

static int mvpp2_config(pktio_entry_t *pktio_entry ODP_UNUSED, const odp_pktio_config_t *config)
{
	ODP_PRINT("RX checksum offload configuration: IPv4 (%u), UDP (%u),"
		  " TCP (%u), SCTP (%u)\n", config->pktin.bit.ipv4_chksum,
		  config->pktin.bit.udp_chksum, config->pktin.bit.tcp_chksum,
		  config->pktin.bit.sctp_chksum);
	ODP_PRINT("TX checksum offload configuration: IPv4 (%u), UDP (%u),"
		  " TCP (%u), SCTP (%u)\n", config->pktout.bit.ipv4_chksum,
		  config->pktout.bit.udp_chksum, config->pktout.bit.tcp_chksum,
		  config->pktout.bit.sctp_chksum);
	ODP_PRINT("RX Dropping offload capability: IPv4 (%u), UDP (%u),"
		  " TCP (%u), SCTP (%u)\n", config->pktin.bit.drop_ipv4_err,
		  config->pktin.bit.drop_udp_err,
		  config->pktin.bit.drop_tcp_err,
		  config->pktin.bit.drop_sctp_err);

	return 0;
}

extern int mvpp2_cos_with_l2_priority(pktio_entry_t *entry,
				      uint8_t num_qos,
				      uint8_t qos_table[]);
extern int mvpp2_cos_with_l3_priority(pktio_entry_t *entry,
				      uint8_t num_qos,
				      uint8_t qos_table[]);

const pktio_if_ops_t mvpp2_pktio_ops = {
	.name = "odp-mvpp2",
	.print = NULL,
	.init_global = mvpp2_init_global,
	.init_local = mvpp2_init_local,
	.term = mvpp2_term_global,
	.open = mvpp2_open,
	.close = mvpp2_close,
	.start = mvpp2_start,
	.stop = mvpp2_stop,
	.capability = mvpp2_capability,
	.config = mvpp2_config,
	.input_queues_config = mvpp2_input_queues_config,
	.output_queues_config = mvpp2_output_queues_config,
#ifndef ODP_MVNMP_GUEST_MODE
	.stats = mvpp2_stats,
	.stats_reset = mvpp2_stats_reset,
#endif /* !ODP_MVNMP_GUEST_MODE */
	.pktin_ts_res = NULL,
	.pktin_ts_from_ns = NULL,
	.mtu_get = mvpp2_mtu_get,
#ifndef ODP_MVNMP_GUEST_MODE
	.promisc_mode_set = mvpp2_promisc_mode_set,
	.promisc_mode_get = mvpp2_promisc_mode_get,
#endif /* !ODP_MVNMP_GUEST_MODE */
	.mac_get = mvpp2_mac_get,
	.link_status = mvpp2_link_status,
	.recv = mvpp2_recv,
	.send = mvpp2_send,
	.cos_with_l2_priority = mvpp2_cos_with_l2_priority,
	.cos_with_l3_priority = mvpp2_cos_with_l3_priority,
};
