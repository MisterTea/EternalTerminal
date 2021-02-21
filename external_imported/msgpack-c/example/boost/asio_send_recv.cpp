// MessagePack for C++ example
//
// Copyright (C) 2017 KONDO Takatoshi
//
//    Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//    http://www.boost.org/LICENSE_1_0.txt)
//

#include <string>
#include <sstream>
#include <iostream>

#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>

#include <msgpack.hpp>

int main() {
    boost::asio::io_service ios;
    std::uint16_t const port = 12345;

    // Server
    std::size_t const window_size = 10;
    boost::asio::ip::tcp::acceptor ac(ios, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port));
    boost::asio::ip::tcp::socket ss(ios);
    std::function<void()> do_accept;
    std::function<void()> do_async_read_some;

    msgpack::unpacker unp;

    do_accept = [&] {
        ac.async_accept(
            ss,
            [&]
            (boost::system::error_code const& e) {
                if (e) {
                    std::cout << __LINE__ << ":" << e.message() << std::endl;
                    return;
                }
                do_async_read_some = [&] {
                    unp.reserve_buffer(window_size);
                    ss.async_read_some(
                        boost::asio::buffer(unp.buffer(), window_size),
                        [&](boost::system::error_code const& e, std::size_t bytes_transferred) {
                            if (e) {
                                std::cout << __LINE__ << ":" << e.message() << std::endl;
                                return;
                            }
                            std::cout << bytes_transferred << " bytes read." << std::endl;
                            unp.buffer_consumed(bytes_transferred);
                            msgpack::object_handle oh;
                            while (unp.next(oh)) {
                                std::cout << oh.get() << std::endl;
                                // In order to finish the program,
                                // return if one complete msgpack is processed.
                                // In actual server, don't return here.
                                return;
                            }
                            do_async_read_some();
                        }
                    );
                };
                do_async_read_some();
            }
        );
    };
    do_accept();

    // Client
    auto host = "localhost";
    boost::asio::ip::tcp::resolver r(ios);

#if BOOST_VERSION < 106600
    boost::asio::ip::tcp::resolver::query q(host, boost::lexical_cast<std::string>(port));
    auto it = r.resolve(q);
    boost::asio::ip::tcp::resolver::iterator end;
#else  // BOOST_VERSION < 106600
    auto eps = r.resolve(host, boost::lexical_cast<std::string>(port));
    auto it = eps.begin();
    auto end = eps.end();
#endif // BOOST_VERSION < 106600

    boost::asio::ip::tcp::socket cs(ios);
    boost::asio::async_connect(
        cs,
        it,
        end,
        [&]
        (boost::system::error_code const& e, boost::asio::ip::tcp::resolver::iterator) {
            if (e) {
                std::cout << __LINE__ << ":" << e.message() << std::endl;
                return;
            }
            std::cout << __LINE__ << ":client connected" << std::endl;
            msgpack::sbuffer sb;
            msgpack::pack(sb, std::make_tuple(42, false, "hello world", 12.3456));
            write(cs, boost::asio::buffer(sb.data(), sb.size()));
        }
    );

    // Start
    ios.run();
}
