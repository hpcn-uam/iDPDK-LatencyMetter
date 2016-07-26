/*-
 *   BSD LICENSE
 * 
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 * 
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <string.h>
#include <sys/queue.h>
#include <stdarg.h>
#include <errno.h>
#include <getopt.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_lpm.h>

#include "main.h"

#ifndef APP_LCORE_IO_FLUSH
#define APP_LCORE_IO_FLUSH           1000000
#endif

#ifndef APP_LCORE_WORKER_FLUSH
#define APP_LCORE_WORKER_FLUSH       1000000
#endif

#ifndef APP_STATS
#define APP_STATS                    1000000
#endif

#define APP_IO_RX_DROP_ALL_PACKETS   1
#define APP_WORKER_DROP_ALL_PACKETS  0
#define APP_IO_TX_DROP_ALL_PACKETS   0

#ifndef APP_IO_RX_PREFETCH_ENABLE
#define APP_IO_RX_PREFETCH_ENABLE    1
#endif

#ifndef APP_WORKER_PREFETCH_ENABLE
#define APP_WORKER_PREFETCH_ENABLE   1
#endif

#ifndef APP_IO_TX_PREFETCH_ENABLE
#define APP_IO_TX_PREFETCH_ENABLE    1
#endif

#if APP_IO_RX_PREFETCH_ENABLE
#define APP_IO_RX_PREFETCH0(p)       rte_prefetch0(p)
#define APP_IO_RX_PREFETCH1(p)       rte_prefetch1(p)
#else
#define APP_IO_RX_PREFETCH0(p)
#define APP_IO_RX_PREFETCH1(p)
#endif

#if APP_WORKER_PREFETCH_ENABLE
#define APP_WORKER_PREFETCH0(p)      rte_prefetch0(p)
#define APP_WORKER_PREFETCH1(p)      rte_prefetch1(p)
#else
#define APP_WORKER_PREFETCH0(p)
#define APP_WORKER_PREFETCH1(p)
#endif

#if APP_IO_TX_PREFETCH_ENABLE
#define APP_IO_TX_PREFETCH0(p)       rte_prefetch0(p)
#define APP_IO_TX_PREFETCH1(p)       rte_prefetch1(p)
#else
#define APP_IO_TX_PREFETCH0(p)
#define APP_IO_TX_PREFETCH1(p)
#endif

static inline void
app_lcore_io_rx_buffer_to_send (
	struct app_lcore_params_io *lp,
	uint32_t worker,
	struct rte_mbuf *mbuf,
	uint32_t bsz)
{
	uint32_t pos;
	int ret;

	pos = lp->rx.mbuf_out[worker].n_mbufs;
	lp->rx.mbuf_out[worker].array[pos ++] = mbuf;
	if (likely(pos < bsz)) {
		lp->rx.mbuf_out[worker].n_mbufs = pos;
		return;
	}

	ret = rte_ring_sp_enqueue_bulk(
		lp->rx.rings[worker],
		(void **) lp->rx.mbuf_out[worker].array,
		bsz);

	if (unlikely(ret == -ENOBUFS)) {
		uint32_t k;
		for (k = 0; k < bsz; k ++) {
			struct rte_mbuf *m = lp->rx.mbuf_out[worker].array[k];
			rte_pktmbuf_free(m);
		}
	}

	lp->rx.mbuf_out[worker].n_mbufs = 0;
	lp->rx.mbuf_out_flush[worker] = 0;

#if APP_STATS
	lp->rx.rings_iters[worker] ++;
	if (likely(ret == 0)) {
		lp->rx.rings_count[worker] ++;
	}
	if (unlikely(lp->rx.rings_iters[worker] == APP_STATS)) {
		unsigned lcore = rte_lcore_id();

		printf("\tI/O RX %u out (worker %u): enq success rate = %.2f (%u/%u)\n",
			lcore,
			(unsigned)worker,
			((double) lp->rx.rings_count[worker]) / ((double) lp->rx.rings_iters[worker]),
			(uint32_t)lp->rx.rings_count[worker],
			(uint32_t)lp->rx.rings_iters[worker]);
		lp->rx.rings_iters[worker] = 0;
		lp->rx.rings_count[worker] = 0;
	}
#endif
}

//#define QUEUE_STATS
static inline void
app_lcore_io_rx(
	struct app_lcore_params_io *lp,
	uint32_t n_workers,
	uint32_t bsz_rd,
	uint32_t bsz_wr,
	uint8_t pos_lb)
{
	struct rte_mbuf *mbuf_1_0, *mbuf_1_1, *mbuf_2_0, *mbuf_2_1;
	uint8_t *data_1_0, *data_1_1 = NULL;
	uint32_t i;

	for (i = 0; i < lp->rx.n_nic_queues; i ++) {
		uint8_t port = lp->rx.nic_queues[i].port;
		uint8_t queue = lp->rx.nic_queues[i].queue;
		uint32_t n_mbufs, j;

		n_mbufs = rte_eth_rx_burst(
			port,
			queue,
			lp->rx.mbuf_in.array,
			(uint16_t) bsz_rd);

		if (unlikely(n_mbufs == 0)) {
			continue;
		}

#if APP_STATS
		lp->rx.nic_queues_iters[i] ++;
		lp->rx.nic_queues_count[i] += n_mbufs;
		if (unlikely(lp->rx.nic_queues_iters[i] == APP_STATS*10)) {
			struct rte_eth_stats stats;
			struct timeval start_ewr, end_ewr;

			rte_eth_stats_get(port, &stats);
			gettimeofday(&lp->rx.end_ewr, NULL);

			start_ewr = lp->rx.start_ewr; end_ewr = lp->rx.end_ewr;

			if(lp->rx.record)
			{
				fprintf(lp->rx.record,"%lu\t%lf\t%.1lf\t%u\n",
				start_ewr.tv_sec,
				(((stats.ibytes)+stats.ipackets*(/*4crc+8prelud+12ifg*/(8+12)))/(((end_ewr.tv_sec * 1000000. + end_ewr.tv_usec) - (start_ewr.tv_sec * 1000000. + start_ewr.tv_usec))/1000000.))/(1000*1000*1000./8.),
				(double)stats.ipackets/((((double)end_ewr.tv_sec * (double)1000000. + (double)end_ewr.tv_usec) - ((double)start_ewr.tv_sec * (double)1000000. + (double)start_ewr.tv_usec)) /(double)1000000.),
				(uint32_t) stats.ierrors
				);
				fflush(lp->rx.record);
			}
			else
			{
#ifdef QUEUE_STATS
			if(queue==0)
			{
#endif
			printf("NIC port %u: drop ratio = %.2f (%u/%u) speed: %lf Gbps (%.1lf pkts/s)\n",
				(unsigned) port,
				(double) stats.ierrors / (double) (stats.ierrors + stats.ipackets),
				(uint32_t) stats.ipackets, (uint32_t) stats.ierrors,
				(((stats.ibytes)+stats.ipackets*(/*4crc+8prelud+12ifg*/(8+12)))/(((end_ewr.tv_sec * 1000000. + end_ewr.tv_usec) - (start_ewr.tv_sec * 1000000. + start_ewr.tv_usec))/1000000.))/(1000*1000*1000./8.),
				stats.ipackets/(((end_ewr.tv_sec * 1000000. + end_ewr.tv_usec) - (start_ewr.tv_sec * 1000000. + start_ewr.tv_usec)) /1000000.)
				);
#ifdef QUEUE_STATS
			}
			printf("NIC port %u:%u: drop ratio = %.2f (%u/%u) speed %.1lf pkts/s\n",
				(unsigned) port, queue,
				(double) stats.ierrors / (double) (stats.ierrors + lp->rx.nic_queues_count[i]),
				(uint32_t) lp->rx.nic_queues_count[i], (uint32_t) stats.ierrors,
				lp->rx.nic_queues_count[i]/(((end_ewr.tv_sec * 1000000. + end_ewr.tv_usec) - (start_ewr.tv_sec * 1000000. + start_ewr.tv_usec)) /1000000.)
				);
#endif
			}
			lp->rx.nic_queues_iters[i] = 0;
			lp->rx.nic_queues_count[i] = 0;

