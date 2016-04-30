// ranger_proxy - A SOCKS5 proxy
// Copyright (C) 2015  RangerUFO <ufownl@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef RANGER_PROXY_ASYNC_CONNECT_HPP
#define RANGER_PROXY_ASYNC_CONNECT_HPP

#include "logger_ostream.hpp"
#include "err.hpp"
#include <caf/io/network/asio_multiplexer.hpp>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <memory>

namespace ranger { namespace proxy {

namespace {

template <class T>
void handle_connect_completed(T* self,
                              const std::string& ep_info,
                              network::asio_tcp_socket&& fd,
                              const boost::system::error_code& ec) {
  using actor_hdl = typename T::actor_hdl;
  if (ec) {
    scoped_actor tmp(self->system());
    log(tmp) << "ERROR: " << ec.message() << ": " << ep_info << std::endl;
    if (!self->is_terminated()) {
      self->send(actor_cast<actor_hdl>(self), make_error(err::network_error, "could not connect to host: " + ep_info));
    }
  } else {
    if (!self->is_terminated()) {
      auto& backend = static_cast<network::asio_multiplexer&>(self->parent().backend());
      auto hdl = backend.add_tcp_scribe(self, std::move(fd));
      self->send(actor_cast<actor_hdl>(self), hdl);
    } else {
      boost::system::error_code ignored_ec;
      using boost::asio::ip::tcp;
      fd.shutdown(tcp::socket::shutdown_both, ignored_ec);
      fd.close(ignored_ec);
    }
  }
}

}

template <class T>
void async_connect(T* self, const in_addr& addr, uint16_t port) {
  strong_actor_ptr ptr(self->ctrl());
  std::string ep_info = std::string(inet_ntoa(addr)) + ":" + std::to_string(port);
  auto fd = std::make_shared<network::asio_tcp_socket>(*self->parent().backend().pimpl());
  using boost::asio::ip::tcp;
  using boost::asio::ip::address_v4;
  fd->async_connect(tcp::endpoint(address_v4(ntohl(addr.s_addr)), port),
    [self, ptr, ep_info, fd] (const boost::system::error_code& ec) {
      handle_connect_completed(self, ep_info, std::move(*fd), ec);
    }
  );
}

template <class T>
void async_connect(T* self, const std::string& host, uint16_t port) {
  strong_actor_ptr ptr(self->ctrl());
  std::string ep_info = host + ":" + std::to_string(port);
  using boost::asio::ip::tcp;
  auto r = std::make_shared<tcp::resolver>(*self->parent().backend().pimpl());
  using boost::system::error_code;
  r->async_resolve(tcp::resolver::query(host, std::to_string(port)),
    [self, ptr, ep_info, r] (const error_code& ec, tcp::resolver::iterator it) {
      if (ec) {
        scoped_actor tmp(self->system());
        log(tmp) << "ERROR: " << ec.message() << ": " << ep_info << std::endl;
        if (!self->is_terminated()) {
          using actor_hdl = typename T::actor_hdl;
          self->send(actor_cast<actor_hdl>(self), make_error(err::network_error, "could not resolve host: " + ep_info));
        }
      } else if (!self->is_terminated()) {
        auto fd = std::make_shared<network::asio_tcp_socket>(*self->parent().backend().pimpl());
        boost::asio::async_connect(*fd, it,
          [self, ptr, ep_info, fd] (const error_code& ec, tcp::resolver::iterator it) {
            handle_connect_completed(self, ep_info, std::move(*fd), ec);
          }
        );
      }
    }
  );
}

} }

#endif  // RANGER_PROXY_ASYNC_CONNECT_HPP
