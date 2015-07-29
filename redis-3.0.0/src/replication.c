/* Asynchronous replication implementation.
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include "redis.h"

#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>

void replicationDiscardCachedMaster(void);
void replicationResurrectCachedMaster(int newfd);
void replicationSendAck(void);
void putSlaveOnline(redisClient *slave);

/* --------------------------- Utility functions ---------------------------- */

/* Return the pointer to a string representing the slave ip:listening_port
 * pair. Mostly useful for logging, since we want to log a slave using its
 * IP address and it's listening port which is more clear for the user, for
 * example: "Closing connection with slave 10.1.2.3:6380". */
/// 返回slave的ip:port
char *replicationGetSlaveName(redisClient *c) {
    static char buf[REDIS_PEER_ID_LEN];
    char ip[REDIS_IP_STR_LEN];

    ip[0] = '\0';
    buf[0] = '\0';
    /// 将描述符转为ip地址
    if (anetPeerToString(c->fd,ip,sizeof(ip),NULL) != -1) {
        if (c->slave_listening_port)
            snprintf(buf,sizeof(buf),"%s:%d",ip,c->slave_listening_port);
        else
            snprintf(buf,sizeof(buf),"%s:<unknown-slave-port>",ip);
    } else { /// 拿ip地址失败,只能返回id
        snprintf(buf,sizeof(buf),"client id #%llu",
            (unsigned long long) c->id);
    }
    return buf;
}

/* ---------------------------------- MASTER -------------------------------- */

/// 创建一个新的replication backlog,里面的变量暂时还不知道有啥用????????
/// [server.repl_backlog_off - server.master_repl_offset] 这段范围为需要同步至slave的变更!!!!!!!
void createReplicationBacklog(void) {
    redisAssert(server.repl_backlog == NULL);
    server.repl_backlog = zmalloc(server.repl_backlog_size);
    server.repl_backlog_histlen = 0;
    server.repl_backlog_idx = 0;
    /* When a new backlog buffer is created, we increment the replication
     * offset by one to make sure we'll not be able to PSYNC with any
     * previous slave. This is needed because we avoid incrementing the
     * master_repl_offset if no backlog exists nor slaves are attached. */
    server.master_repl_offset++;

    /* We don't have any data inside our buffer, but virtually the first
     * byte we have is the next byte that will be generated for the
     * replication stream. */
    server.repl_backlog_off = server.master_repl_offset+1;
}

/* This function is called when the user modifies the replication backlog
 * size at runtime. It is up to the function to both update the
 * server.repl_backlog_size and to resize the buffer and setup it so that
 * it contains the same data as the previous one (possibly less data, but
 * the most recent bytes, or the same data and more free space in case the
 * buffer is enlarged). */
/// 重新设置server.repl_backlog_size为newsize,并分配新的repl_backlog
void resizeReplicationBacklog(long long newsize) {
    /// 最少为REDIS_REPL_BACKLOG_MIN_SIZE(16K)
    if (newsize < REDIS_REPL_BACKLOG_MIN_SIZE)
        newsize = REDIS_REPL_BACKLOG_MIN_SIZE;

    if (server.repl_backlog_size == newsize) return;

    server.repl_backlog_size = newsize;
    if (server.repl_backlog != NULL) {
        /* What we actually do is to flush the old buffer and realloc a new
         * empty one. It will refill with new data incrementally.
         * The reason is that copying a few gigabytes adds latency and even
         * worse often we need to alloc additional space before freeing the
         * old buffer. */
        zfree(server.repl_backlog);
        server.repl_backlog = zmalloc(server.repl_backlog_size);
        server.repl_backlog_histlen = 0;
        server.repl_backlog_idx = 0;
        /* Next byte we have is... the next since the buffer is empty. */
        server.repl_backlog_off = server.master_repl_offset+1;
    }
}

/// 将repl_backlog释放
void freeReplicationBacklog(void) {
    redisAssert(listLength(server.slaves) == 0);
    zfree(server.repl_backlog);
    server.repl_backlog = NULL;
}

/* Add data to the replication backlog.
 * This function also increments the global replication offset stored at
 * server.master_repl_offset, because there is no case where we want to feed
 * the backlog without incrementing the buffer. */
/// 将len长的数据ptr写入repl_backlog中:注意,repl_backlog为环形buffer,不会丢数据????????master_repl_offset和repl_backlog_off都有啥用
void feedReplicationBacklog(void *ptr, size_t len) {
    unsigned char *p = ptr;

    /// 
    server.master_repl_offset += len;

    /* This is a circular buffer, so write as much data we can at every
     * iteration and rewind the "idx" index if we reach the limit. */
    while(len) {
        /// 计算repl_backlog尾部剩余的空间
        size_t thislen = server.repl_backlog_size - server.repl_backlog_idx;
        if (thislen > len) 
        {
            thislen = len;
        }
        memcpy(server.repl_backlog+server.repl_backlog_idx,p,thislen);
        server.repl_backlog_idx += thislen;
        /// repl_backlog是一个环形buffer,将idx重置为0
        if (server.repl_backlog_idx == server.repl_backlog_size)
        {
            server.repl_backlog_idx = 0;
        }
        len -= thislen;
        p += thislen;
        server.repl_backlog_histlen += thislen;
    }
    /// repl_backlog_histlen最长为repl_backlog_size
    if (server.repl_backlog_histlen > server.repl_backlog_size)
        server.repl_backlog_histlen = server.repl_backlog_size;

    /* Set the offset of the first byte we have in the backlog. */
    /// server.repl_backlog_histlen最长为repl_backlog_size,所以server.repl_backlog_off与server.master_repl_offset最多
    /// 只有一个buffer的长度,超过了就无法psync了
    server.repl_backlog_off = server.master_repl_offset -
                              server.repl_backlog_histlen + 1;
}

/* Wrapper for feedReplicationBacklog() that takes Redis string objects
 * as input. */
/// 将string类型的obj添加到repl_backlog中
void feedReplicationBacklogWithObject(robj *o) {
    char llstr[REDIS_LONGSTR_SIZE];
    void *p;
    size_t len;

    if (o->encoding == REDIS_ENCODING_INT) {
        len = ll2string(llstr,sizeof(llstr),(long)o->ptr);
        p = llstr;
    } else {
        len = sdslen(o->ptr);
        p = o->ptr;
    }
    feedReplicationBacklog(p,len);
}

/// 将argc个argv命令发送到slave的dictid数据库中,并写入repl_backlog中
/// 这个函数在propagate()中被调用,根据命令是否写命令,将其传到slave中
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j, len;
    char llstr[REDIS_LONGSTR_SIZE];

    /* If there aren't slaves, and there is no backlog buffer to populate,
     * we can return ASAP. */
    /// repl_backlog为空且slave链表为空
    if (server.repl_backlog == NULL && listLength(slaves) == 0) return;

    /* We can't have slaves attached and no backlog. */
    redisAssert(!(listLength(slaves) != 0 && server.repl_backlog == NULL));

    /* Send SELECT command to every slave if needed. */
    /// 对每个slave进行SELECT命令
    if (server.slaveseldb != dictid) {
        robj *selectcmd;

        /* For a few DBs we have pre-computed SELECT command. */
        if (dictid >= 0 && dictid < REDIS_SHARED_SELECT_CMDS) {
            selectcmd = shared.select[dictid];
        } else {
            int dictid_len;

            dictid_len = ll2string(llstr,sizeof(llstr),dictid);
            selectcmd = createObject(REDIS_STRING,
                sdscatprintf(sdsempty(),
                "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                dictid_len, llstr));
        }

        /* Add the SELECT command into the backlog. */
        if (server.repl_backlog) feedReplicationBacklogWithObject(selectcmd);

        /* Send it to slaves. */
        listRewind(slaves,&li);
        /// 将每一个SELECT命令发送到slave中
        while((ln = listNext(&li))) {
            redisClient *slave = ln->value;
            addReply(slave,selectcmd);
        }

        if (dictid < 0 || dictid >= REDIS_SHARED_SELECT_CMDS)
            decrRefCount(selectcmd);
    }
    server.slaveseldb = dictid;

    /* Write the command to the replication backlog if any. */
    /// 将argc个argv[]命令写入repl_backlog中
    if (server.repl_backlog) {
        char aux[REDIS_LONGSTR_SIZE+3];

        /* Add the multi bulk reply length. */
        aux[0] = '*';
        len = ll2string(aux+1,sizeof(aux)-1,argc);
        aux[len+1] = '\r';
        aux[len+2] = '\n';
        feedReplicationBacklog(aux,len+3);

        for (j = 0; j < argc; j++) {
            long objlen = stringObjectLen(argv[j]);

            /* We need to feed the buffer with the object as a bulk reply
             * not just as a plain string, so create the $..CRLF payload len
             * and add the final CRLF */
            aux[0] = '$';
            len = ll2string(aux+1,sizeof(aux)-1,objlen);
            aux[len+1] = '\r';
            aux[len+2] = '\n';
            feedReplicationBacklog(aux,len+3);
            feedReplicationBacklogWithObject(argv[j]);
            feedReplicationBacklog(aux+len+1,2);
        }
    }

    /* Write the command to every slave. */
    listRewind(server.slaves,&li);
    /// 遍历slave,将命令发送到slave中
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;

        /* Don't feed slaves that are still waiting for BGSAVE to start */
        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) continue;

        /* Feed slaves that are waiting for the initial SYNC (so these commands
         * are queued in the output buffer until the initial SYNC completes),
         * or are already in sync with the master. */

        /* Add the multi bulk length. */
        /// 发送给slave,实际上的发送由sendReplyToClient执行
        addReplyMultiBulkLen(slave,argc);

        /* Finally any additional argument that was not stored inside the
         * static buffer if any (from j to argc). */
        for (j = 0; j < argc; j++)
            addReplyBulk(slave,argv[j]);
    }
}

/// 将client,dictid,argv拼在一起后发送到monitors
void replicationFeedMonitors(redisClient *c, list *monitors, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j;
    sds cmdrepr = sdsnew("+");
    robj *cmdobj;
    struct timeval tv;

    gettimeofday(&tv,NULL);
    /// cmdrepr: "+sec.usec"
    cmdrepr = sdscatprintf(cmdrepr,"%ld.%06ld ",(long)tv.tv_sec,(long)tv.tv_usec);
    /// cmdrepr: "+sec.usec client flag/id"
    if (c->flags & REDIS_LUA_CLIENT) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d lua] ",dictid);
    } else if (c->flags & REDIS_UNIX_SOCKET) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d unix:%s] ",dictid,server.unixsocket);
    } else {
        cmdrepr = sdscatprintf(cmdrepr,"[%d %s] ",dictid,getClientPeerId(c));
    }

    /// 将argv拼接在cmdrepr后面
    for (j = 0; j < argc; j++) {
        if (argv[j]->encoding == REDIS_ENCODING_INT) {
            cmdrepr = sdscatprintf(cmdrepr, "\"%ld\"", (long)argv[j]->ptr);
        } else {
            cmdrepr = sdscatrepr(cmdrepr,(char*)argv[j]->ptr,
                        sdslen(argv[j]->ptr));
        }
        if (j != argc-1)
            cmdrepr = sdscatlen(cmdrepr," ",1);
    }
    cmdrepr = sdscatlen(cmdrepr,"\r\n",2);
    cmdobj = createObject(REDIS_STRING,cmdrepr);

    listRewind(monitors,&li);
    /// 将拼接好的cmdrepr发送到monitor
    while((ln = listNext(&li))) {
        redisClient *monitor = ln->value;
        addReply(monitor,cmdobj);
    }
    decrRefCount(cmdobj);
}

/* Feed the slave 'c' with the replication backlog starting from the
 * specified 'offset' up to the end of the backlog. */