#ifdef QUEUE_STATS
                       	if(queue==0)
#endif
			rte_eth_stats_reset (port);

#ifdef QUEUE_STATS
                  	if(queue==0)
#endif
			lp->rx.start_ewr = end_ewr; // Updating start
		}
#endif

#if APP_IO_RX_DROP_ALL_PACKETS
		for (j = 0; j < n_mbufs; j ++) {
			struct rte_mbuf *pkt = lp->rx.mbuf_in.array[j];
			rte_pktmbuf_free(pkt);
		}

		continue;
#endif

		mbuf_1_0 = lp->rx.mbuf_in.array[0];
		mbuf_1_1 = lp->rx.mbuf_in.array[1];
		data_1_0 = rte_pktmbuf_mtod(mbuf_1_0, uint8_t *);
		if (likely(n_mbufs > 1)) {
			data_1_1 = rte_pktmbuf_mtod(mbuf_1_1, uint8_t *);
		}

		mbuf_2_0 = lp->rx.mbuf_in.array[2];
		mbuf_2_1 = lp->rx.mbuf_in.array[3];
		APP_IO_RX_PREFETCH0(mbuf_2_0);
		APP_IO_RX_PREFETCH0(mbuf_2_1);

		for (j = 0; j + 3 < n_mbufs; j += 2) {
			struct rte_mbuf *mbuf_0_0, *mbuf_0_1;
			uint8_t *data_0_0, *data_0_1;
			uint32_t worker_0, worker_1;

			mbuf_0_0 = mbuf_1_0;
			mbuf_0_1 = mbuf_1_1;
			data_0_0 = data_1_0;
			data_0_1 = data_1_1;

			mbuf_1_0 = mbuf_2_0;
			mbuf_1_1 = mbuf_2_1;
			data_1_0 = rte_pktmbuf_mtod(mbuf_2_0, uint8_t *);
			data_1_1 = rte_pktmbuf_mtod(mbuf_2_1, uint8_t *);
			APP_IO_RX_PREFETCH0(data_1_0);
			APP_IO_RX_PREFETCH0(data_1_1);

			mbuf_2_0 = lp->rx.mbuf_in.array[j+4];
			mbuf_2_1 = lp->rx.mbuf_in.array[j+5];
			APP_IO_RX_PREFETCH0(mbuf_2_0);
			APP_IO_RX_PREFETCH0(mbuf_2_1);

			worker_0 = data_0_0[pos_lb] & (n_workers - 1);
			worker_1 = data_0_1[pos_lb] & (n_workers - 1);

			app_lcore_io_rx_buffer_to_send(lp, worker_0, mbuf_0_0, bsz_wr);
			app_lcore_io_rx_buffer_to_send(lp, worker_1, mbuf_0_1, bsz_wr);
		}

		/* Handle the last 1, 2 (when n_mbufs is even) or 3 (when n_mbufs is odd) packets  */
		for ( ; j < n_mbufs; j += 1) {
			struct rte_mbuf *mbuf;
			uint8_t *data;
			uint32_t worker;

			mbuf = mbuf_1_0;
			mbuf_1_0 = mbuf_1_1;
			mbuf_1_1 = mbuf_2_0;
			mbuf_2_0 = mbuf_2_1;

			data = rte_pktmbuf_mtod(mbuf, uint8_t *);

			APP_IO_RX_PREFETCH0(mbuf_1_0);

			worker = data[pos_lb] & (n_workers - 1);

			app_lcore_io_rx_buffer_to_send(lp, worker, mbuf, bsz_wr);
		}
	}
}

