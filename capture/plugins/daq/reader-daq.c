/* reader-daq.c  -- daq instead of libpcap
 *
 *  Simple plugin that queries the wise service for
 *  ips, domains, email, and md5s which can use various
 *  services to return data.  It caches all the results.
 *
 * Copyright 2012-2016 AOL Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this Software except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "moloch.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "daq.h"
#include "pcap.h"

extern MolochConfig_t        config;
extern MolochPcapFileHdr_t   pcapFileHeader;

LOCAL const DAQ_Module_t    *module;
LOCAL void                  *handle;

LOCAL struct bpf_program    *bpf_programs = 0;

/******************************************************************************/
int reader_daq_stats(MolochReaderStats_t *stats)
{
    DAQ_Stats_t daq_stats;

    int err = daq_get_stats(module, handle, &daq_stats);

    if (err) {
        stats->dropped = stats->total = 0;
        return -1;
    }
    stats->dropped = daq_stats.hw_packets_dropped;
    stats->total = daq_stats.hw_packets_received;
    return 0;
}
/******************************************************************************/
DAQ_Verdict reader_daq_packet_cb(void *UNUSED(user), const DAQ_PktHdr_t *h, const uint8_t *data)
{
    if (unlikely(h->caplen != h->pktlen)) {
        LOG("ERROR - Moloch requires full packet captures caplen: %d pktlen: %d", h->caplen, h->pktlen);
        exit (0);
    }

    MolochPacket_t *packet = MOLOCH_TYPE_ALLOC0(MolochPacket_t);

    packet->pkt           = (u_char *)data,
    packet->ts            = h->ts,
    packet->pktlen        = h->pktlen,

    moloch_packet(packet);
    return DAQ_VERDICT_PASS;
}
/******************************************************************************/
static void *reader_daq_thread()
{
    while (1) {
        int r = daq_acquire(module, handle, -1, reader_daq_packet_cb, NULL);
        if (r)

        // Some kind of failure we quit
        if (unlikely(r)) {
            LOG("DAQ quiting %d %s", r, daq_get_error(module, handle));
            moloch_quit();
            module = 0;
            break;
        }
    }
    return NULL;
}
/******************************************************************************/
void reader_daq_start() {
    int err;


    if (config.bpf) {
        err = daq_set_filter(module, handle, config.bpf);

        if (err) {
            LOG("DAQ set filter error %d %s for  %s", err, daq_get_error(module, handle), config.bpf);
            exit (1);
        }
    }

    err = daq_start(module, handle);

    if (err) {
        LOG("DAQ start error %d %s", err, daq_get_error(module, handle));
        exit (1);
    }

    pcapFileHeader.linktype = daq_get_datalink_type(module, handle);
    pcapFileHeader.snaplen = 16384;


    pcap_t *pcap = pcap_open_dead(pcapFileHeader.linktype, pcapFileHeader.snaplen);
    if (config.dontSaveBPFs) {
        int i;
        if (bpf_programs) {
            for (i = 0; i < config.dontSaveBPFsNum; i++) {
                pcap_freecode(&bpf_programs[i]);
            }
        } else {
            bpf_programs= malloc(config.dontSaveBPFsNum*sizeof(struct bpf_program));
        }
        for (i = 0; i < config.dontSaveBPFsNum; i++) {
            if (pcap_compile(pcap, &bpf_programs[i], config.dontSaveBPFs[i], 0, PCAP_NETMASK_UNKNOWN) == -1) {
                LOG("ERROR - Couldn't compile filter: '%s' with %s", config.dontSaveBPFs[i], pcap_geterr(pcap));
                exit(1);
            }
        }
    }

    g_thread_new("moloch-pcap", &reader_daq_thread, NULL);
}
/******************************************************************************/
void reader_daq_stop() 
{
    if (module)
        daq_breakloop(module, handle);
}
/******************************************************************************/
int reader_daq_should_filter(const MolochPacket_t *UNUSED(packet))
{
    if (bpf_programs) {
        int i;
        for (i = 0; i < config.dontSaveBPFsNum; i++) {
            if (bpf_filter(bpf_programs[i].bf_insns, packet->pkt, packet->pktlen, packet->pktlen)) {
                return i;
                break;
            }
        }
    }
    return -1;
}
/******************************************************************************/
void reader_daq_init(char *UNUSED(name))
{
    int err;
    DAQ_Config_t cfg;


    gchar **dirs = moloch_config_str_list(NULL, "daqModuleDirs", "/usr/local/lib/daq");
    gchar *moduleName = moloch_config_str(NULL, "daqModule", "pcap");

    err = daq_load_modules((const char **)dirs);
    if (err) {
        LOG("Can't load DAQ modules = %d\n", err);
        exit(1);
    }

    module = daq_find_module(moduleName);
    if (!module) {
        LOG("Can't find %s DAQ module\n", moduleName);
        exit(1);
    }


    memset(&cfg, 0, sizeof(cfg));
    cfg.name = config.interface;
    cfg.snaplen = 16384;
    cfg.timeout = -1;
    cfg.mode = DAQ_MODE_PASSIVE;

    char buf[256] = "";
    err = daq_initialize(module, &cfg, &handle, buf, sizeof(buf));

    if (err) {
        LOG("Can't initialize DAQ %d %s\n", err, buf);
        exit(1);
    }

    moloch_reader_start         = reader_daq_start;
    moloch_reader_stop          = reader_daq_stop;
    moloch_reader_stats         = reader_daq_stats;
    moloch_reader_should_filter = reader_daq_should_filter;
}
/******************************************************************************/
void moloch_plugin_init()
{
    moloch_readers_add("daq", reader_daq_init);
}