/// 将server的backlog从offset开始发送给client????????
long long addReplyReplicationBacklog(redisClient *c, long long offset) {
    long long j, skip, len;

    /// slave要求repl从特殊的offset开始传
    redisLog(REDIS_DEBUG, "[PSYNC] Slave request offset: %lld", offset);

    if (server.repl_backlog_histlen == 0) {
        redisLog(REDIS_DEBUG, "[PSYNC] Backlog history len is zero");
        return 0;
    }

    /// 打印repl backlog的大小
    redisLog(REDIS_DEBUG, "[PSYNC] Backlog size: %lld",
             server.repl_backlog_size);
    redisLog(REDIS_DEBUG, "[PSYNC] First byte: %lld",
             server.repl_backlog_off);
    redisLog(REDIS_DEBUG, "[PSYNC] History len: %lld",
             server.repl_backlog_histlen);
    redisLog(REDIS_DEBUG, "[PSYNC] Current index: %lld",
             server.repl_backlog_idx);

    /* Compute the amount of bytes we need to discard. */
    /// 计算出我们跳过的字节数
    skip = offset - server.repl_backlog_off;
    redisLog(REDIS_DEBUG, "[PSYNC] Skipping: %lld", skip);

    /* Point j to the oldest byte, that is actaully our
     * server.repl_backlog_off byte. */
    /// 计算出旧的指针位置
    j = (server.repl_backlog_idx +
        (server.repl_backlog_size-server.repl_backlog_histlen)) %
        server.repl_backlog_size;
    redisLog(REDIS_DEBUG, "[PSYNC] Index of first byte: %lld", j);

    /* Discard the amount of data to seek to the specified 'offset'. */
    /// 跳过skip个byte
    j = (j + skip) % server.repl_backlog_size;

    /* Feed slave with data. Since it is a circular buffer we have to
     * split the reply in two parts if we are cross-boundary. */
    len = server.repl_backlog_histlen - skip;
    redisLog(REDIS_DEBUG, "[PSYNC] Reply total length: %lld", len);
    /// 将len长度的字节发送给slave
    while(len) {
        long long thislen =
            ((server.repl_backlog_size - j) < len) ?
            (server.repl_backlog_size - j) : len;

        redisLog(REDIS_DEBUG, "[PSYNC] addReply() length: %lld", thislen);
        addReplySds(c,sdsnewlen(server.repl_backlog + j, thislen));
        len -= thislen;
        j = 0;
    }
    return server.repl_backlog_histlen - skip;
}

/* This function handles the PSYNC command from the point of view of a
 * master receiving a request for partial resynchronization.
 *
 * On success return REDIS_OK, otherwise REDIS_ERR is returned and we proceed
 * with the usual full resync. */
/// MASTER尝试从某一个off开始重传repl给slave
/// 这个函数用于PSYNC命令
int masterTryPartialResynchronization(redisClient *c) {
    long long psync_offset, psync_len;
    char *master_runid = c->argv[1]->ptr;
    char buf[128];
    int buflen;

    /* Is the runid of this master the same advertised by the wannabe slave
     * via PSYNC? If runid changed this master is a different instance and
     * there is no way to continue. */
    /// 不等于0表示id不同
    if (strcasecmp(master_runid, server.runid)) {
        /* Run id "?" is used by slaves that want to force a full resync. */
        if (master_runid[0] != '?') { /// id不匹配
            redisLog(REDIS_NOTICE,"Partial resynchronization not accepted: "
                "Runid mismatch (Client asked for runid '%s', my runid is '%s')",
                master_runid, server.runid);
        } else { /// '?'表示client想强制重传整个repl
            redisLog(REDIS_NOTICE,"Full resync requested by slave %s",
                replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }

    /* We still have the data our slave is asking for? */
    /// 拿到psync_offset
    if (getLongLongFromObjectOrReply(c,c->argv[2],&psync_offset,NULL) !=
       REDIS_OK) goto need_full_resync;

    /// repl_backlog为空或者psync_offset超过范围,注意看log
    if (!server.repl_backlog ||
        psync_offset < server.repl_backlog_off ||
        psync_offset > (server.repl_backlog_off + server.repl_backlog_histlen))
    {
        redisLog(REDIS_NOTICE,
            "Unable to partial resync with slave %s for lack of backlog (Slave request was: %lld).", replicationGetSlaveName(c), psync_offset);
        if (psync_offset > server.master_repl_offset) {
            redisLog(REDIS_WARNING,
                "Warning: slave %s tried to PSYNC with an offset that is greater than the master replication offset.", replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }

    /* If we reached this point, we are able to perform a partial resync:
     * 1) Set client state to make it a slave.
     * 2) Inform the client we can continue with +CONTINUE
     * 3) Send the backlog data (from the offset to the end) to the slave. */
    c->flags |= REDIS_SLAVE; /// client设置SLAVE标志
    c->replstate = REDIS_REPL_ONLINE; /// client同步状态设置为同步中
    c->repl_ack_time = server.unixtime;
    c->repl_put_online_on_ack = 0;
    listAddNodeTail(server.slaves,c); /// 添加到server.slaves列表中
    /* We can't use the connection buffers since they are used to accumulate
     * new commands at this stage. But we are sure the socket send buffer is
     * empty so this write will never fail actually. */
    /// 向客户端返回+CONTINUE表示可以继续更新,从某个offset继续
    buflen = snprintf(buf,sizeof(buf),"+CONTINUE\r\n");
    /// 写失败
    if (write(c->fd,buf,buflen) != buflen) {
        freeClientAsync(c);
        return REDIS_OK;
    }

    /// 将psync_offset开始处的backlog传给c
    psync_len = addReplyReplicationBacklog(c,psync_offset);
    redisLog(REDIS_NOTICE,
        "Partial resynchronization request from %s accepted. Sending %lld bytes of backlog starting from offset %lld.",
            replicationGetSlaveName(c),
            psync_len, psync_offset);
    /* Note that we don't need to set the selected DB at server.slaveseldb
     * to -1 to force the master to emit SELECT, since the slave already
     * has this state from the previous connection with the master. */

    refreshGoodSlavesCount();
    return REDIS_OK; /* The caller can return, no full resync needed. */

need_full_resync:
    /* We need a full resync for some reason... notify the client. */
    /// 揭示了server.master_repl_offset这个变量的作用!!!!!!!!
    psync_offset = server.master_repl_offset;
    /* Add 1 to psync_offset if it the replication backlog does not exists
     * as when it will be created later we'll increment the offset by one. */
    if (server.repl_backlog == NULL) psync_offset++;
    /* Again, we can't use the connection buffers (see above). */
    /// 向客户端返回+FULLRESYNC表示不能部分同步,要全部更新
    buflen = snprintf(buf,sizeof(buf),"+FULLRESYNC %s %lld\r\n",
                      server.runid,psync_offset);
    if (write(c->fd,buf,buflen) != buflen) {
        freeClientAsync(c);
        return REDIS_OK;
    }
    return REDIS_ERR;
}

/* Start a BGSAVE for replication goals, which is, selecting the disk or
 * socket target depending on the configuration, and making sure that
 * the script cache is flushed before to start.
 *
 * Returns REDIS_OK on success or REDIS_ERR otherwise. */
/// 开始后台rdb保存,为replication做准备
int startBgsaveForReplication(void) {
    int retval;

    redisLog(REDIS_NOTICE,"Starting BGSAVE for SYNC with target: %s",
        server.repl_diskless_sync ? "slaves sockets" : "disk");

    /// 这个变量用来标志是否直接将rdb发往slave,不保存到disk
    if (server.repl_diskless_sync) /// 一般这个变量都设置为0
    {
        retval = rdbSaveToSlavesSockets();
    }
    else /// 后台保存rdb到rdb_filename中
    {
        retval = rdbSaveBackground(server.rdb_filename);
    }

    /* Flush the script cache, since we need that slave differences are
     * accumulated without requiring slaves to match our cached scripts. */
    if (retval == REDIS_OK) 
    {
        replicationScriptCacheFlush();
    }

    return retval;
}

/* SYNC and PSYNC command implemenation. */
/// ?????????????????????再看一次这个函数??????????????????????????
void syncCommand(redisClient *c) {
    /* ignore SYNC if already slave or in monitor mode */
    /// 已经是slave了,直接返回
    if (c->flags & REDIS_SLAVE) return;

    /* Refuse SYNC requests if we are a slave but the link with our master
     * is not ok... */
    /// 没有连上master或者master拒绝了这个链接
    if (server.masterhost && server.repl_state != REDIS_REPL_CONNECTED) {
        addReplyError(c,"Can't SYNC while not connected with my master");
        return;
    }

    /* SYNC can't be issued when the server has pending data to send to
     * the client about already issued commands. We need a fresh reply
     * buffer registering the differences between the BGSAVE and the current
     * dataset, so that we can copy to other slaves if needed. */
    /// 当client的发送缓冲中有数据时,暂时不能SYNC
    if (listLength(c->reply) != 0 || c->bufpos != 0) {
        addReplyError(c,"SYNC and PSYNC are invalid with pending output");
        return;
    }

    redisLog(REDIS_NOTICE,"Slave %s asks for synchronization",
        replicationGetSlaveName(c));

    /* Try a partial resynchronization if this is a PSYNC command.
     * If it fails, we continue with usual full resynchronization, however
     * when this happens masterTryPartialResynchronization() already
     * replied with:
     *
     * +FULLRESYNC <runid> <offset>
     *
     * So the slave knows the new runid and offset to try a PSYNC later
     * if the connection with the master is lost. */
    if (!strcasecmp(c->argv[0]->ptr,"psync")) { /// psync命令
        /// partial sync成功,就返回了
        if (masterTryPartialResynchronization(c) == REDIS_OK) {
            server.stat_sync_partial_ok++;
            return; /* No full resync needed, return. */
        } else { /// partial sync失败了,还要继续sync
            char *master_runid = c->argv[1]->ptr;

            /* Increment stats for failed PSYNCs, but only if the
             * runid is not "?", as this is used by slaves to force a full
             * resync on purpose when they are not albe to partially
             * resync. */
            if (master_runid[0] != '?') server.stat_sync_partial_err++;
        }
    } else { /// sync命令
        /* If a slave uses SYNC, we are dealing with an old implementation
         * of the replication protocol (like redis-cli --slave). Flag the client
         * so that we don't expect to receive REPLCONF ACK feedbacks. */
        c->flags |= REDIS_PRE_PSYNC;
    }

    /* Full resynchronization. */
    server.stat_sync_full++;

    /* Here we need to check if there is a background saving operation
     * in progress, or if it is required to start one */
    /// rdb文件正在后台存储中且存储类型为磁盘
    if (server.rdb_child_pid != -1 &&
        server.rdb_child_type == REDIS_RDB_CHILD_TYPE_DISK)
    {
        /* Ok a background save is in progress. Let's check if it is a good
         * one for replication, i.e. if there is another slave that is
         * registering differences since the server forked to save. */
        redisClient *slave;
        listNode *ln;
        listIter li;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            slave = ln->value;
            if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) break;
        }
        if (ln) {
            /* Perfect, the server is already registering differences for
             * another slave. Set the right state, and copy the buffer. */
            copyClientOutputBuffer(c,slave);
            c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
            redisLog(REDIS_NOTICE,"Waiting for end of BGSAVE for SYNC");
        } else {
            /* No way, we need to wait for the next BGSAVE in order to
             * register differences. */
            c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
            redisLog(REDIS_NOTICE,"Waiting for next BGSAVE for SYNC");
        }
    } else if (server.rdb_child_pid != -1 && 
               server.rdb_child_type == REDIS_RDB_CHILD_TYPE_SOCKET) /// rdb文件正在后台保存中且类型为直接写slave socket
    {
        /* There is an RDB child process but it is writing directly to
         * children sockets. We need to wait for the next BGSAVE
         * in order to synchronize. */
        c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
        redisLog(REDIS_NOTICE,"Waiting for next BGSAVE for SYNC");
    } else {
        if (server.repl_diskless_sync) {
            /* Diskless replication RDB child is created inside
             * replicationCron() since we want to delay its start a
             * few seconds to wait for more slaves to arrive. */
            c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
            if (server.repl_diskless_sync_delay)
                redisLog(REDIS_NOTICE,"Delay next BGSAVE for SYNC");
        } else {
            /* Ok we don't have a BGSAVE in progress, let's start one. */
            if (startBgsaveForReplication() != REDIS_OK) {
                redisLog(REDIS_NOTICE,"Replication failed, can't BGSAVE");
                addReplyError(c,"Unable to perform background save");
                return;
            }
            c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
        }
    }

    if (server.repl_disable_tcp_nodelay)
        anetDisableTcpNoDelay(NULL, c->fd); /* Non critical if it fails. */
    c->repldbfd = -1;
    c->flags |= REDIS_SLAVE;
    server.slaveseldb = -1; /* Force to re-emit the SELECT command. */
    listAddNodeTail(server.slaves,c);
    if (listLength(server.slaves) == 1 && server.repl_backlog == NULL)
        createReplicationBacklog();
    return;
}

/* REPLCONF <option> <value> <option> <value> ...
 * This command is used by a slave in order to configure the replication
 * process before starting it with the SYNC command.
 *
 * Currently the only use of this command is to communicate to the master
 * what is the listening port of the Slave redis instance, so that the
 * master can accurately list slaves and their listening ports in
 * the INFO output.
 *
 * In the future the same command can be used in order to configure
 * the replication to initiate an incremental replication instead of a
 * full resync. */
/// REPLCONF命令的实现,有一部分代码没看懂????????
void replconfCommand(redisClient *c) {
    int j;

    /// 参数错误
    if ((c->argc % 2) == 0) {
        /* Number of arguments must be odd to make sure that every
         * option has a corresponding value. */
        addReply(c,shared.syntaxerr);
        return;
    }

    /* Process every option-value pair. */
    for (j = 1; j < c->argc; j+=2) {
        /// 设置slave 监听端口
        if (!strcasecmp(c->argv[j]->ptr,"listening-port")) {
            long port;

            if ((getLongFromObjectOrReply(c,c->argv[j+1],
                    &port,NULL) != REDIS_OK))
                return;
            c->slave_listening_port = port;
        } else if (!strcasecmp(c->argv[j]->ptr,"ack")) { /// 这个ACK值得是什么意思????????
            /* REPLCONF ACK is used by slave to inform the master the amount
             * of replication stream that it processed so far. It is an
             * internal only command that normal clients should never use. */
            long long offset;

            if (!(c->flags & REDIS_SLAVE)) return;
            if ((getLongLongFromObject(c->argv[j+1], &offset) != REDIS_OK))
                return;
            if (offset > c->repl_ack_off)
                c->repl_ack_off = offset; ///repl_ack_off是什么意思????????
            c->repl_ack_time = server.unixtime;
            /* If this was a diskless replication, we need to really put
             * the slave online when the first ACK is received (which
             * confirms slave is online and ready to get more data). */
            if (c->repl_put_online_on_ack && c->replstate == REDIS_REPL_ONLINE)
                putSlaveOnline(c);
            /* Note: this command does not reply anything! */
            return;
        } else if (!strcasecmp(c->argv[j]->ptr,"getack")) {
            /* REPLCONF GETACK is used in order to request an ACK ASAP
             * to the slave. */
            if (server.masterhost && server.master) replicationSendAck();
            /* Note: this command does not reply anything! */
        } else {
            addReplyErrorFormat(c,"Unrecognized REPLCONF option: %s",
                (char*)c->argv[j]->ptr);
            return;
        }
    }
    addReply(c,shared.ok);
}

/* This function puts a slave in the online state, and should be called just
 * after a slave received the RDB file for the initial synchronization, and
 * we are finally ready to send the incremental stream of commands.
 *
 * It does a few things:
 *
 * 1) Put the slave in ONLINE state (useless when the function is called
 *    because state is already ONLINE but repl_put_online_on_ack is true).
 * 2) Make sure the writable event is re-installed, since calling the SYNC
 *    command disables it, so that we can accumulate output buffer without
 *    sending it to the slave.
 * 3) Update the count of good slaves. */
/// 启用SLAVE
void putSlaveOnline(redisClient *slave) {
    slave->replstate = REDIS_REPL_ONLINE;
    slave->repl_put_online_on_ack = 0;
    slave->repl_ack_time = server.unixtime; /* Prevent false timeout. */
    /// 创建slave的描述符可写事件
    if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE,
        sendReplyToClient, slave) == AE_ERR) {
        redisLog(REDIS_WARNING,"Unable to register writable event for slave bulk transfer: %s", strerror(errno));
        freeClient(slave);
        return;
    }
    refreshGoodSlavesCount();
    redisLog(REDIS_NOTICE,"Synchronization with slave %s succeeded",
        replicationGetSlaveName(slave));
}

