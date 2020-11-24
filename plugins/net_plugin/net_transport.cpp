// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2020-2021 The nchain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
// #include <eosio/chain/types.hpp>

#include "net_transport.hpp"
#include "connection.hpp"
#include "net_plugin_impl.hpp"
#include <libp2p/outcome/outcome.hpp>

#include <sstream>

namespace eosio {

static const std::string unknown = "<unknown>";

extern net_plugin_impl *my_impl;

using tcp_connector_wptr = std::weak_ptr<tcp_connector>;

inline boost::system::error_code convert_error_code(std::error_code &err) {
    return boost::system::errc::make_error_code( static_cast<boost::system::errc::errc_t>(err.value()));
}

bool tcp_connector::init(std::shared_ptr<strand_t> strand, const string &peer_addr) {
    strand_ = strand;
    peer_addr_ = peer_addr;
    string::size_type colon = peer_addr.find(':');
    if (colon == std::string::npos || colon == 0) {
        fc_elog( logger, "Invalid peer address. must be \"host:port[:<blk>|<trx>]\": ${p}", ("p", peer_addr) );
        return false;
    }

    string::size_type colon2 = peer_addr.find(':', colon + 1);

    string host = peer_addr.substr( 0, colon );
    string port = peer_addr.substr( colon + 1, colon2 == string::npos ? string::npos : colon2 - (colon + 1));
    idump((host)(port));

    host_ = host;
    port_ = port;
    set_connection_type( peer_addr_ );
    return true;
}

void tcp_connector::set_connection_type( const string& peer_add ) {
    // host:port:[<trx>|<blk>]
    string::size_type colon = peer_add.find(':');
    string::size_type colon2 = peer_add.find(':', colon + 1);
    string::size_type end = colon2 == string::npos
        ? string::npos : peer_add.find_first_of( " :+=.,<>!$%^&(*)|-#@\t", colon2 + 1 ); // future proof by including most symbols without using regex
    string host = peer_add.substr( 0, colon );
    string port = peer_add.substr( colon + 1, colon2 == string::npos ? string::npos : colon2 - (colon + 1));
    string type = colon2 == string::npos ? "" : end == string::npos ?
        peer_add.substr( colon2 + 1 ) : peer_add.substr( colon2 + 1, end - (colon2 + 1) );

    if( type.empty() ) {
        fc_dlog( logger, "Setting connection type for: ${peer} to both transactions and blocks", ("peer", peer_add) );
        connection_type_ = both;
    } else if( type == "trx" ) {
        fc_dlog( logger, "Setting connection type for: ${peer} to transactions only", ("peer", peer_add) );
        connection_type_ = transactions_only;
    } else if( type == "blk" ) {
        fc_dlog( logger, "Setting connection type for: ${peer} to blocks only", ("peer", peer_add) );
        connection_type_ = blocks_only;
    } else {
        fc_wlog( logger, "Unknown connection type: ${t}", ("t", type) );
    }
}

std::string endpoints_to_string(tcp::resolver::results_type &endpoints) {
    std::stringstream ss;
    ss << "[";
    for (const tcp::endpoint &endpoint : endpoints) {
        ss << endpoint << ",";
    }
    ss << "]";
    return ss.str();
}

void tcp_connector::connect(connector_t::handler_func handler) {

    if (connecting_) { // TODO: need check timeout?
        return;
    }
    connecting_ = true;
    auto self  = shared_from_this();

    tcp::resolver::query query(tcp::v4(), host_, port_);
    // Note: need to add support for IPv6 too

    socket_       = std::make_shared<tcp::socket>(my_impl->thread_pool->get_executor());
    auto resolver = std::make_shared<tcp::resolver>(my_impl->thread_pool->get_executor());

    resolver->async_resolve(
        query, boost::asio::bind_executor(*strand_, [resolver, self, handler](
                                                        const boost::system::error_code &err,
                                                        tcp::resolver::results_type endpoints) {
            if (!err) {
                fc_dlog(logger, "resolv peer:${peer} as ${eps}",
                        ("peer", self->peer_addr_)("eps", endpoints_to_string(endpoints)));

                boost::asio::async_connect(
                    *self->socket_, endpoints,
                    boost::asio::bind_executor(
                        *self->strand_,
                        [resolver, self, handler](const boost::system::error_code &err,
                                                    const tcp::endpoint &endpoint) {
                            if (!err && self->socket_->is_open()) {
                                auto transport =
                                    std::make_shared<tcp_transport>(self->socket_, self->peer_addr_);
                                handler(err, transport);
                                self->connecting_ = false;
                            } else {
                                fc_elog(logger, "connection failed to ${peer}: ${error}",
                                        ("peer", self->peer_addr_)("error", err.message()));
                                self->connecting_ = false;
                                handler(err, nullptr);
                                // c->close( false );
                            }
                        }));
            } else {
                fc_elog(logger, "Unable to resolve ${add}: ${error}",
                        ("add", self->peer_addr_)("error", err.message()));
                // c->connecting = false;
                // ++c->consecutive_immediate_connection_close;
                self->connecting_ = false;
                handler(err, nullptr);
            }
        }));
}

const std::string& tcp_connector::peer_address() const {
    return peer_addr_;
}

tcp_transport::~tcp_transport() {
    close();
}

void tcp_transport::close() {
    boost::system::error_code ec;
    if (socket_) {
        if( socket_->is_open() ) {
            socket_->shutdown( tcp::socket::shutdown_both, ec );
            socket_->close( ec );
        }
        socket_ = nullptr;
    }
    is_init_ = false;
    strand_ = nullptr;
}

bool tcp_transport::init(std::shared_ptr<strand_t> strand) {
    strand_ = strand;
    update_endpoints();

    boost::asio::ip::tcp::no_delay nodelay( true );
    boost::system::error_code ec;
    socket_->set_option( nodelay, ec );
    if( ec ) {
        fc_elog( logger, "connection failed (set_option) ${peer}: ${e1}", ("peer", peer_addr_)( "e1", ec.message() ) );
        return false;
    }
    is_init_ = true;
    return true;
}

void tcp_transport::write(queued_buffer &buffer_queue, write_callback_func cb) {

    //     if( !buffer_queue.ready_to_send() )
    //         return;
    assert(is_init_);

    auto self = shared_from_this();

    std::vector<boost::asio::const_buffer> bufs;
    buffer_queue.fill_out_buffer(bufs);

    strand_->post([self{std::move(self)}, bufs{std::move(bufs)}, cb]() {
        // check not closed?
        if (!self->is_init_) return;

        boost::asio::async_write(
            *self->socket_, bufs,
            boost::asio::bind_executor(
                *self->strand_, [cb](boost::system::error_code ec, std::size_t w) { cb(ec, w); }));
    });
}

void tcp_transport::read(message_buf_t &buffer, std::size_t min_size, read_callback_func cb) {

    assert(is_init_);
    if (my_impl->use_socket_read_watermark) {
        const size_t max_socket_read_watermark = 4096;
        std::size_t socket_read_watermark =
            std::min<std::size_t>(min_size, max_socket_read_watermark);
        boost::asio::socket_base::receive_low_watermark read_watermark_opt(socket_read_watermark);
        boost::system::error_code ec;
        socket_->set_option(read_watermark_opt, ec);
        // if (ec) {
        //     fc_elog(logger, "unable to set read watermark ${peer}: ${e1}",
        //             ("peer", peer_name())("e1", ec.message()));
        // }
    }

    auto completion_handler = [min_size](boost::system::error_code ec,
                                         std::size_t bytes_transferred) -> std::size_t {
        if (ec || bytes_transferred >= min_size) {
            return 0;
        } else {
            return min_size - bytes_transferred;
        }
    };

    boost::asio::async_read(
        *socket_, buffer.get_buffer_sequence_for_boost_async_read(), completion_handler,
        boost::asio::bind_executor(
            *strand_, [cb](boost::system::error_code ec, std::size_t bytes_transferred) {
                cb(ec, bytes_transferred);
            }));
}


bool tcp_transport::is_init() const {
    return is_init_;
}

const endpoint_info_t& tcp_transport::get_endpoint_info() {
    return endpoint_info_;
}

void tcp_transport::update_endpoints() {
    boost::system::error_code ec;
    boost::system::error_code ec2;
    auto rep = socket_->remote_endpoint(ec);
    auto lep = socket_->local_endpoint(ec2);
    // std::lock_guard<std::mutex> g_conn( conn_mtx );
    endpoint_info_.remote_endpoint_ip = ec ? unknown : rep.address().to_string();
    endpoint_info_.remote_endpoint_port = ec ? unknown : std::to_string(rep.port());
    endpoint_info_.local_endpoint_ip = ec2 ? unknown : lep.address().to_string();
    endpoint_info_.local_endpoint_port = ec2 ? unknown : std::to_string(lep.port());
}

bool tcp_listener::init(std::shared_ptr<strand_t> strand) {
    strand_ = strand;
    tcp::endpoint listen_endpoint;
    if (my_impl->p2p_address.size() > 0) {
        auto host = my_impl->p2p_address.substr(0, my_impl->p2p_address.find(':'));
        auto port = my_impl->p2p_address.substr(host.size() + 1, my_impl->p2p_address.size());
        tcp::resolver::query query(tcp::v4(), host.c_str(), port.c_str());
        // Note: need to add support for IPv6 too?

        tcp::resolver resolver(my_impl->thread_pool->get_executor());
        listen_endpoint = *resolver.resolve(query);

        acceptor_.reset(new tcp::acceptor(my_impl->thread_pool->get_executor()));

        if (!my_impl->p2p_server_address.empty()) {
            my_impl->p2p_address = my_impl->p2p_server_address;
        } else {
            if (listen_endpoint.address().to_v4() == address_v4::any()) {
                boost::system::error_code ec;
                auto host = host_name(ec);
                if (ec.value() != boost::system::errc::success) {

                    FC_THROW_EXCEPTION(fc::invalid_arg_exception,
                                       "Unable to retrieve host_name. ${msg}",
                                       ("msg", ec.message()));
                }
                auto port =
                    my_impl->p2p_address.substr(my_impl->p2p_address.find(':'), my_impl->p2p_address.size());
                my_impl->p2p_address = host + port;
            }
        }
    }
    if (acceptor_) {
        try {
            acceptor_->open(listen_endpoint.protocol());
            acceptor_->set_option(tcp::acceptor::reuse_address(true));
            acceptor_->bind(listen_endpoint);
            acceptor_->listen();
        } catch (const std::exception &e) {
            elog("tcp_listener failed to bind to port ${port}",
                 ("port", listen_endpoint.port()));
            throw e;
        }
        return true;
        //  fc_ilog( logger, "starting listener, max clients is ${mc}",("mc",my_impl->max_client_count)
        //  ); my_impl->start_listen_loop();
    }
    return false;
}

void tcp_listener::accept(net_listener::accept_callback_func handler) {
    assert(strand_);
    if (!acceptor_) return;

    auto self   = shared_from_this();
    auto socket = std::make_shared<tcp::socket>(my_impl->thread_pool->get_executor());
    acceptor_->async_accept(
        *socket, boost::asio::bind_executor(*strand_, [self, socket{std::move(socket)},
                                                       handler](boost::system::error_code ec) {
            if (ec) {
                fc_elog(logger, "Error accepting connection: ${m}", ("m", ec.message()));
                // For the listed error codes below, recall start_listen_loop()
                switch (ec.value()) {
                case ECONNABORTED:
                case EMFILE:
                case ENFILE:
                case ENOBUFS:
                case ENOMEM:
                case EPROTO:
                    handler({}, nullptr, ""); // ignore errors
                    return;
                default:
                    handler(ec, nullptr, "");
                    return;
                }
            }

            boost::system::error_code rec;
            const auto &paddr_add = socket->remote_endpoint(rec).address();
            std::string paddr_str;
            if (rec) {
                fc_elog(logger, "Error getting remote endpoint: ${m}", ("m", rec.message()));
            } else {
                paddr_str = paddr_add.to_string();
            }
            // TODO: get peer address
            auto transport = std::make_shared<tcp_transport>(socket, "");
            handler(ec, transport, paddr_str);
        }));
}

void tcp_listener::close() {
    if( acceptor_ ) {
        boost::system::error_code ec;
        acceptor_->cancel( ec );
        acceptor_->close( ec );
        acceptor_ = nullptr;
    }
}

////////////////////////////////////////////////////////////////////////////////
// p2p_connector

bool p2p_connector::init(std::shared_ptr<strand_t> strand, const string &peer_addr) {
    auto server_ma_res =
        libp2p::multi::Multiaddress::create(peer_addr);  // NOLINT
    if (!server_ma_res) {
        fc_elog(
            logger,
            "Invalid peer address. must be \"/ip4/<ip>/tcp/<port>[/<blk>|<trx>]\": ${p} : ${err}: ",
            ("p", peer_addr)("err", server_ma_res.error().message()));
        return false;
    }
    auto server_ma = std::move(server_ma_res.value());

    auto server_peer_id_str = server_ma.getPeerId();
    if (!server_peer_id_str) {
        fc_elog(
            logger,
            "Invalid peer address. must be \"/ip4/<ip>/tcp/<port>[/<blk>|<trx>]\": ${p} : unable to get peer id: ",
            ("p", peer_addr));
        return false;
    }

    auto server_peer_id_res =
        libp2p::peer::PeerId::fromBase58(*server_peer_id_str);
    if (!server_peer_id_res) {
        fc_elog(
            logger,
            "Unable to decode peer id from base 58: ${p}",
            ("p", *server_peer_id_str));
        return false;
    }

    p2p_peer_info peer_info = { server_peer_id_res.value(), {server_ma} };
    peer_info_ = std::make_shared<p2p_peer_info>(peer_info);

    strand_ = strand;
    peer_addr_ = peer_addr;
    return true;
}

void p2p_connector::connect(connector_t::handler_func handler) {

    if (connecting_) { // TODO: need check timeout?
        return;
    }
    connecting_ = true;

    auto injector = libp2p::injector::makeHostInjector();
    auto host     = injector.create<std::shared_ptr<libp2p::Host>>();

    auto self = shared_from_this();
    strand_->post([host, self, handler] { // NOLINT
        const std::string PROTOCOL_ID = "/nchain/1.0.0";

        host->newStream(*self->peer_info_, PROTOCOL_ID, [self, handler](auto &&stream_res) {

            if (!stream_res) {
                handler(convert_error_code(stream_res.error()), nullptr);
                self->connecting_ = false;
                return;
            }

            auto stream    = std::move(stream_res.value());
            auto transport = std::make_shared<p2p_transport>(stream, self->peer_addr_, self->peer_info_);
            handler({}, transport);
            self->connecting_ = false;
        });
    });
}

const std::string& p2p_connector::peer_address() const {
    return peer_addr_;
}

////////////////////////////////////////////////////////////////////////////////
// p2p_connector

p2p_transport::~p2p_transport() {
    close();
}

void p2p_transport::close() {
    boost::system::error_code ec;
    if (stream_) {
        stream_->close([](auto &&) { /* do nothing */ });
        stream_ = nullptr;
    }
    is_init_ = false;
    strand_ = nullptr;
}

bool p2p_transport::init(std::shared_ptr<strand_t> strand) {
    strand_ = strand;
    is_init_ = true;
    return true;
}

void p2p_transport::write(queued_buffer &buffer_queue, write_callback_func cb) {

    auto self = shared_from_this();

    std::vector<boost::asio::const_buffer> bufs;
    buffer_queue.fill_out_buffer(bufs);
    for (auto buf : bufs) {
        gsl::span<const uint8_t> inbuf(static_cast<const uint8_t *>(buf.data()), buf.size());
        stream_->write(inbuf, buf.size(),
                       [self, cb{std::move(cb)}](libp2p::outcome::result<size_t> rw) mutable {
                           if (rw) {
                               cb({}, rw.value());
                           } else {
                               cb(convert_error_code(rw.error()), 0);
                           }
                       });
    }
}

void p2p_transport::read(message_buf_t &buffer, std::size_t min_size, read_callback_func cb) {

    auto self = shared_from_this();

    auto bufs = buffer.get_buffer_sequence_for_boost_async_read();
    assert(bufs.size() > 0);
    auto buf = bufs.front();
    min_size = std::min(min_size, buf.size());
    gsl::span<uint8_t> read_buf(static_cast<uint8_t *>(buf.data()), min_size);
    self->stream_->read(read_buf, min_size,
                        [self, cb{std::move(cb)}](libp2p::outcome::result<size_t> rr) mutable {
                            if (rr) {
                                cb({}, rr.value());
                            } else {
                                cb(convert_error_code(rr.error()), 0);
                            }
                        });
}

bool p2p_transport::is_init() const {
    return is_init_;
}

const endpoint_info_t& p2p_transport::get_endpoint_info() {
    return endpoint_info_;
}

} // namespace eosio