/*
#
# Copyright 2023- IBM Inc. All rights reserved
# SPDX-License-Identifier: LGPL-2.1-or-later
#
*/

#include "config.h"
#if defined(DPFS_RVFS)

#include <vector>
#include <iostream>
#include <memory>
#include <linux/fuse.h>
#include "hal.h"
#include "rvfs.h"
#include "rpc.h"
#include "util/tls_registry.h"
#include "tomlcpp.hpp"

// Each virtio-fs uses at least 3 descriptors (aka queue entries) for each request
#define VIRTIO_FS_MIN_DESCS 3

using namespace erpc;

pthread_key_t dpfs_hal_thread_id_key;
__attribute__((visibility("default")))
uint16_t dpfs_hal_thread_id(void) {
    return (uint16_t) pthread_getspecific(dpfs_hal_thread_id_key);
}
__attribute__((visibility("default")))
uint16_t dpfs_hal_nthreads(struct dpfs_hal *hal)
{
    return 1;
}

struct rpc_msg {
    // Back reference to dpfs_hal for the async_completion
    dpfs_hal *hal;

    // Only filled if the msg is in use, if so it will point to req internally
    ReqHandle *reqh;

    // Based on the max block size of 1MiB (4k pages, so 256 descriptors) and 3 page overhead per request.
    // These will point into the req and resp buffers.
    iovec iov[256+3];
    int in_iovcnt;
    int out_iovcnt;

    rpc_msg(dpfs_hal *hal) : hal(hal), reqh(nullptr),
        iov{{0}}, in_iovcnt(0), out_iovcnt(0)
    {}
};

struct dpfs_hal {
    dpfs_hal_ops ops;
    void *user_data;

    // eRPC
    std::vector<rpc_msg *> avail;
    std::unique_ptr<Nexus> nexus;
    std::unique_ptr<Rpc<CTransport>> rpc;

    dpfs_hal(dpfs_hal_ops o, void *ud) :
        ops(o), user_data(ud), avail() {}
};
    
static void req_handler(ReqHandle *reqh, void *context)
{
    dpfs_hal *hal = static_cast<dpfs_hal *>(context);
    // Messages and their buffers are dynamically allocated
    // The queue_depth of the virtio-fs device is static, so this wont infinitely allocate memory
    // Just be sure to warm up the system before evaulating performance
    rpc_msg *msg;
    if (hal->avail.empty()) {
        msg = new rpc_msg(hal);
    } else {
        msg = hal->avail.back();
        hal->avail.pop_back();
    }

#ifdef DEBUG_ENABLED
    printf("DPFS_HAL_RVFS %s: received eRPC in msg %p\n", __func__, msg);
#endif

    msg->reqh = reqh;

    uint8_t *req_buf = reqh->get_req_msgbuf()->buf_;
    uint8_t *resp_buf = reqh->pre_resp_msgbuf_.buf_;

    // Load the input io vectors
    msg->in_iovcnt = *((int *) req_buf);
    req_buf += sizeof(msg->in_iovcnt);

    size_t i = 0;
    for (; i < msg->in_iovcnt; i++) {
        size_t iov_len = *((size_t *) req_buf);
        req_buf += sizeof(iov_len);

        // Directly map into the NIC buffer for zero copy
        msg->iov[i].iov_base = req_buf;
        msg->iov[i].iov_len = iov_len;

        req_buf += iov_len;
    }

    // Load the output io vectors
    msg->out_iovcnt = *((int *) req_buf);
    req_buf += sizeof(msg->out_iovcnt);
    
    for (; i < msg->in_iovcnt + msg->out_iovcnt; i++) {
        size_t iov_len = *((size_t *) req_buf);
        req_buf += sizeof(iov_len);

        // Directly map into the NIC buffer for zero copy
        msg->iov[i].iov_base = resp_buf;
        msg->iov[i].iov_len = iov_len;

        resp_buf += iov_len;
    }

    int ret = hal->ops.request_handler(hal->user_data,
            msg->iov, msg->in_iovcnt,
            msg->iov+msg->in_iovcnt, msg->out_iovcnt,
            static_cast<void *>(msg), 0);

    if (ret == 0) {
        dpfs_hal_async_complete(msg, DPFS_HAL_COMPLETION_SUCCES);
    } else if (ret == EWOULDBLOCK) {
        // Do nothing, the FS impl has to call async_completion themselves
    } else {
        dpfs_hal_async_complete(msg, DPFS_HAL_COMPLETION_ERROR);
    }
}