static inline void
app_lcore_io_rx_flush(struct app_lcore_params_io *lp, uint32_t n_workers)
{
	uint32_t worker;

	for (worker = 0; worker < n_workers; worker ++) {
		int ret;

		if (likely((lp->rx.mbuf_out_flush[worker] == 0) ||
		           (lp->rx.mbuf_out[worker].n_mbufs == 0))) {
			lp->rx.mbuf_out_flush[worker] = 1;
			continue;
		}

		ret = rte_ring_sp_enqueue_bulk(
			lp->rx.rings[worker],
			(void **) lp->rx.mbuf_out[worker].array,
			lp->rx.mbuf_out[worker].n_mbufs);

		if (unlikely(ret < 0)) {
			uint32_t k;
			for (k = 0; k < lp->rx.mbuf_out[worker].n_mbufs; k ++) {
				struct rte_mbuf *pkt_to_free = lp->rx.mbuf_out[worker].array[k];
				rte_pktmbuf_free(pkt_to_free);
			}
		}

		lp->rx.mbuf_out[worker].n_mbufs = 0;
		lp->rx.mbuf_out_flush[worker] = 1;
	}
}

static inline void
app_lcore_io_tx(
	struct app_lcore_params_io *lp,
	uint32_t n_workers,
	uint32_t bsz_rd,
	uint32_t bsz_wr)
{
	uint32_t worker;

	const uint8_t icmppkt []={
	0x00, 0x1e, 0x4a, 0xe0, 0x52, 0x00, 0x14, 0xdd, 0xa9, 0xd2, 0xef, 0x57, 0x08, 0x00, 0x45, 0x00,
	0x00, 0x54, 0x51, 0x36, 0x40, 0x00, 0x40, 0x01, 0x6b, 0xee, 0x96, 0xf4, 0x3a, 0x72, 0xd8, 0x3a,
	0xd3, 0xe3, 0x08, 0x00, 0xeb, 0xe1, 0x66, 0x02, 0x00, 0x1a, 0x67, 0x72, 0x97, 0x57, 0x00, 0x00,
	0x00, 0x00, 0xe4, 0x64, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
	0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
	0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
	0x36, 0x37
	};
	const int icmppktlen = 98;

	for (worker = 0; worker < n_workers; worker ++) {
		uint32_t i;

		for (i = 0; i < lp->tx.n_nic_ports; i ++) {
			uint8_t port = lp->tx.nic_ports[i];
			//struct rte_ring *ring = lp->tx.rings[port][worker];
			uint32_t n_mbufs, n_pkts;
			//int ret;

			//UNUSED -> uncoment :)
			(void)bsz_rd;
			(void)bsz_wr;

			//n_mbufs = lp->tx.mbuf_out[port].n_mbufs;
			/*ret = rte_ring_sc_dequeue_bulk(
				ring,
				(void **) &lp->tx.mbuf_out[port].array[n_mbufs],
				bsz_rd);

			if (unlikely(ret == -ENOENT)) {
				continue;
			}

			n_mbufs += bsz_rd;*/
			for (n_mbufs=0;n_mbufs<bsz_wr;n_mbufs++)
			{

				lp->tx.mbuf_out[port].array[n_mbufsk]=rte_pktmbuf_alloc (app.pools[0]);
				lp->tx.mbuf_out[port].array[n_mbufs]->pkt_len = icmppktlen;
				
				memcpy(lp->tx.mbuf_out[port].array[n_mbufs]->buf_addr,icmppkt,icmppktlen);
			}

			/*if (unlikely(n_mbufs < bsz_wr)) {
				lp->tx.mbuf_out[port].n_mbufs = n_mbufs;
				continue;
			}*/

			n_pkts = rte_eth_tx_burst(
				port,
				0,
				lp->tx.mbuf_out[port].array,
				(uint16_t) n_mbufs);

			printf("Tx sent %d\n",n_pkts);

#if APP_STATS
			lp->tx.nic_ports_iters[port] ++;
			lp->tx.nic_ports_count[port] += n_pkts;
			if (unlikely(lp->tx.nic_ports_iters[port] == APP_STATS)) {
				struct rte_eth_stats stats;
				unsigned lcore = rte_lcore_id();

				rte_eth_stats_get(port, &stats);

				printf("\t\t\tI/O TX %u out (port %u): NIC drop ratio = %.2f (%u/%u) avg burst size = %.2f\n",
					lcore,
					(unsigned) port,
					(double) stats.oerrors / (double) (stats.oerrors + stats.opackets),
					(uint32_t) stats.opackets, (uint32_t) stats.oerrors,
					((double) lp->tx.nic_ports_count[port]) / ((double) lp->tx.nic_ports_iters[port]));
				lp->tx.nic_ports_iters[port] = 0;
				lp->tx.nic_ports_count[port] = 0;
			}
#endif

			if (unlikely(n_pkts < n_mbufs)) {
				uint32_t k;
				for (k = n_pkts; k < n_mbufs; k ++) {
					struct rte_mbuf *pkt_to_free = lp->tx.mbuf_out[port].array[k];
					rte_pktmbuf_free(pkt_to_free);
				}
			}
			lp->tx.mbuf_out[port].n_mbufs = 0;
			lp->tx.mbuf_out_flush[port] = 0;
		}
	}
}

