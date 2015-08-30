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
#include <chrono>
#include <string.h>

namespace ranger { namespace proxy {

socks5_state::socks5_state(socks5_session::broker_pointer self)
	: m_self(self) {
	// nop
}

socks5_state::~socks5_state() {
	if (m_encryptor) {
		anon_send_exit(m_encryptor, exit_reason::user_shutdown);
	}
}

void socks5_state::init(connection_handle hdl, encryptor enc) {
	m_local_hdl = hdl;
	m_self->configure_read(m_local_hdl, receive_policy::exactly(2));
	m_encryptor = enc;
	m_current_handler = &socks5_state::handle_select_method_header;
}

void socks5_state::handle_new_data(const new_data_msg& msg) {
	if (m_encryptor && msg.handle == m_local_hdl) {
		if (m_current_handler == &socks5_state::handle_stream_data) {
			m_self->send(m_encryptor, decrypt_atom::value, msg.buf);
		} else {
			scoped_actor self;
			self->sync_send(m_encryptor, decrypt_atom::value, msg.buf).await(
				[this] (decrypt_atom, const std::vector<char>& buf) {
					if (m_current_handler) {
						new_data_msg msg;
						msg.handle = m_local_hdl;
						msg.buf = buf;
						(this->*m_current_handler)(msg);
					}
				}
			);
		}
	} else if (m_current_handler) {
		(this->*m_current_handler)(msg);
	}
}

void socks5_state::handle_encrypted_data(const std::vector<char>& buf) {
	write_raw(m_local_hdl, buf);
}

void socks5_state::handle_decrypted_data(const std::vector<char>& buf) {
	write_to_remote(buf);
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

void socks5_state::write_to_local(std::vector<char> buf) const {
	if (m_encryptor) {
		m_self->send(m_encryptor, encrypt_atom::value, std::move(buf));
	} else {
		write_raw(m_local_hdl, std::move(buf));
	}
}

void socks5_state::write_to_remote(std::vector<char> buf) const {
	write_raw(m_remote_hdl, std::move(buf));
}

void socks5_state::write_raw(connection_handle hdl, std::vector<char> buf) const {
	auto& wr_buf = m_self->wr_buf(hdl);
	if (wr_buf.empty()) {
		wr_buf = std::move(buf);
	} else {
		std::copy(buf.begin(), buf.end(), std::back_inserter(wr_buf));
	}
	m_self->flush(hdl);
}

void socks5_state::handle_select_method_header(const new_data_msg& msg) {
	if (static_cast<uint8_t>(msg.buf[0]) != 0x05) {
		aout(m_self) << "ERROR: Protocol version mismatch" << std::endl;
		m_self->quit();
		return;
	}

	uint8_t nmethods = msg.buf[1];
	aout(m_self) << "INFO: recv select method header"
		<< " (nmethods == " << nmethods << ")" << std::endl;
	m_self->configure_read(m_local_hdl, receive_policy::exactly(nmethods));
	m_current_handler = &socks5_state::handle_select_method_data;
}

void socks5_state::handle_select_method_data(const new_data_msg& msg) {
	aout(m_self) << "INFO: recv select method data" << std::endl;
	if (std::find(msg.buf.begin(), msg.buf.end(), 0) != msg.buf.end()) {
		write_to_local({0x05, 0x00});
		m_self->configure_read(m_local_hdl, receive_policy::exactly(4));
		m_current_handler = &socks5_state::handle_request_header;
	} else {
		aout(m_self) << "ERROR: NO ACCEPTABLE METHODS" << std::endl;
		write_to_local({0x05, static_cast<char>(0xFF)});
	}
}

void socks5_state::handle_request_header(const new_data_msg& msg) {
	if (static_cast<uint8_t>(msg.buf[0]) != 0x05) {
		aout(m_self) << "ERROR: Protocol version mismatch" << std::endl;
		m_self->quit();
		return;
	}

	aout(m_self) << "INFO: recv request header" << std::endl;

	if (static_cast<uint8_t>(msg.buf[1]) != 0x01) {
		aout(m_self) << "ERROR: Command not supported" << std::endl;
		write_to_local({0x05, 0x07, 0x00, 0x01});
		m_self->delayed_send(m_self, std::chrono::seconds(2), close_atom::value);
		return;
	}

	switch (static_cast<uint8_t>(msg.buf[3])) {
	case 0x01:	// IPV4
		aout(m_self) << "INFO: CMD[connect] ADDR[ipv4]" << std::endl;
		m_self->configure_read(m_local_hdl, receive_policy::exactly(6));
		m_current_handler = &socks5_state::handle_ipv4_request_data;
		return;
	case 0x03:	// DOMAINNAME
		aout(m_self) << "INFO: CMD[connect] ADDR[domainname]" << std::endl;
		m_self->configure_read(m_local_hdl, receive_policy::exactly(1));
		m_current_handler = &socks5_state::handle_domainname_length;
		return;
	}

	aout(m_self) << "ERROR: Address type not supported" << std::endl;
	write_to_local({0x05, 0x08, 0x00, 0x01});
		m_self->delayed_send(m_self, std::chrono::seconds(2), close_atom::value);
}

void socks5_state::handle_ipv4_request_data(const new_data_msg& msg) {
	in_addr addr;
	memcpy(&addr, &msg.buf[0], sizeof(addr));
	uint16_t port;
	memcpy(&port, &msg.buf[4], sizeof(port));
	port = ntohs(port);

	aout(m_self) << "INFO: connect to " << inet_ntoa(addr) << ":" << port << std::endl;

	auto helper = m_self->spawn(connect_helper_impl, &m_self->parent().backend());
	m_self->send(helper, connect_atom::value, inet_ntoa(addr), port);

	m_self->configure_read(m_local_hdl, receive_policy::at_most(8192));
	m_current_handler = nullptr;

	port = htons(port);

	m_conn_succ_handler = [this, addr, port] (connection_handle remote_hdl) {
		m_self->assign_tcp_scribe(remote_hdl);
		m_remote_hdl = remote_hdl;

		aout(m_self) << "INFO: " << inet_ntoa(addr) << ":" << port << " connected" << std::endl;
		
		std::vector<char> buf = {0x05, 0x00, 0x00, 0x01};
		std::copy(	reinterpret_cast<const char*>(&addr),
					reinterpret_cast<const char*>(&addr) + sizeof(addr),
					std::back_inserter(buf));
		std::copy(	reinterpret_cast<const char*>(&port),
					reinterpret_cast<const char*>(&port) + sizeof(port),
					std::back_inserter(buf));
		write_to_local(std::move(buf));
		m_self->configure_read(m_remote_hdl, receive_policy::at_most(8192));
		m_current_handler = &socks5_state::handle_stream_data;
	};

	m_conn_fail_handler = [this, addr, port] (const std::string& what) {
		aout(m_self) << "ERROR: " << what << std::endl;
		std::vector<char> buf = {0x05, 0x05, 0x00, 0x01};
		std::copy(	reinterpret_cast<const char*>(&addr),
					reinterpret_cast<const char*>(&addr) + sizeof(addr),
					std::back_inserter(buf));
		std::copy(	reinterpret_cast<const char*>(&port),
					reinterpret_cast<const char*>(&port) + sizeof(port),
					std::back_inserter(buf));
		write_to_local(std::move(buf));
		m_self->delayed_send(m_self, std::chrono::seconds(2), close_atom::value);
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

	aout(m_self) << "INFO: connect to " << host << ":" << port << std::endl;

	auto helper = m_self->spawn(connect_helper_impl, &m_self->parent().backend());
	m_self->send(helper, connect_atom::value, host, port);

	m_self->configure_read(m_local_hdl, receive_policy::at_most(8192));
	m_current_handler = nullptr;

	port = htons(port);

	m_conn_succ_handler = [this, host, port] (connection_handle remote_hdl) {
		m_self->assign_tcp_scribe(remote_hdl);
		m_remote_hdl = remote_hdl;

		aout(m_self) << "INFO: " << host << ":" << port << " connected" << std::endl;

		std::vector<char> buf = {0x05, 0x00, 0x00, 0x03, static_cast<char>(host.size())};
		std::copy(host.begin(), host.end(), std::back_inserter(buf));
		std::copy(	reinterpret_cast<const char*>(&port),
					reinterpret_cast<const char*>(&port) + sizeof(port),
					std::back_inserter(buf));
		write_to_local(std::move(buf));
		m_self->configure_read(m_remote_hdl, receive_policy::at_most(8192));
		m_current_handler = &socks5_state::handle_stream_data;
	};

	m_conn_fail_handler = [this, host, port] (const std::string& what) {
		aout(m_self) << "ERROR: " << what << std::endl;
		std::vector<char> buf = {0x05, 0x05, 0x00, 0x03, static_cast<char>(host.size())};
		std::copy(host.begin(), host.end(), std::back_inserter(buf));
		std::copy(	reinterpret_cast<const char*>(&port),
					reinterpret_cast<const char*>(&port) + sizeof(port),
					std::back_inserter(buf));
		write_to_local(std::move(buf));
		m_self->delayed_send(m_self, std::chrono::seconds(2), close_atom::value);
	};
}

void socks5_state::handle_stream_data(const new_data_msg& msg) {
	if (msg.handle == m_local_hdl) {
		write_to_remote(msg.buf);
	} else {
		write_to_local(msg.buf);
	}
}

socks5_session::behavior_type
socks5_session_impl(socks5_session::stateful_broker_pointer<socks5_state> self,
					connection_handle hdl, encryptor enc) {
	self->state.init(hdl, enc);
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
		},
		[self] (encrypt_atom, const std::vector<char>& buf) {
			self->state.handle_encrypted_data(buf);
		},
		[self] (decrypt_atom, const std::vector<char>& buf) {
			self->state.handle_decrypted_data(buf);
		},
		[self] (close_atom) {
			self->quit();
		}
	};
}

} }