// The session management callback that is invoked when sessions are successfully created or destroyed.
static void sm_handler(int, SmEventType event, SmErrType err, void *) {
    std::cout << "Event: " << sm_event_type_str(event) << " Error: " << sm_err_type_str(err) << std::endl;
}

__attribute__((visibility("default")))
struct dpfs_hal *dpfs_hal_new(struct dpfs_hal_params *params, bool start_mock_thread) {
    dpfs_hal *hal = new dpfs_hal(params->ops, params->user_data);
    auto res = toml::parseFile(params->conf_path);
    if (!res.table) {
        std::cerr << "cannot parse file: " << res.errmsg << std::endl;
        delete hal;
        return nullptr;
    }
    auto conf = res.table->getTable("rvfs");
    if (!conf) {
        std::cerr << "missing [rvfs]" << std::endl;
        delete hal;
        return nullptr;
    }
    auto [ok, remote_uri] = conf->getString("remote_uri");
    if (!ok) {
        std::cerr << "The config must contain a `remote_uri` [hostname/ip:UDP_PORT]" << std::endl;
        delete hal;
        return nullptr;
    }
    if (pthread_key_create(&dpfs_hal_thread_id_key, NULL)) {
        std::cerr << "Failed to create thread-local key for dpfs_hal threadid" << std::endl;
        delete hal;
        return nullptr;
    }
    // Only one thread, thread_id=0
    pthread_setspecific(dpfs_hal_thread_id_key, (void *) 0);

    // NUMA node 0
    // 1 background thread, which is unused but created to enable multithreading in eRPC
    hal->nexus = std::unique_ptr<Nexus>(new Nexus(remote_uri, 0, 1));
    hal->nexus->register_req_func(DPFS_RVFS_REQTYPE_FUSE, req_handler);
    
    hal->rpc = std::unique_ptr<Rpc<CTransport>>(new Rpc<CTransport>(hal->nexus.get(), hal, 0, sm_handler));
    // Same as in rvfs_dpu
    hal->rpc->set_pre_resp_msgbuf_size(DPFS_RVFS_MAX_REQRESP_SIZE);

    hal->ops.register_device(hal->user_data, 0);

    std::cout << "DPFS HAL with RVFS frontend online at " << remote_uri << "!" << std::endl;

    return hal;
}

static volatile int keep_running;

static void signal_handler(int dummy)
{
    keep_running = 0;
}

__attribute__((visibility("default")))
void dpfs_hal_loop(struct dpfs_hal *hal) {
    keep_running = 1;
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = signal_handler;
    sigaction(SIGINT, &act, 0);
    sigaction(SIGPIPE, &act, 0);
    sigaction(SIGTERM, &act, 0);

    while(keep_running) {
        hal->rpc->run_event_loop_once();
    }
}

__attribute__((visibility("default")))
int dpfs_hal_poll_io(struct dpfs_hal *hal, uint16_t) {
    hal->rpc->run_event_loop_once();
    return 0;
}

// Just do nothing
__attribute__((visibility("default")))
void dpfs_hal_poll_mmio(struct dpfs_hal *, uint16_t) {}

__attribute__((visibility("default")))
void dpfs_hal_destroy(struct dpfs_hal *hal) {
    while (hal->avail.size()) {
        rpc_msg *msg = hal->avail.back();
        hal->avail.pop_back();
        delete msg;
    }

    hal->ops.unregister_device(hal->user_data, 0);
    delete hal;
}

__attribute__((visibility("default")))
int dpfs_hal_async_complete(void *completion_context, enum dpfs_hal_completion_status)
{
    rpc_msg *msg = (rpc_msg *) completion_context;
    dpfs_hal *hal = msg->hal;

#ifdef DEBUG_ENABLED
    printf("DPFS_HAL_RVFS %s: replying to msg %p\n", __func__, msg);
#endif

    if (!hal->nexus->tls_registry_.is_init()) {
        hal->nexus->tls_registry_.init();
    }

    struct fuse_out_header *fuse_out_header = static_cast<struct fuse_out_header *>(msg->iov[msg->in_iovcnt].iov_base);
    Rpc<CTransport>::resize_msg_buffer(&msg->reqh->pre_resp_msgbuf_, fuse_out_header->len);

    hal->rpc->enqueue_response(msg->reqh, &msg->reqh->pre_resp_msgbuf_);
    hal->avail.push_back(msg);
    return 0;
}

#endif // RVFS