/// 发送rdb文件到slave.这是一个事件函数
void sendBulkToSlave(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *slave = privdata;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    char buf[REDIS_IOBUF_LEN];
    ssize_t nwritten, buflen;

    /* Before sending the RDB file, we send the preamble as configured by the
     * replication process. Currently the preamble is just the bulk count of
     * the file in the form "$<length>\r\n". */
    /// replpreamble:前导,类似于prefix,发送前先发前导
    if (slave->replpreamble) {
        nwritten = write(fd,slave->replpreamble,sdslen(slave->replpreamble));
        if (nwritten == -1) { /// 写失败
            redisLog(REDIS_VERBOSE,"Write error sending RDB preamble to slave: %s",
                strerror(errno));
            freeClient(slave);
            return;
        }
        server.stat_net_output_bytes += nwritten;
        /// 取出replpreamble还未发送的部分
        sdsrange(slave->replpreamble,nwritten,-1);
        /// 为0说明全部发送完毕 
        if (sdslen(slave->replpreamble) == 0) {
            sdsfree(slave->replpreamble);
            slave->replpreamble = NULL;
            /* fall through sending data. */
        } else {
            return;
        }
    }

    /* If the preamble was already transfered, send the RDB bulk data. */
    lseek(slave->repldbfd,slave->repldboff,SEEK_SET);
    /// 读rdb文件,slve->repldbfd是一个指向rdb.filename的文件描述符
    buflen = read(slave->repldbfd,buf,REDIS_IOBUF_LEN);
    if (buflen <= 0) {
        redisLog(REDIS_WARNING,"Read error sending DB to slave: %s",
            (buflen == 0) ? "premature EOF" : strerror(errno));
        freeClient(slave);
        return;
    }
    /// 将rdb写往socket描述符
    if ((nwritten = write(fd,buf,buflen)) == -1) {
        if (errno != EAGAIN) {
            redisLog(REDIS_WARNING,"Write error sending DB to slave: %s",
                strerror(errno));
            freeClient(slave);
        }
        return;
    }
    slave->repldboff += nwritten;
    server.stat_net_output_bytes += nwritten;
    /// 整个rdb全部写完
    if (slave->repldboff == slave->repldbsize) {
        close(slave->repldbfd);
        slave->repldbfd = -1;
        aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
        /// 注册slave的sendReplyToClient事件函数
        putSlaveOnline(slave);
    }
}

/* This function is called at the end of every background saving,
 * or when the replication RDB transfer strategy is modified from
 * disk to socket or the other way around.
 *
 * The goal of this function is to handle slaves waiting for a successful
 * background saving in order to perform non-blocking synchronization, and
 * to schedule a new BGSAVE if there are slaves that attached while a
 * BGSAVE was in progress, but it was not a good one for replication (no
 * other slave was accumulating differences).
 *
 * The argument bgsaveerr is REDIS_OK if the background saving succeeded
 * otherwise REDIS_ERR is passed to the function.
 * The 'type' argument is the type of the child that terminated
 * (if it had a disk or socket target). */
/// bgsave完成后会调用这个函数,将rdb文件发送到'先前状态为等待BGSAVE结束的slave'.
/// 函数中将'等待BGSAVE开始的客户端'状态值为'等待BGSAVE结束',并再一次启动bgsave
/// ???????? BGSAVE刚结束,就可能立即重新启动BGSAVE,是否存在性能问题????????
void updateSlavesWaitingBgsave(int bgsaveerr, int type) {
    listNode *ln;
    int startbgsave = 0;
    listIter li;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;

        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) {
            startbgsave = 1;
            slave->replstate = REDIS_REPL_WAIT_BGSAVE_END;
        } else if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) {
            struct redis_stat buf;

            /* If this was an RDB on disk save, we have to prepare to send
             * the RDB from disk to the slave socket. Otherwise if this was
             * already an RDB -> Slaves socket transfer, used in the case of
             * diskless replication, our work is trivial, we can just put
             * the slave online. */
            if (type == REDIS_RDB_CHILD_TYPE_SOCKET) {
                redisLog(REDIS_NOTICE,
                    "Streamed RDB transfer with slave %s succeeded (socket). Waiting for REPLCONF ACK from slave to enable streaming",
                        replicationGetSlaveName(slave));
                /* Note: we wait for a REPLCONF ACK message from slave in
                 * order to really put it online (install the write handler
                 * so that the accumulated data can be transfered). However
                 * we change the replication state ASAP, since our slave
                 * is technically online now. */
                slave->replstate = REDIS_REPL_ONLINE;
                slave->repl_put_online_on_ack = 1;
                slave->repl_ack_time = server.unixtime; /* Timeout otherwise. */
            } else {
                if (bgsaveerr != REDIS_OK) {
                    freeClient(slave);
                    redisLog(REDIS_WARNING,"SYNC failed. BGSAVE child returned an error");
                    continue;
                }
                /// 打开rdb文件并获取文件状态
                if ((slave->repldbfd = open(server.rdb_filename,O_RDONLY)) == -1 ||
                    redis_fstat(slave->repldbfd,&buf) == -1) {
                    freeClient(slave);
                    redisLog(REDIS_WARNING,"SYNC failed. Can't open/stat DB after BGSAVE: %s", strerror(errno));
                    continue;
                }
                slave->repldboff = 0;
                slave->repldbsize = buf.st_size; /// rdb文件大小
                slave->replstate = REDIS_REPL_SEND_BULK;
                slave->replpreamble = sdscatprintf(sdsempty(),"$%lld\r\n", /// preamble记录的是rdb的大小
                    (unsigned long long) slave->repldbsize);

                aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
                /// 注册发送rdb文件到slave的事件函数
                if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE, sendBulkToSlave, slave) == AE_ERR) {
                    freeClient(slave);
                    continue;
                }
            }
        }
    }
    if (startbgsave) {
        /// 启动后台的rdb保存失败了
        if (startBgsaveForReplication() != REDIS_OK) {
            listIter li;

            listRewind(server.slaves,&li);
            redisLog(REDIS_WARNING,"SYNC failed. BGSAVE failed");
            while((ln = listNext(&li))) {
                redisClient *slave = ln->value;

                /// slave等待BGSAVE开始,BGSAVE失败了,那么将这个slave释放
                if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START)
                    freeClient(slave);
            }
        }
    }
}

/* ----------------------------------- SLAVE -------------------------------- */

