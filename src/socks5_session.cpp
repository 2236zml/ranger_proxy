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

#include "common.hpp"
#include "socks5_session.hpp"
#include "connect_helper.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <iterator>
#include <string.h>

namespace ranger { namespace proxy {

socks5_state::socks5_state(socks5_session::broker_pointer self)
	: m_self(self) {
	// nop
}

void socks5_state::init(connection_handle hdl) {
	m_local_hdl = hdl;
	m_self->configure_read(m_local_hdl, receive_policy::exactly(2));
	m_current_handler = &socks5_state::handle_select_method_header;
}

void socks5_state::handle_new_data(const new_data_msg& msg) {
	if (m_current_handler) {
		(this->*m_current_handler)(msg);
	}
}

void socks5_state::handle_connect_succ(connection_handle hdl) {
	if (m_conn_succ_handler) {
		m_conn_succ_handler(hdl);
	}
}

void socks5_state::handle_connect_fail(const std::string& what) {
	if (m_conn_fail_handler) {
		m_conn_fail_handler(what);
	}
}

void socks5_state::handle_select_method_header(const new_data_msg& msg) {
	if (static_cast<uint8_t>(msg.buf[0]) != 0x05) {
		aout(m_self) << "ERROR: Protocol version mismatch" << std::endl;
		m_self->quit();
		return;
	}

	m_self->configure_read(m_local_hdl, receive_policy::exactly(static_cast<uint8_t>(msg.buf[1])));
	m_current_handler = &socks5_state::handle_select_method_data;
}

void socks5_state::handle_select_method_data(const new_data_msg& msg) {
	if (std::find(msg.buf.begin(), msg.buf.end(), 0) != msg.buf.end()) {
		write(m_local_hdl, {0x05, 0x00}, [this] {
			m_self->configure_read(m_local_hdl, receive_policy::exactly(4));
			m_current_handler = &socks5_state::handle_request_header;
		});
	} else {
		aout(m_self) << "ERROR: NO ACCEPTABLE METHODS" << std::endl;
		write(m_local_hdl, {0x05, static_cast<char>(0xFF)}, [this] {
			m_self->quit();
		});
	}
}

void socks5_state::handle_request_header(const new_data_msg& msg) {
	if (static_cast<uint8_t>(msg.buf[0]) != 0x05) {
		aout(m_self) << "ERROR: Protocol version mismatch" << std::endl;
		m_self->quit();
		return;
	}

	if (static_cast<uint8_t>(msg.buf[1]) != 0x01) {
		aout(m_self) << "ERROR: Command not supported" << std::endl;
		write(m_local_hdl, {0x05, 0x07, 0x00, 0x01}, [this] {
			m_self->quit();
		});
		return;
	}

	switch (static_cast<uint8_t>(msg.buf[3])) {
	case 0x01:	// IPV4
		m_self->configure_read(m_local_hdl, receive_policy::exactly(6));
		m_current_handler = &socks5_state::handle_ipv4_request_data;
		return;
	case 0x03:	// DOMAINNAME
		m_self->configure_read(m_local_hdl, receive_policy::exactly(1));
		m_current_handler = &socks5_state::handle_domainname_length;
		return;
	}

	aout(m_self) << "ERROR: Address type not supported" << std::endl;
	write(m_local_hdl, {0x05, 0x08, 0x00, 0x01}, [this] {
		m_self->quit();
	});
}

void socks5_state::handle_ipv4_request_data(const new_data_msg& msg) {
	in_addr addr;
	memcpy(&addr, &msg.buf[0], sizeof(addr));
	uint16_t port;
	memcpy(&port, &msg.buf[4], sizeof(port));
	port = ntohs(port);

	auto helper = m_self->spawn(connect_helper_impl, &m_self->parent().backend());
	m_self->send(helper, connect_atom::value, inet_ntoa(addr), port);

	m_self->configure_read(m_local_hdl, receive_policy::at_most(8192));
	m_current_handler = nullptr;

	port = htons(port);

	m_conn_succ_handler = [this, addr, port] (connection_handle remote_hdl) {
		m_self->assign_tcp_scribe(remote_hdl);
		m_remote_hdl = remote_hdl;
		
		std::vector<char> buf = {0x05, 0x00, 0x00, 0x01};
		std::copy(reinterpret_cast<const char*>(&addr), reinterpret_cast<const char*>(&addr) + sizeof(addr), std::back_inserter(buf));
		std::copy(reinterpret_cast<const char*>(&port), reinterpret_cast<const char*>(&port) + sizeof(port), std::back_inserter(buf));
		write(m_local_hdl, std::move(buf), [this] {
			m_self->configure_read(m_remote_hdl, receive_policy::at_most(8192));
			m_current_handler = &socks5_state::handle_stream_data;
		});
	};

	m_conn_fail_handler = [this, addr, port] (const std::string& what) {
		aout(m_self) << "ERROR: " << what << std::endl;
		std::vector<char> buf = {0x05, 0x05, 0x00, 0x01};
		std::copy(reinterpret_cast<const char*>(&addr), reinterpret_cast<const char*>(&addr) + sizeof(addr), std::back_inserter(buf));
		std::copy(reinterpret_cast<const char*>(&port), reinterpret_cast<const char*>(&port) + sizeof(port), std::back_inserter(buf));
		write(m_local_hdl, std::move(buf), [this] {
			m_self->quit();
		});
	};
}

void socks5_state::handle_domainname_length(const new_data_msg& msg) {
	m_self->configure_read(m_local_hdl, receive_policy::exactly(static_cast<uint8_t>(msg.buf[0]) + 2));
	m_current_handler = &socks5_state::handle_domainname_request_data;
}

void socks5_state::handle_domainname_request_data(const new_data_msg& msg) {
	std::string host(msg.buf.begin(), msg.buf.begin() + msg.buf.size() - 2);
	uint16_t port;
	memcpy(&port, &msg.buf[msg.buf.size() - 2], sizeof(port));
	port = ntohs(port);

	auto helper = m_self->spawn(connect_helper_impl, &m_self->parent().backend());
	m_self->send(helper, connect_atom::value, host, port);

	m_self->configure_read(m_local_hdl, receive_policy::at_most(8192));
	m_current_handler = nullptr;

	port = htons(port);

	m_conn_succ_handler = [this, host, port] (connection_handle remote_hdl) {
		m_self->assign_tcp_scribe(remote_hdl);
		m_remote_hdl = remote_hdl;

		std::vector<char> buf = {0x05, 0x00, 0x00, 0x03, static_cast<char>(host.size())};
		std::copy(host.begin(), host.end(), std::back_inserter(buf));
		std::copy(reinterpret_cast<const char*>(&port), reinterpret_cast<const char*>(&port) + sizeof(port), std::back_inserter(buf));
		write(m_local_hdl, std::move(buf), [this] {
			m_self->configure_read(m_remote_hdl, receive_policy::at_most(8192));
			m_current_handler = &socks5_state::handle_stream_data;
		});
	};

	m_conn_fail_handler = [this, host, port] (const std::string& what) {
		aout(m_self) << "ERROR: " << what << std::endl;
		std::vector<char> buf = {0x05, 0x05, 0x00, 0x03, static_cast<char>(host.size())};
		std::copy(host.begin(), host.end(), std::back_inserter(buf));
		std::copy(reinterpret_cast<const char*>(&port), reinterpret_cast<const char*>(&port) + sizeof(port), std::back_inserter(buf));
		write(m_local_hdl, std::move(buf), [this] {
			m_self->quit();
		});
	};
}

void socks5_state::handle_stream_data(const new_data_msg& msg) {
	if (msg.handle == m_local_hdl) {
		write(m_remote_hdl, msg.buf, [] {});
	} else {
		write(m_local_hdl, msg.buf, [] {});
	}
}

socks5_session::behavior_type
socks5_session_impl(socks5_session::stateful_broker_pointer<socks5_state> self, connection_handle hdl) {
	self->state.init(hdl);
	return {
		[self] (const new_data_msg& msg) {
			self->state.handle_new_data(msg);
		},
		[self] (const connection_closed_msg&) {
			self->quit();
		},
		[self] (ok_atom, connection_handle hdl) {
			self->state.handle_connect_succ(hdl);
		},
		[self] (error_atom, const std::string& what) {
			self->state.handle_connect_fail(what);
		}
	};
}

} }