static inline void
app_lcore_io_tx_flush(struct app_lcore_params_io *lp)
{
	uint8_t port;

	for (port = 0; port < lp->tx.n_nic_ports; port ++) {
		uint32_t n_pkts;

		if (likely((lp->tx.mbuf_out_flush[port] == 0) ||
		           (lp->tx.mbuf_out[port].n_mbufs == 0))) {
			lp->tx.mbuf_out_flush[port] = 1;
			continue;
		}

		n_pkts = rte_eth_tx_burst(
			port,
			0,
			lp->tx.mbuf_out[port].array,
			(uint16_t) lp->tx.mbuf_out[port].n_mbufs);

		if (unlikely(n_pkts < lp->tx.mbuf_out[port].n_mbufs)) {
			uint32_t k;
			for (k = n_pkts; k < lp->tx.mbuf_out[port].n_mbufs; k ++) {
				struct rte_mbuf *pkt_to_free = lp->tx.mbuf_out[port].array[k];
				rte_pktmbuf_free(pkt_to_free);
			}
		}

		lp->tx.mbuf_out[port].n_mbufs = 0;
		lp->tx.mbuf_out_flush[port] = 1;
	}
}

static void
app_lcore_main_loop_io(void)
{
	uint32_t lcore = rte_lcore_id();
	struct app_lcore_params_io *lp = &app.lcore_params[lcore].io;
	uint32_t n_workers = app_get_lcores_worker();
	uint64_t i = 0;

	uint32_t bsz_rx_rd = app.burst_size_io_rx_read;
	uint32_t bsz_rx_wr = app.burst_size_io_rx_write;

	uint8_t pos_lb = app.pos_lb;

	for ( ; ; ) {
		if (APP_LCORE_IO_FLUSH && (unlikely(i == APP_LCORE_IO_FLUSH))) {
			if (likely(lp->rx.n_nic_queues > 0)) {
				app_lcore_io_rx_flush(lp, n_workers); 
			}
			i = 0;
		}

		if (likely(lp->rx.n_nic_queues > 0)) {
			app_lcore_io_rx(lp, n_workers, bsz_rx_rd, bsz_rx_wr, pos_lb); 
		}

		if (likely(lp->tx.n_nic_ports > 0)) {
			app_lcore_io_tx(lp, 1, app.burst_size_io_tx_read, app.burst_size_io_tx_write); 
		}

		i ++;
	}
}