/* Abort the async download of the bulk dataset while SYNC-ing with master */
/// 中断.rdb文件传输
void replicationAbortSyncTransfer(void) {
    /// 确认当前master状态为传输.rdb文件中
    redisAssert(server.repl_state == REDIS_REPL_TRANSFER);

    /// 接下来停止.rdb文件的传输,关闭socket和文件描述符,删除.rdb临时文件,将master状态置为REDIS_REPL_CONNECT
    aeDeleteFileEvent(server.el,server.repl_transfer_s,AE_READABLE);
    close(server.repl_transfer_s);
    close(server.repl_transfer_fd);
    unlink(server.repl_transfer_tmpfile);
    zfree(server.repl_transfer_tmpfile);
    server.repl_state = REDIS_REPL_CONNECT;
}

/* Avoid the master to detect the slave is timing out while loading the
 * RDB file in initial synchronization. We send a single newline character
 * that is valid protocol but is guaranteed to either be sent entierly or
 * not, since the byte is indivisible.
 *
 * The function is called in two contexts: while we flush the current
 * data with emptyDb(), and while we load the new data received as an
 * RDB file from the master. */
/// 发送一个\n字节到master,用处暂时不明????????
void replicationSendNewlineToMaster(void) {
    static time_t newline_sent;
    /// 1s最多调一次
    if (time(NULL) != newline_sent) {
        /// 记录发送时间
        newline_sent = time(NULL);
        /// 写换行符到repl_transfer_s
        if (write(server.repl_transfer_s,"\n",1) == -1) {
            /* Pinging back in this stage is best-effort. */
        }
    }
}

/* Callback used by emptyDb() while flushing away old data to load
 * the new dataset received by the master. */
/// 清空db后的回调函数
void replicationEmptyDbCallback(void *privdata) {
    REDIS_NOTUSED(privdata);
    replicationSendNewlineToMaster();
}

/* Asynchronously read the SYNC payload we receive from a master */
#define REPL_MAX_WRITTEN_BEFORE_FSYNC (1024*1024*8) /* 8 MB */
/// 这是一个事件函数,重要!
/// slave读master发送的整个.rdb文件(包含读过程,错误处理,读完成处理)
/// 这个函数里面创建了server.master这个client
void readSyncBulkPayload(aeEventLoop *el, int fd, void *privdata, int mask) {
    char buf[4096];
    ssize_t nread, readlen;
    off_t left;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(privdata);
    REDIS_NOTUSED(mask);

    /* Static vars used to hold the EOF mark, and the last bytes received
     * form the server: when they match, we reached the end of the transfer. */
    static char eofmark[REDIS_RUN_ID_SIZE];
    static char lastbytes[REDIS_RUN_ID_SIZE];
    static int usemark = 0;

    /* If repl_transfer_size == -1 we still have to read the bulk length
     * from the master reply. */
    if (server.repl_transfer_size == -1) {  /// slave尚未知道master的.rdb文件大小
        /// 同步读master1024个字节,server.repl_syncio_timeout秒超时
        if (syncReadLine(fd,buf,1024,server.repl_syncio_timeout*1000) == -1) {
            redisLog(REDIS_WARNING,
                "I/O error reading bulk count from MASTER: %s",
                strerror(errno));
            goto error;
        }

        if (buf[0] == '-') { /// 看log可以知道,master因为错误中断了replication
            redisLog(REDIS_WARNING,
                "MASTER aborted replication with an error: %s",
                buf+1);
            goto error;
        } else if (buf[0] == '\0') { /// 读到了一个类似于ping/pong的字节
            /* At this stage just a newline works as a PING in order to take
             * the connection live. So we refresh our last interaction
             * timestamp. */
            server.repl_transfer_lastio = server.unixtime; /// 记录时间
            return;
        } else if (buf[0] != '$') { /// 协议错误,第一个字节一定得是'$'
            redisLog(REDIS_WARNING,"Bad protocol from MASTER, the first byte is not '$' (we received '%s'), are you sure the host and port are right?", buf);
            goto error;
        }

        /* There are two possible forms for the bulk payload. One is the
         * usual $<count> bulk format. The other is used for diskless transfers
         * when the master does not know beforehand the size of the file to
         * transfer. In the latter case, the following format is used:
         *
         * $EOF:<40 bytes delimiter>
         *
         * At the end of the file the announced delimiter is transmitted. The
         * delimiter is long and random enough that the probability of a
         * collision with the actual file content can be ignored. */
        /// 读到的master发来的协议格式为:$EOF:<40 byets delimiter>
        /// 读到这种格式,说明master尚未知道.rdb文件的大小,利用暂时生成的长随机数放在.rdb文件末端,通过对比eofmark确认是否读到了文件末尾
        if (strncmp(buf+1,"EOF:",4) == 0 && strlen(buf+5) >= REDIS_RUN_ID_SIZE) {
            usemark = 1;
            memcpy(eofmark,buf+5,REDIS_RUN_ID_SIZE); /// 保存eofmark
            memset(lastbytes,0,REDIS_RUN_ID_SIZE);
            /* Set any repl_transfer_size to avoid entering this code path
             * at the next call. */
            server.repl_transfer_size = 0; /// 暂时未知,设为0,下次调用不进入这个分支,也就是if (server.repl_transfer_size == -1)
            redisLog(REDIS_NOTICE,
                "MASTER <-> SLAVE sync: receiving streamed RDB from master");
        } else {
            usemark = 0;
            server.repl_transfer_size = strtol(buf+1,NULL,10); /// 保存master .rdb文件的大小到repl_transfer_size中
            redisLog(REDIS_NOTICE,
                "MASTER <-> SLAVE sync: receiving %lld bytes from master",
                (long long) server.repl_transfer_size);
        }
        return;
    }

    /* Read bulk data */
    if (usemark) { /// 每次读都是buf的大小4096
        readlen = sizeof(buf);
    } else { /// 每次都满一个buf或者剩余字节
        left = server.repl_transfer_size - server.repl_transfer_read;
        readlen = (left < (signed)sizeof(buf)) ? left : (signed)sizeof(buf);
    }

    nread = read(fd,buf,readlen);
    /// 读错误
    if (nread <= 0) {
        redisLog(REDIS_WARNING,"I/O error trying to sync with MASTER: %s",
            (nread == -1) ? strerror(errno) : "connection lost");
        /// 中断这个.rdb的流传输
        replicationAbortSyncTransfer();
        return;
    }
    /// 更新读取到的字节数
    server.stat_net_input_bytes += nread;

    /* When a mark is used, we want to detect EOF asap in order to avoid
     * writing the EOF mark into the file... */
    int eof_reached = 0;

    if (usemark) {
        /* Update the last bytes array, and check if it matches our delimiter.*/
        /// 每一次读取,如果超过了REDIS_RUN_ID_SIZE,那么要将尾端的REDIS_RUN_ID_SIZE个字节拷贝到lastbytes中,用于判断是否读到了EOF
        if (nread >= REDIS_RUN_ID_SIZE) {
            memcpy(lastbytes,buf+nread-REDIS_RUN_ID_SIZE,REDIS_RUN_ID_SIZE);
        } else { /// 读取到少于REDIS_RUN_ID_SIZE,将这部分拼在lastbytes尾部,左移lastbytes中对于的字节到正确的位置
            int rem = REDIS_RUN_ID_SIZE-nread;
            memmove(lastbytes,lastbytes+nread,rem);
            memcpy(lastbytes+rem,buf,nread);
        }
        /// 比对lastbytes和eofmark,相同则说明到了流的末端
        if (memcmp(lastbytes,eofmark,REDIS_RUN_ID_SIZE) == 0) 
        {
            eof_reached = 1;
        }
    }

    server.repl_transfer_lastio = server.unixtime;
    /// ????????
    if (write(server.repl_transfer_fd,buf,nread) != nread) {
        redisLog(REDIS_WARNING,"Write error or short write writing to the DB dump file needed for MASTER <-> SLAVE synchronization: %s", strerror(errno));
        goto error;
    }
    server.repl_transfer_read += nread;

    /* Delete the last 40 bytes from the file if we reached EOF. */
    /// 到达了EOF
    if (usemark && eof_reached) {
        /// 将文件末端的REDIS_RUN_ID_SIZE个字节裁剪掉
        if (ftruncate(server.repl_transfer_fd,
            server.repl_transfer_read - REDIS_RUN_ID_SIZE) == -1)
        {
            redisLog(REDIS_WARNING,"Error truncating the RDB file received from the master for SYNC: %s", strerror(errno));
            goto error;
        }
    }

    /* Sync data on disk from time to time, otherwise at the end of the transfer
     * we may suffer a big delay as the memory buffers are copied into the
     * actual disk. */
    /// 读取超过了8MB,同步数据到磁盘中,使用了性能较高的sync_file_range
    if (server.repl_transfer_read >=
        server.repl_transfer_last_fsync_off + REPL_MAX_WRITTEN_BEFORE_FSYNC)
    {
        /// 计算出本次同步的字节数
        off_t sync_size = server.repl_transfer_read -
                          server.repl_transfer_last_fsync_off;
        /// 从上次读取到的位置开始,将本次读取到的字节数sync到磁盘中
        rdb_fsync_range(server.repl_transfer_fd,
            server.repl_transfer_last_fsync_off, sync_size);
        server.repl_transfer_last_fsync_off += sync_size;
    }

    /* Check if the transfer is now complete */
    if (!usemark) { /// 判断非$EOF<>的是否到达了文件末尾
        if (server.repl_transfer_read == server.repl_transfer_size)
            eof_reached = 1;
    }

    if (eof_reached) { /// 整个master 的rdb文件都读完了
        /// 重命名接收到的rdb文件
        if (rename(server.repl_transfer_tmpfile,server.rdb_filename) == -1) {
            redisLog(REDIS_WARNING,"Failed trying to rename the temp DB into dump.rdb in MASTER <-> SLAVE synchronization: %s", strerror(errno));
            replicationAbortSyncTransfer();
            return;
        }

        /// 清空slave db的操作
        redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync: Flushing old data");
        signalFlushedDb(-1);
        /// 清db,完成后回调replicationEmptyDbCallback
        emptyDb(replicationEmptyDbCallback);
        /* Before loading the DB into memory we need to delete the readable
         * handler, otherwise it will get called recursively since
         * rdbLoad() will call the event loop to process events from time to
         * time for non blocking loading. */
        /// 删除读取master .rdb文件的事件函数
        aeDeleteFileEvent(server.el,server.repl_transfer_s,AE_READABLE);
        redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync: Loading DB in memory");
        /// 从.rdb文件中加载数据库
        if (rdbLoad(server.rdb_filename) != REDIS_OK) {
            redisLog(REDIS_WARNING,"Failed trying to load the MASTER synchronization DB from disk");
            replicationAbortSyncTransfer();
            return;
        }
        /* Final setup of the connected slave <- master link */
        /// 删除临时文件
        zfree(server.repl_transfer_tmpfile);
        /// 关闭保存.rdb文件的描述符
        close(server.repl_transfer_fd);
        /// 保存slave对应的master的各种配置
        server.master = createClient(server.repl_transfer_s);
        server.master->flags |= REDIS_MASTER;
        server.master->authenticated = 1;
        server.repl_state = REDIS_REPL_CONNECTED;
        server.master->reploff = server.repl_master_initial_offset;
        memcpy(server.master->replrunid, server.repl_master_runid,
            sizeof(server.repl_master_runid));
        /* If master offset is set to -1, this master is old and is not
         * PSYNC capable, so we flag it accordingly. */
        /// 不支持PSYNC
        if (server.master->reploff == -1)
            server.master->flags |= REDIS_PRE_PSYNC;
        redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync: Finished with success");
        /* Restart the AOF subsystem now that we finished the sync. This
         * will trigger an AOF rewrite, and when done will start appending
         * to the new file. */
        /// 尝试启动AOF
        if (server.aof_state != REDIS_AOF_OFF) {
            int retry = 10;

            stopAppendOnly();
            /// 重试10次 
            while (retry-- && startAppendOnly() == REDIS_ERR) {
                redisLog(REDIS_WARNING,"Failed enabling the AOF after successful master synchronization! Trying it again in one second.");
                sleep(1);
            }
            if (!retry) {
                redisLog(REDIS_WARNING,"FATAL: this slave instance finished the synchronization with its master, but the AOF can't be turned on. Exiting now.");
                exit(1);
            }
        }
    }

    return;

error:
    /// 发生了错误,中断.rdb文件的传输
    replicationAbortSyncTransfer();
    return;
}

