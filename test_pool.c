while (1)
    {
        int nfds = epoll_wait(epfd, events, 16, -1);
        if (nfds < 0)
        {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;

            // 【分支 A】串口有动静：读传感器帧 -> 落库
            if (fd == serial_fd)
            {
                for (;;)
                {
                    char buf[256];
                    int len = serial_read(buf, sizeof(buf));
                    if (len < 0) // 数据读错
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 设置成非阻塞模式，读干了，ET 下必须 break，否则死循环 CPU 100%
                        perror("串口读出错");
                        break;
                    }
                    if (len == 0) break;// 主动断开连接

                    if (serial_rx_len + len > (int)sizeof(serial_rx_buf))
                    {
                        serial_rx_len = 0;   // 防溢出
                    }
                    memcpy(serial_rx_buf + serial_rx_len, buf, len);
                    serial_rx_len += len;

                    int offset = 0;
                    while (serial_rx_len - offset > 0)
                    {
                        uint8_t type;
                        char json_payload[MAX_PAYLOAD + 1];
                        int json_len;
                        int res = proto_unpack((uint8_t *)(serial_rx_buf + offset), serial_rx_len - offset, &type, (uint8_t *)json_payload, &json_len);

                        if (res > 0)
                        {
                            printf("【串口拆包】Type: %d, JSON: %s\n", type, json_payload);

                            cJSON *root = cJSON_Parse(json_payload);
                            if (root)
                            {
                                cJSON *temp_node = cJSON_GetObjectItem(root, "temp");
                                cJSON *hum_node = cJSON_GetObjectItem(root, "hum");
                                if (temp_node && hum_node)
                                {
                                    double temp = temp_node->valuedouble;
                                    double hum = hum_node->valuedouble;
                                    db_insert_sensor(temp, hum, (int)time(NULL));
                                    printf("  -> 已存入数据库\n");
                                }
                                cJSON_Delete(root); //移动进度条
                            }
                            offset += res;
                        }
                        else if (res == -1) { break; }
                        else if (res == -2) { serial_rx_len = 0; break; }
                    }

                    if (offset > 0 && offset < serial_rx_len)
                    {
                        memmove(serial_rx_buf, serial_rx_buf + offset, serial_rx_len - offset);
                        serial_rx_len -= offset;
                    }
                    else if (offset == serial_rx_len)
                    {
                        serial_rx_len = 0;
                    }
                }
            }

            // 【分支 B】新客户端连进来
            else if (fd == listen_fd)
            {
                for (;;)
                {
                    int c_fd = accept(listen_fd, NULL, NULL);
                    if (c_fd < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
                        break;
                    }

                    set_nonblocking(c_fd);
                    printf("【网络】客户端接入 fd=%d\n", c_fd);

                    ClientContext *ctx = alloc_client(c_fd);
                    if (ctx == NULL)
                    {
                        fprintf(stderr, "【网络】连接数已满，拒绝 fd=%d\n", c_fd);
                        close(c_fd);
                        continue;
                    }

                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = c_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, c_fd, &ev);
                }
            }

            // 【分支 C】周期推送定时器到点：给订阅客户端推最新数据
            else if (fd == timer_fd)
            {
                uint64_t exp;
                while (read(timer_fd, &exp, sizeof(exp)) > 0) {}   // 排空（ET 下要读到 EAGAIN）
                push_latest_to_subscribers();
            }

            // 【分支 D】已连接的客户端发消息
            else
            {
                ClientContext *ctx = find_client(fd);
                if (ctx == NULL)
                {
                    // 防御性兜底：fd 既然在 epoll 里，就该有对应槽位
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    continue;
                }

                for (;;)
                {
                    char buf[256];
                    int len = recv(fd, buf, sizeof(buf) - 1, 0);

                    if (len > 0)
                    {

                        if (ctx->buf_len + len > (int)sizeof(ctx->recv_buf))
                        {
                            ctx->buf_len = 0;
                        }
                        memcpy(ctx->recv_buf + ctx->buf_len, buf, len);
                        ctx->buf_len += len;

                        int offset = 0;
                        while (ctx->buf_len - offset > 0)
                        {
                            uint8_t type;
                            char json_payload[MAX_PAYLOAD + 1];
                            int json_len;

                            int res = proto_unpack((uint8_t *)(ctx->recv_buf + offset), ctx->buf_len - offset, &type, (uint8_t *)json_payload, &json_len);

                            if (res > 0)
                            {
                                printf("【网络拆包】fd=%d Type: %d, JSON: %s\n", fd, type, json_payload);

                                if (type == TYPE_SUBSCRIBE)
                                {
                                    ctx->subscribed = 1;
                                    send_frame(fd, TYPE_HEARTBEAT, "{\"ok\":\"subscribed\"}");
                                    printf("【订阅】fd=%d 订阅了周期推送\n", fd);
                                }
                                else if (type == TYPE_SUBSCRIBE_STOP)
                                {
                                    ctx->subscribed = 0;
                                    send_frame(fd, TYPE_HEARTBEAT, "{\"ok\":\"unsubscribed\"}");
                                    printf("【订阅】fd=%d 取消了订阅\n", fd);
                                }
                                else
                                {
                                    // 查询类：丢线程池异步执行（耗时的解析/查库不阻塞 epoll 主循环）
                                    QueryTaskArgs *args = (QueryTaskArgs *)malloc(sizeof(QueryTaskArgs));
                                    if (args != NULL)
                                    {
                                        args->client_fd = fd;
                                        args->type = type;
                                        strncpy(args->query, json_payload, sizeof(args->query) - 1);
                                        args->query[sizeof(args->query) - 1] = '\0';
                                        threadpool_add_task(pool, background_db_query, args);
                                    }
                                }
                                offset += res;
                            }
                            else if (res == -1) { break; }
                            else if (res == -2) { ctx->buf_len = 0; break; }
                        }

                        if (offset > 0 && offset < ctx->buf_len)
                        {
                            memmove(ctx->recv_buf, ctx->recv_buf + offset, ctx->buf_len - offset);
                            ctx->buf_len -= offset;
                        }
                        else if (offset == ctx->buf_len)
                        {
                            ctx->buf_len = 0;
                        }
                    }
                    else if (len == 0)
                    {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        free_client(ctx);
                        printf("【网络】客户端 fd=%d 断开\n", fd);
                        break;
                    }
                    else
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        free_client(ctx);
                        printf("【网络】客户端 fd=%d 出错断开\n", fd);
                        break;
                    }
                }
            }
        }
    }

    threadpool_destroy(pool);
    db_close();
    serial_close();
    if (timer_fd >= 0) close(timer_fd);
    close(epfd);
    return 0;
}