static inline void
app_lcore_worker(
	struct app_lcore_params_worker *lp,
	uint32_t bsz_rd)
{
	uint32_t i;

	for (i = 0; i < lp->n_rings_in; i ++) {
		struct rte_ring *ring_in = lp->rings_in[i]; //posible false sharing
		uint32_t j;
		int ret;

		ret = rte_ring_sc_dequeue_bulk(
			ring_in,
			(void **) lp->mbuf_in.array,
			bsz_rd);

		if (unlikely(ret == -ENOENT)) {
			continue;
		}

#if APP_WORKER_DROP_ALL_PACKETS
		for (j = 0; j < bsz_rd; j ++) {
			struct rte_mbuf *pkt = lp->mbuf_in.array[j];
			rte_pktmbuf_free(pkt);
		}

		continue;
#endif

		APP_WORKER_PREFETCH1(rte_pktmbuf_mtod(lp->mbuf_in.array[0], unsigned char *));
		APP_WORKER_PREFETCH0(lp->mbuf_in.array[1]);

		for (j = 0; j < bsz_rd; j ++) {
			//struct ipv4_hdr *ipv4_hdr;
			//uint32_t ipv4_dst, pos;
			//uint8_t port;

			if (likely(j < bsz_rd - 1)) {
				APP_WORKER_PREFETCH1(rte_pktmbuf_mtod(lp->mbuf_in.array[j+1], unsigned char *));
			}
			if (likely(j < bsz_rd - 2)) {
				APP_WORKER_PREFETCH0(lp->mbuf_in.array[j+2]);
			}

			/*Obtenemos el paquete...*/
			
			/*se lo pasamos al trabajador*/
			//ipv4_hdr = (struct ipv4_hdr *)(rte_pktmbuf_mtod(pkt, unsigned char *) + sizeof(struct ether_hdr));
			//ipv4_dst = rte_be_to_cpu_32(ipv4_hdr->dst_addr);

			/*Buscamos en la tabla LPM (que no lo hacemos realmente...)*/
			/*if (unlikely(rte_lpm_lookup(lp->lpm_table, ipv4_dst, &port) != 0)) {
				port = pkt->pkt.in_port;
			}*/
			
		}
	}
}