/* Send a synchronous command to the master. Used to send AUTH and
 * REPLCONF commands before starting the replication with SYNC.
 *
 * The command returns an sds string representing the result of the
 * operation. On error the first byte is a "-".
 */
/// 将...（未知参数拼接后)写往描述符fd,并返回读fd返回的数据
char *sendSynchronousCommand(int fd, ...) {
    va_list ap;
    sds cmd = sdsempty();
    char *arg, buf[256];

    /* Create the command to send to the master, we use simple inline
     * protocol for simplicity as currently we only send simple strings. */
    va_start(ap,fd);
    /// 拼接参数在一条cmd中
    while(1) {
        arg = va_arg(ap, char*);
        if (arg == NULL) break;

        if (sdslen(cmd) != 0) cmd = sdscatlen(cmd," ",1);
        cmd = sdscat(cmd,arg);
    }
    cmd = sdscatlen(cmd,"\r\n",2);

    /* Transfer command to the server. */
    /// 将cmd同步写至fd
    if (syncWrite(fd,cmd,sdslen(cmd),server.repl_syncio_timeout*1000) == -1) {
        sdsfree(cmd);
        return sdscatprintf(sdsempty(),"-Writing to master: %s",
                strerror(errno));
    }
    sdsfree(cmd);

    /* Read the reply from the server. */
    /// 同步从fd读
    if (syncReadLine(fd,buf,sizeof(buf),server.repl_syncio_timeout*1000) == -1)
    {
        return sdscatprintf(sdsempty(),"-Reading from master: %s",
                strerror(errno));
    }
    /// 返回从fd读到的内容
    return sdsnew(buf);
}

/* Try a partial resynchronization with the master if we are about to reconnect.
 * If there is no cached master structure, at least try to issue a
 * "PSYNC ? -1" command in order to trigger a full resync using the PSYNC
 * command in order to obtain the master run id and the master replication
 * global offset.
 *
 * This function is designed to be called from syncWithMaster(), so the
 * following assumptions are made:
 *
 * 1) We pass the function an already connected socket "fd".
 * 2) This function does not close the file descriptor "fd". However in case
 *    of successful partial resynchronization, the function will reuse
 *    'fd' as file descriptor of the server.master client structure.
 *
 * The function returns:
 *
 * PSYNC_CONTINUE: If the PSYNC command succeded and we can continue.
 * PSYNC_FULLRESYNC: If PSYNC is supported but a full resync is needed.
 *                   In this case the master run_id and global replication
 *                   offset is saved.
 * PSYNC_NOT_SUPPORTED: If the server does not understand PSYNC at all and
 *                      the caller should fall back to SYNC.
 */

#define PSYNC_CONTINUE 0
#define PSYNC_FULLRESYNC 1
#define PSYNC_NOT_SUPPORTED 2
/// slave尝试进行PSYNC
int slaveTryPartialResynchronization(int fd) {
    char *psync_runid;
    char psync_offset[32];
    sds reply;

    /* Initially set repl_master_initial_offset to -1 to mark the current
     * master run_id and offset as not valid. Later if we'll be able to do
     * a FULL resync using the PSYNC command we'll set the offset at the
     * right value, so that this information will be propagated to the
     * client structure representing the master into server.master. */
    server.repl_master_initial_offset = -1;

    /// 缓存了master,将master_id和psync偏移量拼接在命令中
    if (server.cached_master) {
        psync_runid = server.cached_master->replrunid;
        snprintf(psync_offset,sizeof(psync_offset),"%lld", server.cached_master->reploff+1);
        redisLog(REDIS_NOTICE,"Trying a partial resynchronization (request %s:%s).", psync_runid, psync_offset);
    } else { /// 没缓存master,拼接'PSYNC ？ -1'强制全部同步
        redisLog(REDIS_NOTICE,"Partial resynchronization not possible (no cached master)");
        psync_runid = "?";
        memcpy(psync_offset,"-1",3);
    }

    /* Issue the PSYNC command */
    /// 发送命令至master,并得到响应reply
    reply = sendSynchronousCommand(fd,"PSYNC",psync_runid,psync_offset,NULL);

    /// 进入这个分支表示要进行一次FULLRESYNC
    if (!strncmp(reply,"+FULLRESYNC",11)) {
        char *runid = NULL, *offset = NULL;

        /* FULL RESYNC, parse the reply in order to extract the run id
         * and the replication offset. */
        runid = strchr(reply,' ');
        if (runid) {
            runid++;
            offset = strchr(runid,' ');
            if (offset) offset++;
        }
        /// 获取master run_id 错误(读到的格式错误)
        if (!runid || !offset || (offset-runid-1) != REDIS_RUN_ID_SIZE) {
            redisLog(REDIS_WARNING,
                "Master replied with wrong +FULLRESYNC syntax.");
            /* This is an unexpected condition, actually the +FULLRESYNC
             * reply means that the master supports PSYNC, but the reply
             * format seems wrong. To stay safe we blank the master
             * runid to make sure next PSYNCs will fail. */
            /// 清空repl_master_runid
            memset(server.repl_master_runid,0,REDIS_RUN_ID_SIZE+1);
        } else { 
            /// 保存master_runid
            memcpy(server.repl_master_runid, runid, offset-runid-1);
            server.repl_master_runid[REDIS_RUN_ID_SIZE] = '\0';
            /// 保存master偏移量
            server.repl_master_initial_offset = strtoll(offset,NULL,10);
            redisLog(REDIS_NOTICE,"Full resync from master: %s:%lld",
                server.repl_master_runid,
                server.repl_master_initial_offset);
        }
        /* We are going to full resync, discard the cached master structure. */
        replicationDiscardCachedMaster();
        sdsfree(reply);
        return PSYNC_FULLRESYNC;
    }

    /// 继续同步
    if (!strncmp(reply,"+CONTINUE",9)) {
        /* Partial resync was accepted, set the replication state accordingly */
        redisLog(REDIS_NOTICE,
            "Successful partial resynchronization with master.");
        sdsfree(reply);
        replicationResurrectCachedMaster(fd);
        return PSYNC_CONTINUE;
    }

    /* If we reach this point we receied either an error since the master does
     * not understand PSYNC, or an unexpected reply from the master.
     * Return PSYNC_NOT_SUPPORTED to the caller in both cases. */

    /// master出错
    if (strncmp(reply,"-ERR",4)) {
        /* If it's not an error, log the unexpected event. */
        redisLog(REDIS_WARNING,
            "Unexpected reply to PSYNC from master: %s", reply);
    } else { /// master出现未知错误或者不支持PSYNC
        redisLog(REDIS_NOTICE,
            "Master does not support PSYNC or is in "
            "error state (reply: %s)", reply);
    }
    sdsfree(reply);
    replicationDiscardCachedMaster();
    return PSYNC_NOT_SUPPORTED;
}

/// 这是一个事件函数
/// slave与master同步事件函数
/// 这个事件函数本身只会存在短时间
/// 当一个slave与master连接上,会做以下几件事
/// 1) 发送PING至master 
/// 2) 接收来自master的PONG
/// 3) 尝试PSYNC,如果成功,马上可以进入master发送命令给slave,slave回ack这种状态,如果PSYNC失败,则准备SYNC
/// 4) 创建接收master发送的.rdb事件函数,准备接收master的.rdb
void syncWithMaster(aeEventLoop *el, int fd, void *privdata, int mask) {
    char tmpfile[256], *err;
    int dfd, maxtries = 5;
    int sockerr = 0, psync_result;
    socklen_t errlen = sizeof(sockerr);
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(privdata);
    REDIS_NOTUSED(mask);

    /* If this event fired after the user turned the instance into a master
     * with SLAVEOF NO ONE we must just return ASAP. */
    /// 没有replaction
    if (server.repl_state == REDIS_REPL_NONE) {
        close(fd);
        return;
    }

    /* Check for errors in the socket. */
    /// 检查socket是否存在错误
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1)
        sockerr = errno;
    if (sockerr) {
        /// 存在错误则删除本描述符上的读/写事件函数(其实就是删除这整个事件函数本身)
        aeDeleteFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE);
        redisLog(REDIS_WARNING,"Error condition on socket for SYNC: %s",
            strerror(sockerr));
        goto error;
    }

    /* If we were connecting, it's time to send a non blocking PING, we want to
     * make sure the master is able to reply before going into the actual
     * replication process where we have long timeouts in the order of
     * seconds (in the meantime the slave would block). */
    /// slave正与master连接中
    if (server.repl_state == REDIS_REPL_CONNECTING) {
        redisLog(REDIS_NOTICE,"Non blocking connect for SYNC fired the event.");
        /* Delete the writable event so that the readable event remains
         * registered and we can wait for the PONG reply. */
        /// 因为发送完了PING之后就不需要写事件了,而是等待MASTER的PONG,可以将写事件删除
        aeDeleteFileEvent(server.el,fd,AE_WRITABLE);
        server.repl_state = REDIS_REPL_RECEIVE_PONG;
        /* Send the PING, don't check for errors at all, we have the timeout
         * that will take care about this. */
        /// 往master发送ping命令,确认是否可以收到pong,以最大程度保证接下来的同步
        syncWrite(fd,"PING\r\n",6,100);
        return;
    }

    /* Receive the PONG command. */
    /// 已经发送了ping,等待接收pong
    if (server.repl_state == REDIS_REPL_RECEIVE_PONG) {
        char buf[1024];

        /* Delete the readable event, we no longer need it now that there is
         * the PING reply to read. */
        /// 已经从MASTER接收到了PONG,不再需要读任何的PONG,可以将这个描述符上的读事件删除,到这里,这个函数就再也不会进入了
        aeDeleteFileEvent(server.el,fd,AE_READABLE);

        /* Read the reply with explicit timeout. */
        buf[0] = '\0';
        /// 从master同步读取一行
        if (syncReadLine(fd,buf,sizeof(buf),
            server.repl_syncio_timeout*1000) == -1)
        {
            redisLog(REDIS_WARNING,
                "I/O error reading PING reply from master: %s",
                strerror(errno));
            goto error;
        }

        /* We accept only two replies as valid, a positive +PONG reply
         * (we just check for "+") or an authentication error.
         * Note that older versions of Redis replied with "operation not
         * permitted" instead of using a proper error code, so we test
         * both. */
        /// 接收到的不是这三者中的任意一种,表明出现了错误
        if (buf[0] != '+' &&
            strncmp(buf,"-NOAUTH",7) != 0 &&
            strncmp(buf,"-ERR operation not permitted",28) != 0)
        {
            redisLog(REDIS_WARNING,"Error reply to PING from master: '%s'",buf);
            goto error;
        } else { /// 成功接收pong命令
            redisLog(REDIS_NOTICE,
                "Master replied to PING, replication can continue...");
        }
    }

    /* AUTH with the master if required. */
    if(server.masterauth) {
        /// 发送用户名/密码认证到master
        err = sendSynchronousCommand(fd,"AUTH",server.masterauth,NULL);
        if (err[0] == '-') {
            redisLog(REDIS_WARNING,"Unable to AUTH to MASTER: %s",err);
            sdsfree(err);
            goto error;
        }
        sdsfree(err);
    }

    /* Set the slave port, so that Master's INFO command can list the
     * slave listening port correctly. */
    /// 设置slave的端口
    {
        sds port = sdsfromlonglong(server.port);
        err = sendSynchronousCommand(fd,"REPLCONF","listening-port",port,
                                         NULL);
        sdsfree(port);
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF listening-port. */
        if (err[0] == '-') {
            redisLog(REDIS_NOTICE,"(Non critical) Master does not understand REPLCONF listening-port: %s", err);
        }
        sdsfree(err);
    }

    /* Try a partial resynchonization. If we don't have a cached master
     * slaveTryPartialResynchronization() will at least try to use PSYNC
     * to start a full resynchronization so that we get the master run id
     * and the global offset, to try a partial resync at the next
     * reconnection attempt. */
    /// 尝试进行psync
    psync_result = slaveTryPartialResynchronization(fd);
    if (psync_result == PSYNC_CONTINUE) {
        redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync: Master accepted a Partial Resynchronization.");
        return;
    }

    /* Fall back to SYNC if needed. Otherwise psync_result == PSYNC_FULLRESYNC
     * and the server.repl_master_runid and repl_master_initial_offset are
     * already populated. */
    /// master不支持psync
    if (psync_result == PSYNC_NOT_SUPPORTED) {
        redisLog(REDIS_NOTICE,"Retrying with SYNC...");
        /// 发送sync到master
        if (syncWrite(fd,"SYNC\r\n",6,server.repl_syncio_timeout*1000) == -1) {
            redisLog(REDIS_WARNING,"I/O error writing to MASTER: %s",
                strerror(errno));
            goto error;
        }
    }

    /* Prepare a suitable temp file for bulk transfer */
    /// 打开/创建tmp文件用于接收master的.rdb文件
    while(maxtries--) {
        snprintf(tmpfile,256,
            "temp-%d.%ld.rdb",(int)server.unixtime,(long int)getpid());
        dfd = open(tmpfile,O_CREAT|O_WRONLY|O_EXCL,0644);
        if (dfd != -1) break;
        sleep(1);
    }
    if (dfd == -1) {
        redisLog(REDIS_WARNING,"Opening the temp file needed for MASTER <-> SLAVE synchronization: %s",strerror(errno));
        goto error;
    }

    /* Setup the non blocking download of the bulk file. */
    /// 创建从master读取.rdb文件的事件函数
    if (aeCreateFileEvent(server.el,fd, AE_READABLE,readSyncBulkPayload,NULL)
            == AE_ERR)
    {
        redisLog(REDIS_WARNING,
            "Can't create readable event for SYNC: %s (fd=%d)",
            strerror(errno),fd);
        goto error;
    }

    /// 保存slave的各种状态
    server.repl_state = REDIS_REPL_TRANSFER;
    server.repl_transfer_size = -1;
    server.repl_transfer_read = 0;
    server.repl_transfer_last_fsync_off = 0;
    server.repl_transfer_fd = dfd;
    server.repl_transfer_lastio = server.unixtime;
    server.repl_transfer_tmpfile = zstrdup(tmpfile);
    return;

