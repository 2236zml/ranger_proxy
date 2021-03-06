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

#ifndef TEST_UTIL_HPP
#define TEST_UTIL_HPP

#include <caf/all.hpp>
#include <caf/io/all.hpp>
#include <caf/io/network/asio_multiplexer_impl.hpp>
#include <gtest/gtest.h>
#include <iostream>
#include <string>
#include "scope_guard.hpp"

using echo_service =
  caf::io::experimental::minimal_server::extend<
    caf::replies_to<caf::publish_atom>
    ::with_either<caf::ok_atom, uint16_t>
    ::or_else<caf::error_atom, std::string>
  >;

echo_service::behavior_type
echo_service_impl(echo_service::broker_pointer self) {
  return {
    [=] (const caf::io::new_connection_msg& msg) {
      self->configure_read(msg.handle, caf::io::receive_policy::at_most(8192));
    },
    [=] (const caf::io::new_data_msg& msg) {
      self->write(msg.handle, msg.buf.size(), msg.buf.data());
      self->flush(msg.handle);
      self->close(msg.handle);
    },
    [] (const caf::io::connection_closed_msg&) {},
    [] (const caf::io::acceptor_closed_msg&) {},
    [=] (caf::publish_atom)
      -> caf::either<caf::ok_atom, uint16_t>::or_else<caf::error_atom, std::string> {
      try {
        return {caf::ok_atom::value, self->add_tcp_doorman().second};
      } catch (const caf::network_error& e) {
        return {caf::error_atom::value, e.what()};
      }
    }
  };
}

class ranger_proxy_test : public testing::Test {
public:
  static void SetUpTestCase() {
    caf::io::set_middleman<caf::io::network::asio_multiplexer>();
  }

  static void TearDownTestCase() {
    caf::shutdown();
  }

protected:
  void TearDown() override {
    caf::await_all_actors_done();
  }
};

class echo_test : public ranger_proxy_test {
protected:
  void SetUp() final {
    m_echo = caf::io::spawn_io(echo_service_impl);
    caf::scoped_actor self;
    self->sync_send(m_echo, caf::publish_atom::value).await(
      [this] (caf::ok_atom, uint16_t port) {
        m_port = port;
      },
      [] (caf::error_atom, const std::string& what) {
        std::cout << "ERROR: " << what << std::endl;
      }
    );
    ASSERT_NE(0, m_port);
  }

  void TearDown() final {
    caf::anon_send_exit(m_echo, caf::exit_reason::kill);
    ranger_proxy_test::TearDown();
  }

  echo_service m_echo;
  uint16_t m_port {0};
};

using ranger::proxy::scope_guard;

#endif  // TEST_UTIL_HPP