static inline void
app_lcore_worker_flush(struct app_lcore_params_worker *lp)
{
	uint32_t port;

	for (port = 0; port < APP_MAX_NIC_PORTS; port ++) {
		int ret;

		if (unlikely(lp->rings_out[port] == NULL)) {
			continue;
		}

		if (likely((lp->mbuf_out_flush[port] == 0) ||
		           (lp->mbuf_out[port].n_mbufs == 0))) {
			lp->mbuf_out_flush[port] = 1;
			continue;
		}

		ret = rte_ring_sp_enqueue_bulk(
			lp->rings_out[port],
			(void **) lp->mbuf_out[port].array,
			lp->mbuf_out[port].n_mbufs);

		if (unlikely(ret < 0)) {
			uint32_t k;
			for (k = 0; k < lp->mbuf_out[port].n_mbufs; k ++) {
				struct rte_mbuf *pkt_to_free = lp->mbuf_out[port].array[k];
				rte_pktmbuf_free(pkt_to_free);
			}
		}

		lp->mbuf_out[port].n_mbufs = 0;
		lp->mbuf_out_flush[port] = 1;
	}
}

static void
app_lcore_main_loop_worker(void) {
	uint32_t lcore = rte_lcore_id();
	struct app_lcore_params_worker *lp = &app.lcore_params[lcore].worker;
	uint64_t i = 0;

	uint32_t bsz_rd = app.burst_size_worker_read;

	for ( ; ; ) {
		/*if (APP_LCORE_WORKER_FLUSH && (unlikely(i == APP_LCORE_WORKER_FLUSH))) {
			app_lcore_worker_flush(lp);
			i = 0;
		}*/

		app_lcore_worker(lp, bsz_rd);

		i ++;
	}
}

int
app_lcore_main_loop(__attribute__((unused)) void *arg)
{
	struct app_lcore_params *lp;
	unsigned lcore;

	lcore = rte_lcore_id();
	lp = &app.lcore_params[lcore];

	if (lp->type == e_APP_LCORE_IO) {
		printf("Logical core %u (I/O) main loop.\n", lcore);
		app_lcore_main_loop_io();
	}

	if (lp->type == e_APP_LCORE_WORKER) {
		printf("Logical core %u (worker %u) main loop.\n",
			lcore,
			(unsigned) lp->worker.worker_id);
		app_lcore_main_loop_worker();
	}
	
	/** External **/
	if (lp->type == e_APP_LCORE_WORKER_SLAVE) {
		printf("Logical core %u (worker slave) main loop.\n",
			lcore);
		//external_slave();
	}
	return 0;
}