error:
    close(fd);
    server.repl_transfer_s = -1;
    server.repl_state = REDIS_REPL_CONNECT;
    return;
}

/// slave连接到master,并创建同步的事件函数
int connectWithMaster(void) {
    int fd;

    /// 创建sock描述符
    fd = anetTcpNonBlockBindConnect(NULL,
        server.masterhost,server.masterport,REDIS_BIND_ADDR);
    if (fd == -1) {
        redisLog(REDIS_WARNING,"Unable to connect to MASTER: %s",
            strerror(errno));
        return REDIS_ERR;
    }

    /// 为什么这个事件函数绑定了READABLE|WRITEABLE ????????
    /// 可能是因为syncWithMaster中有read|write fd的操作,且有主动删除读/写操作
    if (aeCreateFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE,syncWithMaster,NULL) ==
            AE_ERR)
    {
        close(fd);
        redisLog(REDIS_WARNING,"Can't create readable event for SYNC");
        return REDIS_ERR;
    }

    /// 保存状态
    server.repl_transfer_lastio = server.unixtime;
    server.repl_transfer_s = fd;
    server.repl_state = REDIS_REPL_CONNECTING;
    return REDIS_OK;
}

/* This function can be called when a non blocking connection is currently
 * in progress to undo it. */
/// 关闭slave与master的连接,删除描述符上的事件函数
void undoConnectWithMaster(void) {
    int fd = server.repl_transfer_s;

    /// 检查是否已经连接
    redisAssert(server.repl_state == REDIS_REPL_CONNECTING ||
                server.repl_state == REDIS_REPL_RECEIVE_PONG);
    /// 删除事件函数
    aeDeleteFileEvent(server.el,fd,AE_READABLE|AE_WRITABLE);
    close(fd);
    server.repl_transfer_s = -1;
    server.repl_state = REDIS_REPL_CONNECT;
}

/* This function aborts a non blocking replication attempt if there is one
 * in progress, by canceling the non-blocking connect attempt or
 * the initial bulk transfer.
 *
 * If there was a replication handshake in progress 1 is returned and
 * the replication state (server.repl_state) set to REDIS_REPL_CONNECT.
 *
 * Otherwise zero is returned and no operation is perforemd at all. */
/// 根据slave与master连接状态,中断slave与master的连接/传输
int cancelReplicationHandshake(void) {
    /// 如果已经在传输.rdb文件,那么中断传送
    if (server.repl_state == REDIS_REPL_TRANSFER) {
        replicationAbortSyncTransfer();
    } 
    else if (server.repl_state == REDIS_REPL_CONNECTING || server.repl_state == REDIS_REPL_RECEIVE_PONG)
    {   /// 连接中或者刚连接上,断开与master的连接
        undoConnectWithMaster();
    } else {
        return 0;
    }
    return 1;
}

/* Set replication to the specified master address and port. */
/// 设置slave的master为ip,port并做一些释放旧的slave工作
void replicationSetMaster(char *ip, int port) {
    /// 删除原来的master
    sdsfree(server.masterhost);
    /// 指定新的master ip
    server.masterhost = sdsnew(ip);
    /// 指定新的master port
    server.masterport = port;
    /// 释放原来的master
    /// slave<->master互为client
    if (server.master) 
    {
        freeClient(server.master);
    }

    /// 将block的client断开
    disconnectAllBlockedClients(); /* Clients blocked in master, now slave. */
    /// 将(旧的)slave全部断开
    disconnectSlaves(); /* Force our slaves to resync with us as well. */
    /// 将cache的master丢弃
    replicationDiscardCachedMaster(); /* Don't try a PSYNC. */
    /// 释放repl_backlog
    freeReplicationBacklog(); /* Don't allow our chained slaves to PSYNC. */
    /// 中断与master的连接
    cancelReplicationHandshake();

    /// 重新设置状态
    server.repl_state = REDIS_REPL_CONNECT;
    server.master_repl_offset = 0;
    server.repl_down_since = 0;
}

/* Cancel replication, setting the instance as a master itself. */
/// 取消replication,原先身份为slave,现升级成为了master
void replicationUnsetMaster(void) {
    /// masterhost为空
    if (server.masterhost == NULL) 
    {
        return; /* Nothing to do. */
    }

    sdsfree(server.masterhost);
    server.masterhost = NULL;

    if (server.master) {
        if (listLength(server.slaves) == 0) {
            /* If this instance is turned into a master and there are no
             * slaves, it inherits the replication offset from the master.
             * Under certain conditions this makes replicas comparable by
             * replication offset to understand what is the most updated. */
            /// 将slave的master_repl_offset设置为之前他所同步的master的reploff,现在这台slave已经是已master身份运行
            server.master_repl_offset = server.master->reploff;
            freeReplicationBacklog();
        }
        /// 将以前这台slave所属的master client释放
        freeClient(server.master);
    }
    
    /// 进行关闭清理工作
    replicationDiscardCachedMaster();
    cancelReplicationHandshake();
    /// 记录保存状态
    server.repl_state = REDIS_REPL_NONE;
}

/// SLAVEOF ip port
/// 主动成为ip/port的slave
void slaveofCommand(redisClient *c) {
    /* SLAVEOF is not allowed in cluster mode as replication is automatically
     * configured using the current address of the master node. */
    /// cluster模式下不允许slaveof
    if (server.cluster_enabled) {
        addReplyError(c,"SLAVEOF not allowed in cluster mode.");
        return;
    }

    /* The special host/port combination "NO" "ONE" turns the instance
     * into a master. Otherwise the new master address is set. */
    /// SLAVEOF NO ONE表示我不想成为任何人的slave,我自己就是master
    if (!strcasecmp(c->argv[1]->ptr,"no") &&
        !strcasecmp(c->argv[2]->ptr,"one")) {
        if (server.masterhost) {
            replicationUnsetMaster();
            redisLog(REDIS_NOTICE,"MASTER MODE enabled (user request)");
        }
    } else {
        long port;

        if ((getLongFromObjectOrReply(c, c->argv[2], &port, NULL) != REDIS_OK))
            return;

        /* Check if we are already attached to the specified slave */
        /// 检查是否已经成为了这个ip的slave
        if (server.masterhost && !strcasecmp(server.masterhost,c->argv[1]->ptr)
            && server.masterport == port) {
            redisLog(REDIS_NOTICE,"SLAVE OF would result into synchronization with the master we are already connected with. No operation performed.");
            addReplySds(c,sdsnew("+OK Already connected to specified master\r\n"));
            return;
        }
        /* There was no previous master or the user specified a different one,
         * we can continue. */
        /// 设置ip:port为master
        replicationSetMaster(c->argv[1]->ptr, port);
        redisLog(REDIS_NOTICE,"SLAVE OF %s:%d enabled (user request)",
            server.masterhost, server.masterport);
    }
    addReply(c,shared.ok);
}

/* ROLE command: provide information about the role of the instance
 * (master or slave) and additional information related to replication
 * in an easy to process format. */
/// ROLE拿到当前redis-server的角色master|slave 并打印具体信息
void roleCommand(redisClient *c) {
    /// master
    if (server.masterhost == NULL) {
        listIter li;
        listNode *ln;
        void *mbcount;
        int slaves = 0;

        addReplyMultiBulkLen(c,3);
        /// 返回身份标识
        addReplyBulkCBuffer(c,"master",6);
        /// 返回master_repl_offset
        addReplyLongLong(c,server.master_repl_offset);
        /// 返回slave列表
        mbcount = addDeferredMultiBulkLength(c);
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            redisClient *slave = ln->value;
            char ip[REDIS_IP_STR_LEN];

            if (anetPeerToString(slave->fd,ip,sizeof(ip),NULL) == -1) continue;
            if (slave->replstate != REDIS_REPL_ONLINE) continue;
            /// slave列表一个item包含3项信息
            addReplyMultiBulkLen(c,3);
            /// slave ip
            addReplyBulkCString(c,ip);
            /// slave port
            addReplyBulkLongLong(c,slave->slave_listening_port);
            /// slave repl_ack_off
            addReplyBulkLongLong(c,slave->repl_ack_off);
            slaves++;
        }
        setDeferredMultiBulkLength(c,mbcount,slaves);
    } else { /// slave
        char *slavestate = NULL;

        /// 返回5个item
        addReplyMultiBulkLen(c,5);
        addReplyBulkCBuffer(c,"slave",5);
        /// master ip
        addReplyBulkCString(c,server.masterhost);
        /// master port
        addReplyLongLong(c,server.masterport);
        /// master <-> slvae 状态
        switch(server.repl_state) {
        case REDIS_REPL_NONE: slavestate = "none"; break;
        case REDIS_REPL_CONNECT: slavestate = "connect"; break;
        case REDIS_REPL_CONNECTING: slavestate = "connecting"; break;
        case REDIS_REPL_RECEIVE_PONG: /* see next */
        case REDIS_REPL_TRANSFER: slavestate = "sync"; break;
        case REDIS_REPL_CONNECTED: slavestate = "connected"; break;
        default: slavestate = "unknown"; break;
        }
        addReplyBulkCString(c,slavestate);
        /// master reploff
        addReplyLongLong(c,server.master ? server.master->reploff : -1);
    }
}

