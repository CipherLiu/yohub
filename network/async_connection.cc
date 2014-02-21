#include "network/async_connection.h"
#include "network/event_pool.h"
#include <boost/bind.hpp>

using namespace yohub;

AsyncConnection::AsyncConnection(EventPool* event_pool,
                                 int socket_fd,
                                 int conn_id,
                                 const InetAddress& local_addr,
                                 const InetAddress& peer_addr)
    : event_pool_(event_pool),
      socket_(socket_fd),
      channel_(event_pool_, socket_fd),
      local_addr_(local_addr),
      peer_addr_(peer_addr),
      id_(conn_id),
      refs_(0),
      is_connected_(0)
{
    channel_.SetReadCallback(boost::bind(&AsyncConnection::OnRead, this));
    channel_.SetWriteCallback(boost::bind(&AsyncConnection::OnWrite, this));
    channel_.SetCloseCallback(boost::bind(&AsyncConnection::OnClose, this));
}

AsyncConnection::~AsyncConnection() {
}

void AsyncConnection::Write(const char* data, size_t size) {
    std::string s(data, size);
    event_pool_->PostJob(
        boost::bind(&AsyncConnection::QueueWrite, this, s), channel_);
}

void AsyncConnection::Establish() {
    if (AtomicSetValue(is_connected_, 1) == 0) {
        channel_.EnableRead();
        this->Acquire();
        on_connection_cb_(this);
        this->Release();
    } else {
        LOG_WARN("Connection already established");
    }
}

void AsyncConnection::Destroy() {
    if (AtomicSetValue(is_connected_, 0) == 1) {
        channel_.DisableAll();
        channel_.Unregister();
    } else {
        LOG_WARN("Connection already destroyed");
    }
}

void AsyncConnection::QueueWrite(const std::string& s) {
    if (AtomicGetValue(is_connected_) == 0) {
        LOG_WARN("Stop writing: onnection already destroyed.");
        return;
    }

    const char* data = s.data();
    size_t size = s.size();

    if (!channel_.WriteAllowed() && out_buffer_.ReadableBytes() == 0) {
        int result = ::write(channel_.fd(), data, size);
        if (result >= 0) {
            size -= result;
            if (size == 0 && on_write_completion_cb_)
                this->Acquire();
                on_write_completion_cb_(this);
                this->Release();
        } else {
            if (errno == EWOULDBLOCK) {
                LOG_TRACE("Waiting for next write.");
            } else {
                LOG_WARN("write error: %s", strerror(errno));
            }
        }
    }

    if (size > 0) {
        out_buffer_.Append(data + s.size() - size, size);
        if (!channel_.WriteAllowed())
            channel_.EnableWrite();
    }
}

void AsyncConnection::OnRead() {
    int result = in_buffer_.ReadFd(channel_.fd());
    if (result > 0) {
        this->Acquire();
        on_read_completion_cb_(this, &in_buffer_);
        this->Release();
    } else if (result == 0) {
        OnClose();
    } else {
        LOG_WARN("OnRead error occur, please check it.");
    }
}

void AsyncConnection::OnWrite() {
    assert(channel_.WriteAllowed());

    Slice slice = out_buffer_.ToSlice();
    int result = ::write(channel_.fd(), slice.data(), slice.size());
    
    if (result > 0) {
        out_buffer_.ReadableForward(result);
        if (out_buffer_.ReadableBytes() == 0) {
            channel_.DisableWrite();
            if (on_write_completion_cb_) {
                this->Acquire();
                on_write_completion_cb_(this);
                this->Release();
            }
        }
    } else {
        LOG_WARN("OnWrite failed, error: %s", strerror(errno));
    }
}

void AsyncConnection::OnClose() {
    if (AtomicSetValue(is_connected_, 0) == 1) {
        channel_.DisableAll();
        this->Acquire();
        on_close_cb_(this);
        this->Release();
    } else {
        LOG_WARN("Connection never connected.");
    }
}
