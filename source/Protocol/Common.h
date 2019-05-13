/*-
 * Copyright (c) 2019 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#ifndef __PROTOCOL_COMMON_H__
#define __PROTOCOL_COMMON_H__

namespace tzrpc {

namespace ServiceID {

enum {
    RAFT_SERVICE = 1,   // raft core service
    CLIENT_SERVICE = 2,
};

}

} // end namespace sisyphus


#endif // __PROTOCOL_COMMON_H__