/* Send a REPLCONF ACK command to the master to inform it about the current
 * processed offset. If we are not connected with a master, the command has
 * no effects. */
/// 发送reploff的偏移量ACK到master
void replicationSendAck(void) {
    redisClient *c = server.master;

    if (c != NULL) {
        /// 置REDIS_MASTER_FORCE_REPLY位
        c->flags |= REDIS_MASTER_FORCE_REPLY;
        addReplyMultiBulkLen(c,3);
        /// 发送命令REPLCONF ACK c->reploff到master
        addReplyBulkCString(c,"REPLCONF");
        addReplyBulkCString(c,"ACK");
        addReplyBulkLongLong(c,c->reploff);
        /// 清空REDIS_MASTER_FORCE_REPLY位
        c->flags &= ~REDIS_MASTER_FORCE_REPLY;
    }
}

/* ---------------------- MASTER CACHING FOR PSYNC -------------------------- */

/* In order to implement partial synchronization we need to be able to cache
 * our master's client structure after a transient disconnection.
 * It is cached into server.cached_master and flushed away using the following
 * functions. */

/* This function is called by freeClient() in order to cache the master
 * client structure instead of destryoing it. freeClient() will return
 * ASAP after this function returns, so every action needed to avoid problems
 * with a client that is really "suspended" has to be done by this function.
 *
 * The other functions that will deal with the cached master are:
 *
 * replicationDiscardCachedMaster() that will make sure to kill the client
 * as for some reason we don't want to use it in the future.
 *
 * replicationResurrectCachedMaster() that is used after a successful PSYNC
 * handshake in order to reactivate the cached master.
 */
void replicationCacheMaster(redisClient *c) {
    listNode *ln;

    /// 确保master存在且暂未cached_master
    redisAssert(server.master != NULL && server.cached_master == NULL);
    redisLog(REDIS_NOTICE,"Caching the disconnected master state.");

    /* Remove from the list of clients, we don't want this client to be
     * listed by CLIENT LIST or processed in any way by batch operations. */
    /// 将client从server.clients列表中删除
    /// 这个client扮演的是什么角色????????
    ln = listSearchKey(server.clients,c);
    redisAssert(ln != NULL);
    listDelNode(server.clients,ln);

    /* Save the master. Server.master will be set to null later by
     * replicationHandleMasterDisconnection(). */
    /// cache住master
    /// cached_master不是任何时候都cached的,而是在slave想断开master之前cached,下一次连接可以从cached的状态恢复
    server.cached_master = server.master;

    /* Remove the event handlers and close the socket. We'll later reuse
     * the socket of the new connection with the master during PSYNC. */
    /// 删除client上的读写事件函数
    aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
    aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
    close(c->fd);

    /* Set fd to -1 so that we can safely call freeClient(c) later. */
    c->fd = -1;

    /* Invalidate the Peer ID cache. */
    /// 清空client唯一id
    if (c->peerid) {
        sdsfree(c->peerid);
        c->peerid = NULL;
    }

    /* Caching the master happens instead of the actual freeClient() call,
     * so make sure to adjust the replication state. This function will
     * also set server.master to NULL. */
    replicationHandleMasterDisconnection();
}

/* Free a cached master, called when there are no longer the conditions for
 * a partial resync on reconnection. */
/// 将cached_master丢弃
void replicationDiscardCachedMaster(void) {
    if (server.cached_master == NULL) return;

    redisLog(REDIS_NOTICE,"Discarding previously cached master state.");
    server.cached_master->flags &= ~REDIS_MASTER;
    /// 关闭cached_master
    freeClient(server.cached_master);
    server.cached_master = NULL;
}

/* Turn the cached master into the current master, using the file descriptor
 * passed as argument as the socket for the new master.
 *
 * This function is called when successfully setup a partial resynchronization
 * so the stream of data that we'll receive will start from were this
 * master left. */
/// Resurrect:复活
/// 将master从cached_master中恢复,并创建事件函数,如果必要的话
void replicationResurrectCachedMaster(int newfd) {
    /// 将master从cached_master中恢复
    server.master = server.cached_master;
    server.cached_master = NULL;
    server.master->fd = newfd;
    server.master->flags &= ~(REDIS_CLOSE_AFTER_REPLY|REDIS_CLOSE_ASAP);
    server.master->authenticated = 1;
    server.master->lastinteraction = server.unixtime;
    server.repl_state = REDIS_REPL_CONNECTED;

    /* Re-add to the list of clients. */
    /// 添加master client到slave.client列表中
    listAddNodeTail(server.clients,server.master);
    /// 创建server.master上的描述符可读事件函数
    if (aeCreateFileEvent(server.el, newfd, AE_READABLE,
                          readQueryFromClient, server.master)) {
        redisLog(REDIS_WARNING,"Error resurrecting the cached master, impossible to add the readable handler: %s", strerror(errno));
        freeClientAsync(server.master); /* Close ASAP. */
    }

    /* We may also need to install the write handler as well if there is
     * pending data in the write buffers. */
    /// 如果master写缓冲区中有数据,创建可写事件函数
    if (server.master->bufpos || listLength(server.master->reply)) {
        if (aeCreateFileEvent(server.el, newfd, AE_WRITABLE,
                          sendReplyToClient, server.master)) {
            redisLog(REDIS_WARNING,"Error resurrecting the cached master, impossible to add the writable handler: %s", strerror(errno));
            freeClientAsync(server.master); /* Close ASAP. */
        }
    }
}

/* ------------------------- MIN-SLAVES-TO-WRITE  --------------------------- */

/* This function counts the number of slaves with lag <= min-slaves-max-lag.
 * If the option is active, the server will prevent writes if there are not
 * enough connected slaves with the specified lag (or less). */
/// 刷新网络状态良好的slave数量
void refreshGoodSlavesCount(void) {
    listIter li;
    listNode *ln;
    int good = 0;

    /// server没有做最少slave和slave最低延迟限制,直接返回
    if (!server.repl_min_slaves_to_write ||
        !server.repl_min_slaves_max_lag) return;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;
        time_t lag = server.unixtime - slave->repl_ack_time;

        /// 网络一个来回的时间在repl_min_slaves_max_lag里面
        if (slave->replstate == REDIS_REPL_ONLINE &&
            lag <= server.repl_min_slaves_max_lag) good++;
    }
    /// 刷新网络状态良好的slave数量
    server.repl_good_slaves_count = good;
}

/// LUA SCRIPT相关的暂且不看
/* ----------------------- REPLICATION SCRIPT CACHE --------------------------
 * The goal of this code is to keep track of scripts already sent to every
 * connected slave, in order to be able to replicate EVALSHA as it is without
 * translating it to EVAL every time it is possible.
 *
 * We use a capped collection implemented by a hash table for fast lookup
 * of scripts we can send as EVALSHA, plus a linked list that is used for
 * eviction of the oldest entry when the max number of items is reached.
 *
 * We don't care about taking a different cache for every different slave
 * since to fill the cache again is not very costly, the goal of this code
 * is to avoid that the same big script is trasmitted a big number of times
 * per second wasting bandwidth and processor speed, but it is not a problem
 * if we need to rebuild the cache from scratch from time to time, every used
 * script will need to be transmitted a single time to reappear in the cache.
 *
 * This is how the system works:
 *
 * 1) Every time a new slave connects, we flush the whole script cache.
 * 2) We only send as EVALSHA what was sent to the master as EVALSHA, without
 *    trying to convert EVAL into EVALSHA specifically for slaves.
 * 3) Every time we trasmit a script as EVAL to the slaves, we also add the
 *    corresponding SHA1 of the script into the cache as we are sure every
 *    slave knows about the script starting from now.
 * 4) On SCRIPT FLUSH command, we replicate the command to all the slaves
 *    and at the same time flush the script cache.
 * 5) When the last slave disconnects, flush the cache.
 * 6) We handle SCRIPT LOAD as well since that's how scripts are loaded
 *    in the master sometimes.
 */

/* Initialize the script cache, only called at startup. */
void replicationScriptCacheInit(void) {
    server.repl_scriptcache_size = 10000;
    server.repl_scriptcache_dict = dictCreate(&replScriptCacheDictType,NULL);
    server.repl_scriptcache_fifo = listCreate();
}

/* Empty the script cache. Should be called every time we are no longer sure
 * that every slave knows about all the scripts in our set, or when the
 * current AOF "context" is no longer aware of the script. In general we
 * should flush the cache:
 *
 * 1) Every time a new slave reconnects to this master and performs a
 *    full SYNC (PSYNC does not require flushing).
 * 2) Every time an AOF rewrite is performed.
 * 3) Every time we are left without slaves at all, and AOF is off, in order
 *    to reclaim otherwise unused memory.
 */
void replicationScriptCacheFlush(void) {
    dictEmpty(server.repl_scriptcache_dict,NULL);
    listRelease(server.repl_scriptcache_fifo);
    server.repl_scriptcache_fifo = listCreate();
}

/* Add an entry into the script cache, if we reach max number of entries the
 * oldest is removed from the list. */
void replicationScriptCacheAdd(sds sha1) {
    int retval;
    sds key = sdsdup(sha1);

    /* Evict oldest. */
    if (listLength(server.repl_scriptcache_fifo) == server.repl_scriptcache_size)
    {
        listNode *ln = listLast(server.repl_scriptcache_fifo);
        sds oldest = listNodeValue(ln);

        retval = dictDelete(server.repl_scriptcache_dict,oldest);
        redisAssert(retval == DICT_OK);
        listDelNode(server.repl_scriptcache_fifo,ln);
    }

    /* Add current. */
    retval = dictAdd(server.repl_scriptcache_dict,key,NULL);
    listAddNodeHead(server.repl_scriptcache_fifo,key);
    redisAssert(retval == DICT_OK);
}

/* Returns non-zero if the specified entry exists inside the cache, that is,
 * if all the slaves are aware of this script SHA1. */
int replicationScriptCacheExists(sds sha1) {
    return dictFind(server.repl_scriptcache_dict,sha1) != NULL;
}

/* ----------------------- SYNCHRONOUS REPLICATION --------------------------
 * Redis synchronous replication design can be summarized in points:
 *
 * - Redis masters have a global replication offset, used by PSYNC.
 * - Master increment the offset every time new commands are sent to slaves.
 * - Slaves ping back masters with the offset processed so far.
 *
 * So synchronous replication adds a new WAIT command in the form:
 *
 *   WAIT <num_replicas> <milliseconds_timeout>
 *
 * That returns the number of replicas that processed the query when
 * we finally have at least num_replicas, or when the timeout was
 * reached.
 *
 * The command is implemented in this way:
 *
 * - Every time a client processes a command, we remember the replication
 *   offset after sending that command to the slaves.
 * - When WAIT is called, we ask slaves to send an acknowledgement ASAP.
 *   The client is blocked at the same time (see blocked.c).
 * - Once we receive enough ACKs for a given offset or when the timeout
 *   is reached, the WAIT command is unblocked and the reply sent to the
 *   client.
 */

/* This just set a flag so that we broadcast a REPLCONF GETACK command
 * to all the slaves in the beforeSleep() function. Note that this way
 * we "group" all the clients that want to wait for synchronouns replication
 * in a given event loop iteration, and send a single GETACK for them all. */
void replicationRequestAckFromSlaves(void) {
    server.get_ack_from_slaves = 1;
}

/* Return the number of slaves that already acknowledged the specified
 * replication offset. */
/// 返回slave列表中状态为REDIS_REPL_ONLINE且repl_ack_off大于offset的数量
int replicationCountAcksByOffset(long long offset) {
    listIter li;
    listNode *ln;
    int count = 0;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        redisClient *slave = ln->value;

        if (slave->replstate != REDIS_REPL_ONLINE) continue;
        if (slave->repl_ack_off >= offset) count++;
    }
    return count;
}

/* WAIT for N replicas to acknowledge the processing of our latest
 * write command (and all the previous commands). */
/// WAIT N timeout
/// 暂且还不知道这个命令有啥用????????
void waitCommand(redisClient *c) {
    mstime_t timeout;
    long numreplicas, ackreplicas;
    long long offset = c->woff;

    /* Argument parsing. */
    /// 解析参数中的numreplicas数目
    if (getLongFromObjectOrReply(c,c->argv[1],&numreplicas,NULL) != REDIS_OK)
        return;
    /// 解析timeout参数
    if (getTimeoutFromObjectOrReply(c,c->argv[2],&timeout,UNIT_MILLISECONDS)
        != REDIS_OK) return;

    /* First try without blocking at all. */
    /// 获取repl_ack_off超过了c->woff的slave数
    ackreplicas = replicationCountAcksByOffset(c->woff);
    /// 如果满足,直接返回
    if (ackreplicas >= numreplicas || c->flags & REDIS_MULTI) {
        addReplyLongLong(c,ackreplicas);
        return;
    }

    /* Otherwise block the client and put it into our list of clients
     * waiting for ack from slaves. */
    /// 将client阻塞
    c->bpop.timeout = timeout;
    c->bpop.reploffset = offset;
    c->bpop.numreplicas = numreplicas;
    listAddNodeTail(server.clients_waiting_acks,c);
    blockClient(c,REDIS_BLOCKED_WAIT);

    /* Make sure that the server will send an ACK request to all the slaves
     * before returning to the event loop. */
    replicationRequestAckFromSlaves();
}

/* This is called by unblockClient() to perform the blocking op type
 * specific cleanup. We just remove the client from the list of clients
 * waiting for replica acks. Never call it directly, call unblockClient()
 * instead. */
/// 将阻塞的client从等待replication中接触阻塞
void unblockClientWaitingReplicas(redisClient *c) {
    listNode *ln = listSearchKey(server.clients_waiting_acks,c);
    redisAssert(ln != NULL);
    listDelNode(server.clients_waiting_acks,ln);
}

/* Check if there are clients blocked in WAIT that can be unblocked since
 * we received enough ACKs from slaves. */
/// 处理等待repl ack的client并将满足条件的client接触阻塞
void processClientsWaitingReplicas(void) {
    long long last_offset = 0;
    int last_numreplicas = 0;

    listIter li;
    listNode *ln;

    listRewind(server.clients_waiting_acks,&li);
    while((ln = listNext(&li))) {
        redisClient *c = ln->value;

        /* Every time we find a client that is satisfied for a given
         * offset and number of replicas, we remember it so the next client
         * may be unblocked without calling replicationCountAcksByOffset()
         * if the requested offset / replicas were equal or less. */

        if (last_offset && last_offset > c->bpop.reploffset &&
                           last_numreplicas > c->bpop.numreplicas)
        {
            unblockClient(c);
            addReplyLongLong(c,last_numreplicas);
        } else {
            int numreplicas = replicationCountAcksByOffset(c->bpop.reploffset);

            if (numreplicas >= c->bpop.numreplicas) {
                last_offset = c->bpop.reploffset;
                last_numreplicas = numreplicas;
                unblockClient(c);
                addReplyLongLong(c,numreplicas);
            }
        }
    }
}

/* Return the slave replication offset for this instance, that is
 * the offset for which we already processed the master replication stream. */
/// 返回master或者cached master中的offset
long long replicationGetSlaveOffset(void) {
    long long offset = 0;

    if (server.masterhost != NULL) {
        if (server.master) {
            offset = server.master->reploff;
        } else if (server.cached_master) {
            offset = server.cached_master->reploff;
        }
    }
    /* offset may be -1 when the master does not support it at all, however
     * this function is designed to return an offset that can express the
     * amount of data processed by the master, so we return a positive
     * integer. */
    if (offset < 0) offset = 0;
    return offset;
}

/* --------------------------- REPLICATION CRON  ---------------------------- */

/* Replication cron function, called 1 time per second. */
/// replication周期函数
void replicationCron(void) {
    /* Non blocking connection timeout? */
    /// 检查与master的连接是否超时
    if (server.masterhost &&
        (server.repl_state == REDIS_REPL_CONNECTING ||
         server.repl_state == REDIS_REPL_RECEIVE_PONG) &&
        (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        redisLog(REDIS_WARNING,"Timeout connecting to the MASTER...");
        /// 断开与master的连接
        undoConnectWithMaster();
    }

    /* Bulk transfer I/O timeout? */
    /// 检查slave与master传输.rdb是否超时
    if (server.masterhost && server.repl_state == REDIS_REPL_TRANSFER &&
        (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        redisLog(REDIS_WARNING,"Timeout receiving bulk data from MASTER... If the problem persists try to set the 'repl-timeout' parameter in redis.conf to a larger value.");
        /// 中断.rdb文件传输
        replicationAbortSyncTransfer();
    }

    /* Timed out master when we are an already connected slave? */
    /// 检查master<->slave同步是否超时
    if (server.masterhost && server.repl_state == REDIS_REPL_CONNECTED &&
        (time(NULL)-server.master->lastinteraction) > server.repl_timeout)
    {
        redisLog(REDIS_WARNING,"MASTER timeout: no data nor PING received...");
        freeClient(server.master);
    }

    /* Check if we should connect to a MASTER */
    /// 检查是否可以连上master
    if (server.repl_state == REDIS_REPL_CONNECT) {
        redisLog(REDIS_NOTICE,"Connecting to MASTER %s:%d",
            server.masterhost, server.masterport);
        /// 尝试连接至master
        if (connectWithMaster() == REDIS_OK) {
            redisLog(REDIS_NOTICE,"MASTER <-> SLAVE sync started");
        }
    }

    /* Send ACK to master from time to time.
     * Note that we do not send periodic acks to masters that don't
     * support PSYNC and replication offsets. */
    /// 这是一个slave且master支持psync
    if (server.masterhost && server.master &&
        !(server.master->flags & REDIS_PRE_PSYNC))
    {
        /// 发送ack到master
        replicationSendAck();
    }

    /* If we have attached slaves, PING them from time to time.
     * So slaves can implement an explicit timeout to masters, and will
     * be able to detect a link disconnection even if the TCP connection
     * will not actually go down. */
    /// 到了发送ping给slave的周期
    if (!(server.cronloops % (server.repl_ping_slave_period * server.hz))) {
        listIter li;
        listNode *ln;
        robj *ping_argv[1];

        /* First, send PING */
        ping_argv[0] = createStringObject("PING",4);
        /// 发送PING给所有的slave
        replicationFeedSlaves(server.slaves, server.slaveseldb, ping_argv, 1);
        decrRefCount(ping_argv[0]);

        /* Second, send a newline to all the slaves in pre-synchronization
         * stage, that is, slaves waiting for the master to create the RDB file.
         * The newline will be ignored by the slave but will refresh the
         * last-io timer preventing a timeout. */
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            redisClient *slave = ln->value;

            /// 如果slave正在等待master产生rdb文件,那么定时给slave发送'\n',只是为了不让slave因为timeout被断开
            if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START ||
                (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END &&
                 server.rdb_child_type != REDIS_RDB_CHILD_TYPE_SOCKET))
            {
                if (write(slave->fd, "\n", 1) == -1) {
                    /* Don't worry, it's just a ping. */
                }
            }
        }
    }

    /* Disconnect timedout slaves. */
    if (listLength(server.slaves)) {
        listIter li;
        listNode *ln;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            redisClient *slave = ln->value;

            /// 不在同步中
            if (slave->replstate != REDIS_REPL_ONLINE) continue;
            /// 不支持psync
            if (slave->flags & REDIS_PRE_PSYNC) continue;
            /// 超时了,释放客户端
            if ((server.unixtime - slave->repl_ack_time) > server.repl_timeout)
            {
                redisLog(REDIS_WARNING, "Disconnecting timedout slave: %s",
                    replicationGetSlaveName(slave));
                freeClient(slave);
            }
        }
    }

    /* If we have no attached slaves and there is a replication backlog
     * using memory, free it after some (configured) time. */
    /// 没有slave且设置了repl_backlog_time_limit
    if (listLength(server.slaves) == 0 && server.repl_backlog_time_limit &&
        server.repl_backlog)
    {
        time_t idle = server.unixtime - server.repl_no_slaves_since;

        /// replaction闲置时间超过了repl_backlog_time_limit
        if (idle > server.repl_backlog_time_limit) {
            /// 清除backlog
            freeReplicationBacklog();
            redisLog(REDIS_NOTICE,
                "Replication backlog freed after %d seconds "
                "without connected slaves.",
                (int) server.repl_backlog_time_limit);
        }
    }

    /* If AOF is disabled and we no longer have attached slaves, we can
     * free our Replication Script Cache as there is no need to propagate
     * EVALSHA at all. */
    /// LUA相关,先不看
    if (listLength(server.slaves) == 0 &&
        server.aof_state == REDIS_AOF_OFF &&
        listLength(server.repl_scriptcache_fifo) != 0)
    {
        replicationScriptCacheFlush();
    }

    /* If we are using diskless replication and there are slaves waiting
     * in WAIT_BGSAVE_START state, check if enough seconds elapsed and
     * start a BGSAVE.
     *
     * This code is also useful to trigger a BGSAVE if the diskless
     * replication was turned off with CONFIG SET, while there were already
     * slaves in WAIT_BGSAVE_START state. */
    /// 没有rdb子进程且没有aof子进程
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1) {
        time_t idle, max_idle = 0;
        int slaves_waiting = 0;
        listNode *ln;
        listIter li;

        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            redisClient *slave = ln->value;
            /// slave等待rdb文件后台生成
            if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) {
                idle = server.unixtime - slave->lastinteraction;
                /// 设置最长闲置时间
                if (idle > max_idle) 
                {
                    max_idle = idle;
                }
                slaves_waiting++;
            }
        }

        /// 有等待rdb后台生成的slave且闲置超过了repl_diskless_sync_delay
        if (slaves_waiting && max_idle > server.repl_diskless_sync_delay) {
            /* Start a BGSAVE. Usually with socket target, or with disk target
             * if there was a recent socket -> disk config change. */
            /// 开始生成rdb文件,只不过是diskless,也就是说rdb文件是直接发往对端socket的
            if (startBgsaveForReplication() == REDIS_OK) {
                /* It started! We need to change the state of slaves
                 * from WAIT_BGSAVE_START to WAIT_BGSAVE_END in case
                 * the current target is disk. Otherwise it was already done
                 * by rdbSaveToSlavesSockets() which is called by
                 * startBgsaveForReplication(). */
                listRewind(server.slaves,&li);
                while((ln = listNext(&li))) {
                    redisClient *slave = ln->value;
                    if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START)
                        /// 修改slave状态为等待rdb结束
                        slave->replstate = REDIS_REPL_WAIT_BGSAVE_END;
                }
            }
        }
    }

    /* Refresh the number of slaves with lag <= min-slaves-max-lag. */
    /// 刷新良好的slave数量
    refreshGoodSlavesCount();
}
